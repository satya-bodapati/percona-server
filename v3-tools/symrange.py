#!/usr/bin/env python3
"""Exact top-level symbol ranges for a C/C++ file.

Brace counting only works once comments and literals are gone - a '{' inside
a comment is what made the earlier version run one function's body into the
next. Strip first, then count.
"""
import re, sys

def strip_noncode(src):
    """Replace comment and literal bodies with spaces, preserving offsets."""
    out = list(src)
    i, n = 0, len(src)
    state = None
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ''
        if state is None:
            if c == '/' and nxt == '/':
                state = 'line'; out[i] = out[i + 1] = ' '; i += 2; continue
            if c == '/' and nxt == '*':
                state = 'block'; out[i] = out[i + 1] = ' '; i += 2; continue
            if c in '"\'':
                state = c; out[i] = ' '; i += 1; continue
            i += 1
        elif state == 'line':
            if c == '\n': state = None
            else: out[i] = ' '
            i += 1
        elif state == 'block':
            if c == '*' and nxt == '/':
                out[i] = out[i + 1] = ' '; state = None; i += 2; continue
            if c != '\n': out[i] = ' '
            i += 1
        else:
            if c == '\\':
                out[i] = ' '
                if i + 1 < n and src[i + 1] != '\n': out[i + 1] = ' '
                i += 2; continue
            if c == state: state = None
            out[i] = ' ' if c != '\n' else c
            i += 1
    return ''.join(out)

DEF = re.compile(r'^[A-Za-z_][\w:<>,*&\[\] ]*?\**([A-Za-z_]\w*(?:::[A-Za-z_]\w*)?)\s*\(')

def ranges(path):
    code = strip_noncode(open(path).read()).split('\n')
    out, i = [], 0
    while i < len(code):
        m = DEF.match(code[i])
        if m and not code[i].lstrip().startswith(('return', 'if', 'for', 'while', 'switch')):
            depth, started, j = 0, False, i
            while j < len(code):
                depth += code[j].count('{') - code[j].count('}')
                if '{' in code[j]: started = True
                if started and depth <= 0: break
                if not started and ';' in code[j]: break
                j += 1
            if started:
                out.append((m.group(1), i + 1, j + 1))
                i = j + 1
                continue
        i += 1
    return out

if __name__ == '__main__':
    for name, a, b in ranges(sys.argv[1]):
        print('%-40s %5d %5d  (%d lines)' % (name, a, b, b - a + 1))
