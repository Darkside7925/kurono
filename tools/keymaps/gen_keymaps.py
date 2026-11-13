#!/usr/bin/env python3
# builds src/assets/keymaps/keymaps.bin: compiled xkb keymaps (evdev keycodes)
# for the wayland server, one per layout Kurono offers. needs xkbcli on the
# build host. format, little endian: u32 magic 'KMP1', u32 count, then count
# entries of { char name[8], u32 offset, u32 length } and the keymap texts,
# each with its trailing nul (length counts it). (satoru)
import struct, subprocess, sys, os
LAYOUTS = ["gb", "de", "fr", "es", "it"]   # keep the image small: about 73 kb each (satoru)
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "../../src/assets/keymaps/keymaps.bin")
blobs = []
for l in LAYOUTS:
    t = subprocess.run(["xkbcli", "compile-keymap", "--layout", l], capture_output=True, check=True).stdout
    blobs.append((l, t + b"\0"))
hdr = struct.pack("<II", 0x31504D4B, len(blobs))
off = 8 + 16 * len(blobs)
table = b""; data = b""
for name, b in blobs:
    table += struct.pack("<8sII", name.encode(), off + len(data), len(b))
    data += b
    while len(data) % 16: data += b"\0"
open(out, "wb").write(hdr + table + data)
print(out, len(hdr + table + data), "bytes,", len(blobs), "layouts")
# end (satoru)
