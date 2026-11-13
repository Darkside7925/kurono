#!/bin/bash
# end to end desktop regression for the KLD display bridge: for each compositor
# mode (mirror, extend, primary) boot a second cirrus-vga, wait for the bridge
# to take the desktop over, open the settings window through the rig hook, and
# require frames/drops/scanout to look sane.
#
#   REGRESS drm_desktop: n/3
#
# env: KLD_SCRATCH, KLD_REG_TIMEOUT (120), KLD_SMP (4), KLD_MEM_MB (2048).
# end (satoru)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
SCRATCH="$SCRATCH_ROOT/regress_drm_desktop_$STAMP"
mkdir -p "$SCRATCH"
TIMEOUT=${KLD_REG_TIMEOUT:-150}
SMP=${KLD_SMP:-4}
MEM=${KLD_MEM_MB:-2048}
PASS=0
QP=

cleanup() {
    if [ -n "$QP" ] && kill -0 "$QP" 2>/dev/null; then
        kill "$QP" 2>/dev/null; sleep 1; kill -9 "$QP" 2>/dev/null
    fi
    wait 2>/dev/null
}
trap cleanup EXIT

echo "[regress] scratch $SCRATCH"
echo "[regress] building the drmcirrus container"
make -C "$ROOT/kld" driver=drmcirrus LINUX="${LINUX:-/mnt/windows/Users/ibrah/kw/scratch/kld/linux-6.12.99}" -j"$(nproc)" > "$SCRATCH/build_kld.log" 2>&1 || { echo "REGRESS drm_desktop: FAIL container build"; exit 1; }
cp "$ROOT/kld/build/drmcirrus/kld-drmcirrus.elf" "$ROOT/src/userprogs/kld_drmcirrus.elf"

for mode in mirror extend primary; do
    ISO="$SCRATCH/$mode.iso"
    LOG="$SCRATCH/$mode.log"
    MON="$SCRATCH/$mode.mon"
    SHOT="$SCRATCH/$mode.ppm"
    echo "[regress] building/booting $mode"
    make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 KURONO_KLD_EMBED=1 \
        KURONO_EXTRA_CMDLINE="kurono.kld=auto kurono.kld.take=1013:00b8 kurono.kld.display=$mode kurono.kld.display.windowtest=1 kurono.autologin=1 kurono.gfxstats=1" \
        > "$SCRATCH/build_$mode.log" 2>&1 || { echo "[regress] $mode build FAIL"; continue; }
    cp "$ROOT/build/kurono.iso" "$ISO"
    rm -f "$LOG" "$MON" "$SHOT"
    qemu-system-x86_64 -enable-kvm -cpu host -smp "$SMP" -m "${MEM}M" -machine pc \
        -nodefaults -device VGA,id=vga0,addr=0x3 -device cirrus-vga,id=cirrus0,addr=0x4 \
        -boot d -cdrom "$ISO" -display none -no-reboot \
        -serial file:"$LOG" -monitor unix:"$MON",server,nowait > "$SCRATCH/$mode.err" 2>&1 &
    QP=$!
    python3 - "$LOG" "$MON" "$SHOT" "$mode" "$TIMEOUT" <<'PY'
import socket, sys, time
log, mon, shot, mode, timeout = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5])
def read():
    try: return open(log, 'rb').read().decode('utf-8', 'replace')
    except: return ''
def hmp(cmd):
    s = socket.socket(socket.AF_UNIX); s.connect(mon); s.settimeout(10); time.sleep(0.3)
    try: s.recv(65536)
    except: pass
    s.sendall((cmd + '\n').encode()); time.sleep(0.6)
    try: s.recv(65536)
    except: pass
    s.close()
want = 'desktop ' + mode.upper()
deadline = time.time() + timeout
setup = window = False
while time.time() < deadline:
    t = read()
    setup = setup or (want in t)
    window = window or ('windowtest opened' in t)
    if setup and window:
        time.sleep(6)
        hmp('screendump %s cirrus0' % shot)
        break
    time.sleep(2)
t = read()
frames = 0; drops = 0
for line in t.splitlines():
    if '[klddisp] stats' in line:
        for f in line.split():
            if f.startswith('frames='): frames = int(f.split('=')[1])
            if f.startswith('drops='): drops = int(f.split('=')[1])
import os
ok = setup and window and frames >= 10 and drops == 0 and os.path.exists(shot)
nonblack = 0
if ok:
    try:
        with open(shot, 'rb') as f:
            assert f.readline().strip() == b'P6'
            w, h = map(int, f.readline().split()); f.readline()
            d = f.read()
        for i in range(0, len(d), 3 * 64):
            if d[i] or d[i+1] or d[i+2]: nonblack += 1
        ok = nonblack > 100
    except Exception:
        ok = False
print('[regress] %s setup=%s window=%s frames=%d drops=%d nonblack=%d %s' %
      (mode, setup, window, frames, drops, nonblack, 'PASS' if ok else 'FAIL'))
sys.exit(0 if ok else 1)
PY
    rc=$?
    kill "$QP" 2>/dev/null; wait "$QP" 2>/dev/null; QP=
    rm -f "$ISO"
    if [ "$rc" -eq 0 ]; then PASS=$((PASS + 1)); echo "[regress] $mode PASS"; else echo "[regress] $mode FAIL"; fi
done
echo "REGRESS drm_desktop: $PASS/3"
[ "$PASS" -eq 3 ]
