#!/usr/bin/env python3
# names_sum.py <names_X.log> — pair each 0x005000 command with its FCPr trace line.
import re, sys
lines = open(sys.argv[1]).read().split('\n')
cmds, i = [], 0
while i < len(lines):
    l = lines[i]
    m = re.match(r'op=0x005000 req\[(\d+)\]=(\S*)(.*)', l)
    if m:
        c = {'req': m.group(2), 'failed': 'FAILED' in m.group(3), 'hex': []}
        if not c['failed']:
            j = i + 2
            while j < len(lines) and lines[j].startswith('    '):
                c['hex'] += lines[j].split(); j += 1
        cmds.append(c)
    i += 1
tr = [l for l in lines if 'FCPr op=0x005000' in l]
for c, t in zip(cmds, tr):
    m = re.search(r'err=(\d+) size=(\d+)', t)
    c['err'], c['size'] = (int(m.group(1)), int(m.group(2))) if m else ('?', '?')
print(f"{len(cmds)} commands, {len(tr)} traced")
for c in cmds:
    b = bytes(int(x, 16) for x in c['hex'])
    name = b.split(b'\0')[0].decode('latin1')
    nul = b.find(b'\0')
    tail = b[nul+1:] if nul >= 0 else b''
    extra = tail.rstrip(b'\0')
    print(f"req={c['req']:<16} err={c.get('err')} size={c.get('size'):<3} "
          + ("FAILED" if c['failed'] else f"{name!r}" + (f" TAIL={extra.hex()}" if extra else "")))
