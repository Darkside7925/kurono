#!/usr/bin/env python3
# disklog_unwrap.py KURONO.LOG [out.txt] - put a wrapped session log back in
# time order. (satoru)
#
# the file is [boot head][ring]. once the ring has wrapped, the newest data
# ends at a "===== [disklog] END OF NEWEST DATA ... head=N =====" line and
# the oldest surviving tail lines follow it. output: head, then the ring from
# just after the marker to its end, then the ring start up to the marker, with
# NUL padding dropped. a log that never wrapped is printed as is. (satoru)
import re, sys

def main():
    if len(sys.argv) < 2:
        print("usage: disklog_unwrap.py KURONO.LOG [out.txt]", file=sys.stderr)
        return 2
    data = open(sys.argv[1], 'rb').read()
    m = re.search(rb'\n===== \[disklog\] END OF NEWEST DATA\. older lines follow; head=(\d+) =====\n', data)
    if not m:
        out = data
    else:
        head = int(m.group(1))
        # the marker sits in its own lba; skip the rest of that lba too (satoru)
        tail_start = m.end()
        lba = 512
        while tail_start % lba:
            tail_start += 1
        older = data[tail_start:]
        newer = data[head:m.start()]
        # the oldest ring part starts mid-line: drop up to the first newline (satoru)
        nl = older.find(b'\n')
        older = older[nl + 1:] if nl >= 0 else older
        out = (data[:head] + b'\n===== [unwrap] boot head ends; ring (oldest kept) starts =====\n'
               + older + b'\n===== [unwrap] ring wraps here =====\n' + newer)
    out = out.replace(b'\x00', b'')
    if len(sys.argv) > 2:
        open(sys.argv[2], 'wb').write(out)
    else:
        sys.stdout.buffer.write(out)
    return 0

if __name__ == '__main__':
    sys.exit(main())
# end (satoru)
