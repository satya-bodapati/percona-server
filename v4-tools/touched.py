#!/usr/bin/env python3
"""Which top-level symbols does the branch actually change in a file?

Usage:  touched.py <repo> <base> <tip> <path>

Prints each symbol of the tip's version whose line range overlaps a line the
diff adds, plus any added lines that fall outside every symbol (includes,
file-scope constants) marked as <file-scope>.
"""
import sys, subprocess, re, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symrange import ranges

def main():
    repo, base, tip, path = sys.argv[1:5]
    src = subprocess.run(['git', '-C', repo, 'show', '%s:%s' % (tip, path)],
                         capture_output=True, text=True).stdout
    tmp = '/tmp/_touched' + os.path.splitext(path)[1]
    open(tmp, 'w').write(src)

    # new-side line numbers the diff adds
    diff = subprocess.run(['git', '-C', repo, 'diff', '-U0', base, tip, '--', path],
                          capture_output=True, text=True).stdout
    addl, cur = set(), 0
    for l in diff.splitlines():
        m = re.match(r'^@@ -\S+ \+(\d+)(?:,(\d+))? @@', l)
        if m:
            cur = int(m.group(1)); continue
        if l.startswith('+') and not l.startswith('+++'):
            addl.add(cur); cur += 1
        elif l.startswith(' '):
            cur += 1

    syms = ranges(tmp)
    covered = set()
    for name, a, b in syms:
        hit = sum(1 for x in addl if a <= x <= b)
        if hit:
            print('  %-44s %5d-%-5d  +%d' % (name, a, b, hit))
        covered.update(range(a, b + 1))
    loose = sorted(x for x in addl if x not in covered)
    if loose:
        print('  %-44s %s  +%d' % ('<file-scope>',
              '%d-%d' % (loose[0], loose[-1]), len(loose)))

main()
