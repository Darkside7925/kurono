#!/bin/bash
# kill/restart loop for the KLD bochs family: the display bridge asks the
# container to exit every 120 frames (kurono.kld.display.killloop=1), the auto
# runner restarts it, kurono.ptaudit=1 watches the page tables, and with
# kurono.kld.aliastest=1 every container instance holds a live broker vmap
# alias of scattered pages when it dies. the run ends after KLD_KILLS exits.
#
#   REGRESS drm_bochs_killloop <mode>: PASS|FAIL kills=n exits=n aliases=n ...
#
# usage: regress_drm_bochs_killloop.sh <mirror|extend|primary>
# env: KLD_SCRATCH, KLD_KILLS (10), KLD_REG_TIMEOUT (600), KLD_SMP (4),
#      KLD_MEM_MB (2048), KLD_EXTRA, KLD_NOBUILD=1.
# end (satoru)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MODE=${1:-primary}
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
SCRATCH="$SCRATCH_ROOT/regress_drm_bochs_killloop_${MODE}_$STAMP"
LOG="$SCRATCH/serial.log"
ISO="$SCRATCH/loop.iso"
KILLS=${KLD_KILLS:-10}
TIMEOUT=${KLD_REG_TIMEOUT:-600}
SMP=${KLD_SMP:-4}
MEM=${KLD_MEM_MB:-2048}
NEED_KB=$((MEM * 1024 + 6 * 1024 * 1024))
LINUX=${LINUX:-/mnt/windows/Users/ibrah/kw/scratch/kld/linux-6.12.99}
QP=
mkdir -p "$SCRATCH"
cleanup() {
    if [ -n "$QP" ] && kill -0 "$QP" 2>/dev/null; then
        kill "$QP" 2>/dev/null; sleep 1; kill -9 "$QP" 2>/dev/null
    fi
    wait 2>/dev/null
    rm -f "$ISO"
}
trap cleanup EXIT
fail() { echo "REGRESS drm_bochs_killloop $MODE: FAIL $1"; exit 1; }

if [ "${KLD_NOBUILD:-0}" != 1 ]; then
    make -C "$ROOT/kld" driver=bochs LINUX="$LINUX" -j"$(nproc)" > "$SCRATCH/build_kld.log" 2>&1 \
        || fail "container build (see $SCRATCH/build_kld.log)"
    cp "$ROOT/kld/build/bochs/kld-bochs.elf" "$ROOT/src/userprogs/kld_bochs.elf" || fail "copy container"
fi
make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 KURONO_KLD_EMBED=1 \
    KURONO_EXTRA_CMDLINE="kurono.kld=auto kurono.kld.take=1234:1111@0:4.0 kurono.kld.display=$MODE kurono.kld.display.killloop=1 kurono.kld.aliastest=1 kurono.ptaudit=1 kurono.autologin=1 kurono.gfxstats=1 ${KLD_EXTRA:-}" \
    > "$SCRATCH/build_iso.log" 2>&1 || fail "iso build (see $SCRATCH/build_iso.log)"
cp "$ROOT/build/kurono.iso" "$ISO" || fail "copy iso"

while :; do
    avail=$(awk '/^MemAvailable:/ { print $2 }' /proc/meminfo)
    [ -n "$avail" ] && [ "$avail" -ge "$NEED_KB" ] && break
    echo "[regress] waiting for memory: ${avail:-?} kB / $NEED_KB kB" >&2
    sleep 10
done
qemu-system-x86_64 -enable-kvm -cpu host -smp "$SMP" -m "${MEM}M" -machine pc \
    -nodefaults -device VGA,id=vga0,addr=0x3 -device bochs-display,id=bochs0,addr=0x4 \
    -boot d -cdrom "$ISO" -display none -no-reboot \
    -serial file:"$LOG" > "$SCRATCH/qemu.err" 2>&1 &
QP=$!
echo "[regress] qemu pid $QP, scratch $SCRATCH" >&2

end=$((SECONDS + TIMEOUT))
while [ "$SECONDS" -lt "$end" ]; do
    n=$(grep -a -c 'container exited rc=' "$LOG" 2>/dev/null)
    [ "${n:-0}" -ge "$KILLS" ] && break
    grep -a -q -E 'KERNEL PANIC|\[PTAUDIT\] [a-z-]+ va=' "$LOG" 2>/dev/null && break
    kill -0 "$QP" 2>/dev/null || break
    sleep 2
done
sleep 8   # the last release and restart settle (satoru)
kill "$QP" 2>/dev/null; wait "$QP" 2>/dev/null; QP=

python3 -I - "$LOG" "$MODE" "$KILLS" "$ROOT/kld/tests/warn_allowlist.txt" <<'PY'
import re, sys
log, mode, want, allow = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
t = open(log, 'rb').read().decode('utf-8', 'replace')
kills = t.count('killtest: asking the container to exit')
exits = t.count('container exited rc=')
alias_pass = len(re.findall(r'\[kld-bochs\] aliastest .* PASS', t))
alias_fail = len(re.findall(r'\[kld-bochs\] aliastest .* FAIL', t))
aud = len(re.findall(r'\[PTAUDIT\] \S+ va=', t))   # not the 'armed' banner (satoru)
panic = len(re.findall(r'KERNEL PANIC|PANIC:', t))
ffault = t.count('[ffault]')
wd = t.count('unresponsive')
stub = t.count('[kld-stub]')
keys = []
try:
    for a in open(allow):
        a = a.strip()
        if a and not a.startswith('#'): keys.append(a.split()[0])
except Exception: pass
warns = [l for l in t.splitlines() if '[kld-warn]' in l and not any(k in l for k in keys)]
pmm = [int(x) for x in re.findall(r'pmm_used_mb=(\d+)', t)]
# baseline: the first sample after the first desktop setup; end: the last. (satoru)
first_setup = t.find('desktop ')
base = None
for m in re.finditer(r'pmm_used_mb=(\d+)', t):
    if m.start() > first_setup: base = int(m.group(1)); break
ok = (exits >= want and kills >= want and alias_pass >= exits and alias_fail == 0 and
      aud == 0 and panic == 0 and ffault == 0 and wd == 0 and stub == 0 and not warns)
print('REGRESS drm_bochs_killloop %s: %s kills=%d exits=%d aliastest_pass=%d fail=%d ptaudit=%d '
      'panic=%d ffault=%d watchdog=%d kld-stub=%d kld-warn=%d pmm_used_mb boot=%s after-setup=%s last=%s (log %s)' %
      (mode, 'PASS' if ok else 'FAIL', kills, exits, alias_pass, alias_fail, aud, panic, ffault, wd,
       stub, len(warns), pmm[0] if pmm else '?', base, pmm[-1] if pmm else '?', log))
for l in warns[:5]: print('  warn: ' + l)
sys.exit(0 if ok else 1)
PY
# end (satoru)
