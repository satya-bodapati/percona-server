#!/usr/bin/env python3
"""Resolve `git apply --3way` conflict regions by picking a side per region.

Usage:  resolve.py <file> <n>=ours|theirs [<n>=ours|theirs ...]

Regions are numbered from 1 in file order. Any region not named is left
conflicted, so nothing is resolved silently.
"""
import sys

def main():
    path = sys.argv[1]
    pick = {}
    for a in sys.argv[2:]:
        n, _, side = a.partition('=')
        pick[int(n)] = side

    out, i, n = [], 0, 0
    lines = open(path).readlines()
    while i < len(lines):
        if not lines[i].startswith('<<<<<<<'):
            out.append(lines[i]); i += 1; continue
        n += 1
        i += 1
        ours = []
        while not lines[i].startswith(('|||||||', '=======')):
            ours.append(lines[i]); i += 1
        if lines[i].startswith('|||||||'):          # diff3 style
            i += 1
            while not lines[i].startswith('======='):
                i += 1
        i += 1
        theirs = []
        while not lines[i].startswith('>>>>>>>'):
            theirs.append(lines[i]); i += 1
        i += 1
        side = pick.get(n)
        if side is None:
            sys.exit('%s: region %d not specified' % (path, n))
        out.extend(ours if side == 'ours' else theirs)
    open(path, 'w').write(''.join(out))
    print('  %s: resolved %d region(s)' % (path, n))

main()
