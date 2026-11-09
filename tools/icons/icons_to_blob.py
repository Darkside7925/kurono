#!/usr/bin/env python3
"""Rasterize the modern icon sets into src/assets/icons/modern_icons.bin.

The kernel has no svg renderer, so the icons ship as 128x128 premultiplied
ARGB bitmaps (little-endian uint32 0xAARRGGBB), one per AppIcons::Id in enum
order: first the colourful set (src/assets/icons/*.svg), then the minimal set
(src/assets/icons/minimal/*.svg). Kurono area-averages them to whatever size
it draws at. Needs rsvg-convert and Pillow.

Blob layout: "KICN", u32 count, u32 size, then count * size * size u32 pixels.
"""
import os, struct, subprocess, sys, tempfile
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '../../src/assets/icons')
ORDER = ['terminal', 'files', 'calculator', 'editor', 'settings', 'browser',
         'tasks', 'media', 'home', 'web', 'generic']   # = AppIcons::Id order
SIZE = 128

SETS = [SRC, os.path.join(SRC, 'minimal')]
out = bytearray(b'KICN' + struct.pack('<II', len(ORDER) * len(SETS), SIZE))
with tempfile.TemporaryDirectory() as tmp:
    for name in [os.path.join(d, n) for d in SETS for n in ORDER]:
        png = os.path.join(tmp, 'icon.png')
        subprocess.run(['rsvg-convert', '-w', str(SIZE), '-h', str(SIZE),
                        name + '.svg', '-o', png], check=True)
        raw = Image.open(png).convert('RGBA').tobytes()
        for i in range(0, len(raw), 4):
            r, g, b, a = raw[i], raw[i + 1], raw[i + 2], raw[i + 3]
            r, g, b = (r * a + 127) // 255, (g * a + 127) // 255, (b * a + 127) // 255
            out += struct.pack('<I', (a << 24) | (r << 16) | (g << 8) | b)
path = os.path.join(SRC, 'modern_icons.bin')
with open(path, 'wb') as f:
    f.write(out)
print('wrote', path, len(out), 'bytes')
