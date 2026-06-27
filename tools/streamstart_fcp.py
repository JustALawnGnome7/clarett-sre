#!/usr/bin/env python3
"""
streamstart_fcp.py — isolate the stream-start mode selector.

Given a vfio_region_* trace that captures the idle->streaming transition, this finds
each stream-engine ARM (write of 0x7 to 0x110, or the 0x210/0x310 base writes) and
lists every FCP mailbox command issued in the window BEFORE the first arm, EXCLUDING
the GET_METER (0x001001) GUI-polling noise.

Rationale (clarett-8prex-data-plane.md): the 8PreX arms with only GET_METER beforehand
(descriptor mode == hardware default). So on the 2Pre, any NON-meter FCP before the arm
is the candidate flat-mode switch. If this prints nothing but meter noise, the selector
is a guest-RAM seed, not an FCP -> pivot to pmemsave-before-arm.

Usage:  ./tools/streamstart_fcp.py 2pre_streamstart.log
        ./tools/streamstart_fcp.py 2pre_streamstart.log --window 400   # lines before arm
"""
import argparse, re, sys

WR = re.compile(r'region0\+0x([0-9a-fA-F]+),\s*0x([0-9a-fA-F]+),\s*\d+\)')
RD = re.compile(r'region0\+0x([0-9a-fA-F]+),\s*\d+\)\s*=\s*0x([0-9a-fA-F]+)')

MBOX = 0x8020
DOORBELL = 0x408
METER = 0x001001

def parse(path):
    """Return list of (lineno, kind, off, val) for region0 accesses."""
    out = []
    for ln, line in enumerate(open(path, errors='replace')):
        if 'region0+' not in line:
            continue
        if 'vfio_region_write' in line:
            m = WR.search(line)
            if m: out.append((ln, 'w', int(m.group(1),16), int(m.group(2),16)))
        elif 'vfio_region_read' in line:
            m = RD.search(line)
            if m: out.append((ln, 'r', int(m.group(1),16), int(m.group(2),16)))
    return out

def find_arms(ev):
    """Stream arms: write 0x7 to 0x110, or a write to 0x210/0x310 base."""
    arms = []
    for ln, k, off, val in ev:
        if k == 'w' and ((off == 0x110 and val == 0x7) or off in (0x210, 0x310)):
            arms.append(ln)
    return arms

def fcp_cmds(ev):
    """Reconstruct (lineno_of_cmd_write, opcode, payload_words) per mailbox submit."""
    cur = {}
    cmds = []
    for ln, k, off, val in ev:
        if k != 'w':
            continue
        if MBOX <= off < MBOX + 0x410:
            cur[off - MBOX] = (ln, val)
        elif off == DOORBELL and val == 1:
            cmd = cur.get(0x00, (ln, 0))[1]
            op = cmd & 0x00ffffff
            cmdln = cur.get(0x00, (ln, 0))[0]
            data = [cur[o][1] for o in sorted(cur) if o >= 0x10]
            cmds.append((cmdln, op, data))
            cur = {}
    return cmds

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--window', type=int, default=0,
                    help='only show FCP within N lines before the first arm (0 = all before)')
    a = ap.parse_args()
    ev = parse(a.log)
    arms = find_arms(ev)
    cmds = fcp_cmds(ev)
    if not arms:
        print("NO stream arm found in this capture — did streaming actually (re)start?")
        return
    first = arms[0]
    print(f"total region0 events: {len(ev)}")
    print(f"stream arms at lines: {arms[:12]}{' ...' if len(arms)>12 else ''}")
    print(f"first arm at line {first}\n")
    lo = first - a.window if a.window else 0
    interesting = [c for c in cmds if lo <= c[0] < first and c[1] != METER]
    print(f"=== NON-meter FCP before first arm (lines {lo}..{first}) ===")
    if not interesting:
        print("  (none — only GET_METER noise. Selector is likely a guest-RAM seed, not FCP.)")
    for cmdln, op, data in interesting:
        ds = ' '.join('%08x' % d for d in data[:8])
        print(f"  line {cmdln:7d}  op=0x{op:06x}  data=[{ds}]")
    # also summarize opcode counts in the whole pre-arm window for context
    from collections import Counter
    pre = Counter(c[1] for c in cmds if lo <= c[0] < first)
    print(f"\n  opcode counts in window: " +
          ', '.join(f'0x{op:06x}:{n}' for op, n in pre.most_common()))

if __name__ == '__main__':
    main()
