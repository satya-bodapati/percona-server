#!/usr/bin/env python3
"""Materialise the v3 branch from the map, one commit per category.

Runs inside a scratch worktree checked out at BASE. For each category N it
writes every file assigned to categories <= N at its category-N content, then
commits. The tip must end byte-identical to the oracle.
"""
import sys, os, re, subprocess, collections
sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
from symrange import ranges

ORACLE = 'v2-verified'
ROOT = 'storage/innobase/'
TOP = ('sql/', 'unittest/', 'include/my_', 'mysys/', 'vector-common/',
       'storage/', 'share/', 'vec-hnsw', 'mysql-test/')

TITLES = {
 1:  'vector aux tables: create, drop, rename, truncate',
 2:  'percona_vec_aux_id and its label counter',
 3:  'vector index syntax: options, validation, bounds',
 4:  'the vector runtime: graph, arena, persistor, memory budget',
 5:  'the write path: INSERT, UPDATE and the aux sub-transaction',
 6:  'build the index: ADD INDEX, ddl::Builder, bulk aux load',
 7:  'the read path: kNN search and the optimizer',
 8:  'ALTER and DDL policy for a vector-indexed table',
 9:  'error reporting: corruption, dangling nodes, build failures',
 10: 'design document for HNSW aux storage',
}

def norm(f):
    if f.startswith('include/') and not f.startswith('include/my_'):
        return ROOT + f
    return f if f.startswith(TOP) else ROOT + f

def load_map(path):
    sym, whole = collections.defaultdict(dict), {}
    for line in open(path):
        line = line.split('#')[0].strip()
        if not line:
            continue
        p = line.split()
        if len(p) >= 3 and p[1] == 'FILE':
            whole[norm(p[2])] = int(p[0])
        elif len(p) >= 3 and p[0].isdigit():
            sym[norm(p[1])][p[2].split('::')[-1]] = int(p[0])
    return sym, whole

def show(path):
    r = subprocess.run(['git', 'show', '%s:%s' % (ORACLE, path)],
                       capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None

CMAKE = ('storage/innobase/CMakeLists.txt', 'unittest/gunit/innodb/CMakeLists.txt')

def cmake_at(n, path, sym, whole):
    """A build file may only list sources that exist yet. Drop any line naming
    a source whose own category is later than n."""
    src = show(path)
    if src is None:
        return None
    out = []
    for line in src.split('\n'):
        m = re.search(r'([\w/]+\.cc)', line)
        if m:
            cand = m.group(1)
            for key in list(whole) + list(sym):
                if key.endswith('/' + cand.split('/')[-1]):
                    cat = whole.get(key) or min(sym[key].values())
                    if cat > n:
                        break
            else:
                out.append(line)
                continue
            continue
        out.append(line)
    return '\n'.join(out)

def prune_header(n, path, sym):
    """A header that must exist before its declarations do: emit it with the
    later-category declarations removed."""
    src = show(path)
    if src is None:
        return None
    tmp = '/tmp/_v3hdr.h'
    open(tmp, 'w').write(src)
    lines = src.split('\n')
    drop = set()
    for name, a, b in ranges(tmp):
        cat = sym.get(path, {}).get(name.split('::')[-1])
        if cat is not None and cat > n:
            drop.update(range(a - 1, b))
    return '\n'.join(l for i, l in enumerate(lines) if i not in drop)

def content_at(n, path, sym, whole):
    if path.endswith('.h') and path in sym:
        # never prune a header - see above
        return show(path) if min(sym[path].values()) <= n else None
    if path in CMAKE:
        return cmake_at(n, path, sym, whole) if whole.get(path, 99) <= n else None
    if path in whole:
        return show(path) if whole[path] <= n else None
    if path not in sym:
        return None
    src = show(path)
    if src is None:
        return None
    tmp = '/tmp/_v3gen.cc'
    open(tmp, 'w').write(src)
    lines = src.split('\n')
    drop = set()
    for name, a, b in ranges(tmp):
        cat = sym[path].get(name.split('::')[-1])
        if cat is not None and cat > n:
            drop.update(range(a - 1, b))
    if not any(c <= n for c in sym[path].values()):
        return None
    return '\n'.join(l for i, l in enumerate(lines) if i not in drop)

def header_floor(paths, sym, whole):
    """Earliest category at which each header must exist.

    A .cc file carries its final #include lines, so any header it names has
    to be present from that file's own category - even when the header's
    declarations belong to a later one. Without this the early categories
    fail with 'No such file or directory'."""
    # Only headers that do not exist in the base tree need forcing. An
    # upstream header is already there; emitting it early would bring its
    # FINAL content with it and break paired invariants - my_base.h bumping
    # HA_ERR_LAST while my_handler_errors.h still has the old string count.
    base_files = set(subprocess.run(
        ['git', 'ls-tree', '-r', '--name-only', 'ecb908769a5'],
        capture_output=True, text=True).stdout.split())
    floor = {}
    for p in paths:
        cat = whole.get(p) or (min(sym[p].values()) if p in sym else None)
        if cat is None or not p.endswith(('.cc', '.h')):
            continue
        src = show(p) or ''
        for m in re.finditer(r'#include\s+"([\w0-9]+\.h)"', src):
            for q in paths:
                if q.endswith('/' + m.group(1)) and q not in base_files:
                    floor[q] = min(floor.get(q, 99), cat)
    # Transitive closure: a header dragged forward brings its own includes
    # with it, or it fails with "No such file or directory".
    changed = True
    while changed:
        changed = False
        for h, c in list(floor.items()):
            src = show(h) or ''
            for m in re.finditer(r'#include\s+"([\w0-9]+\.h)"', src):
                for q in paths:
                    if q.endswith('/' + m.group(1)) and q not in base_files \
                       and floor.get(q, 99) > c:
                        floor[q] = c
                        changed = True
    return floor

def main(mapfile, wt):
    sym, whole = load_map(mapfile)
    paths = sorted(set(whole) | set(sym))
    FLOOR = header_floor(paths, sym, whole)
    os.chdir(wt)
    for n in range(1, 11):
        for p in paths:
            c = content_at(n, p, sym, whole)
            if c is None and p.endswith('.h') and FLOOR.get(p, 99) <= n:
                c = show(p)                            # whole header, unpruned
            if c is None:
                continue
            d = os.path.dirname(p)
            if d:
                os.makedirs(d, exist_ok=True)
            open(p, 'w').write(c)
        subprocess.run(['git', 'add', '-A'], check=True)
        if subprocess.run(['git', 'diff', '--cached', '--quiet']).returncode == 0:
            print('  [%2d] nothing to commit' % n)
            continue
        # R9: format the staged patch with the hook's own tool, which also
        # stages the fix, so the pre-commit gate passes on its own.
        subprocess.run([os.path.expanduser('~/scripts/githooks/apply-format'),
                        '--apply-to-staged'], capture_output=True)
        subprocess.run(['git', 'add', '-A'], check=True)
        msg = 'PS-11299: [%d] %s' % (n, TITLES[n])
        subprocess.run(['git', 'commit', '-q', '-m', msg], check=True)
        st = subprocess.run(['git', 'show', '--stat', '--format=', 'HEAD'],
                            capture_output=True, text=True).stdout.strip().split('\n')[-1]
        print('  [%2d] %-58s %s' % (n, TITLES[n][:58], st.strip()))

if __name__ == '__main__':
    main(os.path.abspath(sys.argv[1]), sys.argv[2])
