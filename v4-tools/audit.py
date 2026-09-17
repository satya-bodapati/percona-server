#!/usr/bin/env python3
"""What landed in each commit by decision, and what came along for the ride.

Usage:  audit.py <map> <repo> <base> <branch>

For every file a commit changes, say whether the map put it there (an explicit
assignment) or whether it arrived incidentally - file-scope code, an include,
a header dragged in by something else. Incidental content is what makes a
commit look like it is doing someone else's job.
"""
import sys, os, subprocess, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ROOT = 'storage/innobase/'
TOP  = ('sql/', 'unittest/', 'include/my_', 'mysys/', 'vector-common/',
        'storage/', 'share/', 'vec-hnsw', 'mysql-test/')

def norm(f):
    if f.startswith('include/') and not f.startswith('include/my_'):
        return ROOT + f
    return f if f.startswith(TOP) else ROOT + f

def main():
    mapfile, repo, base, branch = sys.argv[1:5]
    os.chdir(repo)
    assigned = collections.defaultdict(set)      # step -> {paths}
    for line in open(mapfile):
        line = line.split('#')[0].strip()
        if not line:
            continue
        p = line.split()
        if len(p) >= 3 and p[1] == 'FILE':
            assigned[int(p[0])].add(norm(p[2]))
        elif len(p) >= 3 and p[0].isdigit():
            assigned[int(p[0])].add(norm(p[1]))

    commits = subprocess.run(['git', 'rev-list', '--reverse', '%s..%s' % (base, branch)],
                             capture_output=True, text=True).stdout.split()
    for i, c in enumerate(commits, 1):
        subj = subprocess.run(['git', 'log', '-1', '--format=%s', c],
                              capture_output=True, text=True).stdout.strip()
        subj = subj.split('] ', 1)[-1]
        files = subprocess.run(['git', 'show', '--name-only', '--format=', c],
                               capture_output=True, text=True).stdout.split()
        code = [f for f in files if not f.startswith('mysql-test')]
        stray = [f for f in code if f not in assigned[i]]
        print('\n[%d] %s' % (i, subj))
        print('    by decision: %d   incidental: %d' % (len(code) - len(stray), len(stray)))
        for f in stray:
            n = subprocess.run(['git', 'show', '--numstat', '--format=', c, '--', f],
                               capture_output=True, text=True).stdout.split()
            print('      ~ %-52s %s lines' % (f.replace('storage/innobase/', ''),
                                              n[0] if n else '?'))

main()
