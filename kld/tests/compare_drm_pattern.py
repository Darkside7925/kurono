#!/usr/bin/env python3
# compare a QEMU PPM screendump of the cirrus device with the kdrm_client
# d1 test pattern. one line on stdout, exit 0 on an exact match.
#
# pattern: 8 vertical bars, a 16x16 one-pixel checkerboard at (64,64), and the
# frame-0 counter block (32x32, black) at (96,64). the cirrus driver's shadow
# plane keeps RGB888 for a 1024-wide xr24 framebuffer (width*3 = 3072 <=
# CIRRUS_MAX_PITCH 4088), so the colours reach the scanout exactly. (satoru)
#
# end (satoru)
import sys

def expected(w, h):
    bars = [(255,255,255),(255,255,0),(0,255,255),(0,255,0),
            (255,0,255),(255,0,0),(0,0,255),(0,0,0)]
    bw = w // 8
    img = bytearray(w*h*3)
    def fill(x0, y0, ww, hh, c):
        for y in range(y0, min(y0+hh, h)):
            row = y*w*3
            for x in range(x0, min(x0+ww, w)):
                o = row + x*3
                img[o], img[o+1], img[o+2] = c
    for i, c in enumerate(bars):
        x = i*bw
        fill(x, 0, (w-x) if i == 7 else bw, h, c)
    for y in range(16):
        for x in range(16):
            fill(64+x, 64+y, 1, 1, (255,255,255) if ((x^y)&1) else (0,0,0))
    fill(96, 64, 32, 32, (0,0,0))
    return img

def main():
    if len(sys.argv) != 2:
        print('usage: compare_drm_pattern.py <ppm>')
        return 2
    with open(sys.argv[1], 'rb') as f:
        if f.readline().strip() != b'P6':
            print('pattern compare: not a P6 PPM')
            return 1
        dims = f.readline().split()
        maxv = int(f.readline())
        w, h = int(dims[0]), int(dims[1])
        data = f.read()
    if maxv != 255 or len(data) != w*h*3:
        print('pattern compare: bad PPM %dx%d max=%d bytes=%d' % (w, h, maxv, len(data)))
        return 1
    exp = expected(w, h)
    match = 0
    first = None
    for i in range(w*h):
        o = i*3
        if data[o:o+3] == exp[o:o+3]:
            match += 1
        elif first is None:
            first = i
    if first is None:
        print('pattern compare: %d/%d pixels exact at %dx%d' % (match, w*h, w, h))
        return 0
    x, y = first % w, first // w
    print('pattern compare: %d/%d exact, first mismatch at %d,%d got %s want %s' %
          (match, w*h, x, y, tuple(data[first*3:first*3+3]), tuple(exp[first*3:first*3+3])))
    return 1

if __name__ == '__main__':
    sys.exit(main())

# end (satoru)
