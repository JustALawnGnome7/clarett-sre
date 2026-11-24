#!/usr/bin/env python3
# cat7_sum.py <cat7_X.log> — each command with its true reply (trimmed to size), section headers kept.
import re, sys
L = open(sys.argv[1]).read().split('\n')
tr = [l for l in L if re.search(r'FCPr op=0x00700', l)]
k, i = 0, 0
while i < len(L):
    l = L[i]
    if l.startswith('##') or (l.startswith('# card')):
        print(l)
    m = re.match(r'op=(0x\w+) req\[\d+\]=(\S*)(.*)', l)
    if m:
        hexb = []
        j = i + 2
        while j < len(L) and L[j].startswith('    '):
            hexb += L[j].split(); j += 1
        t = tr[k] if k < len(tr) else ''
        k += 1
        mm = re.search(r'err=(\d+) size=(\d+)', t)
        err, size = (int(mm.group(1)), int(mm.group(2))) if mm else ('?', 0)
        if 'FAILED' in m.group(3):
            body = 'FAILED'
        else:
            b = hexb[:size]
            if m.group(1) in ('0x007002', '0x007003'):
                recs = [b[x:x+16] for x in range(0, len(b), 16)]
                body = f'{len(recs)} recs: ' + ' | '.join(''.join(r) for r in recs)
            else:
                ws = [int(''.join(reversed(b[x:x+4])), 16) for x in range(0, len(b) - 3, 4)]
                body = ' '.join(b) + (f'  u32={ws}' if ws else '')
        print(f'  {m.group(1)} req={m.group(2):<18} err={err} size={size:<4} {body}')
    i += 1
print(f'# {k} commands, {len(tr)} traced')
