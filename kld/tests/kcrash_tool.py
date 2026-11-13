#!/usr/bin/env python3
# host side reader/editor for the kcrash files on a test disk image (gpt, first
# partition fat32 KURONO). formats: src/kernel/kcrash.h. edits are in place in
# the file's own clusters, never a fat or directory write. (satoru)
#
#   kcrash_tool.py IMG PART_OFF size NAME83        -> byte size from the dir entry
#   kcrash_tool.py IMG PART_OFF log                -> header + every record
#   kcrash_tool.py IMG PART_OFF drv                -> KDRIVERS.CFG entries
#   kcrash_tool.py IMG PART_OFF boot               -> KBOOT.CFG fields
#   kcrash_tool.py IMG PART_OFF setpolicy FAM POL  -> POL = enabled|disabled|nostop
import struct
import sys


def fnv(b):
    h = 2166136261
    for x in b:
        h ^= x
        h = (h * 16777619) & 0xFFFFFFFF
    return h


class Fat:
    def __init__(self, img, off):
        self.f = open(img, 'r+b')
        self.off = off
        bs = self.rd(0, 90)
        self.bps = struct.unpack_from('<H', bs, 11)[0]
        self.spc = bs[13]
        rsvd = struct.unpack_from('<H', bs, 14)[0]
        nfats = bs[16]
        fatsz = struct.unpack_from('<I', bs, 36)[0]
        self.root = struct.unpack_from('<I', bs, 44)[0]
        self.clus = self.bps * self.spc
        self.fat = rsvd * self.bps
        self.data = self.fat + nfats * fatsz * self.bps

    def rd(self, o, n):
        self.f.seek(self.off + o)
        return self.f.read(n)

    def wr(self, o, b):
        self.f.seek(self.off + o)
        self.f.write(b)

    def nxt(self, c):
        return struct.unpack('<I', self.rd(self.fat + 4 * c, 4))[0] & 0x0FFFFFFF

    def coff(self, c):
        return self.data + (c - 2) * self.clus

    def find(self, n83):
        c = self.root
        while 2 <= c < 0x0FFFFFF8:
            d = self.rd(self.coff(c), self.clus)
            for o in range(0, self.clus, 32):
                e = d[o:o + 32]
                if e[0] == 0:
                    return None
                if e[0] == 0xE5 or e[11] == 0x0F or (e[11] & 0x18):
                    continue
                if e[:11] == n83:
                    first = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
                    return first, struct.unpack_from('<I', e, 28)[0]
            c = self.nxt(c)
        return None

    def read_file(self, n83):
        first, size = self.find(n83)
        out = b''
        c = first
        while len(out) < size:
            out += self.rd(self.coff(c), self.clus)
            c = self.nxt(c)
        return first, out[:size]


def n83(s):
    base, ext = s.split('.')
    return (base.ljust(8) + ext.ljust(3)).encode()


def main():
    img, off, cmd = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    fs = Fat(img, off)
    if cmd == 'size':
        r = fs.find(n83(sys.argv[4]))
        print(r[1] if r else -1)
    elif cmd == 'log':
        _, b = fs.read_file(n83('KCRASH.LOG'))
        magic, ver, nxt, cnt, ck = struct.unpack_from('<5I', b, 0)
        ok = magic == 0x4852434B and ck == fnv(b[:16])
        print('header magic_ok=%d version=%d next=%d count=%d' % (ok, ver, nxt, cnt))
        for i in range(16):
            r = b[4096 + i * 4096:4096 + i * 4096 + 128]
            if struct.unpack_from('<I', r, 0)[0] != 0x3152434B:
                continue
            (magic, ver, kind, boot, seq, up, rtc, stop) = struct.unpack_from('<IHHIIQQI', r, 0)
            fam = r[36:52].split(b'\0')[0].decode()
            bdf, ven, dev, pid, rc, sig = struct.unpack_from('<IHHiii', r, 52)
            fault = struct.unpack_from('<Q', r, 76)[0]
            att, res = struct.unpack_from('<HH', r, 84)
            act = r[92:124].split(b'\0')[0].decode()
            ck = struct.unpack_from('<I', r, 124)[0]
            print('record slot=%d seq=%d boot=%d kind=%d family=%s bdf=%x id=%04x:%04x pid=%d rc=%d sig=%d '
                  'fault=%x attempt=%d result=%d action="%s" rtc=%d csum_ok=%d'
                  % (i, seq, boot, kind, fam, bdf, ven, dev, pid, rc, sig, fault, att, res, act, rtc,
                     ck == fnv(r[:124])))
    elif cmd == 'drv':
        _, b = fs.read_file(n83('KDRIVERS.CFG'))
        ok = struct.unpack_from('<I', b, 0)[0] == 0x5652444B and struct.unpack_from('<I', b, 8)[0] == fnv(b[16:4096])
        print('kdrivers magic_csum_ok=%d' % ok)
        for i in range(64):
            e = b[16 + 32 * i:48 + 32 * i]
            if e[0] == 0:
                continue
            fam = e[:16].split(b'\0')[0].decode()
            fails, lastb = struct.unpack_from('<II', e, 20)
            print('drv family=%s policy=%d fails=%d last_fail_boot=%d' % (fam, e[16], fails, lastb))
    elif cmd == 'boot':
        _, b = fs.read_file(n83('KBOOT.CFG'))
        ok = struct.unpack_from('<I', b, 0)[0] == 0x544F424B and struct.unpack_from('<I', b, 508)[0] == fnv(b[:508])
        print('kboot magic_csum_ok=%d boot_count=%d flags=%x' % (ok, struct.unpack_from('<I', b, 8)[0],
                                                                 struct.unpack_from('<I', b, 12)[0]))
    elif cmd == 'setpolicy':
        fam, pol = sys.argv[4].encode(), {'enabled': 0, 'disabled': 1, 'nostop': 2}[sys.argv[5]]
        first, b = fs.read_file(n83('KDRIVERS.CFG'))
        b = bytearray(b)
        slot = None
        for i in range(64):
            e = b[16 + 32 * i:48 + 32 * i]
            if e[:16].split(b'\0')[0] == fam or (slot is None and e[0] == 0):
                slot = i
                if e[:16].split(b'\0')[0] == fam:
                    break
        o = 16 + 32 * slot
        if b[o:o + 16].split(b'\0')[0] != fam:
            b[o:o + 32] = bytes(32)
            b[o:o + len(fam)] = fam
        b[o + 16] = pol
        struct.pack_into('<I', b, 8, fnv(bytes(b[16:4096])))
        # one cluster run (the kernel refuses fragmented files anyway) (satoru)
        fs.wr(fs.coff(first), bytes(b))
        print('set %s policy=%d' % (fam.decode(), pol))
    fs.f.close()


main()

# end (satoru)
