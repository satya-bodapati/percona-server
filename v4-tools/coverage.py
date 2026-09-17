#!/usr/bin/env python3
"""Did v4 lose any of v2's code?

Usage:  coverage.py <repo> <base> <v2-tip> <v4-tip> [drops.txt]

Compares the set of lines v2 adds against the set v4 adds. Anything v2 adds
that v4 does not is either an intentional drop (list it in drops.txt, one
substring per line) or code that went missing. Anything v4 adds that v2 does
not is new material and is listed too.

Whitespace-only differences are ignored; this is about content, not layout.
"""
import sys, subprocess, collections

def added(repo, a, b):
    out = subprocess.run(['git', '-C', repo, 'diff', a, b],
                         capture_output=True, text=True).stdout
    c = collections.Counter()
    for l in out.splitlines():
        if l.startswith('+') and not l.startswith('+++'):
            s = ' '.join(l[1:].split())
            if s:
                c[s] += 1
    return c

def main():
    repo, base, v2, v4 = sys.argv[1:5]
    drops = []
    if len(sys.argv) > 5:
        drops = [l.strip() for l in open(sys.argv[5])
                 if l.strip() and not l.startswith('#')]

    a, b = added(repo, base, v2), added(repo, base, v4)
    lost  = [(l, n - b[l]) for l, n in a.items() if n > b[l]]
    extra = [(l, n - a[l]) for l, n in b.items() if n > a[l]]

    def dropped(line):
        return any(d in line for d in drops)

    unexplained = [(l, n) for l, n in lost if not dropped(l)]
    print('  v2 adds %d distinct lines, v4 adds %d' % (len(a), len(b)))
    print('  in v2 but not v4: %d  (%d explained by drops.txt)'
          % (len(lost), len(lost) - len(unexplained)))
    print('  in v4 but not v2: %d' % len(extra))
    if unexplained:
        print('  --- unexplained losses (first 40) ---')
        for l, n in unexplained[:40]:
            print('   -%s %s' % (('x%d' % n) if n > 1 else '  ', l[:110]))
    if extra:
        print('  --- new in v4 (first 25) ---')
        for l, n in extra[:25]:
            print('   +%s %s' % (('x%d' % n) if n > 1 else '  ', l[:110]))
    return 1 if unexplained else 0

sys.exit(main())
