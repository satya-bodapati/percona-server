#!/usr/bin/env python3
"""Every name a commit introduces that nothing uses until a later commit.

Usage:  dead-on-arrival.py <repo> <base> <branch>

Takes every identifier the branch adds that does not exist in the base tree -
functions, variables, constants, macros, enum members, struct fields - and
compares the commit that first contains it against the first commit whose
function bodies actually mention it. A name defined commits before anything
reads it is content sitting in the wrong commit.
"""
import sys, os, re, subprocess, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symrange import ranges, strip_noncode

WORD = re.compile(r'\b[A-Za-z_][A-Za-z0-9_]{4,}\b')

def show(rev, path):
    return subprocess.run(['git', 'show', '%s:%s' % (rev, path)],
                          capture_output=True, text=True).stdout

def main():
    repo, base, branch = sys.argv[1:4]
    os.chdir(repo)
    files = [f for f in subprocess.run(
                ['git', 'diff', '--name-only', base, branch],
                capture_output=True, text=True).stdout.split()
             if f.endswith(('.cc', '.h', '.ic'))]

    # identifiers the branch introduces
    base_words, tip_words = set(), set()
    for f in files:
        base_words |= set(WORD.findall(strip_noncode(show(base, f))))
        tip_words  |= set(WORD.findall(strip_noncode(show(branch, f))))
    new = {w for w in tip_words - base_words if not w.isupper() or '_' in w}

    commits = subprocess.run(['git', 'rev-list', '--reverse', '%s..%s' % (base, branch)],
                             capture_output=True, text=True).stdout.split()
    first_seen, first_used = {}, {}
    for i, c in enumerate(commits, 1):
        touched = [f for f in subprocess.run(['git', 'show', '--name-only', '--format=', c],
                                             capture_output=True, text=True).stdout.split()
                   if f.endswith(('.cc', '.h', '.ic'))]
        seen, used = set(), set()
        for f in touched:
            src = show(c, f)
            if not src:
                continue
            code = strip_noncode(src)
            seen |= set(WORD.findall(code)) & new
            tmp = '/tmp/_doa' + os.path.splitext(f)[1]
            open(tmp, 'w').write(src)
            lines = code.split('\n')
            bodies = '\n'.join('\n'.join(lines[a - 1:b]) for _, a, b in ranges(tmp))
            used |= set(WORD.findall(bodies)) & new
        for w in seen:
            first_seen.setdefault(w, i)
        for w in used:
            first_used.setdefault(w, i)

    bad = [(w, first_seen[w], first_used.get(w)) for w in first_seen
           if first_used.get(w, 99) > first_seen[w]]
    by_commit = collections.defaultdict(list)
    for w, a, b in bad:
        by_commit[a].append((w, b))
    for a in sorted(by_commit):
        print('\n  [%d] defines %d name(s) nothing uses until later:' % (a, len(by_commit[a])))
        for w, b in sorted(by_commit[a])[:14]:
            print('        %-34s first used in %s' % (w, ('[%d]' % b) if b else 'NO COMMIT'))
        if len(by_commit[a]) > 14:
            print('        ... and %d more' % (len(by_commit[a]) - 14))
    print('\n  %d of %d introduced names are used no earlier than a later commit'
          % (len(bad), len(first_seen)))

main()
