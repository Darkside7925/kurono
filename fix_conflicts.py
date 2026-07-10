#!/usr/bin/env python3
"""Resolve all git merge conflicts in favor of HEAD."""
import os, re

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'src')
EXTS = {'.cpp', '.h', '.asm', '.ld'}
MARKER = re.compile(
    r'^<{7} HEAD\n(.*?)^={7}\n(.*?)^>{7} .*?\n',
    re.MULTILINE | re.DOTALL
)

count = 0
for root, dirs, files in os.walk(SRC):
    for fn in files:
        if os.path.splitext(fn)[1] not in EXTS:
            continue
        path = os.path.join(root, fn)
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            text = f.read()
        if '<<<<<<<' not in text:
            continue
        new_text = MARKER.sub(r'\1', text)
        n = text.count('<<<<<<< HEAD')
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(new_text)
        print(f'Fixed {n} conflict(s): {os.path.relpath(path, os.path.dirname(SRC))}')
        count += n

print(f'\nTotal: {count} conflicts resolved')
