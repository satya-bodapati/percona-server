#!/usr/bin/env python3
"""Emit a file as it should look after categories 1..N.

For a file split by symbol: start from the oracle's version and delete the
bodies of symbols assigned to a later category, using exact ranges so a
brace in a comment cannot take the wrong lines.

  gen-tree.py <N> <file>        # prints the category-N version to stdout
"""
import sys, subprocess, collections
sys.path.insert(0, 'v3-tools')
from symrange import ranges

ROOT = 'storage/innobase/'
TOP = ('sql/', 'unittest/', 'include/my_', 'mysys/', 'vector-common/',
       'storage/', 'share/', 'vec-hnsw')

def norm(f):
    if f.startswith('include/') and not f.startswith('include/my_'):
        return ROOT + f
    return f if f.startswith(TOP) else ROOT + f

def load_map(path='v3-tools/map.txt'):
    sym, whole, split = collections.defaultdict(dict), {}, set()
    for line in open(path):
        line = line.split('#')[0].strip()
        if not line:
            continue
        p = line.split()
        if p[0] == 'SPLIT':
            split.add(norm(p[1]))
        elif len(p) >= 3 and p[1] == 'FILE':
            whole[norm(p[2])] = int(p[0])
        elif len(p) >= 3:
            sym[norm(p[1])][p[2].split('::')[-1]] = int(p[0])
    return sym, whole, split

def category_version(n, path, oracle='v2-verified'):
    sym, whole, split = load_map()
    key = norm(path)
    if key in whole:
        return None if whole[key] > n else subprocess.run(
            ['git', 'show', '%s:%s' % (oracle, key)],
            capture_output=True, text=True).stdout
    if key not in sym:
        return None
    src = subprocess.run(['git', 'show', '%s:%s' % (oracle, key)],
                         capture_output=True, text=True).stdout
    tmp = '/tmp/_gen_%d' % id(src)
    open(tmp, 'w').write(src)
    lines = src.split('\n')
    drop = set()
    for name, a, b in ranges(tmp):
        short = name.split('::')[-1]
        cat = sym[key].get(short)
        if cat is not None and cat > n:
            drop.update(range(a - 1, b))
    kept = [l for i, l in enumerate(lines) if i not in drop]
    return '\n'.join(kept)

if __name__ == '__main__':
    n, f = int(sys.argv[1]), sys.argv[2]
    out = category_version(n, f)
    sys.stdout.write('' if out is None else out)
