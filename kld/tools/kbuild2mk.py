#!/usr/bin/env python3
# kbuild2mk - print the object list a Kbuild makefile would build for one
# composite module, evaluated against the PINNED kernel config.
#
#   kbuild2mk.py <kbuild Makefile> <module> <autoconf.h> [VAR=y ...]
#
# Handles the forms driver makefiles actually use:
#   <module>-y += a.o b.o \
#   <module>-$(CONFIG_FOO) += c.o
#   <module>-objs := d.o
# A CONFIG_ symbol counts as enabled when autoconf.h defines it (=y) or defines
# CONFIG_<X>_MODULE (=m, because KLD builds everything as a module). Extra
# VAR=y pairs set non-CONFIG make variables (e.g. ACPI_FUTURE_USAGE=y).
#
# This is what replaces "wildcard *.c": the wildcard silently builds files the
# real kernel build never compiles (other bus glue, other SoCs), which then add
# phantom symbols to the census. (satoru)
#
# end (satoru)
import re
import sys


def load_config(path):
    cfg = {}
    with open(path) as f:
        for line in f:
            m = re.match(r'#define\s+(CONFIG_\w+)\s+(.*)', line)
            if not m:
                continue
            name, val = m.group(1), m.group(2).strip()
            if name.endswith('_MODULE'):
                cfg[name[:-7]] = 'm'
            elif val == '1':
                cfg.setdefault(name, 'y')
    return cfg


def logical_lines(path):
    buf = ''
    with open(path) as f:
        for raw in f:
            line = raw.split('#', 1)[0].rstrip('\n')
            if line.endswith('\\'):
                buf += line[:-1] + ' '
                continue
            buf += line
            if buf.strip():
                yield buf
            buf = ''
    if buf.strip():
        yield buf


def main():
    if len(sys.argv) < 4:
        sys.stderr.write(__doc__ or 'usage: kbuild2mk.py Makefile module autoconf.h [VAR=y]\n')
        return 2
    mk, mod, autoconf = sys.argv[1:4]
    cfg = load_config(autoconf)
    for kv in sys.argv[4:]:
        k, _, v = kv.partition('=')
        cfg[k] = v
    objs = []
    pat = re.compile(r'^\s*' + re.escape(mod) + r'-(y|objs|\$\((\w+)\))\s*[:+]?=\s*(.*)$')
    for line in logical_lines(mk):
        m = pat.match(line)
        if not m:
            continue
        kind, var, rest = m.group(1), m.group(2), m.group(3)
        if var is not None and cfg.get(var) not in ('y', 'm'):
            continue
        for o in rest.split():
            if o.endswith('.o') and o not in objs:
                objs.append(o)
    print(' '.join(objs))
    return 0


if __name__ == '__main__':
    sys.exit(main())

# end (satoru)
