#!/usr/bin/env python3
"""What each commit actually changes, named by symbol rather than by file.

Usage:  symbols-per-commit.py <repo> <base> <branch> [from-step]

File names say almost nothing about whether a change belongs in a commit.
This lists the functions each commit adds or alters, which is the level at
which "does this belong here?" can be answered.
"""
import sys, os, re, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symrange import ranges

def main():
    repo, base, branch = sys.argv[1:4]
    start = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    os.chdir(repo)
    commits = subprocess.run(['git', 'rev-list', '--reverse', '%s..%s' % (base, branch)],
                             capture_output=True, text=True).stdout.split()
    for i, c in enumerate(commits, 1):
        if i < start:
            continue
        subj = subprocess.run(['git', 'log', '-1', '--format=%s', c],
                              capture_output=True, text=True).stdout.strip().split('] ', 1)[-1]
        print('\n==== [%d] %s' % (i, subj))
        files = [f for f in subprocess.run(['git', 'show', '--name-only', '--format=', c],
                                           capture_output=True, text=True).stdout.split()
                 if f.endswith(('.cc', '.h', '.ic'))]
        for f in files:
            src = subprocess.run(['git', 'show', '%s:%s' % (c, f)],
                                 capture_output=True, text=True).stdout
            if not src:
                continue
            tmp = '/tmp/_spc' + os.path.splitext(f)[1]
            open(tmp, 'w').write(src)
            diff = subprocess.run(['git', 'show', '-U0', '--format=', c, '--', f],
                                  capture_output=True, text=True).stdout
            add, cur = set(), 0
            for l in diff.splitlines():
                m = re.match(r'^@@ -\S+ \+(\d+)', l)
                if m:
                    cur = int(m.group(1)); continue
                if l.startswith('+') and not l.startswith('+++'):
                    add.add(cur); cur += 1
                elif l.startswith(' '):
                    cur += 1
            hit, covered = [], set()
            for name, a, b in ranges(tmp):
                n = sum(1 for x in add if a <= x <= b)
                if n:
                    hit.append((name, n))
                covered.update(range(a, b + 1))
            loose = len([x for x in add if x not in covered])
            short = f.replace('storage/innobase/', '')
            if hit or loose:
                print('  %s' % short)
                for name, n in sorted(hit, key=lambda t: -t[1]):
                    print('      %-52s +%d' % (name, n))
                if loose:
                    print('      %-52s +%d' % ('<file scope: includes, tables, constants>', loose))

main()
