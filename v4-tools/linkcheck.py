#!/usr/bin/env python3
"""Find callers the map places before their callees.

Usage:  linkcheck.py <map> <repo>

Builds the set of symbols the branch introduces (functions absent from the
base tree) with the step each is assigned, then scans every mapped symbol's
final text for references to them. A reference to a symbol assigned a later
step is a link error waiting to happen, and is reported with both steps.
"""
import sys, os, re, subprocess, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symrange import ranges

ORACLE, BASE = 'vector-persistence-v2', 'ecb908769a5'
ROOT = 'storage/innobase/'
TOP  = ('sql/', 'unittest/', 'include/my_', 'mysys/', 'vector-common/',
        'storage/', 'share/', 'vec-hnsw', 'mysql-test/')

def norm(f):
    if f.startswith('include/') and not f.startswith('include/my_'):
        return ROOT + f
    return f if f.startswith(TOP) else ROOT + f

def show(path, rev=ORACLE):
    r = subprocess.run(['git', 'show', '%s:%s' % (rev, path)],
                       capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None

def main():
    mapfile, repo = sys.argv[1], sys.argv[2]
    os.chdir(repo)
    sym, whole = collections.defaultdict(dict), {}
    for line in open(mapfile):
        line = line.split('#')[0].strip()
        if not line:
            continue
        p = line.split()
        if len(p) >= 3 and p[1] == 'FILE':
            whole[norm(p[2])] = int(p[0])
        elif len(p) >= 3 and p[0].isdigit():
            sym[norm(p[1])][p[2]] = int(p[0])

    # step at which each definition becomes available
    step_of, where = {}, {}
    for path, d in sym.items():
        for name, st in d.items():
            short = name.split('::')[-1]
            step_of[short] = min(step_of.get(short, 99), st)
            where[short] = path
    for path, st in whole.items():
        if not path.endswith(('.cc', '.h')) or show(path, BASE) is not None:
            continue                      # only files the branch introduces
        src = show(path) or ''
        tmp = '/tmp/_lc' + os.path.splitext(path)[1]
        open(tmp, 'w').write(src)
        for name, a, b in ranges(tmp):
            short = name.split('::')[-1]
            step_of[short] = min(step_of.get(short, 99), st)
            where.setdefault(short, path)

    # every mapped symbol's own text, and what it references
    bad = []
    for path in sorted(set(sym) | set(whole)):
        if not path.endswith('.cc'):
            continue
        src = show(path)
        if src is None:
            continue
        tmp = '/tmp/_lc2.cc'
        open(tmp, 'w').write(src)
        lines = src.split('\n')
        for name, a, b in ranges(tmp):
            short = name.split('::')[-1]
            own = sym.get(path, {}).get(name, sym.get(path, {}).get(short))
            if own is None:
                own = whole.get(path)
            if own is None:
                continue
            body = '\n'.join(lines[a - 1:b])
            for tok in set(re.findall(r'\b([a-z_][a-z0-9_]{3,})\s*\(', body)):
                if tok == short or tok not in step_of:
                    continue
                # generic method names (open, close, check, commit_*) match far
                # more than the branch's own functions; only trust the names
                # the branch actually introduced as free functions
                if not tok.startswith(('vec_', 'dd_add_vec', 'dd_set_vec',
                                       'dd_get_vec', 'dd_create_vec',
                                       'innobase_vector', 'innobase_support')):
                    continue
                if step_of[tok] > own:
                    bad.append((own, path, short, step_of[tok], tok))
    bad.sort()
    for own, path, short, st, tok in bad:
        print('  step %-2d %-26s %-34s -> %-28s step %d'
              % (own, path.split('/')[-1], short, tok, st))
    print('  %d caller/callee ordering problems' % len(bad))

main()
