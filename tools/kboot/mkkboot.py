#!/usr/bin/env python3
# writes an empty KBOOT.CFG (512 bytes, magic 'KBOT', version 1, fnv-1a 32 seal
# at 508; layout in src/system/bootcfg.h) so recovery and A/B updates work
# from the first boot of an image. usage: mkkboot.py OUT (satoru)
import struct, sys
b = bytearray(512)
struct.pack_into('<IHH', b, 0, 0x544F424B, 1, 0)
h = 2166136261
for x in b[:508]:
    h ^= x
    h = (h * 16777619) & 0xffffffff
struct.pack_into('<I', b, 508, h)
open(sys.argv[1], 'wb').write(b)
# end (satoru)
