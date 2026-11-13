#!/bin/bash
# iwlwifi M2 bring-up rig: build the unmodified iwlwifi family into a KLD
# container, embed it and the firmware, boot with the real BE200 (0000:82:00.0)
# passed through by VFIO, and capture the container log.
#
#   REGRESS iwlwifi: PASS
#   REGRESS iwlwifi: FAIL <reason>
#
# only 0000:82:00.0 is ever passed. qemu runs under sudo and is killed by its
# own pid. KLD slot rule: -smp 4 -m 2G (accepted), MemAvailable >= 8 GiB.
# end (satoru)
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(cat "$ROOT/kld/KLD_KERNEL_VERSION")
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
SCRATCH="$SCRATCH_ROOT/regress_iwlwifi_$STAMP"
LOG="$SCRATCH/serial.log"
ISO="$SCRATCH/kurono_iwlwifi_$STAMP.iso"
PIDFILE="$SCRATCH/qemu_iwlwifi_$STAMP.pid"
TIMEOUT=${KLD_REG_TIMEOUT:-600}
BDF=0000:82:00.0
SUCCESS=${KLD_IWL_SUCCESS:-"\[kld-iwlwifi\] wiphy registered"}
GUEST_SMP=${KLD_SMP:-4}
GUEST_RAM_MB=${KLD_MEM_MB:-2048}
GUEST_RAM_KB=$((GUEST_RAM_MB * 1024))
NEED_MEM_KB=$((GUEST_RAM_KB + 6 * 1024 * 1024))
QP=

alive() { [ -n "$QP" ] && sudo -n kill -0 "$QP" 2>/dev/null; }
cleanup() {
    if alive; then
        sudo -n kill "$QP" 2>/dev/null
        sleep 1
        alive && sudo -n kill -9 "$QP" 2>/dev/null
    fi
    rm -f "$PIDFILE"
    rm -f "$ISO"
}
# a [kld-warn] line is a FAIL unless it matches warn_allowlist.txt. after a
# merge, a new warn is the most valuable line in the log. (satoru)
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

fail() {
    echo "REGRESS iwlwifi: FAIL $1"
    if [ -f "$LOG" ]; then
        { echo "--- serial.log (last 80 lines) ---"; tail -80 "$LOG"; } >&2
    fi
    exit 1
}

if [ -z "${LINUX:-}" ]; then
    for c in "$ROOT/../third_party/linux-$KVER" \
             "$SCRATCH_ROOT/linux-$KVER" \
             "/mnt/windows/Users/ibrah/Downloads/OS/third_party/linux-$KVER"; do
        if [ -d "$c/include/linux" ]; then LINUX="$c"; break; fi
    done
fi
[ -n "${LINUX:-}" ] && [ -d "$LINUX/include/linux" ] || fail "no configured linux-$KVER tree"
command -v qemu-system-x86_64 >/dev/null || fail "qemu-system-x86_64 not found"
command -v musl-gcc >/dev/null || fail "musl-gcc not found"
[ -e /dev/kvm ] || fail "/dev/kvm not available"
DRV=$(basename "$(readlink -f "/sys/bus/pci/devices/$BDF/driver" 2>/dev/null)" 2>/dev/null)
[ "$DRV" = "vfio-pci" ] || fail "$BDF is not bound to vfio-pci (driver=${DRV:-none})"
sudo -n true 2>/dev/null || fail "sudo -n not available"

mkdir -p "$SCRATCH"

echo "[iwlwifi] fetching and verifying firmware" >&2
"$ROOT/kld/tools/fetch_firmware.sh" >"$SCRATCH/fetch.log" 2>&1 || fail "fetch_firmware"

echo "[iwlwifi] building the iwlwifi container" >&2
make -C "$ROOT/kld" driver=iwlwifi LINUX="$LINUX" -j"$(nproc)" \
    >"$SCRATCH/build.log" 2>&1 || fail "container build (see $SCRATCH/build.log)"
cp "$ROOT/kld/build/iwlwifi/kld-iwlwifi.elf" "$ROOT/src/userprogs/kld_iwlwifi.elf" \
    || fail "copy container"
cp "$ROOT/kld/build/iwlwifi/kld-iwlwifi.elf" "$SCRATCH/kld-iwlwifi.elf" || fail "keep container ELF"

echo "[iwlwifi] building the ISO with the firmware" >&2
make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 \
    KURONO_KLD_EMBED=1 KURONO_KLD_FW=1 \
    KURONO_EXTRA_CMDLINE='kurono.kld=iwlwifi kurono.sched=2' \
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

echo "[iwlwifi] booting qemu with $BDF (-smp $GUEST_SMP -m ${GUEST_RAM_MB}M)" >&2
sudo -n qemu-system-x86_64 -enable-kvm -cpu host -smp "$GUEST_SMP" \
    -m "${GUEST_RAM_MB}M" -machine pc -nodefaults \
    -device VGA,addr=0x3 -device vfio-pci,host="$BDF" -boot d -cdrom "$ISO" \
    -display none -no-reboot -serial file:"$LOG" -pidfile "$PIDFILE" \
    >"$SCRATCH/qemu.err" 2>&1 &
for _ in $(seq 1 50); do [ -f "$PIDFILE" ] && break; sleep 0.2; done
[ -f "$PIDFILE" ] && QP=$(cat "$PIDFILE")

ok=0
deadline=$((SECONDS + TIMEOUT))
while [ "$SECONDS" -lt "$deadline" ]; do
    if [ -f "$LOG" ] && grep -q "$SUCCESS" "$LOG"; then ok=1; break; fi
    if [ -f "$LOG" ] && grep -q '\[kld-iwlwifi\] container exited' "$LOG"; then break; fi
    if [ -n "$QP" ] && ! alive; then break; fi
    sleep 1
done
sleep 2

if [ "$ok" = 1 ]; then
    echo "[iwlwifi] success marker seen" >&2
    check_kld_warns
    echo "REGRESS iwlwifi: PASS"
    echo "$LOG"
    exit 0
fi
if [ -f "$LOG" ] && grep -q '\[kld-iwlwifi\] container exited' "$LOG"; then
    fail "container exited before the success marker"
fi
fail "timeout waiting for the success marker"

# end (satoru)
