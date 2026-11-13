#!/bin/bash
# kill/restart loop for the touchpad containers: kurono.kld.loop=N makes the
# kernel runner start the apitest-slot container, let it exit, run the broker
# release and start it again, N times, with kurono.ptaudit=1 watching the
# paging structures. PASS needs N exits with rc=0, N loop lines, zero
# [PTAUDIT] hits, no panic, no [kld-stub], no unlisted [kld-warn], and the
# frames in use after the last iteration within a small margin of the first.
#
#   usage: regress_kld_loop.sh sim|none      sim = i2chidsim (blob tables, models)
#                                            none = i2chid (broker mode, QEMU tables)
#   REGRESS kld-loop <mode>: PASS|FAIL ...
#
# env: KLD_LOOPS (10), KLD_EXTRA (more boot flags), KLD_REG_TIMEOUT (600),
#      KLD_SMP (4), KLD_RAM_MB (2048), KLD_SCRATCH, LINUX. (satoru)
set -u
MODE=${1:-sim}
case "$MODE" in sim) FAM=i2chidsim ;; none) FAM=i2chid ;; *) echo "usage: $0 sim|none"; exit 2 ;; esac
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(cat "$ROOT/kld/KLD_KERNEL_VERSION")
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
SCRATCH="$SCRATCH_ROOT/regress_kld_loop_${MODE}_$STAMP"
LOG="$SCRATCH/serial.log"
ISO="$SCRATCH/loop_$STAMP.iso"
LOOPS=${KLD_LOOPS:-10}
TIMEOUT=${KLD_REG_TIMEOUT:-600}
GUEST_SMP=${KLD_SMP:-4}
GUEST_RAM_MB=${KLD_RAM_MB:-2048}
NEED_KB=$((GUEST_RAM_MB * 1024 + 6 * 1024 * 1024))
QP=
cleanup() {
    if [ -n "$QP" ] && kill -0 "$QP" 2>/dev/null; then kill "$QP" 2>/dev/null; sleep 1; kill -9 "$QP" 2>/dev/null; fi
    wait 2>/dev/null
    rm -f "$ISO" "$ROOT/build/kurono.iso"
}
fail() { echo "REGRESS kld-loop $MODE: FAIL $1"; exit 1; }
trap cleanup EXIT
if [ -z "${LINUX:-}" ]; then
    for c in "$ROOT/../third_party/linux-$KVER" "$SCRATCH_ROOT/linux-$KVER" \
             "/mnt/windows/Users/ibrah/Downloads/OS/third_party/linux-$KVER"; do
        [ -d "$c/include/linux" ] && LINUX=$c && break
    done
fi
[ -n "${LINUX:-}" ] && [ -d "$LINUX/include/linux" ] || fail "no configured linux-$KVER tree"
mkdir -p "$SCRATCH"
make -C "$ROOT/kld" driver=$FAM LINUX="$LINUX" -j"$(nproc)" >"$SCRATCH/build.log" 2>&1 \
    || fail "container build (see $SCRATCH/build.log)"
cp "$ROOT/kld/build/$FAM/kld-$FAM.elf" "$ROOT/src/userprogs/kld_apitest.elf" || fail "copy container"
make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 KURONO_KLD_EMBED=1 \
    KURONO_EXTRA_CMDLINE="kurono.kld=apitest kurono.kld.family=$FAM kurono.sched=2 kurono.kld.loop=$LOOPS kurono.ptaudit=1 ${KLD_EXTRA:-}" \
    >"$SCRATCH/iso.log" 2>&1 || fail "iso build (see $SCRATCH/iso.log)"
cp "$ROOT/build/kurono.iso" "$ISO" || fail "copy iso"
while :; do
    avail=$(awk '/^MemAvailable:/ { print $2 }' /proc/meminfo)
    [ -n "$avail" ] && [ "$avail" -ge "$NEED_KB" ] && break
    echo "[regress] waiting for memory: ${avail:-?} kB / $NEED_KB kB" >&2
    sleep 10
done
qemu-system-x86_64 -enable-kvm -cpu host -smp "$GUEST_SMP" -m "${GUEST_RAM_MB}M" -machine pc -nodefaults \
    -device VGA,addr=0x3 -boot d -cdrom "$ISO" -display none -no-reboot \
    -serial file:"$LOG" >"$SCRATCH/qemu.err" 2>&1 &
QP=$!
echo "[regress] qemu pid $QP, log $LOG" >&2
end=$((SECONDS + TIMEOUT))
while [ "$SECONDS" -lt "$end" ]; do
    n=$(grep -a -c '\[kld-loop\] [0-9]*/' "$LOG" 2>/dev/null)
    [ "${n:-0}" -ge "$LOOPS" ] && break
    grep -a -q -E 'KERNEL PANIC|\[PTAUDIT\] [a-z-]+ va=' "$LOG" 2>/dev/null && break
    kill -0 "$QP" 2>/dev/null || break
    sleep 2
done
sleep 5
kill "$QP" 2>/dev/null; for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$QP" 2>/dev/null || break; sleep 1; done
kill -9 "$QP" 2>/dev/null; wait "$QP" 2>/dev/null; QP=
[ -f "$LOG" ] || fail "no serial log"
python3 -I - "$LOG" "$MODE" "$LOOPS" "$ROOT/kld/tests/warn_allowlist.txt" <<'PY'
import re, sys
log, mode, want, allow = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
t = open(log, 'rb').read().decode('utf-8', 'replace')
exits = len(re.findall(r'\[apitest\] container exited rc=0\b', t))
bad_exits = len(re.findall(r'\[apitest\] container exited rc=(?!0\b)\d+', t))
loops = re.findall(r'\[kld-loop\] (\d+)/(\d+) used frames now (\d+) before the first start (\d+)', t)
aud = len(re.findall(r'\[PTAUDIT\] \S+ va=', t))
armed = 'PTAUDIT] armed' in t
panic = len(re.findall(r'KERNEL PANIC|PANIC:', t))
stub = t.count('[kld-stub]')
keys = []
try:
    for a in open(allow):
        a = a.strip()
        if a and not a.startswith('#'): keys.append(a.split()[0])
except Exception: pass
warns = [l for l in t.splitlines() if '[kld-warn]' in l and not any(k in l for k in keys)]
used = [int(x[2]) for x in loops]
base = int(loops[0][3]) if loops else -1
first = used[0] if used else -1
last = used[-1] if used else -1
# frames in use after iteration 1 vs after the last: the first run warms caches, so
# the comparison is first-vs-last, with the pre-start count printed beside it. (satoru)
delta = last - first
ok = (exits >= want and bad_exits == 0 and len(loops) >= want and aud == 0 and armed and
      panic == 0 and stub == 0 and not warns and abs(delta) <= 64)
print('REGRESS kld-loop %s: %s exits=%d bad_exits=%d loops=%d ptaudit_hits=%d armed=%s panic=%d kld-stub=%d kld-warn=%d '
      'used_frames before-first=%d after-1=%d after-last=%d delta(last-first)=%d series=%s (log %s)' %
      (mode, 'PASS' if ok else 'FAIL', exits, bad_exits, len(loops), aud, armed, panic, stub, len(warns),
       base, first, last, delta, used, log))
for l in warns[:5]: print('  warn: ' + l)
sys.exit(0 if ok else 1)
PY
# end (satoru)
