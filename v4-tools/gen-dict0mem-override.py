#!/usr/bin/env python3
"""Write commit 1's intermediate dict0mem.h.

Commit 1 owns the aux-table flags and the is_aux / is_vec_aux predicates.
Everything else the branch adds to this header belongs later: the hidden
column's fields, the index runtime pointer, and the persistent-metadata type.
That last one matters most - the enum must not grow before the persister that
handles the new type is registered, or the base write loop walks onto a null
persister and the server segfaults during bootstrap.
"""
import subprocess, os

ORACLE, BASE = 'vector-persistence-v2', 'ecb908769a5'
PATH = 'storage/innobase/include/dict0mem.h'
OUT  = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    'overrides/1', PATH)

def show(rev):
    return subprocess.run(['git', 'show', '%s:%s' % (rev, PATH)],
                          capture_output=True, text=True).stdout

TOK = ('vec_aux_autoinc_next_id', 'vec_aux_autoinc_persisted', 'vec_aux_col',
       'Vec_runtime', 'PM_TABLE_VEC_IDX_ID', 'set_vec_next_id_if_bigger',
       'get_vec_next_id', 'VecIdxIdPersister')

def main():
    lines = show(ORACLE).split('\n')
    out, i = [], 0
    while i < len(lines):
        l = lines[i]
        if 'PM_BIGGEST_TYPE' in l:
            out.append('  PM_BIGGEST_TYPE = 3')            # the base tree's value
            i += 1; continue
        if 'PersistentTableMetadata(table_id_t id' in l:   # restore the base ctor
            out.append(l)
            out.append('      : m_id(id), m_version(version), m_corrupted_ids(), '
                       'm_autoinc(0) {}')
            while i < len(lines) and '{}' not in lines[i]:
                i += 1
            i += 1; continue
        if any(t in l for t in TOK):
            depth = l.count('{') - l.count('}')
            j = i
            while depth > 0 and j + 1 < len(lines):
                j += 1; depth += lines[j].count('{') - lines[j].count('}')
            while out and (out[-1].lstrip().startswith(('/**', '/*', '*', '//'))
                           or not out[-1].strip()):
                out.pop()
            i = j + 1; continue
        out.append(l); i += 1
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, 'w').write('\n'.join(out))
    src = '\n'.join(out)
    assert src.count('/*') == src.count('*/'), 'comments unbalanced'
    for t in TOK:
        assert t not in src, 'leftover: ' + t
    print('  wrote %s  (%d lines, oracle has %d)'
          % (OUT.split('overrides/')[-1], len(out), len(lines)))

main()
