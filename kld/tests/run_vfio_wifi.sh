#!/bin/bash
# boot a Kurono ISO in QEMU+KVM with the real BE200 Wi-Fi card passed through by
# VFIO and capture the serial log. the card 0000:82:00.0 must already be bound
# to vfio-pci (see the M0 report / Step 5 command).
#
#   usage: run_vfio_wifi.sh <iso> [timeout_seconds]
#
# serial log: $KLD_SCRATCH/serial_vfio_<UTC timestamp>.log
# defaults: KLD_SCRATCH=/mnt/windows/Users/ibrah/kw/scratch/kld, timeout 180.
# qemu runs under sudo because vfio needs its guest memory locked.
#
# QEMU slot rule for KLD: -smp 2 -m 2G, start when MemAvailable >= guest RAM +
# 6 GiB regardless of other QEMUs; unique names under scratch/kld; kill
# only the pid we started. (satoru)
# end (satoru)
set -u

if [ $# -lt 1 ]; then
    echo "usage: $0 <iso> [timeout_seconds]" >&2
    exit 2
fi
ISO=$1
SECS=${2:-180}
BDF=0000:82:00.0
SCRATCH=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
LOG="$SCRATCH/serial_vfio_$STAMP.log"
PIDFILE="$SCRATCH/qemu_vfio_$STAMP.pid"
GUEST_SMP=${KLD_SMP:-2}
GUEST_RAM_MB=${KLD_MEM_MB:-2048}
GUEST_RAM_KB=$((GUEST_RAM_MB * 1024))
NEED_MEM_KB=$((GUEST_RAM_KB + 6 * 1024 * 1024))
QP=

[ -f "$ISO" ] || { echo "run_vfio_wifi: no ISO at $ISO" >&2; exit 2; }
DRV=$(basename "$(readlink -f "/sys/bus/pci/devices/$BDF/driver" 2>/dev/null)" 2>/dev/null)
[ "$DRV" = "vfio-pci" ] || {
    echo "run_vfio_wifi: $BDF is not bound to vfio-pci (driver=${DRV:-none})" >&2
    exit 2
}
sudo -n true 2>/dev/null || { echo "run_vfio_wifi: sudo -n not available" >&2; exit 2; }

alive() {
    [ -n "$QP" ] && sudo -n kill -0 "$QP" 2>/dev/null
}

cleanup() {
    if alive; then
        sudo -n kill "$QP" 2>/dev/null
        sleep 1
        alive && sudo -n kill -9 "$QP" 2>/dev/null
    fi
    rm -f "$PIDFILE"
}
trap cleanup EXIT

mkdir -p "$SCRATCH"
waited=0
while :; do
    avail=$(awk '/^MemAvailable:/ { print $2 }' /proc/meminfo)
    [ -n "$avail" ] && [ "$avail" -ge "$NEED_MEM_KB" ] && break
    waited=$((waited + 10))
    [ "$waited" -ge 1200 ] && {
        echo "run_vfio_wifi: MemAvailable ${avail:-?} kB below $NEED_MEM_KB kB after 20 min" >&2
        exit 1
    }
    echo "run_vfio_wifi: waiting for memory: ${avail:-?} kB / $NEED_MEM_KB kB" >&2
    sleep 10
done

echo "run_vfio_wifi: qemu with $BDF (-smp $GUEST_SMP -m ${GUEST_RAM_MB}M), log $LOG" >&2
sudo -n qemu-system-x86_64 -enable-kvm -cpu host -smp "$GUEST_SMP" -m "${GUEST_RAM_MB}M" -machine pc -nodefaults \
    -device VGA,addr=0x3 -device vfio-pci,host="$BDF" -boot d -cdrom "$ISO" \
    -display none -no-reboot -serial file:"$LOG" -pidfile "$PIDFILE" \
    >"$SCRATCH/qemu_vfio_$STAMP.err" 2>&1 &
SUDO_PID=$!
for _ in $(seq 1 50); do
    [ -f "$PIDFILE" ] && break
    sleep 0.2
done
if [ -f "$PIDFILE" ]; then QP=$(cat "$PIDFILE"); fi

deadline=$((SECONDS + SECS))
while [ "$SECONDS" -lt "$deadline" ]; do
    if [ -f "$LOG" ] && grep -qi '8086:272b' "$LOG"; then
        break
    fi
    if [ -n "$QP" ] && ! alive; then
        break
    fi
    sleep 1
done
sleep 2

if [ -f "$LOG" ] && grep -qi '8086:272b' "$LOG"; then
    echo "run_vfio_wifi: card visible in the guest" >&2
else
    echo "run_vfio_wifi: card NOT seen before the timeout" >&2
fi
echo "$LOG"
[ -f "$LOG" ] || exit 1
grep -qi '8086:272b' "$LOG"

# end (satoru)
