#!/usr/bin/env python3
"""Strip `#if defined(TARGET_PSP)` blocks from a z2442 source file.

Used to separate the reference port's PSP-specific edits from plain decomp
drift: run this over a z2442 file and diff the result against reference/oot.
Whatever is left is upstream drift, not a port fix.
"""
import re, sys

IF = re.compile(r'^\s*#\s*if(n?def)?\b(.*)$')
ELIF = re.compile(r'^\s*#\s*elif\b')
ELSE = re.compile(r'^\s*#\s*else\b')
ENDIF = re.compile(r'^\s*#\s*endif\b')


def mentions_psp(expr):
    return 'TARGET_PSP' in expr


def strip(lines):
    out, stack = [], []          # stack: (is_psp_cond, keep_state)
    for line in lines:
        m = IF.match(line)
        if m:
            neg = m.group(1) == 'ndef'
            expr = m.group(2)
            if mentions_psp(expr) and not stack_suppressed(stack):
                # decide which branch survives on a non-PSP build
                positive = ('!' in expr.split('TARGET_PSP')[0][-2:]) or neg
                stack.append(('psp', positive))
                continue
            stack.append(('plain', True))
            out.append(line)
            continue
        if ELSE.match(line) or ELIF.match(line):
            if stack and stack[-1][0] == 'psp':
                stack[-1] = ('psp', not stack[-1][1])
                continue
            out.append(line)
            continue
        if ENDIF.match(line):
            if stack and stack.pop()[0] == 'psp':
                continue
            out.append(line)
            continue
        if all(k for t, k in stack if t == 'psp'):
            out.append(line)
    return out


def stack_suppressed(stack):
    return not all(k for t, k in stack if t == 'psp')


if __name__ == '__main__':
    src = open(sys.argv[1], encoding='utf8', errors='replace').read().splitlines(True)
    sys.stdout.writelines(strip(src))
