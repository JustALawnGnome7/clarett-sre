#!/usr/bin/env python3
"""Correlate 0x400 notification-cause transitions with the mailbox commands FC
issues around them. Tests the Apollo-derived hypothesis that notification bits
0x1/0x2/0x3 are a staged handshake with *distinct* per-bit follow-up reads,
rather than one blanket monitor re-read.

For each transition of the 0x400 cause register value we print:
  - the value change (old -> new)
  - the last mailbox command submitted before it
  - the next mailbox command submitted after it (opcode + offset/len if GET/SET)
"""
import re, sys

LINE = re.compile(r'region0\+0x([0-9a-f]+),\s*(?:0x([0-9a-f]+),\s*)?4\)?(?:\s*=\s*0x([0-9a-f]+))?')

OPNAMES = {
    0x800000: "GET_DATA", 0x800001: "SET_DATA", 0x800002: "DATA_CMD",
    0x800005: "READ_SEG", 0x001001: "GET_METER", 0x005000: "CONFIG_PUSH",
    0x000002: "INIT_2", 0x000000: "INIT_1", 0x000003: "REBOOT",
    0x006000: "GET_6.0", 0x006001: "GET_6.1", 0x006002: "GET_6.2",
    0x007000: "GET_7.0", 0x007001: "GET_7.1", 0x007002: "GET_7.2", 0x007003: "GET_7.3",
}

def opname(op):
    return OPNAMES.get(op & 0x7fffff, f"op=0x{op&0x7fffff:x}")

def parse(path):
    """Yield events in order: ('cmd', dict) for a submitted mailbox command,
    ('n', value) for a 0x400 read."""
    # mailbox staging registers
    stage = {}   # offset -> last written value
    for raw in open(path, errors='replace'):
        if 'vfio_region_write' in raw:
            m = re.search(r'region0\+0x([0-9a-f]+),\s*0x([0-9a-f]+),\s*4', raw)
            if not m:
                continue
            off = int(m.group(1), 16); val = int(m.group(2), 16)
            if 0x8020 <= off <= 0x80ff:
                stage[off] = val
            elif off == 0x408 and val == 1:
                # doorbell submit: snapshot the staged command
                cmd = stage.get(0x8020, 0)
                szseq = stage.get(0x8024, 0)
                seq = (szseq >> 16) & 0xffff
                arg0 = stage.get(0x8030, 0)   # offset for GET/SET_DATA
                arg1 = stage.get(0x8034, 0)   # len
                yield ('cmd', dict(op=cmd, seq=seq, arg0=arg0, arg1=arg1))
        elif 'vfio_region_read' in raw:
            m = re.search(r'region0\+0x([0-9a-f]+),\s*4\)\s*=\s*0x([0-9a-f]+)', raw)
            if not m:
                continue
            off = int(m.group(1), 16); val = int(m.group(2), 16)
            if off == 0x400:
                yield ('n', val)

def fmt_cmd(c):
    if c is None:
        return "(none)"
    name = opname(c['op'])
    if (c['op'] & 0x7fffff) in (0x800000, 0x800001):
        return f"{name} seq={c['seq']} off=0x{c['arg0']:x} len=0x{c['arg1']:x}"
    return f"{name} seq={c['seq']}"

def main():
    for path in sys.argv[1:]:
        print(f"\n===== {path} =====")
        events = list(parse(path))
        # per-value: how many transitions INTO it, and what command follows
        from collections import Counter, defaultdict
        into = Counter()
        follow = defaultdict(Counter)   # value -> Counter(next opcode name)
        last_cmd = None
        cur = None
        transitions = []
        pending_val = None
        for i, (kind, payload) in enumerate(events):
            if kind == 'cmd':
                if pending_val is not None:
                    # first command after a transition
                    follow[pending_val][fmt_cmd(payload).split(' seq=')[0]] += 1
                    transitions.append((pending_val_from, pending_val, last_cmd, payload))
                    pending_val = None
                last_cmd = payload
            else:  # notification read
                if payload != cur:
                    into[payload] += 1
                    pending_val = payload
                    pending_val_from = cur
                    cur = payload
        # summary
        print(f"total commands: {sum(1 for k,_ in events if k=='cmd')}, "
              f"0x400 reads: {sum(1 for k,_ in events if k=='n')}")
        print("0x400 value distribution of transitions INTO each value:")
        for v, n in sorted(into.items()):
            print(f"   ->0x{v:x}: {n:4d} transitions   next-cmd: "
                  + ", ".join(f"{name}×{c}" for name, c in follow[v].most_common(6)))
        # show the rare (non-0x3) transitions in detail
        print("Detail of non-steady (0x1/0x2/0x0) transitions:")
        shown = 0
        for frm, to, lc, nc in transitions:
            if to == 3:
                continue
            print(f"   0x{frm if frm is not None else 0:x}->0x{to:x}  "
                  f"after[{fmt_cmd(lc)}]  before[{fmt_cmd(nc)}]")
            shown += 1
            if shown >= 40:
                print("   ... (truncated)")
                break

if __name__ == '__main__':
    main()
