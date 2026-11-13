#!/bin/bash
# hidsim regression: builds the software ELAN06FA HID-over-I2C family, embeds
# it through the existing apitest runner and boots it with
# kurono.kld=apitest kurono.sched=2. requires the final [hidsim] N/N PASS with
# N equal to the number of test lines emitted.
#
#   REGRESS hidsim: PASS
#   REGRESS hidsim: FAIL <reason>
#
# the ISO is deleted whether the run passes or fails; only the serial log is
# kept. env: LINUX, KLD_SCRATCH, KLD_REG_TIMEOUT (300), KLD_SMP (4),
# KLD_MEM_MB (2048). (satoru)
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(cat "$ROOT/kld/KLD_KERNEL_VERSION")
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
SCRATCH="$SCRATCH_ROOT/regress_hidsim_$STAMP"
LOG="$SCRATCH/serial.log"
ISO="$SCRATCH/kurono_hidsim_$STAMP.iso"
TIMEOUT=${KLD_REG_TIMEOUT:-300}
GUEST_SMP=${KLD_SMP:-4}
GUEST_RAM_MB=${KLD_MEM_MB:-2048}
GUEST_RAM_KB=$((GUEST_RAM_MB * 1024))
NEED_MEM_KB=$((GUEST_RAM_KB + 6 * 1024 * 1024))
QP=

cleanup() {
    if [ -n "$QP" ] && kill -0 "$QP" 2>/dev/null; then
        kill "$QP" 2>/dev/null
        sleep 1
        kill -9 "$QP" 2>/dev/null
    fi
    wait 2>/dev/null
    rm -f "$ISO" "$ROOT/build/kurono.iso"
}

fail() {
    echo "REGRESS hidsim: FAIL $1"
    if [ -f "$LOG" ]; then
        { echo "--- serial.log (last 50 lines) ---"; tail -50 "$LOG"; } >&2
    fi
    exit 1
}

trap cleanup EXIT

if [ -z "${LINUX:-}" ]; then
    for c in "$ROOT/../third_party/linux-$KVER" \
             "$SCRATCH_ROOT/linux-$KVER" \
             "/mnt/windows/Users/ibrah/Downloads/OS/third_party/linux-$KVER"; do
        if [ -d "$c/include/linux" ]; then LINUX="$c"; break; fi
    done
fi
[ -n "${LINUX:-}" ] && [ -d "$LINUX/include/linux" ] || fail "no configured linux-$KVER tree (set LINUX=)"
command -v qemu-system-x86_64 >/dev/null || fail "qemu-system-x86_64 not found"
command -v musl-gcc >/dev/null || fail "musl-gcc not found"
[ -e /dev/kvm ] || fail "/dev/kvm not available"

mkdir -p "$SCRATCH"

echo "[regress] building the hidsim container" >&2
make -C "$ROOT/kld" driver=hidsim LINUX="$LINUX" -j"$(nproc)" \
    >"$SCRATCH/build.log" 2>&1 || fail "container build (see $SCRATCH/build.log)"
cp "$ROOT/kld/build/hidsim/kld-hidsim.elf" "$ROOT/src/userprogs/kld_apitest.elf" \
    || fail "copy container"

echo "[regress] building the ISO (reusing the apitest runner)" >&2
make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 \
    KURONO_KLD_EMBED=1 \
    KURONO_EXTRA_CMDLINE='kurono.kld=apitest kurono.kld.family=i2chid kurono.sched=2' \
    >"$SCRATCH/iso.log" 2>&1 || fail "iso build (see $SCRATCH/iso.log)"
[ -f "$ROOT/build/kurono.iso" ] || fail "iso missing after build"
cp "$ROOT/build/kurono.iso" "$ISO" || fail "copy iso"

waited=0
while :; do
    avail=$(awk '/^MemAvailable:/ { print $2 }' /proc/meminfo)
    [ -n "$avail" ] && [ "$avail" -ge "$NEED_MEM_KB" ] && break
    waited=$((waited + 10))
    [ "$waited" -ge 1200 ] && fail "MemAvailable ${avail:-?} kB below $NEED_MEM_KB kB after 20 min"
    echo "[regress] waiting for memory: ${avail:-?} kB / $NEED_MEM_KB kB" >&2
    sleep 10
done

echo "[regress] booting qemu (-smp $GUEST_SMP -m ${GUEST_RAM_MB}M), log $LOG" >&2
qemu-system-x86_64 -enable-kvm -cpu host -smp "$GUEST_SMP" -m "${GUEST_RAM_MB}M" \
    -machine pc -nodefaults -device VGA,addr=0x3 -boot d -cdrom "$ISO" \
    -display none -no-reboot -serial file:"$LOG" \
    >"$SCRATCH/qemu.err" 2>&1 &
QP=$!

ok=0
deadline=$((SECONDS + TIMEOUT))
while [ "$SECONDS" -lt "$deadline" ]; do
    if [ -f "$LOG" ] && grep -q '\[hidsim\] [0-9]*/[0-9]* PASS' "$LOG"; then
        ok=1
        break
    fi
    if [ -f "$LOG" ] && grep -q '\[hidsim\] [0-9]*/[0-9]* FAIL' "$LOG"; then
        break
    fi
    if [ -f "$LOG" ] && grep -q 'container exited' "$LOG"; then
        break
    fi
    kill -0 "$QP" 2>/dev/null || break
    sleep 1
done

kill "$QP" 2>/dev/null
for _ in $(seq 1 10); do kill -0 "$QP" 2>/dev/null || break; sleep 1; done
kill -9 "$QP" 2>/dev/null
wait "$QP" 2>/dev/null
QP=

[ -f "$LOG" ] || fail "qemu produced no serial log"
grep -q '\[kld-input\] pointer bridge active' "$LOG" || fail "no kernel pointer injection line"
grep -q '\[kld-input\]' "$LOG" || fail "no kernel input line"
final=$(grep '\[hidsim\] [0-9]*/[0-9]* PASS' "$LOG" | tail -1)
[ -n "$final" ] || fail "no final [hidsim] N/N PASS"
num=$(echo "$final" | sed -E 's/.*\[hidsim\] ([0-9]+)\/([0-9]+) PASS/\1 \2/')
pass=$(echo "$num" | cut -d' ' -f1)
total=$(echo "$num" | cut -d' ' -f2)
lines=$(grep -c -E '\[hidsim\] H[1-4]_.*: (PASS|FAIL)' "$LOG")
[ "$pass" = "$total" ] || fail "final line is $pass/$total"
[ "$total" = "$lines" ] || fail "final denominator $total != emitted H lines $lines"
if grep -q -E '\[hidsim\] H[1-4]_.*: FAIL' "$LOG"; then
    fail "a test failed: $(grep -E '\[hidsim\] H[1-4]_.*: FAIL' "$LOG" | head -1)"
fi

echo "[regress] serial log kept at $LOG" >&2
echo "$final" >&2
echo "REGRESS hidsim: PASS"
exit 0

# end (satoru)
