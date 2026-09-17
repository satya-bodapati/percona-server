#!/usr/bin/env python3
"""Filter a unified diff down to selected hunks, per file.

Usage:  hunkpick.py <patch> <file>=<spec> [<file>=<spec> ...]

<spec> is a comma-separated list of 1-based hunk numbers (as counted within
that file's diff), or "all", or "none". Files not named are dropped entirely.
The filtered patch goes to stdout with corrected hunk headers, so it applies
with plain `git apply`.
"""
import sys, re

def parse(patch):
    """Split a patch into [(path, header_lines, [hunk_lines...]), ...]."""
    files, cur = [], None
    for line in patch.splitlines(keepends=True):
        if line.startswith('diff --git '):
            cur = {'hdr': [line], 'hunks': [], 'path': line.split(' b/')[-1].rstrip('\n')}
            files.append(cur)
        elif cur is None:
            continue
        elif line.startswith('@@'):
            cur['hunks'].append([line])
        elif cur['hunks']:
            cur['hunks'][-1].append(line)
        else:
            cur['hdr'].append(line)
    return files

def renumber(hunk, delta):
    """Rewrite @@ -a,b +c,d @@ so the new-side start accounts for dropped hunks."""
    m = re.match(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(.*)$', hunk[0])
    o_s, o_c = int(m.group(1)), int(m.group(2) or 1)
    n_c = int(m.group(4) or 1)
    tail = m.group(5)
    hunk[0] = '@@ -%d,%d +%d,%d @@%s\n' % (o_s, o_c, o_s + delta, n_c, tail)
    return n_c - o_c

def main():
    patch = open(sys.argv[1]).read()
    want = {}
    for arg in sys.argv[2:]:
        f, _, spec = arg.partition('=')
        want[f] = spec
    out = []
    for fd in parse(patch):
        spec = want.get(fd['path'])
        if spec is None or spec == 'none':
            continue
        if spec == 'all':
            keep = set(range(1, len(fd['hunks']) + 1))
        else:
            keep = {int(x) for x in spec.split(',') if x.strip()}
        bad = keep - set(range(1, len(fd['hunks']) + 1))
        if bad:
            sys.exit('%s: no such hunk %s (file has %d)' %
                     (fd['path'], sorted(bad), len(fd['hunks'])))
        sel = [h for i, h in enumerate(fd['hunks'], 1) if i in keep]
        if not sel:
            continue
        out.extend(fd['hdr'])
        delta = 0
        for h in sel:
            delta += renumber(h, delta)
            out.extend(h)
    sys.stdout.write(''.join(out))

main()
