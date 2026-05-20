import re, sys
nums = {}
for line in open(sys.argv[1]):
    m = re.match(r'#define\s+(LSYS_\w+)\s+(\d+)', line)
    if m:
        n = int(m.group(2))
        if n in nums: print(f'DUP {n}: {nums[n]} vs {m.group(1)}')
        nums[n] = m.group(1)
