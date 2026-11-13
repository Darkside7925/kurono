#!/bin/bash
# end to end run of the drm userspace interface (D5): the drmcirrus container with
# kurono.kld.dri=1 and a test program (kld/tests/dri_app.c) running inside it.
#
#   regress_dri.sh <test id> [timeout s] [extra kernel cmdline]
#
# prints the [dri-app] lines and one result line:
#   REGRESS dri_<id>: PASS | FAIL <reason>
# env: LINUX, KLD_SCRATCH, KLD_SMP (4), KLD_MEM_MB (2048), DRI_OFF=1 (flag off),
#      DRI_FAMILY=bochs (the second drm family, default drmcirrus). (satoru)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(cat "$ROOT/kld/KLD_KERNEL_VERSION")
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld/dri}
TEST=${1:-1}
TIMEOUT=${2:-240}
EXTRA=${3:-}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
SCRATCH="$SCRATCH_ROOT/run_dri${TEST}_$STAMP"
LOG="$SCRATCH/serial.log"
ISO="$SCRATCH/kurono_dri_$STAMP.iso"
GUEST_SMP=${KLD_SMP:-4}
GUEST_RAM_MB=${KLD_MEM_MB:-2048}
NEED_MEM_KB=$((GUEST_RAM_MB * 1024 + 6 * 1024 * 1024))
QP=
cleanup() {
    if [ -n "$QP" ] && kill -0 "$QP" 2>/dev/null; then kill "$QP" 2>/dev/null; sleep 1; kill -9 "$QP" 2>/dev/null; fi
    wait 2>/dev/null
    rm -f "$ISO"
}
fail() { echo "REGRESS dri_$TEST: FAIL $1"; [ -f "$LOG" ] && { echo "--- last 20 serial lines ---"; tail -20 "$LOG"; } >&2; exit 1; }
trap cleanup EXIT
[ -z "${LINUX:-}" ] && LINUX="$SCRATCH_ROOT/../linux-$KVER"
[ -d "$LINUX/include/linux" ] || fail "no linux tree"
mkdir -p "$SCRATCH"
FAM=${DRI_FAMILY:-drmcirrus}
if [ "$FAM" = bochs ]; then TAKE=1234:1111@0:4.0; QDEV="-device bochs-display,id=bochs0,addr=0x4"; ELF=kld_bochs.elf; CE=kld-bochs.elf
else TAKE=1013:00b8; QDEV="-device cirrus-vga,id=cirrus0,addr=0x4"; ELF=kld_drmcirrus.elf; CE=kld-drmcirrus.elf; fi
make -C "$ROOT/kld" driver=$FAM LINUX="$LINUX" -j"$(nproc)" >"$SCRATCH/build.log" 2>&1 || fail "container build ($SCRATCH/build.log)"
cp "$ROOT/kld/build/$FAM/$CE" "$ROOT/src/userprogs/$ELF" || fail "copy container"
if [ "${DRI_OFF:-0}" = 1 ]; then CMD="kurono.kld=auto kurono.kld.take=$TAKE $EXTRA"
else CMD="kurono.kld=auto kurono.kld.take=$TAKE kurono.kld.dri=1 kurono.kld.dri.test=$TEST $EXTRA"; fi
make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 KURONO_KLD_EMBED=1 \
    KURONO_EXTRA_CMDLINE="$CMD" >"$SCRATCH/iso.log" 2>&1 || fail "iso build ($SCRATCH/iso.log)"
cp "$ROOT/build/kurono.iso" "$ISO" || fail "copy iso"
waited=0
while :; do
    avail=$(awk '/^MemAvailable:/ { print $2 }' /proc/meminfo)
    [ -n "$avail" ] && [ "$avail" -ge "$NEED_MEM_KB" ] && break
    waited=$((waited + 10)); [ "$waited" -ge 1200 ] && fail "memory"
    sleep 10
done
echo "[regress] cmdline: $CMD ; serial log $LOG" >&2
qemu-system-x86_64 -enable-kvm -cpu host -smp "$GUEST_SMP" -m "${GUEST_RAM_MB}M" -machine pc -nodefaults \
    -device VGA,addr=0x3 $QDEV -boot d -cdrom "$ISO" -display none \
    -no-reboot -serial file:"$LOG" -monitor unix:"$SCRATCH/mon.sock",server,nowait >"$SCRATCH/qemu.err" 2>&1 &
QP=$!
deadline=$((SECONDS + TIMEOUT))
res=
while [ "$SECONDS" -lt "$deadline" ]; do
    if [ -f "$LOG" ]; then
        if [ "${DRI_OFF:-0}" = 1 ]; then grep -aq '\[kdrm\] moving block frames=120' "$LOG" && { res=ok; break; }
        else
            if [ "$TEST" = 6 ]; then
                [ "$(grep -ac 'child [0-9]* done ok=' "$LOG")" -ge 3 ] && { res=ok; break; }
            else
                grep -aq "\[dri-app\] t[0-9a-z]*\( loop\)\? \(PASS\|FAIL\)\|\[dri-app\] all " "$LOG" && { res=ok; break; }
            fi
        fi
    fi
    kill -0 "$QP" 2>/dev/null || break
    sleep 2
done
sleep 2
grep -a '\[dri-app\]\|\[kld-dri\]\|\[kdrm-uapi\]\|\[dri\]\|kld-stub\|kld-warn\|PANIC\|panic' "$LOG" | head -${DRI_LINES:-80}
[ -n "$res" ] || fail "no result line within ${TIMEOUT}s"
grep -aq '\[kld-stub\]' "$LOG" && fail "[kld-stub] hit"
if [ "${DRI_OFF:-0}" = 1 ]; then grep -aq '\[dri\]\|\[kld-dri\]' "$LOG" && fail "dri lines with the flag off"; echo "REGRESS dri_off: PASS"; exit 0; fi
grep -aq 'CHECK FAILED' "$LOG" && fail "a check failed"
grep -aq '\[kld-warn\] broker tlb-proven free queue full' "$LOG" && fail "the broker proof queue overflowed (frames leaked)"
if [ "$TEST" = 6 ]; then
    grep -a 'child [0-9]* done' "$LOG" | grep -qv 'bad=0' && fail "a surviving process read corrupted data"
    grep -aq 'container detached' "$LOG" || fail "the container never detached (it was not really killed)"
    [ "$(grep -a 'child [0-9]* done' "$LOG" | grep -c 'ioctl=19')" -ge 3 ] || fail "an ioctl on a file of the dead container did not answer ENODEV"
fi
grep -a "\[dri-app\] t[0-9a-z]* FAIL" "$LOG" && fail "test reported FAIL"
if [ "$TEST" = 4 ]; then
    mx=$(grep -a '\[kld-dri\] kld-dri:' "$LOG" | sed 's/.*objs=\([0-9]*\).*/\1/' | sort -n | tail -1)
    mn=$(grep -a '\[kld-dri\] kld-dri:' "$LOG" | sed 's/.*objs=\([0-9]*\).*/\1/' | sort -n | head -1)
    echo "[regress] kernel object count over the loop: min $mn max $mx" >&2
    [ $((mx - mn)) -le 1 ] || fail "kernel object count grew ($mn -> $mx)"
fi
echo "REGRESS dri_$TEST: PASS (log $LOG)"
