#!/usr/bin/env python3
# cat7_recs.py <cat7_X.sum> <names_X.sum> — decode section-A channel records.
import re, sys
names = {}
for l in open(sys.argv[2]).readlines()[1:]:
    m = re.match(r"req=(..)(..) +err=0 size=\d+ +'(.*)'$", l.strip())
    if m and m.group(2) == '00': names[int(m.group(1), 16)] = m.group(3)
sec = open(sys.argv[1]).read().split('## B')[0]
for l in sec.split('\n'):
    m = re.match(r'\s+(0x00700[23]) req=(0[0-2])0000004000 .*recs: (.*)', l)
    if not m: continue
    recs = [bytes.fromhex(r.strip()) for r in m.group(3).split('|')]
    f = lambda r: (r[0], r[1], int.from_bytes(r[2:4],'little'), int.from_bytes(r[4:6],'little'),
                   int.from_bytes(r[6:8],'little'), int.from_bytes(r[8:12],'little'), int.from_bytes(r[12:16],'little'))
    F = [f(r) for r in recs]
    odd = [x for x in F if x[0] or x[2] != x[3] or x[5]]
    # group runs of same class/type
    runs, cur = [], None
    for x in F:
        key = (x[4], x[6])
        if cur and cur[0] == key and x[2] == cur[2] + 1 and x[1] == cur[4] + 1:
            cur[2] = x[2]; cur[4] = x[1]
        else:
            cur = [key, x[2], x[2], x[1], x[1]]; runs.append(cur)
    desc = '; '.join(f"idx{r[3]}-{r[4]} {names.get(r[1],hex(r[1]))}..{names.get(r[2],hex(r[2]))} class={names.get(r[0][0],hex(r[0][0]))} type={r[0][1]}" for r in runs)
    print(f"{m.group(1)} band {m.group(2)}: {len(F)} ch; anomalies={len(odd)} :: {desc}")
