#!/usr/bin/env python3
# d4.2: every register outside the display block that i915 writes before the
# gt is declared wedged (path a), from one model rig run. usage:
#   d42_list.py <run dir> <kld-i915.elf> > list.txt          (satoru)
import subprocess, sys, collections, re

run, elf = sys.argv[1], sys.argv[2]
log = open(run + '/serial.log', 'rb').read().decode('latin-1').replace('\r', '')
rows = []
for line in log.split('\n'):
    if 'declaring it wedged' in line:
        break
    m = re.search(r'\[i915m\] w (0x[0-9a-f]+) (0x[0-9a-f]+) (0x[0-9a-f]+) ([DX])', line)
    if m and m.group(4) == 'X':
        rows.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3)))

rets = sorted(set(r[2] for r in rows))
names = {}
if rets:
    out = subprocess.run(['addr2line', '-f', '-e', elf] + rets, capture_output=True, text=True).stdout.split('\n')
    for i, a in enumerate(rets):
        names[a] = out[2 * i] if 2 * i < len(out) else '?'

def cat(o):
    b = o - 0x380000 if o >= 0x380000 else o
    media = ' (media gt)' if o >= 0x380000 else ''
    if b in (0xa188, 0xa278) or 0xa540 <= b <= 0xa61c: return 'forcewake request' + media
    if b in (0x941c,): return 'gdrst (gt reset)' + media
    if (b & 0xfff) == 0x0d0 and b < 0x200000 and not 0x190000 <= b <= 0x1901ff: return 'engine RING_RESET_CTL'
    if b in (0xfd0, 0xfd4): return 'mcr steering' + media
    if 0x4800 <= b <= 0x48ff: return 'pat index (gt)' + media
    if o == 0x101008: return 'ggtt flush (GFX_FLSH_CNTL)'
    if 0x190000 <= o <= 0x1901ff: return 'interrupt master / gt interrupt enable+mask'
    if 0xc000 <= b <= 0xcfff: return 'guc registers' + media
    if 0x138000 <= o <= 0x138fff: return 'pcode / soc'
    if (b & 0xfff) in (0x098, 0x09c, 0x0a8) and b < 0x200000: return 'engine imr/mode'
    if 0x9000 <= b <= 0x9fff or 0xb000 <= b <= 0xbfff or 0xe000 <= b <= 0xefff or 0x7000 <= b <= 0x7fff: return 'gt workaround / config' + media
    return 'other' + media

per = collections.OrderedDict()
for o, v, r in rows:
    e = per.setdefault(o, [0, v, v, set()])
    e[0] += 1
    e[2] = v
    e[3].add(names.get(r, r))

print('# d4.2: writes outside the display block before "declaring it wedged"')
print('# run %s' % run)
print('# total writes %d, unique registers %d' % (len(rows), len(per)))
bycat = collections.Counter()
for o, e in per.items():
    bycat[cat(o)] += 1
print('# by category (unique registers):')
for c, n in bycat.most_common():
    print('#   %4d  %s' % (n, c))
print('# offset    count first      last       category | writers')
for o, e in sorted(per.items()):
    print('0x%06x %5d 0x%08x 0x%08x %s | %s' % (o, e[0], e[1], e[2], cat(o), ','.join(sorted(e[3]))))
# end (satoru)
