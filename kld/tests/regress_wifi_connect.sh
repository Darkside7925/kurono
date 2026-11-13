#!/bin/bash
# wifi connect M4 rig: build the unmodified iwlwifi family into a KLD
# container, pass the owner's wifi_test.conf through QEMU fw_cfg only, and
# require association + WPA2 + data-path + HTTPS markers on the real BE200.
#
#   REGRESS wifi_connect: PASS
#   REGRESS wifi_connect: FAIL <reason>
#
# Secret rules: no set -x, the conf is never copied into the run directory,
# only its byte count is logged, and the PSK is checked against the log and
# every run file before PASS. Only 0000:82:00.0 is passed.
#
# KLD_WIFI_FWSTORE=1 boots a kernel WITHOUT the embedded firmware and attaches
# a KURONO FAT32 store on NVMe; the driver must be served every file it asks
# for from that store. KLD_WIFI_NOSTORE=1 is the negative: same kernel, no
# store disk, the container must fail nonzero without a hang or fault.
# KLD_WIFI_IOMMU=1 boots q35 with QEMU's intel-iommu and requires the iwlwifi
# domain to be released with mapped frames and zero DMAR faults.
# KLD_WIFI_IOMMU_OFF=1 adds kurono.kld.iommu=0 for the A/B control.
#
# Normal connect runs print three verdicts (VERDICT DRIVER / TRAFFIC / HOST) and
# exit 0 when DRIVER passes and (TRAFFIC passes or the run is host-disturbed: a
# HARD steal line or a guest gap over 3 s). The line "REGRESS wifi_connect:
# PASS" still ends a passing run. KLD_WIFI_STRICT=1 keeps the old all-or-nothing
# rule (60/60 first-try GETs, large download) so old and new numbers compare.
# KLD_WIFI_APLOSS=1 runs the disconnect/reconnect mode and additionally
# requires the second association within 20 s of the local deauth: a real AP
# loss would need control of the owner's access point or the radio path, so the
# loss is forced locally (wifi_ctl issues the deauth), never at the AP.
# TRAFFIC needs >= 57 of 60 GETs (a failed GET is retried once and counted as
# retried) and a large download of at least 2.0 MiB/s: the 20 undisturbed
# passing runs of the T1 A/B measured 5.4 to 6.9 MiB/s (median 6.5), the four
# lower ones (0.8 to 3.1) were host-stalled runs. (satoru)
# end (satoru)
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(cat "$ROOT/kld/KLD_KERNEL_VERSION")
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
SCRATCH="$SCRATCH_ROOT/regress_wifi_connect_$STAMP"
LOG="$SCRATCH/serial.log"
ISO="$SCRATCH/kurono_wifi_connect_$STAMP.iso"
PIDFILE="$SCRATCH/qemu_wifi_connect_$STAMP.pid"
CONF=${KLD_WIFI_CONF:-$SCRATCH_ROOT/wifi_test.conf}
TIMEOUT=${KLD_REG_TIMEOUT:-420}
BDF=0000:82:00.0
GUEST_SMP=${KLD_SMP:-4}
GUEST_RAM_MB=${KLD_MEM_MB:-2048}
GUEST_RAM_KB=$((GUEST_RAM_MB * 1024))
NEED_MEM_KB=$((GUEST_RAM_KB + 6 * 1024 * 1024))
NEGATIVE=${KLD_WIFI_NEGATIVE:-0}
FWSTORE=${KLD_WIFI_FWSTORE:-0}
NOSTORE=${KLD_WIFI_NOSTORE:-0}
IOMMU=${KLD_WIFI_IOMMU:-0}
IOMMU_OFF=${KLD_WIFI_IOMMU_OFF:-0}
STRICT=${KLD_WIFI_STRICT:-0}
APLOSS=${KLD_WIFI_APLOSS:-0}
[ "$APLOSS" = 1 ] && KLD_WIFI_RECONNECT=1
BIG_FLOOR_MIBS=2.0
MODE3=0
if [ "$NEGATIVE" != 1 ] && [ "$NOSTORE" != 1 ] && [ "${KLD_WIFI_SOAK:-0}" != 1 ] \
   && [ "${KLD_WIFI_RECONNECT:-0}" != 1 ] && [ "$STRICT" != 1 ]; then MODE3=1; fi
DRV_REASONS=()
TRF_REASONS=()
LOAD_START=$(cut -d' ' -f1-3 /proc/loadavg)
[ "$FWSTORE" = 1 ] && [ "$NOSTORE" = 1 ] && { echo "REGRESS wifi_connect: FAIL KLD_WIFI_FWSTORE and KLD_WIFI_NOSTORE are exclusive" >&2; exit 1; }
FWDISK="$SCRATCH/fwstore.img"
STORE="$SCRATCH/store"
PART_OFF_BYTES=$((2048 * 512))
PART_MB=44
QP=

alive() { [ -n "$QP" ] && sudo -n kill -0 "$QP" 2>/dev/null; }
cleanup() {
    if alive; then
        sudo -n kill "$QP" 2>/dev/null
        sleep 1
        alive && sudo -n kill -9 "$QP" 2>/dev/null
    fi
    rm -f "$PIDFILE" "$ISO" "$FWDISK"
    [ -n "${BAD_CONF:-}" ] && rm -f "$BAD_CONF"
}
trap cleanup EXIT

fail() {
    echo "REGRESS wifi_connect: FAIL $1"
    if [ -f "$LOG" ]; then
        { echo "--- serial.log (last 60 lines) ---"; tail -60 "$LOG"; } >&2
    fi
    exit 1
}

# checks that are DRIVER or TRAFFIC verdict items in the three-verdict mode and
# a hard failure otherwise. (satoru)
dfail() { if [ "$MODE3" = 1 ]; then DRV_REASONS+=("$1"); else fail "$1"; fi; }
tfail() { if [ "$MODE3" = 1 ]; then TRF_REASONS+=("$1"); else fail "$1"; fi; }

if [ -z "${LINUX:-}" ]; then
    for c in "$ROOT/../third_party/linux-$KVER" \
             "$SCRATCH_ROOT/linux-$KVER" \
             "/mnt/windows/Users/ibrah/Downloads/OS/third_party/linux-$KVER"; do
        if [ -d "$c/include/linux" ]; then LINUX="$c"; break; fi
    done
fi
[ -n "${LINUX:-}" ] && [ -d "$LINUX/include/linux" ] || fail "no configured linux-$KVER tree"
[ -r "$CONF" ] || fail "wifi conf not readable"
command -v qemu-system-x86_64 >/dev/null || fail "qemu-system-x86_64 not found"
command -v musl-gcc >/dev/null || fail "musl-gcc not found"
[ -e /dev/kvm ] || fail "/dev/kvm not available"
DRV=$(basename "$(readlink -f "/sys/bus/pci/devices/$BDF/driver" 2>/dev/null)" 2>/dev/null)
[ "$DRV" = "vfio-pci" ] || fail "$BDF is not bound to vfio-pci (driver=${DRV:-none})"
sudo -n true 2>/dev/null || fail "sudo -n not available"

mkdir -p "$SCRATCH"

# negative: derive a one-character-wrong PSK in a temp file under the run dir.
BAD_CONF=
REAL_CONF=$CONF
REAL_PSK=
BAD_PSK=
if [ "$NEGATIVE" = 1 ]; then
    REAL_SSID=
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            SSID=*) REAL_SSID=${line#SSID=} ;;
            PSK=*) REAL_PSK=${line#PSK=} ;;
        esac
    done < "$CONF"
    [ -n "$REAL_SSID" ] && [ -n "$REAL_PSK" ] || fail "bad wifi conf for negative"
    last=${REAL_PSK: -1}
    case "$last" in
        A) repl=B ;; Z) repl=A ;; *) repl=A ;;
    esac
    BAD_PSK="${REAL_PSK%?}$repl"
    BAD_CONF="$SCRATCH/wifi_negative.conf"
    printf 'SSID=%s\nPSK=%s\n' "$REAL_SSID" "$BAD_PSK" > "$BAD_CONF"
    CONF="$BAD_CONF"
fi

echo "[wifi-connect] fetching and verifying firmware" >&2
"$ROOT/kld/tools/fetch_firmware.sh" >"$SCRATCH/fetch.log" 2>&1 || fail "fetch_firmware"

echo "[wifi-connect] building the iwlwifi container" >&2
make -C "$ROOT/kld" driver=iwlwifi LINUX="$LINUX" -j"$(nproc)" \
    >"$SCRATCH/build.log" 2>&1 || fail "container build (see $SCRATCH/build.log)"
cp "$ROOT/kld/build/iwlwifi/kld-iwlwifi.elf" "$ROOT/src/userprogs/kld_iwlwifi.elf" \
    || fail "copy container"
cp "$ROOT/kld/build/iwlwifi/kld-iwlwifi.elf" "$SCRATCH/kld-iwlwifi.elf" || fail "keep container ELF"

if [ "$FWSTORE" = 1 ] || [ "$NOSTORE" = 1 ]; then
    echo "[wifi-connect] building the iwlwifi firmware store" >&2
    "$ROOT/kld/tools/pack_firmware.sh" iwlwifi --device 8086:272b "$STORE" \
        >"$SCRATCH/pack.log" 2>&1 || fail "pack_firmware (see $SCRATCH/pack.log)"
    find "$STORE/firmware/iwlwifi" -name 'FW*.BIN' | grep -q . \
        || fail "firmware store has no files"
    if [ "$FWSTORE" = 1 ]; then
        echo "[wifi-connect] creating the KURONO GPT/FAT32 store disk" >&2
        dd if=/dev/zero of="$FWDISK" bs=1M count=48 status=none || fail "create store disk"
        sgdisk -n 1:2048:+${PART_MB}M -t 1:0700 -c 1:KURONO "$FWDISK" >/dev/null \
            || fail "sgdisk store disk"
        dd if=/dev/zero of="$SCRATCH/part.img" bs=1M count=$PART_MB status=none || fail "create store partition"
        mkfs.vfat -F 32 -n KURONO "$SCRATCH/part.img" >/dev/null || fail "mkfs.vfat store partition"
        dd if="$SCRATCH/part.img" of="$FWDISK" bs=1M seek=1 conv=notrunc status=none || fail "write store partition"
        rm -f "$SCRATCH/part.img"
        mmd -i "$FWDISK@@$PART_OFF_BYTES" ::/firmware || fail "mtools mkdir firmware"
        mmd -i "$FWDISK@@$PART_OFF_BYTES" ::/firmware/iwlwifi || fail "mtools mkdir iwlwifi"
        mcopy -i "$FWDISK@@$PART_OFF_BYTES" "$STORE"/firmware/iwlwifi/* ::/firmware/iwlwifi/ \
            || fail "mtools copy iwlwifi store"
        mcopy -i "$FWDISK@@$PART_OFF_BYTES" "$STORE"/firmware/REGULATO.DB ::/firmware/ \
            || fail "mtools copy regulatory.db"
        mcopy -i "$FWDISK@@$PART_OFF_BYTES" "$STORE"/firmware/REGULATO.P7S ::/firmware/ \
            || fail "mtools copy regulatory.db.p7s"
    fi
fi

echo "[wifi-connect] building the ISO" >&2
EXTRA_CMDLINE=
[ -n "${KLD_WIFI_BAND:-}" ] && EXTRA_CMDLINE="$EXTRA_CMDLINE kurono.kldwifi.band=$KLD_WIFI_BAND"
[ "${KLD_WIFI_SOAK:-0}" = 1 ] && EXTRA_CMDLINE="$EXTRA_CMDLINE kurono.kldwifi.soak=1"
[ "${KLD_WIFI_RECONNECT:-0}" = 1 ] && EXTRA_CMDLINE="$EXTRA_CMDLINE kurono.kldwifi.reconnect=1"
[ "$IOMMU_OFF" = 1 ] && EXTRA_CMDLINE="$EXTRA_CMDLINE kurono.kld.iommu=0"
[ -n "${KLD_EXTRA_CMDLINE:-}" ] && EXTRA_CMDLINE="$EXTRA_CMDLINE $KLD_EXTRA_CMDLINE"   # e.g. kurono.directmap=1 (satoru)
[ "${KLD_WIFI_STATS:-0}" = 1 ] && EXTRA_CMDLINE="$EXTRA_CMDLINE kurono.kld.stats=1"
FWMAKE='KURONO_KLD_FW=1'
if [ "$FWSTORE" = 1 ] || [ "$NOSTORE" = 1 ]; then
    FWMAKE=
    EXTRA_CMDLINE="$EXTRA_CMDLINE kurono.diskguard=1"
fi
rm -f "$ROOT/build/kurono_base.elf" "$ROOT/build/kurono.elf" "$ROOT/build/kurono.iso"
make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 \
    KURONO_KLD_EMBED=1 $FWMAKE \
    KURONO_EXTRA_CMDLINE="kurono.kld=iwlwifi kurono.kldwifi=connect kurono.kld.bench=1 kurono.sched=2 $EXTRA_CMDLINE" \
    >"$SCRATCH/iso.log" 2>&1 || fail "iso build (see $SCRATCH/iso.log)"
[ -f "$ROOT/build/kurono.iso" ] || fail "iso missing after build"
cp "$ROOT/build/kurono.iso" "$ISO" || fail "copy iso"

waited=0
while :; do
    avail=$(awk '/^MemAvailable:/ { print $2 }' /proc/meminfo)
    [ -n "$avail" ] && [ "$avail" -ge "$NEED_MEM_KB" ] && break
    waited=$((waited + 10))
    [ "$waited" -ge 1200 ] && fail "MemAvailable below $NEED_MEM_KB kB after 20 min"
    sleep 10
done

DISK_ARGS=()
if [ "$FWSTORE" = 1 ]; then
    DISK_ARGS=(-drive file="$FWDISK",if=none,id=fwstore,format=raw -device nvme,serial=kuronofw,drive=fwstore)
fi
MACHINE_ARGS=(-machine pc)
if [ "$IOMMU" = 1 ]; then
    MACHINE_ARGS=(-machine q35,kernel-irqchip=split -device intel-iommu,caching-mode=on,aw-bits=39)
fi
echo "[wifi-connect] booting qemu with $BDF (-smp $GUEST_SMP -m ${GUEST_RAM_MB}M, -nic none, fwstore=$FWSTORE nostore=$NOSTORE iommu=$IOMMU off=$IOMMU_OFF)" >&2
sudo -n qemu-system-x86_64 -enable-kvm -cpu host -smp "$GUEST_SMP" \
    -m "${GUEST_RAM_MB}M" "${MACHINE_ARGS[@]}" -nodefaults -nic none \
    -device VGA,addr=0x3 -device vfio-pci,host="$BDF" -boot d -cdrom "$ISO" \
    "${DISK_ARGS[@]}" \
    -fw_cfg name=opt/kurono/wifi.conf,file="$CONF" \
    -display none -no-reboot -serial file:"$LOG" -pidfile "$PIDFILE" \
    >"$SCRATCH/qemu.err" 2>&1 &
for _ in $(seq 1 50); do [ -f "$PIDFILE" ] && break; sleep 0.2; done
[ -f "$PIDFILE" ] && QP=$(cat "$PIDFILE")

deadline=$((SECONDS + TIMEOUT))
while [ "$SECONDS" -lt "$deadline" ]; do
    if [ -f "$LOG" ] && grep -q '\[kld-iwlwifi\] container exited' "$LOG"; then
        break
    fi
    if [ -n "$QP" ] && ! alive; then break; fi
    sleep 1
done
sleep 2

[ -f "$LOG" ] || dfail "qemu produced no serial log"
if [ "$NEGATIVE" = 1 ]; then
    grep -aq 'wpa2: no 3/4 from the AP (wrong passphrase?)' "$LOG" \
        || dfail "negative test did not print the wrong-passphrase line"
    grep -aq '\[kld-iwlwifi\] container exited rc=0' "$LOG" \
        || dfail "negative test did not exit cleanly"
elif [ "$NOSTORE" = 1 ]; then
    grep -aq '\[kld-fw\]' "$LOG" || dfail "nostore: no [kld-fw] decision line"
    grep -aq 'not in store\|no single FAT32 KURONO partition\|firmware store' "$LOG" \
        || dfail "nostore: no store miss line"
    grep -aq '\[kld-iwlwifi\] container exited rc=' "$LOG" || dfail "nostore: no container exit"
    grep -a '\[kld-iwlwifi\] container exited' "$LOG" | grep -aq 'rc=0' \
        && dfail "nostore: container exited rc=0 without firmware"
    if grep -aqE 'ffault|LX_UNIMPL|PANIC|EXCEPTION' "$LOG"; then
        fail "nostore: fatal line in serial log"
    fi
    echo "[wifi-connect] no-store negative PASS (bounded, nonzero, no fault)" >&2
elif [ "${KLD_WIFI_SOAK:-0}" = 1 ]; then
    grep -aq '\[kld-wifi\] associated:' "$LOG" || dfail "soak: no association"
    grep -aq 'wpa2: 4-way handshake complete' "$LOG" || dfail "soak: no WPA2"
    grep -aq 'https ok .* over wlan0' "$LOG" || dfail "soak: no HTTPS"
    grep -aq 'soak minute=30 ' "$LOG" || dfail "soak: no minute 30"
    grep -aq 'soak DONE ok=' "$LOG" || dfail "soak: no summary"
    grep -a 'soak DONE' "$LOG" | grep -aq 'fail=0 big_fail=0' || dfail "soak: a fetch failed"
    grep -aq '\[kld-wifi\] disconnect done' "$LOG" || dfail "soak: no clean disconnect"
    grep -aq '\[kld-iwlwifi\] container exited rc=0' "$LOG" || dfail "soak: rc != 0"
    grep -aq '\[dhcp\] lease bound' "$LOG" || dfail "soak: no DHCP lease"
elif [ "${KLD_WIFI_RECONNECT:-0}" = 1 ]; then
    [ "$(grep -ac '\[kld-wifi\] associated:' "$LOG")" -ge 2 ] || dfail "reconnect: fewer than 2 associations"
    [ "$(grep -ac 'wpa2: 4-way handshake complete' "$LOG")" -ge 2 ] || dfail "reconnect: fewer than 2 handshakes"
    grep -aq '\[kld-wifi\] reconnect in 5s' "$LOG" || dfail "reconnect: no wait marker"
    grep -aq 'reconnect GET ok .* over wlan0' "$LOG" || dfail "reconnect: second GET failed"
    grep -aq '\[kld-wifi\] disconnect done' "$LOG" || dfail "reconnect: no clean disconnect"
    grep -aq '\[kld-iwlwifi\] container exited rc=0' "$LOG" || dfail "reconnect: rc != 0"
    grep -aq '\[dhcp\] lease bound' "$LOG" || dfail "reconnect: no DHCP lease"
    if grep -aq '\[dhcp\] lease bound: ip 10\.0\.2\.' "$LOG"; then dfail "reconnect: 10.0.2.x"; fi
else
    if [ "$FWSTORE" = 1 ]; then
        served=$(grep -a '\[kld-fw\] iwlwifi/.*: served from the KURONO store' "$LOG" | wc -l)
        [ "$served" -ge 3 ] || dfail "fwstore: only $served files served from the store"
        echo "[wifi-connect] fwstore served $served iwlwifi files" >&2
    fi
    grep -aq '\[kld-wifi\] associated:' "$LOG" || dfail "no association marker"
    grep -aq 'wpa2: 4-way handshake complete' "$LOG" || dfail "no WPA2 completion marker"
    [ "$(grep -ac '\[kld-wifi\] associated:' "$LOG")" -le 1 ] || dfail "association not kept: more than one association"
    if grep -aqE 'Connection to AP .* lost|beacon loss|[Dd]isassociated from|was deauthenticated|Deauthenticated' "$LOG"; then
        dfail "association lost: $(grep -aE -m1 'Connection to AP .* lost|beacon loss|[Dd]isassociated from|was deauthenticated|Deauthenticated' "$LOG" | cut -c1-90)"
    fi
    grep -aq '\[kld-wifi\] disconnect done' "$LOG" || dfail "no clean disconnect marker"
    grep -aq '\[kld-iwlwifi\] container exited rc=0' "$LOG" || dfail "container did not exit rc=0"
    grep -aq '\[dhcp\] lease bound' "$LOG" || dfail "no kernel DHCP lease"
    if grep -aq '\[dhcp\] lease bound: ip 10\.0\.2\.' "$LOG"; then
        dfail "DHCP lease is the forbidden 10.0.2.x"
    fi
    # traffic items
    grep -aq 'https ok .* over wlan0' "$LOG" || tfail "no HTTPS success marker"
    if [ "$MODE3" = 1 ]; then
        okline=$(grep -a 'GET 60/60 one-per-second' "$LOG" | head -1)
        gets=$(printf '%s' "$okline" | sed -n 's/.*ok=\([0-9]*\)\/60.*/\1/p')
        first=$(printf '%s' "$okline" | sed -n 's/.*first_try=\([0-9]*\).*/\1/p')
        retried=$(printf '%s' "$okline" | sed -n 's/.*retried=\([0-9]*\).*/\1/p')
        if [ -z "$gets" ]; then tfail "no 60/60 HTTPS GET marker"
        elif [ "$gets" -lt 57 ]; then tfail "only $gets of 60 GETs succeeded (first try ${first:-?}, retried ${retried:-?})"; fi
        big=$(grep -a 'https big .* MiB/s over wlan0' "$LOG" | head -1 | sed -n 's/.*= \([0-9.]*\) MiB\/s.*/\1/p')
        if [ -z "$big" ]; then tfail "no large HTTPS download marker"
        elif ! awk -v b="$big" -v f="$BIG_FLOOR_MIBS" 'BEGIN { exit !(b >= f) }'; then tfail "large download $big MiB/s is under the $BIG_FLOOR_MIBS floor"; fi
    else
        grep -aq '60/60 .*over wlan0' "$LOG" || dfail "no 60/60 HTTPS GET marker"
        grep -a 'GET 60/60 one-per-second' "$LOG" | grep -aq 'first_try=60' || dfail "not 60/60 first-try HTTPS GETs"
        grep -aq 'https big .* MiB/s over wlan0' "$LOG" || dfail "no large HTTPS download marker"
    fi
fi
grep -aq '\[kld-stub\]' "$LOG" && dfail "a generated stub was hit: $(grep -a -m1 '\[kld-stub\]' "$LOG")"
if grep -aqE 'ffault|LX_UNIMPL|PANIC|EXCEPTION' "$LOG"; then
    dfail "fatal line in serial log"
fi
# firmware errors / restarts: a driver item unless a guest stall over 3 s explains them. (satoru)
FWERR=$(grep -aEc 'Microcode SW error|Hardware error detected|is stuck [0-9]|Restarting 0x|Start IWL Error Log Dump' "$LOG")
if [ "$IOMMU" = 1 ]; then
    if [ "$IOMMU_OFF" = 1 ]; then
        grep -aq '\[kld-iommu\] vt-d on' "$LOG" && dfail "iommu off: translation still on"
        if grep -aq '\[kld-iommu\] fault' "$LOG"; then dfail "iommu off: a fault was logged"; fi
    else
        grep -aq '\[kld-iommu\] vt-d on' "$LOG" || dfail "iommu: translation did not turn on"
        grep -aq '\[kld-iommu\] domain .* for iwlwifi bdf ' "$LOG" || dfail "iommu: no iwlwifi domain"
        grep -aq '\[kld-iommu\] domain .* released: device blocked, mapped [1-9][0-9]* frames' "$LOG" \
            || dfail "iommu: no mapped frames at release"
        if grep -aq '\[kld-iommu\] fault' "$LOG"; then dfail "iommu: DMAR fault logged"; fi
        echo "[wifi-connect] iommu $(grep -a -m1 '\[kld-iommu\] domain .* released' "$LOG" | sed 's/.*released/released/')" >&2
    fi
fi
if [ "$IOMMU" = 1 ]; then
    if [ "$IOMMU_OFF" = 1 ]; then
        grep -aq '\[kld-iommu\] vt-d on' "$LOG" && fail "iommu off: translation still on"
        if grep -aq '\[kld-iommu\] fault' "$LOG"; then fail "iommu off: a fault was logged"; fi
    else
        grep -aq '\[kld-iommu\] vt-d on' "$LOG" || fail "iommu: translation did not turn on"
        grep -aq '\[kld-iommu\] domain .* for iwlwifi bdf ' "$LOG" || fail "iommu: no iwlwifi domain"
        grep -aq '\[kld-iommu\] domain .* released: device blocked, mapped [1-9][0-9]* frames' "$LOG" \
            || fail "iommu: no mapped frames at release"
        if grep -aq '\[kld-iommu\] fault' "$LOG"; then fail "iommu: DMAR fault logged"; fi
        echo "[wifi-connect] iommu $(grep -a -m1 '\[kld-iommu\] domain .* released' "$LOG" | sed 's/.*released/released/')" >&2
    fi
fi

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
        if [ "$ok" = 0 ]; then echo "[warn] $w" >&2; bad=1; fi
    done < <(grep -a '\[kld-warn\]' "$LOG" 2>/dev/null || true)
    [ "$bad" = 0 ] || dfail "unallowlisted [kld-warn] line"
    # the iwl-trans.c:111 allowlist entry is only for the tcm work (upstream race):
    # the warning's work_fn must resolve to iwl_mvm_tcm_work in the kept container. (satoru)
    if grep -aq '\[kld-warn\] .*iwl-trans.c:111' "$LOG" 2>/dev/null; then
        local n=0 wf fn
        while IFS= read -r wf; do
            wf=${wf##*work_fn=0x0x}
            fn=$(addr2line -f -e "$SCRATCH/kld-iwlwifi.elf" "0x$wf" 2>/dev/null | head -1)
            n=$((n + 1))
            [ "$fn" = iwl_mvm_tcm_work ] || dfail "iwl-trans.c:111 warning from ${fn:-unknown work}, not iwl_mvm_tcm_work"
        done < <(grep -a '\[kld-warn-bt\].*work_fn=' "$LOG")
        [ "$n" -gt 0 ] || dfail "iwl-trans.c:111 warning without a work_fn (not the tcm work)"
        echo "[wifi-connect] iwl-trans.c:111 from iwl_mvm_tcm_work (allowlisted upstream race)" >&2
    fi
}
check_kld_warns

# Secret scan: never print the passphrase, even on failure.
rm -f "$BAD_CONF"
PSK=""
while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
        PSK=*) PSK=${line#PSK=} ;;
    esac
done < "$REAL_CONF"
if [ -n "$PSK" ]; then
    if grep -aFq -- "$PSK" "$LOG"; then
        fail "secret scan: passphrase found in serial.log"
    fi
    if grep -rFq -- "$PSK" "$SCRATCH" 2>/dev/null; then
        fail "secret scan: passphrase found in the run directory"
    fi
fi
if [ -n "${BAD_PSK:-}" ]; then
    if grep -aFq -- "$BAD_PSK" "$LOG"; then
        fail "secret scan: negative passphrase found in serial.log"
    fi
fi
echo "secret scan: clean" >&2

echo "[wifi-connect] serial log kept at $LOG" >&2
grep -a '\[kld-wifi\] associated:\|wpa2: 4-way\|\[dhcp\] lease bound\|secret scan' "$LOG" >&2

# reconnect timing (KLD_WIFI_APLOSS=1): second 4-way handshake within 20 s of the deauth. (satoru)
if [ "$APLOSS" = 1 ]; then
    t_deauth=$(grep -a 'deauthenticating from' "$LOG" | head -1 | sed -n 's/^\[\([0-9]*\)\].*/\1/p')
    t_second=$(grep -a 'wpa2: 4-way handshake complete' "$LOG" | sed -n 2p | sed -n 's/^\[\([0-9]*\)\].*/\1/p')
    if [ -z "$t_deauth" ] || [ -z "$t_second" ]; then echo "VERDICT DRIVER: FAIL no deauth/second handshake timestamps" ; echo "REGRESS wifi_connect: FAIL reconnect timing"; exit 1; fi
    echo "[wifi-connect] reconnect took $((t_second - t_deauth)) ms after the local deauth" >&2
    [ $((t_second - t_deauth)) -le 20000 ] || { echo "VERDICT DRIVER: FAIL reconnect took $((t_second - t_deauth)) ms (limit 20000)"; echo "REGRESS wifi_connect: FAIL reconnect over 20 s"; exit 1; }
fi

if [ "$MODE3" = 1 ] || [ "$STRICT" = 1 ]; then
    LOAD_END=$(cut -d' ' -f1-3 /proc/loadavg)
    mhz=$(awk -F: '/^cpu MHz/ { print $2; exit }' /proc/cpuinfo)
    gapk=$(grep -a -o 'gapmax_kcyc=[0-9]*' "$LOG" | sed 's/.*=//' | sort -n | tail -1)
    gap_s=$(awk -v k="${gapk:-0}" -v m="${mhz:-2500}" 'BEGIN { printf "%.2f", k * 1000 / (m * 1000000) }')
    steals=$(grep -ac '\[kls\] HARD steal' "$LOG")
    disturbed=no
    if [ "$steals" -gt 0 ] || awk -v g="$gap_s" 'BEGIN { exit !(g > 3.0) }'; then disturbed=yes; fi
    # host load average above 8 (1 min, at the start or at the end of the run,
    # the download is in between) also counts as host-disturbed. (satoru)
    load_hi=no
    for l1 in "${LOAD_START%% *}" "${LOAD_END%% *}"; do
        awk -v l="$l1" 'BEGIN { exit !(l > 8.0) }' && load_hi=yes
    done
    if [ "$load_hi" = yes ]; then disturbed=yes; echo "[wifi-connect] host load average above 8: tagged host-disturbed (load>8)" >&2; fi
    # a firmware error only counts against the driver when no host stall explains it
    if [ "$FWERR" -gt 0 ] && [ "$disturbed" = no ]; then
        DRV_REASONS+=("firmware error/restart without a host stall ($(grep -aE -m1 'Microcode SW error|Hardware error detected|is stuck [0-9]' "$LOG" | cut -c1-70))")
    fi
    DV=PASS; DR=
    [ "${#DRV_REASONS[@]}" -eq 0 ] || { DV=FAIL; DR=" ($(IFS=';'; echo "${DRV_REASONS[*]}"))"; }
    TV=PASS; TR=
    [ "${#TRF_REASONS[@]}" -eq 0 ] || { TV=FAIL; TR=" ($(IFS=';'; echo "${TRF_REASONS[*]}"))"; }
    okline=$(grep -a 'GET 60/60 one-per-second' "$LOG" | head -1)
    echo "VERDICT DRIVER: $DV$DR"
    echo "VERDICT TRAFFIC: $TV$TR gets=$(printf '%s' "$okline" | sed -n 's/.*ok=\([0-9]*\)\/60.*/\1/p') first_try=$(printf '%s' "$okline" | sed -n 's/.*first_try=\([0-9]*\).*/\1/p') retried=$(printf '%s' "$okline" | sed -n 's/.*retried=\([0-9]*\).*/\1/p') big_mibs=$(grep -a 'https big .* MiB/s' "$LOG" | head -1 | sed -n 's/.*= \([0-9.]*\) MiB.*/\1/p')"
    echo "VERDICT HOST: load_start=\"$LOAD_START\" load_end=\"$LOAD_END\" gapmax_s=$gap_s hard_steals=$steals fw_errors=$FWERR load_over_8=$load_hi disturbed=$disturbed"
    if [ "$STRICT" != 1 ]; then
        if [ "$DV" = PASS ] && { [ "$TV" = PASS ] || [ "$disturbed" = yes ]; }; then
            [ "$TV" = PASS ] || echo "[wifi-connect] traffic failed in a host-disturbed run: judged on DRIVER only" >&2
            echo "REGRESS wifi_connect: PASS"
            echo "$LOG"
            exit 0
        fi
        echo "REGRESS wifi_connect: FAIL driver=$DV traffic=$TV disturbed=$disturbed$DR$TR"
        exit 1
    fi
fi
echo "REGRESS wifi_connect: PASS"
echo "$LOG"
exit 0
