#!/usr/bin/env python3
# regenerate src/kernel/kld_gt_allow.inc from a d4.2 model run directory.
# usage: gen_gt_allow.py <regress_i915_model_... dir> <kld-i915.elf> (satoru)
import sys, re
run, elf = sys.argv[1], sys.argv[2]
log = open(run + '/serial.log', 'rb').read().decode('latin-1').replace('\r', '')
offs = set()
for line in log.split('\n'):
    m = re.search(r'\[i915m\] w (0x[0-9a-f]+) 0x[0-9a-f]+ 0x[0-9a-f]+ X', line)
    if m:
        offs.add(int(m.group(1), 16))
offs = sorted(offs)
with open('src/kernel/kld_gt_allow.inc', 'w') as o:
    o.write('// AUTO-GENERATED from a d4.2 model run by kld/tests/gen_gt_allow.py.\n')
    o.write('// the non-display registers the i915 display path writes, path (a). (satoru)\n')
    o.write('static const uint32_t kGtAllow[] = {\n')
    for i in range(0, len(offs), 8):
        o.write('    ' + ', '.join('0x%06x' % x for x in offs[i:i+8]) + ',\n')
    o.write('};\n// end (satoru)\n')
print(len(offs), 'registers')
# end (satoru)
