#!/bin/bash
# S3c runtime half: start the i2chid family in QEMU with no matching LPSS
# device, and require every ordered init step, no stub trap and a clean exit.
#
#   REGRESS i2chid-sim: PASS
#   REGRESS i2chid-sim: FAIL <reason>
#
# Reuses the apitest embed slot (the family tag is set with kurono.kld.family so
# the broker knows it is the acpi container); KLD_EXTRA adds boot flags. the ISO
# is deleted pass or fail. (satoru)
# end (satoru)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(cat "$ROOT/kld/KLD_KERNEL_VERSION")
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
SCRATCH="$SCRATCH_ROOT/regress_i2chid_sim_$STAMP"
LOG="$SCRATCH/serial.log"
ISO="$SCRATCH/kurono_i2chid_sim_$STAMP.iso"
TIMEOUT=${KLD_REG_TIMEOUT:-300}
GUEST_SMP=${KLD_SMP:-4}
GUEST_RAM_MB=${KLD_RAM_MB:-2048}
QP=
cleanup() {
    if [ -n "$QP" ] && kill -0 "$QP" 2>/dev/null; then kill "$QP" 2>/dev/null; sleep 1; kill -9 "$QP" 2>/dev/null; fi
    wait 2>/dev/null
    rm -f "$ISO" "$ROOT/build/kurono.iso"
}
fail() {
    echo "REGRESS i2chid-sim: FAIL $1"
    [ -f "$LOG" ] && { echo "--- serial.log (last 60 lines) ---"; tail -60 "$LOG"; } >&2
    exit 1
}

# a [kld-warn] line is a FAIL unless it matches warn_allowlist.txt. (satoru)
check_kld_warns() {
    local allow="$ROOT/kld/tests/warn_allowlist.txt" bad=0 w a key ok
    [ -f "$LOG" ] || return 0
    while IFS= read -r w; do
        ok=0
        if [ -f "$allow" ]; then
            while IFS= read -r a; do
                case "$a" in ""|\#*) continue ;; esac
                key=${a%% *}
                case "$w" in *"$key"*) ok=1; break ;; esac
            done < "$allow"
        fi
        if [ "$ok" = 0 ]; then
            echo "[warn] $w" >&2
            bad=1
        fi
    done < <(grep -a '\[kld-warn\]' "$LOG" 2>/dev/null || true)
    [ "$bad" = 0 ] || fail "unallowlisted [kld-warn] line"
}
trap cleanup EXIT
if [ -z "${LINUX:-}" ]; then
    for c in "$ROOT/../third_party/linux-$KVER" "$SCRATCH_ROOT/linux-$KVER" \
             "/mnt/windows/Users/ibrah/Downloads/OS/third_party/linux-$KVER"; do
        [ -d "$c/include/linux" ] && LINUX=$c && break
    done
fi
[ -n "${LINUX:-}" ] && [ -d "$LINUX/include/linux" ] || fail "no configured linux-$KVER tree"
command -v qemu-system-x86_64 >/dev/null || fail "qemu-system-x86_64 not found"
command -v musl-gcc >/dev/null || fail "musl-gcc not found"
[ -e /dev/kvm ] || fail "/dev/kvm not available"
mkdir -p "$SCRATCH"
echo "[regress] building i2chid" >&2
make -C "$ROOT/kld" driver=i2chidsim LINUX="$LINUX" -j"$(nproc)" >"$SCRATCH/build.log" 2>&1 \
    || fail "container build (see $SCRATCH/build.log)"
cp "$ROOT/kld/build/i2chidsim/kld-i2chidsim.elf" "$ROOT/src/userprogs/kld_apitest.elf" \
    || fail "copy container"
echo "[regress] building ISO (apitest embed slot)" >&2
make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 KURONO_KLD_EMBED=1 \
    KURONO_EXTRA_CMDLINE="kurono.kld=apitest kurono.kld.family=i2chidsim kurono.sched=2 ${KLD_EXTRA:-}" >"$SCRATCH/iso.log" 2>&1 \
    || fail "iso build (see $SCRATCH/iso.log)"
[ -f "$ROOT/build/kurono.iso" ] || fail "iso missing"
cp "$ROOT/build/kurono.iso" "$ISO" || fail "copy iso"
echo "[regress] booting qemu (-smp $GUEST_SMP -m ${GUEST_RAM_MB}M), log $LOG" >&2
qemu-system-x86_64 -enable-kvm -cpu host -smp "$GUEST_SMP" -m "${GUEST_RAM_MB}M" \
    -machine pc -nodefaults -device VGA,addr=0x3 -boot d -cdrom "$ISO" \
    -display none -no-reboot -serial file:"$LOG" >"$SCRATCH/qemu.err" 2>&1 &
QP=$!
deadline=$((SECONDS + TIMEOUT))
while [ "$SECONDS" -lt "$deadline" ]; do
    if [ -f "$LOG" ] && grep -q '\[kld\] container done' "$LOG"; then break; fi
    if [ -f "$LOG" ] && grep -qE 'container aborting|LX_UNIMPL|\[kld-stub\]' "$LOG"; then break; fi
    kill -0 "$QP" 2>/dev/null || break
    sleep 1
done
kill "$QP" 2>/dev/null
for _ in $(seq 1 10); do kill -0 "$QP" 2>/dev/null || break; sleep 1; done
kill -9 "$QP" 2>/dev/null
wait "$QP" 2>/dev/null
QP=
[ -f "$LOG" ] || fail "qemu produced no serial log"
grep -qE 'container aborting|LX_UNIMPL|\[kld-stub\]' "$LOG" && fail "stub/abort: $(grep -m1 -E 'container aborting|LX_UNIMPL|\[kld-stub\]' "$LOG")"
grep -q '\[i2chid_sim\] family init rc=0 bound=1' "$LOG" || fail "sim family init/bound missing"
grep -q '\[lx_device\] device_add i2c_designware\.0' "$LOG" || fail "mfd cell i2c_designware.0 missing"
grep -q '\[lx_platform\] probe i2c_designware\.0' "$LOG" || fail "designware probe not entered"
grep -q '\[i2chidsim\] C2 hid rc=2 match=1' "$LOG" || fail "HID descriptor transfer"
grep -q '\[i2chidsim\] C2 report rc=2 match=1' "$LOG" || fail "report descriptor transfer"
grep -q '\[i2chidsim\] C2 noack rc=121' "$LOG" || fail "NOACK path"
grep -q 'timeout rc=110 recovered=1' "$LOG" || fail "timeout/recovery"
# C3.1c: the in-container acpica enumerator, same fields as S3a. (satoru)
grep -qF '[kld-tp-acpi] device \_SB_.PC02.I2C1.TPD0 hid=ELAN06FA cid=PNP0C50 sta=0xf' "$LOG" || fail "C3.1 TPD0 path/hid"
grep -qF 'i2c addr=0x15 speed=400000 access=7-bit controller=\_SB.PC02.I2C1 controller-hid=- pci=0000:80:15.1 adr=0x150001 bbn=0x80 seg=0x0' "$LOG" || fail "C3.1 TPD0 i2c resources"
grep -qF 'gpio-int pin=552 trigger=level polarity=active-low share=exclusive debounce=0 controller=\_SB.PGPI controller-hid=INTC1084' "$LOG" || fail "C3.1 TPD0 gpio line (host-consistent GPIO 552)"
grep -qF 'hid-descriptor=0x1' "$LOG" || fail "C3.1 TPD0 _DSM"
grep -qF 'devices in namespace: 249' "$LOG" || fail "C3.1 namespace device count"
grep -qF '[i2chidsim] C3.1 PNP0C50 found=1' "$LOG" || fail "C3.1 PNP0C50 count"
# C3.2: i2c-core-acpi created the client, i2c-hid-acpi probed it through the
# designware model, and its irq came from the ACPI GpioInt. (satoru)
grep -qF '[kld-acpi] PCI companion I2C1 adr 0x00150001' "$LOG" || fail "C3.2 PCI companion"
grep -qF '[i2chidsim] C4.1 INTC1084 platform device' "$LOG" || fail "C4.1 INTC1084 platform device"
grep -qF '[kld-acpi] _CRS irq gsi=14 count=1 level active-low shared' "$LOG" || fail "C4.1 GpioInt GSI"
grep -qF 'platform res mem 0x006e0000-0x006effff' "$LOG" || fail "C4.1 community range 0"
grep -qF 'platform res mem 0x00690000-0x0069ffff' "$LOG" || fail "C4.1 community range 4"
grep -qF '[i2chidsim] C4.2 pinctrl-meteorpoint probe rc=0, 5 communities, 339 pads, gpiochip registered' "$LOG" || fail "C4.2 pinctrl probe"
grep -qF '[i2chidsim] C4.3 pin 310 = community 4 pad GPP_I_8 PADCFG0=0x80800102 (gpio, level active-low)' "$LOG" || fail "C4.3 pad configuration"
grep -qF '[i2chidsim] C4.3 negatives ok' "$LOG" || fail "C4.3 negatives"
grep -qE '^\[[0-9]+\] \[i2chidsim\] C3.2 client=TPD0 addr=0x15 irq=[0-9]+ driver=i2c_hid_acpi$' "$LOG" || fail "C3.2 client/driver"
grep -qF '[i2chidsim] C3.2 dw timing:' "$LOG" || fail "C3.2 designware timing"
# C3.4: the physical device property, the power method list and the two D0
# halves (PMCSR through the PCI layer, _PS0 hook through lx_acpi). (satoru)
grep -qF '[kld-acpi] power \_SB.PC02.I2C1:' "$LOG" || fail "C3.4 I2C1 power list"
grep -qF '[kld-acpi] power \_SB.PC02.I2C1.TPD0:' "$LOG" || fail "C3.4 TPD0 power list"
grep -qF '[lx_pci] PMCSR -> D0 at cap' "$LOG" || fail "C3.4 PMCSR D0 write"
grep -qF '[i2chidsim] C3.4 PMCSR=0x0000 state=D0' "$LOG" || fail "C3.4 PMCSR read back"
grep -qF '[kld-acpi] companion I2C1' "$LOG" || fail "C3.4 companion power hook"
# C3.3: the checklist and the four S2 scenarios, end to end. (satoru)
for n in 1 2 4 5 6 7; do
    grep -q "\[kld-tp\] step $n ok" "$LOG" || fail "C3.3 checklist step $n"
done
grep -qF '[i2chidsim] C3.3 scenarios 4/4 PASS' "$LOG" || fail "C3.3 scenarios"
grep -q '\[kld-input\]' "$LOG" || fail "pointer bridge did not reach the kernel"
check_kld_warns
grep -q 'container exited rc=0' "$LOG" || fail "container did not exit 0"
for s in i2c-core-base pinctrl-meteorpoint intel-lpss intel-lpss-pci i2c-designware-platdrv i2c-hid-acpi hid-multitouch; do
    grep -q "\[i2chid\] init $s rc=0" "$LOG" || fail "missing init step $s"
done
echo "[regress] serial log kept at $LOG" >&2
echo "REGRESS i2chid-sim: PASS"
exit 0

# end (satoru)
