#!/usr/bin/env python3
"""Materialise the v4 branch: 12 commits, each a slice of v2's final tree.

Phase A is a pure re-split. Every line is v2's own, so the tip must come out
byte-identical to the oracle - that is the check, run after every build.

For each step N the builder writes each mapped file at its step-N content:
symbols assigned to a later step are dropped, everything else is the oracle's
text. Where a function's final form cannot work at its own step - because it
also carries a later step's change - an override supplies the intermediate
form. Overrides live in overrides/<step>/<file>/<symbol>.txt so every
deviation is in one place; the final step always restores the oracle's text,
which is what keeps the tip identical.
"""
import sys, os, re, subprocess, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symrange import ranges

ORACLE = 'vector-persistence-v2'
BASE   = 'ecb908769a5'
ROOT   = 'storage/innobase/'
TOP    = ('sql/', 'unittest/', 'include/my_', 'mysys/', 'vector-common/',
          'storage/', 'share/', 'vec-hnsw', 'mysql-test/')
NSTEPS = 11

TITLES = {
 1:  'vector aux tables: create, drop, rename, truncate',
 2:  'the hidden label column, and its counter in the DD',
 3:  'read an aux table back: vec_aux_verify and vec_next_id',
 4:  'the write path: aux tables on DDL, the graph, the sub-transaction',
 5:  'search',
 6:  'load failures: corrupt graphs and transient refusals',
 7:  'arena accounting and innodb_hnsw_max_memory',
 8:  'ALTER: build through ddl::Builder, and survive a rebuild',
 9:  'ALTER: refuse INSTANT, and the combinations',
 10: 'refuse IMPORT and EXPORT',
 11: 'design document for HNSW aux storage',
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
            sym[norm(p[1])][p[2]] = int(p[0])
    return sym, whole

def show(path, rev=ORACLE):
    r = subprocess.run(['git', 'show', '%s:%s' % (rev, path)],
                       capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None

CMAKE = ('storage/innobase/CMakeLists.txt', 'unittest/gunit/innodb/CMakeLists.txt')

BASE_FILES = None

def base_tree():
    global BASE_FILES
    if BASE_FILES is None:
        BASE_FILES = set(subprocess.run(
            ['git', 'ls-tree', '-r', '--name-only', BASE],
            capture_output=True, text=True).stdout.split())
    return BASE_FILES

def cmake_at(n, path, sym, whole):
    """A build file may only name sources that exist yet - but only sources the
    branch introduces can be missing. Withholding one the base tree already
    builds just drops it from the link."""
    src = show(path)
    if src is None:
        return None
    out = []
    for line in src.split('\n'):
        m = re.search(r'([\w/]+\.cc)', line)
        if m and not any(b.endswith('/' + m.group(1).split('/')[-1])
                         for b in base_tree()):
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

OVERRIDE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'overrides')

def file_override(n, path):
    """A whole-file intermediate form at step n, if any.

    Used for tests: a test whose later sections need a feature that has not
    arrived yet lands here in a shortened form, and the step that supplies
    the feature restores the oracle's full file. The tip is unaffected,
    because the last word is always the oracle's."""
    best = None
    for s in range(1, n + 1):
        p = os.path.join(OVERRIDE_DIR, str(s), path)
        if os.path.isfile(p):
            best = p
    return open(best).read() if best else None

def override(n, path, name):
    """The hand-written intermediate form of one symbol at step n, if any.

    Looks for the newest override at or before step n, so a form introduced
    at step 2 stays in force until another step replaces it or the symbol's
    own step arrives and the oracle's text takes over."""
    best = None
    for s in range(1, n + 1):
        p = os.path.join(OVERRIDE_DIR, str(s), path, name + '.txt')
        if os.path.exists(p):
            best = p
    return open(best).read().rstrip('\n') if best else None

def base_symbols(path):
    """name -> the base tree's text for that symbol, for every symbol the base
    file already has. A symbol assigned to a later step must fall back to this
    text, not vanish: dropping it would delete upstream code."""
    src = show(path, BASE)
    if src is None:
        return {}
    tmp = '/tmp/_v4base' + os.path.splitext(path)[1]
    open(tmp, 'w').write(src)
    lines = src.split('\n')
    out = {}
    for name, a, b in ranges(tmp):
        text = '\n'.join(lines[a - 1:b])
        out[name] = text
        short = name.split('::')[-1]
        out[short] = None if short in out and out[short] != text else text
    return {k: v for k, v in out.items() if v is not None}

def content_at(n, path, sym, whole):
    src = show(path)
    if src is None:
        return None
    if path in CMAKE:
        return cmake_at(n, path, sym, whole) if whole.get(path, 99) <= n else None
    if path in whole and path not in sym:
        if whole[path] <= n:
            return src
        return file_override(n, path)      # a shortened form, if one exists
    if path not in sym:
        return None
    if not any(c <= n for c in sym[path].values()) and whole.get(path, 99) > n:
        return None

    tmp = '/tmp/_v4gen' + os.path.splitext(path)[1]
    open(tmp, 'w').write(src)
    lines = src.split('\n')
    based = base_symbols(path)
    out, drop = [], {}
    for name, a, b in ranges(tmp):
        short = name.split('::')[-1]
        cat = sym[path].get(name, sym[path].get(short))
        ov  = override(n, path, name) or override(n, path, short)
        if ov is not None and (cat is None or cat > n):
            drop[(a, b)] = ov            # hand-written intermediate form
        elif cat is not None and cat > n:
            # not this step's yet: keep what the base tree had, or drop the
            # symbol entirely if the branch is what introduced it
            drop[(a, b)] = based.get(name, based.get(short))
    skip = set()
    for (a, b), _ in drop.items():
        skip.update(range(a - 1, b))
    for i, l in enumerate(lines):
        rep = next((v for (a, b), v in drop.items() if i == a - 1), '__none__')
        if rep != '__none__':
            if rep is not None:
                out.append(rep)
            continue
        if i not in skip:
            out.append(l)
    return '\n'.join(out)

def header_floor(paths, sym, whole):
    """A .cc carries its final #include lines, so a header it names has to
    exist from that file's own step - even if its declarations belong later.
    Only headers absent from the base tree need forcing."""
    base_files = set(subprocess.run(['git', 'ls-tree', '-r', '--name-only', BASE],
                                    capture_output=True, text=True).stdout.split())
    floor = {}
    for p in paths:
        cat = whole.get(p) or (min(sym[p].values()) if p in sym else None)
        if cat is None or not p.endswith(('.cc', '.h')):
            continue
        for m in re.finditer(r'#include\s+"([\w0-9]+\.h)"', show(p) or ''):
            for q in paths:
                if q.endswith('/' + m.group(1)) and q not in base_files:
                    floor[q] = min(floor.get(q, 99), cat)
    changed = True
    while changed:
        changed = False
        for h, c in list(floor.items()):
            for m in re.finditer(r'#include\s+"([\w0-9]+\.h)"', show(h) or ''):
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
    for n in range(1, NSTEPS + 1):
        for p in paths:
            c = content_at(n, p, sym, whole)
            if c is None and p.endswith('.h') and FLOOR.get(p, 99) <= n:
                c = show(p)
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
        subprocess.run([os.path.expanduser('~/scripts/githooks/apply-format'),
                        '--apply-to-staged'], capture_output=True)
        subprocess.run(['git', 'add', '-A'], check=True)
        msg = 'PS-11299: [%d/%d] %s' % (n, NSTEPS, TITLES[n])
        subprocess.run(['git', 'commit', '-q', '-m', msg], check=True)
        st = subprocess.run(['git', 'show', '--stat', '--format=', 'HEAD'],
                            capture_output=True, text=True).stdout.strip().split('\n')[-1]
        print('  [%2d] %-52s %s' % (n, TITLES[n][:52], st.strip()))

if __name__ == '__main__':
    main(os.path.abspath(sys.argv[1]), sys.argv[2])
