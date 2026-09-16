#!/usr/bin/env python3
"""Validate the v3 assignment map against the oracle, before any commit exists.

  check-map.py <base> <oracle>

Checks:
  C1 coverage      - every changed non-test file is assigned or marked SPLIT
  C2 forward refs  - ADVISORY hint only; the per-commit build is the authority
"""
import re, subprocess, sys, collections

BASE, ORACLE = sys.argv[1], sys.argv[2]
ROOT = 'storage/innobase/'
TOP = ('sql/', 'unittest/', 'include/', 'mysys/', 'vector-common/', 'storage/',
       'share/', 'vec-hnsw')

def norm(f):
    if f.startswith('include/') and not f.startswith('include/my_'):
        return ROOT + f
    return f if f.startswith(TOP) else ROOT + f

syms, assigned, split = [], set(), set()
for line in open('v3-tools/map.txt'):
    line = line.split('#')[0].strip()
    if not line:
        continue
    p = line.split()
    if p[0] == 'SPLIT':
        split.add(norm(p[1]))
    elif len(p) >= 3 and p[1] == 'FILE':
        assigned.add(norm(p[2]))
    elif len(p) >= 3:
        assigned.add(norm(p[1]))
        syms.append((int(p[0]), p[1], p[2]))

rc = 0

files = subprocess.run(['git', 'diff', '--name-only', BASE, ORACLE, '--', '.',
                        ':!mysql-test'], capture_output=True, text=True).stdout.split()
missing = [f for f in files if f not in assigned and f not in split]
print('C1 coverage: %d changed, %d assigned, %d split-pending, %d uncovered'
      % (len(files), sum(f in assigned for f in files),
         sum(f in split for f in files), len(missing)))
for f in missing:
    print('     UNCOVERED', f)
    rc = 1

# C2 is a cheap pre-filter, not an authority. Restrict it to our own
# distinctively-named symbols: generic upstream names (close, open,
# create_table) match everywhere and the crude body extraction below
# cannot tell one function's body from the next reliably. The compiler,
# via the per-commit build, is what actually proves the ordering.
DISTINCT = re.compile(r'^(vec_|Vec_|dd_vec|dd_add_vec|dd_set_vec|dd_get_vec|dict_table_vec)')
sym2cat = {s.split('::')[-1]: c for c, _, s in syms
           if DISTINCT.match(s.split('::')[-1])}
byfile = collections.defaultdict(list)
for c, f, s in syms:
    byfile[f].append((c, s))

def bodies(path, names):
    try:
        src = open(norm(path)).read().split('\n')
    except OSError:
        return {}
    out = {}
    for i, l in enumerate(src):
        m = re.match(r'^[A-Za-z_][\w:<>,* &]*?\**([A-Za-z_]\w*)\(', l)
        if not m or m.group(1) not in names:
            continue
        d, started, j = 0, False, i
        while j < len(src):
            d += src[j].count('{') - src[j].count('}')
            if '{' in src[j]:
                started = True
            if started and d <= 0:
                break
            j += 1
        out.setdefault(m.group(1), []).append('\n'.join(src[i:j + 1]))
    return out

viol = set()
for f, lst in byfile.items():
    bs = bodies(f, {s.split('::')[-1] for _, s in lst})
    for cat, sym in lst:
        short = sym.split('::')[-1]
        if not DISTINCT.match(short):
            continue
        for body in bs.get(short, []):
            for other, ocat in sym2cat.items():
                if other != short and ocat > cat and \
                   re.search(r'\b' + re.escape(other) + r'\s*\(', body):
                    viol.add((cat, short, ocat, other))
# ADVISORY ONLY - does not fail the check. The body extraction is textual
# brace counting, which a brace inside a comment or string defeats, so it
# reports references that do not exist. The per-commit build is what proves
# the ordering; this is only a hint about where to look first.
print('C2 forward refs (ADVISORY, textual - expect false positives): %d' % len(viol))
for v in sorted(viol)[:10]:
    print('     cat %d %s -> cat %d %s' % v)

print('RESULT:', 'PASS' if rc == 0 else 'FAIL')
sys.exit(rc)
