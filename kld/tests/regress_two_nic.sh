#!/bin/bash
# kurono.kld=auto with TWO e1000e adapters (8086:10d3 at 0:4.0 and 0:5.0): the
# e1000e family gets one container, pinned to the first matched bdf; the
# second adapter is logged "left unclaimed" (one job per family: the kld net
# bridge is single-instance and the native wired driver stands down per kind).
# pass: exactly one container, its grant is the first adapter, the second is
# never granted, dhcp binds through the container.
#
#   REGRESS two_nic: PASS|FAIL <reason>
#
# env: LINUX, KLD_SCRATCH, KLD_REG_TIMEOUT (240), KLD_NOBUILD=1.
# end (satoru)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
SCRATCH="$SCRATCH_ROOT/regress_two_nic_$STAMP"
LOG="$SCRATCH/serial.log"
ISO="$SCRATCH/twonic.iso"
TIMEOUT=${KLD_REG_TIMEOUT:-240}
LINUX=${LINUX:-/mnt/windows/Users/ibrah/kw/scratch/kld/linux-6.12.99}
NEED_KB=$((2048 * 1024 + 6 * 1024 * 1024))
QP=
mkdir -p "$SCRATCH"
cleanup() {
    if [ -n "$QP" ] && kill -0 "$QP" 2>/dev/null; then kill "$QP"; sleep 1; kill -9 "$QP" 2>/dev/null; fi
    wait 2>/dev/null; rm -f "$ISO"
}
trap cleanup EXIT
fail() { echo "REGRESS two_nic: FAIL $1"; exit 1; }

if [ "${KLD_NOBUILD:-0}" != 1 ]; then
    make -C "$ROOT/kld" driver=e1000e LINUX="$LINUX" -j"$(nproc)" > "$SCRATCH/build_kld.log" 2>&1 \
        || fail "container build"
    cp "$ROOT/kld/build/e1000e/kld-e1000e.elf" "$ROOT/src/userprogs/kld_e1000e.elf" || fail "copy container"
fi
make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 KURONO_KLD_EMBED=1 \
    KURONO_EXTRA_CMDLINE="kurono.kld=auto kurono.kld.take=8086:10d3 kurono.sched=2" \
    > "$SCRATCH/build_iso.log" 2>&1 || fail "iso build"
cp "$ROOT/build/kurono.iso" "$ISO" || fail "copy iso"
while :; do a=$(awk '/^MemAvailable:/{print $2}' /proc/meminfo); [ "$a" -ge "$NEED_KB" ] && break; sleep 10; done
qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 2048M -machine pc -nodefaults \
    -device VGA,addr=0x3 -boot d -cdrom "$ISO" \
    -netdev user,id=n0 -device e1000e,netdev=n0,addr=0x4 \
    -netdev user,id=n1 -device e1000e,netdev=n1,addr=0x5 \
    -display none -no-reboot -serial file:"$LOG" > "$SCRATCH/qemu.err" 2>&1 &
QP=$!
end=$((SECONDS + TIMEOUT))
while [ "$SECONDS" -lt "$end" ]; do
    grep -aq '\[dhcp\] lease bound' "$LOG" 2>/dev/null && break
    kill -0 "$QP" 2>/dev/null || break
    sleep 2
done
sleep 5
kill "$QP" 2>/dev/null; wait "$QP" 2>/dev/null; QP=
starts=$(grep -ac 'launching /usr/bin/kld-e1000e' "$LOG")
g4=$(grep -ac 'granted 0:4.0' "$LOG")
g5=$(grep -ac 'granted 0:5.0' "$LOG")
unclaimed=$(grep -ac '8086:10d3 at 0:5.0 .*left unclaimed' "$LOG")
lease=$(grep -ac '\[dhcp\] lease bound' "$LOG")
echo "[regress] starts=$starts granted_0:4.0=$g4 granted_0:5.0=$g5 unclaimed_0:5.0=$unclaimed lease=$lease log $LOG"
[ "$starts" = 1 ] && [ "$g4" -ge 1 ] && [ "$g5" = 0 ] && [ "$unclaimed" = 1 ] && [ "$lease" -ge 1 ] \
    || fail "starts=$starts g4=$g4 g5=$g5 unclaimed=$unclaimed lease=$lease"
echo "REGRESS two_nic: PASS"
# end (satoru)
