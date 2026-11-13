#!/bin/bash
# d3 mode-change gate for the KLD bochs family: the kernel pattern mode drives
# 20 runtime mode changes (1024x768 -> 1920x1080 -> 1280x800 ...) through the
# translator while the bridge is attached, with kurono.ptaudit=1. for every
# change the host takes DEVICE-side evidence from the QEMU monitor:
#   A  a screendump right away (vm running),
#   then the vm is stopped and the bochs dispi registers are read (xres, yres,
#   bpp, enable, virt_width, x/y offset) to compute the scanout start,
#   VRAM is saved at that start, at offset 0 and at the previous change's
#   start, B a screendump while stopped, the vm continues,
#   C a screendump 2 s later.
# a change passes when the kernel scanout check is exact, the device is in the
# requested mode, its scanout start equals the buffer object offset the
# container reported for that commit and the offset of the pointer the
# translator copies into, and both the vram at the scanout start and B match
# the pattern with that change's own counter colour. the pmm used-frame count
# brackets the loop.
#
#   REGRESS drm_bochs_modes: PASS n/20
#   REGRESS drm_bochs_modes: FAIL <reason>
#
# env: LINUX, KLD_SCRATCH, KLD_REG_TIMEOUT (750), KLD_SMP (4), KLD_MEM_MB (2048),
#      KLD_EXTRA (more kernel args), KLD_NOBUILD=1 (reuse the built container).
# end (satoru)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
SCRATCH="$SCRATCH_ROOT/regress_drm_bochs_modes_$STAMP"
LOG="$SCRATCH/serial.log"
ISO="$SCRATCH/modes.iso"
MON="$SCRATCH/mon.sock"
TIMEOUT=${KLD_REG_TIMEOUT:-750}
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
fail() { echo "REGRESS drm_bochs_modes: FAIL $1"; exit 1; }

if [ "${KLD_NOBUILD:-0}" != 1 ]; then
    make -C "$ROOT/kld" driver=bochs LINUX="$LINUX" -j"$(nproc)" > "$SCRATCH/build_kld.log" 2>&1 \
        || fail "container build (see $SCRATCH/build_kld.log)"
    cp "$ROOT/kld/build/bochs/kld-bochs.elf" "$ROOT/src/userprogs/kld_bochs.elf" || fail "copy container"
fi
make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 KURONO_KLD_EMBED=1 \
    KURONO_EXTRA_CMDLINE="kurono.kld=auto kurono.kld.take=1234:1111@0:4.0 kurono.kld.display=pattern kurono.kld.display.patterncycle=1 kurono.ptaudit=1 ${KLD_EXTRA:-}" \
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
    -serial file:"$LOG" -monitor unix:"$MON",server,nowait > "$SCRATCH/qemu.err" 2>&1 &
QP=$!
echo "[regress] qemu pid $QP, scratch $SCRATCH" >&2

python3 -I - "$LOG" "$MON" "$SCRATCH" "$TIMEOUT" "$ROOT/kld/tests" <<'PY'
import os, re, socket, sys, time
log, mon, scratch, timeout, tests = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5]

def read():
    try: return open(log, 'rb').read().decode('utf-8', 'replace')
    except Exception: return ''

def hmp(cmd, wait=0.5):
    s = socket.socket(socket.AF_UNIX); s.connect(mon); s.settimeout(1.5); time.sleep(0.1)
    try: s.recv(65536)
    except Exception: pass
    s.sendall((cmd + '\n').encode()); time.sleep(wait)
    out = b''
    try:
        while True:
            d = s.recv(65536)
            if not d: break
            out += d
    except Exception: pass
    s.close()
    return out.decode('utf-8', 'replace')

def expected(w, h, ctr):
    bars = [(255,255,255),(255,255,0),(0,255,255),(0,255,0),(255,0,255),(255,0,0),(0,0,255),(0,0,0)]
    bw = w // 8
    row = bytearray()
    for x in range(w): row += bytes(bars[min(x // bw, 7)])
    img = bytearray(bytes(row) * h)
    for y in range(64, 80):
        for x in range(64, 80):
            o = (y * w + x) * 3
            img[o:o+3] = bytes((255,255,255) if ((x-64) ^ (y-64)) & 1 else (0,0,0))
    for y in range(64, 96):
        o = (y * w + 96) * 3
        img[o:o+96] = bytes(ctr) * 32
    return bytes(img)

def ppm(path):
    try: d = open(path, 'rb').read()
    except Exception: return None
    p = d.split(b'\n', 3)
    if len(p) < 4 or p[0] != b'P6': return None
    w, h = map(int, p[1].split())
    return w, h, p[3][:w*h*3]

def raw(path, w, h, stride):
    try: d = open(path, 'rb').read()
    except Exception: return None
    out = bytearray(w * h * 3)
    for y in range(h):
        r = d[y*stride:y*stride + w*4]
        if len(r) < w*4: return None
        o = y * w * 3
        out[o:o+w*3:3] = r[2::4]; out[o+1:o+w*3:3] = r[1::4]; out[o+2:o+w*3:3] = r[0::4]
    return bytes(out)

def exact(a, b):
    if a is None or b is None or len(a) != len(b): return -1
    if a == b: return len(a) // 3
    return sum(1 for i in range(0, len(a), 3) if a[i:i+3] == b[i:i+3])

def regs(bar2):
    out = hmp('xp /16xh 0x%x' % (bar2 + 0x500), 0.4)
    v = []
    for line in out.splitlines():
        m = re.match(r'^\s*[0-9a-f]+:\s+(.*)$', line)
        if m: v += [int(t, 16) for t in m.group(1).split() if t.startswith('0x')]
    return v

deadline = time.time() + timeout
bar0 = bar2 = va0 = None
while time.time() < deadline and bar0 is None:
    t = read()
    m = re.search(r'Framebuffer size \d+ kB @ 0x([0-9a-f]+), mmio @ 0x([0-9a-f]+)', t)
    v = re.search(r'\[lx_pci\] bar mapped bar=0 rel=0x0+ va=0x([0-9a-f]+)', t)
    if m and v: bar0, bar2, va0 = int(m.group(1), 16), int(m.group(2), 16), int(v.group(1), 16)
    time.sleep(0.5)
if bar0 is None:
    print('[regress] no bochs probe lines'); sys.exit(3)
print('[regress] vram 0x%x mmio 0x%x container bar0 va 0x%x' % (bar0, bar2, va0))
seen = 0; good = 0; prev_start = 0
summary = open(os.path.join(scratch, 'changes.txt'), 'w')
while time.time() < deadline and seen < 20:
    t = read()
    ms = list(re.finditer(r'\[klddisp\] pattern mode (\d+)x(\d+) exact (\d+)/(\d+) change (\d+)', t))
    new = [m for m in ms if int(m.group(5)) > seen]
    if not new:
        if re.search(r'PANIC|KERNEL PANIC|\[ffault\]', t): print('[regress] panic/fault in log'); break
        time.sleep(0.2); continue
    m = new[0]; n = int(m.group(5)); seen = n
    w, h, kex, ktot = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))
    ctr = ((n*8) & 0xf8, (n*4) & 0xfc, (n*8) & 0xf8)
    before = t[:m.start()]
    cm = list(re.finditer(r'\[kld-bochs\] commit fb=(\d+) bo_offset=0x([0-9a-f]+) pitch=(\d+) vaddr=0x([0-9a-f]+)', before))
    fc = list(re.finditer(r'\[kdrm\] full copy dst=0x([0-9a-f]+) pitch=(\d+) (\d+)x(\d+)', before))
    bo = int(cm[-1].group(2), 16) if cm else -1
    fbid = int(cm[-1].group(1)) if cm else -1
    vaddr = int(cm[-1].group(4), 16) if cm else -1
    dst = int(fc[-1].group(1), 16) if fc else -1
    pa = os.path.join(scratch, 'c%02d_A.ppm' % n)
    hmp('screendump %s bochs0' % pa, 0.4)
    hmp('stop', 0.2)
    # the capture is valid only if the guest has not started the next change.
    # (satoru)
    after = read()[m.end():]
    late = ('[kdrm] setmode' in after) or ('mode change acked' in after)
    r = regs(bar2)
    if len(r) < 10:
        print('[regress] change %d: dispi registers unreadable: %r' % (n, r)); hmp('cont', 0.1); break
    xres, yres, bpp, en, vw, xo, yo = r[1], r[2], r[3], r[4], r[6], r[8], r[9]
    stride = max(vw, xres) * (bpp // 8)
    start = yo * stride + xo * (bpp // 8)
    fs = os.path.join(scratch, 'c%02d_start.bin' % n)
    fz = os.path.join(scratch, 'c%02d_zero.bin' % n)
    fp = os.path.join(scratch, 'c%02d_prev.bin' % n)
    size = stride * yres
    hmp('pmemsave 0x%x %d "%s"' % (bar0 + start, size, fs), 0.6)
    hmp('pmemsave 0x%x %d "%s"' % (bar0, size, fz), 0.6)
    hmp('pmemsave 0x%x %d "%s"' % (bar0 + prev_start, size, fp), 0.6)
    pb = os.path.join(scratch, 'c%02d_B.ppm' % n)
    hmp('screendump %s bochs0' % pb, 0.4)
    hmp('cont', 0.2)
    time.sleep(2.0)
    pc = os.path.join(scratch, 'c%02d_C.ppm' % n)
    hmp('screendump %s bochs0' % pc, 0.4)
    exp = expected(w, h, ctr)
    vs = exact(raw(fs, w, h, stride), exp)
    vz = exact(raw(fz, w, h, stride), exp)
    vp = exact(raw(fp, w, h, stride), exp)
    def sd(p):
        q = ppm(p)
        if not q: return 'none', -1
        return '%dx%d' % (q[0], q[1]), (exact(q[2], exp) if (q[0], q[1]) == (w, h) else 0)
    da, ea = sd(pa); db, eb = sd(pb); dc, ec = sd(pc)
    tot = w * h
    ok = (not late and kex == ktot == tot and (xres, yres, bpp) == (w, h, 32) and (en & 1) and
          start == bo and vaddr - va0 == bo and dst == vaddr and vs == tot and eb == tot)
    good += 1 if ok else 0
    line = ('change %2d %dx%d kernel %d/%d | regs %dx%dx%d en=%d vw=%d xo=%d yo=%d start=0x%x | '
            'container fb=%d bo=0x%x vaddr-bar=0x%x copydst=%s | vram@start %d zero %d prev(0x%x) %d | '
            'shot A %s %d B %s %d C %s %d | %s' %
            (n, w, h, kex, ktot, xres, yres, bpp, en, vw, xo, yo, start, fbid, bo,
             (vaddr - va0) if vaddr >= 0 else -1, 'same' if dst == vaddr else hex(dst),
             vs, vz, prev_start, vp, da, ea, db, eb, dc, ec,
             'OK' if ok else ('BAD late capture' if late else 'BAD')))
    print(line); summary.write(line + '\n'); summary.flush()
    prev_start = start
    if ok:
        for f in (pa, pb, pc, fs, fz, fp):
            try: os.unlink(f)
            except Exception: pass
t = read()
s0 = re.search(r'pattern cycle start pmm_used_frames=(\d+)', t)
s1 = re.search(r'pattern cycle end pmm_used_frames=(\d+)', t)
aud = len(re.findall(r'\[PTAUDIT\] \S+ va=', t))   # not the 'armed' banner (satoru)
warn = [l for l in t.splitlines() if '[kld-warn]' in l]
stub = len(re.findall(r'\[kld-stub\]', t))
print('[regress] changes seen %d device-proven %d pmm_used_frames start=%s end=%s ptaudit=%d kld-warn=%d kld-stub=%d' %
      (seen, good, s0.group(1) if s0 else '?', s1.group(1) if s1 else '?', aud, len(warn), stub))
for l in warn[:5]: print('[regress] warn: ' + l)
sys.exit(0 if (seen == 20 and good == 20 and aud == 0 and stub == 0) else 1)
PY
rc=$?
kill "$QP" 2>/dev/null; wait "$QP" 2>/dev/null; QP=
if [ "$rc" -eq 0 ]; then echo "REGRESS drm_bochs_modes: PASS 20/20 (log $LOG)"; else echo "REGRESS drm_bochs_modes: FAIL rc=$rc (log $LOG)"; fi
exit $rc

# end (satoru)
