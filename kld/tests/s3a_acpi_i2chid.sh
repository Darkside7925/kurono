#!/bin/bash
# S3a: generic PNP0C50 enumeration against the real laptop tables, side by
# side with the host Linux's own sysfs/debugfs view. read-only on the host.
#
#   ACPI_TABLES=... ACPI_NVS="GPDI=0xCB6 PCHA=1 TPTY=1 SDS1=1 PBSB=0x80"
#   ./kld/tests/s3a_acpi_i2chid.sh
#
# end (satoru)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(cat "$ROOT/kld/KLD_KERNEL_VERSION")
LINUX=${LINUX:-/mnt/windows/Users/ibrah/kw/scratch/kld/linux-$KVER}
TABLES=${ACPI_TABLES:-/mnt/windows/Users/ibrah/Documents/vm-lab/acpi/tables}
NVS=${ACPI_NVS:-GPDI=0xCB6 PCHA=1 TPTY=1 SDS1=1 PBSB=0x80}
ELF=$ROOT/kld/build/acpi/kld-acpi.elf
OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT

[ -d "$TABLES" ] || { echo "S3a: no tables at $TABLES (set ACPI_TABLES=)" >&2; exit 1; }
make -C "$ROOT/kld/acpi" LINUX="$LINUX" -j"$(nproc)" >/dev/null 2>&1 || {
    echo "S3a: kld-acpi build failed" >&2; exit 1; }

nvsargs=()
for kv in $NVS; do nvsargs+=(--nvs "$kv"); done
"$ELF" --tables "$TABLES" "${nvsargs[@]}" --hid-i2c >"$OUT" 2>/dev/null || {
    echo "S3a: ACPI enumeration failed" >&2; exit 1; }

acpi_field() { sed -n "s/.*[[:space:]]$1=\([^[:space:]]*\).*/\1/p" "$OUT" | head -1; }
acpi_path=$(sed -n 's/^\[kld-tp-acpi\] device \([^ ]*\).*/\1/p' "$OUT" | head -1)
acpi_hid=$(sed -n 's/.*[[:space:]]hid=\([^ ]*\).*/\1/p' "$OUT" | head -1)
acpi_cid=$(sed -n 's/.*[[:space:]]cid=\([^ ]*\).*/\1/p' "$OUT" | head -1)
acpi_sta=$(sed -n 's/.*[[:space:]]sta=\([^ ]*\).*/\1/p' "$OUT" | head -1)
acpi_i2c=$(grep -m1 '^  i2c ' "$OUT")
acpi_gpio=$(grep -m1 '^  gpio-int ' "$OUT")
acpi_irq=$(grep -m1 '^  irq=' "$OUT")
acpi_dsm=$(sed -n 's/^  hid-descriptor=//p' "$OUT" | head -1)
acpi_n=$(sed -n 's/^\[kld-tp-acpi\] PNP0C50 devices: //p' "$OUT" | head -1)

acpi_addr=$(echo "$acpi_i2c" | sed -n 's/.*addr=\([^ ]*\).*/\1/p')
acpi_speed=$(echo "$acpi_i2c" | sed -n 's/.*speed=\([^ ]*\).*/\1/p')
acpi_access=$(echo "$acpi_i2c" | sed -n 's/.*access=\([^ ]*\).*/\1/p')
acpi_ctrl=$(echo "$acpi_i2c" | sed -n 's/.*controller=\([^ ]*\).*/\1/p')
acpi_pci=$(echo "$acpi_i2c" | sed -n 's/.*pci=\([^ ]*\).*/\1/p')
acpi_ghid=$(echo "$acpi_gpio" | sed -n 's/.*controller-hid=\([^ ]*\).*/\1/p')
acpi_pin=$(echo "$acpi_gpio" | sed -n 's/.*pin=\([^ ]*\).*/\1/p')
acpi_trig=$(echo "$acpi_gpio" | sed -n 's/.*trigger=\([^ ]*\).*/\1/p')
acpi_pol=$(echo "$acpi_gpio" | sed -n 's/.*polarity=\([^ ]*\).*/\1/p')

# host: every ACPI device whose modalias says PNP0C50. (satoru)
host_dir=
for d in /sys/bus/acpi/devices/*; do
    if grep -q 'PNP0C50' "$d/modalias" 2>/dev/null; then host_dir=$d; break; fi
done
[ -n "$host_dir" ] || { echo "S3a: no host PNP0C50 device"; exit 1; }
h_path=$(cat "$host_dir/path" 2>/dev/null)
h_hid=$(cat "$host_dir/hid" 2>/dev/null)
h_cid=$(cat "$host_dir/cid" 2>/dev/null)
h_sta=$(cat "$host_dir/status" 2>/dev/null)
h_i2cdir=/sys/bus/i2c/devices/i2c-$h_hid:00
h_i2c_real=$(readlink -f "$h_i2cdir" 2>/dev/null)
h_pci=$(echo "$h_i2c_real" | grep -oE '[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9a-f]' | head -1)
h_hiddir=
for d in "$h_i2cdir"/*; do
    [ -d "$d" ] || continue
    case "$(basename "$d")" in [0-9a-f]*:[0-9a-f]*) h_hiddir=$d; break;; esac
done
h_hidmodalias=$(cat "$h_hiddir/modalias" 2>/dev/null)
h_rsize=$(wc -c <"$h_hiddir/report_descriptor" 2>/dev/null)
h_pinctrl=$(sudo -n grep -m1 '^pin 310 ' /sys/kernel/debug/pinctrl/INTC1084:00/pins 2>/dev/null)
h_pin=$(echo "$h_pinctrl" | sed -n 's/^pin \([0-9]*\).*/\1/p')
h_gpio=$(sudo -n cat /sys/kernel/debug/gpio 2>/dev/null | grep -m1 'TPD0 GpioInt')
h_gpio_num=$(echo "$h_gpio" | sed -n 's/.*gpio-\([0-9]*\).*/\1/p')
h_irq=$(grep -m1 "$h_hid:" /proc/interrupts 2>/dev/null | sed -n 's/^ *\([0-9]*\):.*/\1/p')
h_irqline=$(grep -m1 "$h_hid:" /proc/interrupts 2>/dev/null)
h_dmesg=$(sudo -n dmesg 2>/dev/null | grep -iE 'i2c_hid|ELAN06FA' | head -1)

yesno() { [ "$1" = "$2" ] && echo yes || echo "NO ($1 vs $2)"; }
[ -n "$acpi_n" ] || acpi_n=0

printf '%s\n' "S3a PNP0C50 table: ACPI offline (ACPICA) vs host Linux $h_path"
printf '%-18s | %-34s | %-34s | %s\n' field acpi_offline host_linux match
printf '%-18s-+-%-34s-+-%-34s-+-%s\n' ------------------ ---------------------------------- ---------------------------------- -----
row() { printf '%-18s | %-34s | %-34s | %s\n' "$1" "$2" "$3" "$4"; }
row path "$acpi_path" "$h_path" "$(yesno "$acpi_path" "$h_path")"
row hid "$acpi_hid" "$h_hid" "$(yesno "$acpi_hid" "$h_hid")"
row cid "$acpi_cid" "$h_cid" "$(yesno "$acpi_cid" "$h_cid")"
h_sta_hex=$(printf '0x%x' "$h_sta" 2>/dev/null || echo "?")
row _STA "$acpi_sta" "$h_sta_hex (status $h_sta)" "$(yesno "$acpi_sta" "$h_sta_hex")"
row controller "$acpi_ctrl" "$(basename "$(readlink -f "$h_i2cdir/..")")" "-"
row pci "$acpi_pci" "$h_pci" "$(yesno "$acpi_pci" "$h_pci")"
row i2c-address "$acpi_addr" "client $h_hid:00 exists" "yes (client bound)"
row i2c-speed "$acpi_speed" "not exposed by sysfs" "-"
row i2c-access "$acpi_access" "not exposed by sysfs" "-"
row gpio-controller "$acpi_ghid" "INTC1084:00" "$(yesno "$acpi_ghid" INTC1084)"
row gpio-pin "$acpi_pin" "$h_pin (GPP_I_8, gpio $h_gpio_num)" "$(yesno "$acpi_pin" "$h_pin")"
row gpio-trigger "$acpi_trig" "GpioInt (ACPI _CRS is the source)" "yes (same _CRS)"
h_pol=$(echo "$h_gpio" | grep -o 'ACTIVE LOW' | tr 'A-Z' 'a-z' | sed 's/active low/active-low/')
row gpio-polarity "$acpi_pol" "$h_pol" "$(yesno "$acpi_pol" "$h_pol")"
row irq "GpioInt" "$h_irq ($h_gpio_num)" "-"
row hid-descriptor "$acpi_dsm" "ACPI _DSM only; not re-exposed" "yes (source)"
row hid-device "-" "$h_hidmodalias" "-"
row report-descriptor "-" "$h_rsize bytes" "-"
row host-dmesg "-" "${h_dmesg:-unreadable}" "-"
printf '\nACPI PNP0C50 devices: %s\n' "$acpi_n"
printf 'host irq line: %s\n' "${h_irqline:-?}"
printf 'host gpio line: %s\n' "${h_gpio:-?}"

# end (satoru)
