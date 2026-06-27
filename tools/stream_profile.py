#!/usr/bin/env python3
"""stream_profile.py — pull the data-plane stream-engine register traffic out of a
QEMU vfio_region_* trace, in chronological order.

The control-plane decoders (fcp_decode.py) ignore the DMA/stream block; this one does
the opposite: it shows only the registers that arm and drive the audio engine, so a
streaming capture can pin the per-block geometry (channel count + fragment size) for a
model whose stream_frag is not yet known (e.g. the 2Pre, asymmetric TX4/RX14).

Watched offsets (8PreX reference values in parens):
  0x108 stream IRQ cfg   (0x10)        0x200/0x300  block0/1 cause (read-to-clear)
  0x10c stream IRQ cfg2  (0x1e70700)   0xN04        +CHANS  (0x1c = 28)
  0x110 stream IRQ arm   (0x7 -> 0x0)  0xN08        +SIZE   (0x1c0)  <- the target
                                       0xN0c        +CTRL   (1 = enable)
                                       0xN10/0xN14  +BASE lo/hi
                                       0xN18        +PTR    (DMA position, read-only)

Usage: python3 tools/stream_profile.py STREAM.log [--summary]
  (default: chronological event log; --summary: first/last value per offset)
"""
import re
import sys

RE_WRITE = re.compile(
    r'(\S+)\s+vfio_region_write\s+\([^)]*region(\d+)\+0x([0-9a-fA-F]+),\s*0x([0-9a-fA-F]+),\s*(\d+)\)')
RE_READ = re.compile(
    r'(\S+)\s+vfio_region_read\s+\([^)]*region(\d+)\+0x([0-9a-fA-F]+),\s*(\d+)\)\s*=\s*0x([0-9a-fA-F]+)')

OFF_NAME = {
    0x108: "irqcfg", 0x10c: "irqcfg2", 0x110: "irqarm",
    0x100: "vec0cause", 0x104: "irqen",
}
SUB = {0x00: "cause", 0x04: "CHANS", 0x08: "SIZE", 0x0c: "CTRL",
       0x10: "BASElo", 0x14: "BASEhi", 0x18: "PTR"}


def label(off):
    if off in OFF_NAME:
        return OFF_NAME[off]
    for blk, tag in ((0x200, "blk0"), (0x300, "blk1")):
        if blk <= off <= blk + 0x1f:
            return f"{tag}.{SUB.get(off - blk, hex(off - blk))}"
    return None


def watched(off):
    return label(off) is not None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    summary = "--summary" in sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(1)

    events = []   # (ts, kind, off, val, width)
    with open(args[0]) as f:
        for line in f:
            m = RE_WRITE.search(line)
            if m and m.group(2) == "0":
                off = int(m.group(3), 16)
                if watched(off):
                    events.append((m.group(1), "W", off, int(m.group(4), 16), int(m.group(5))))
                continue
            m = RE_READ.search(line)
            if m and m.group(2) == "0":
                off = int(m.group(3), 16)
                if watched(off):
                    events.append((m.group(1), "R", off, int(m.group(5), 16), int(m.group(4))))

    if not events:
        print("no stream-engine register traffic found")
        return

    if summary:
        seen = {}
        for ts, kind, off, val, w in events:
            d = seen.setdefault(off, {"label": label(off), "W": [], "R": []})
            d[kind].append(val)
        for off in sorted(seen):
            d = seen[off]
            ws = d["W"]
            rs = d["R"]
            parts = []
            if ws:
                parts.append(f"W[{len(ws)}] first=0x{ws[0]:x} last=0x{ws[-1]:x} uniq={sorted(set(hex(x) for x in ws))[:6]}")
            if rs:
                parts.append(f"R[{len(rs)}] first=0x{rs[0]:x} last=0x{rs[-1]:x}")
            print(f"0x{off:04x} {d['label']:<14} {' | '.join(parts)}")
        return

    for ts, kind, off, val, w in events:
        print(f"{ts}  {kind} 0x{off:04x} {label(off):<14} = 0x{val:08x} ({w}b)")


if __name__ == "__main__":
    main()
