#!/bin/bash
# the translator's drm client .restore: desktop PRIMARY on the bochs device,
# then (kurono.kld.restoretest=1) a second in-container drm client commits a
# solid magenta framebuffer the way a userspace master would, then
# drm_client_dev_restore() runs as on lastclose. device-side proof from QEMU
# screendumps of bochs0: before (desktop), after the intruder (magenta), after
# the restore (desktop again, no magenta).
#
#   REGRESS drm_bochs_restore: PASS|FAIL ...
#
# env: KLD_SCRATCH, KLD_REG_TIMEOUT (200), KLD_NOBUILD=1, LINUX.
# end (satoru)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
SCRATCH="$SCRATCH_ROOT/regress_drm_bochs_restore_$STAMP"
LOG="$SCRATCH/serial.log"; ISO="$SCRATCH/r.iso"; MON="$SCRATCH/mon.sock"
TIMEOUT=${KLD_REG_TIMEOUT:-200}
LINUX=${LINUX:-/mnt/windows/Users/ibrah/kw/scratch/kld/linux-6.12.99}
NEED_KB=$((2048 * 1024 + 6 * 1024 * 1024))
QP=
mkdir -p "$SCRATCH"
cleanup() { if [ -n "$QP" ] && kill -0 "$QP" 2>/dev/null; then kill "$QP"; sleep 1; kill -9 "$QP" 2>/dev/null; fi; wait 2>/dev/null; rm -f "$ISO"; }
trap cleanup EXIT
fail() { echo "REGRESS drm_bochs_restore: FAIL $1"; exit 1; }
if [ "${KLD_NOBUILD:-0}" != 1 ]; then
    make -C "$ROOT/kld" driver=bochs LINUX="$LINUX" -j"$(nproc)" > "$SCRATCH/build_kld.log" 2>&1 || fail "container build"
    cp "$ROOT/kld/build/bochs/kld-bochs.elf" "$ROOT/src/userprogs/kld_bochs.elf" || fail "copy container"
fi
make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 KURONO_KLD_EMBED=1 \
    KURONO_EXTRA_CMDLINE="kurono.kld=auto kurono.kld.take=1234:1111@0:4.0 kurono.kld.display=primary kurono.kld.restoretest=1 kurono.autologin=1 kurono.gfxstats=1" \
    > "$SCRATCH/build_iso.log" 2>&1 || fail "iso build"
cp "$ROOT/build/kurono.iso" "$ISO" || fail "copy iso"
while :; do a=$(awk '/^MemAvailable:/{print $2}' /proc/meminfo); [ "$a" -ge "$NEED_KB" ] && break; sleep 10; done
qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 2048M -machine pc -nodefaults \
    -device VGA,id=vga0,addr=0x3 -device bochs-display,id=bochs0,addr=0x4 \
    -boot d -cdrom "$ISO" -display none -no-reboot \
    -serial file:"$LOG" -monitor unix:"$MON",server,nowait > "$SCRATCH/qemu.err" 2>&1 &
QP=$!
python3 -I - "$LOG" "$MON" "$SCRATCH" "$TIMEOUT" <<'PY'
import socket, sys, time, os
log, mon, d, timeout = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
def read():
    try: return open(log, 'rb').read().decode('utf-8', 'replace')
    except Exception: return ''
def shot(name):
    p = os.path.join(d, name)
    s = socket.socket(socket.AF_UNIX); s.connect(mon); s.settimeout(1.5); time.sleep(0.2)
    try: s.recv(65536)
    except Exception: pass
    s.sendall(('screendump %s bochs0\n' % p).encode()); time.sleep(1.0); s.close()
    q = open(p, 'rb').read().split(b'\n', 3); w, h = map(int, q[1].split()); px = q[3]
    n = w * h; mag = sum(1 for i in range(0, n * 3, 3 * 7) if px[i:i+3] == b'\xff\x00\xff')
    nb = sum(1 for i in range(0, n * 3, 3 * 7) if px[i] or px[i+1] or px[i+2])
    tot = len(range(0, n * 3, 3 * 7))
    return w, h, mag * 100.0 / tot, nb * 100.0 / tot
end = time.time() + timeout
def wait(pat):
    while time.time() < end:
        if pat in read(): return True
        time.sleep(0.5)
    return False
ok = wait('desktop PRIMARY'); time.sleep(5)
b = shot('before.ppm') if ok else None
ok = ok and wait('restoretest intruder committed'); time.sleep(2)
m = shot('intruder.ppm') if ok else None
ok = ok and wait('restoretest restore called'); time.sleep(3)
a = shot('after.ppm') if ok else None
t = read()
rs = 'restore: desktop framebuffer back on the crtc' in t
print('[regress] before %s intruder %s after %s restore_line=%s' % (b, m, a, rs))
good = (b and m and a and rs and b[2] < 1 and b[3] > 20 and m[2] > 99 and a[2] < 1 and a[3] > 20)
for f in ('before.ppm', 'intruder.ppm', 'after.ppm'):
    if good:
        try: os.unlink(os.path.join(d, f))
        except Exception: pass
sys.exit(0 if good else 1)
PY
rc=$?
[ "$rc" = 0 ] && echo "REGRESS drm_bochs_restore: PASS (log $LOG)" || echo "REGRESS drm_bochs_restore: FAIL (log $LOG)"
exit $rc
# end (satoru)
