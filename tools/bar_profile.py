#!/usr/bin/env python3
"""bar_profile.py — profile Clarett 2Pre BAR0 register activity from QEMU vfio_region_* traces.

Buckets every region0 access by offset and flags offsets OUTSIDE the known control-plane register
map (spec/clarett-fcp-transport.md §8). The point: capture a PCM streaming session and see
which *new* registers light up — those are the data-plane DMA engine (ring base / size / pointer /
control). See spec/clarett-data-plane.md for the capture plan.

Audio samples move by bus-master DMA and never touch the BAR, so this shows the streaming *setup and
control*, not the sample flow — dump guest RAM at the ring base for the buffer layout.

Usage:
  ./tools/bar_profile.py streaming.log                # full table
  ./tools/bar_profile.py streaming.log --new-only      # only registers outside the control-plane map
  sudo tail -f .../Windows10-custom.log | ./tools/bar_profile.py -   # live; Ctrl-C prints the report
"""
import argparse
import re
import sys
from collections import Counter

RE_WRITE = re.compile(
    r'vfio_region_write\s+\([^)]*region(\d+)\+0x([0-9a-fA-F]+),\s*0x([0-9a-fA-F]+),\s*(\d+)\)')
RE_READ = re.compile(
    r'vfio_region_read\s+\([^)]*region(\d+)\+0x([0-9a-fA-F]+),\s*(\d+)\)\s*=\s*0x([0-9a-fA-F]+)')

# Known control-plane register map (start, end inclusive, label). Anything else = data-plane candidate.
KNOWN = [
    (0x000, 0x000, "caps/version"),
    (0x004, 0x008, "init status reads"),
    (0x010, 0x014, "device serial"),
    (0x100, 0x100, "vec0 cause: mailbox-done"),
    (0x104, 0x104, "IRQ enable mask"),
    (0x200, 0x200, "vec1 cause"),
    (0x300, 0x300, "vec2 cause"),
    (0x400, 0x400, "vec3 cause: notifications"),
    (0x408, 0x408, "doorbell"),
    (0x410, 0x417, "GET-response DMA addr lo/hi"),
    (0x500, 0x51f, "IRQ summary/mask block"),
    (0x8000, 0x801f, "fw-info header"),
    (0x8020, 0x842f, "FCP mailbox"),
]

DISTINCT_CAP = 64   # beyond this many distinct values, treat a register as "varying" (pointer/data)


def classify(off):
    for lo, hi, label in KNOWN:
        if lo <= off <= hi:
            return label
    return None          # None => NEW / data-plane candidate


def addr_like(v):
    """A bus-address-looking value: non-trivial and page-aligned."""
    return v >= 0x1000 and (v & 0xfff) == 0


class Reg:
    __slots__ = ("reads", "writes", "sizes", "wvals", "rvals", "varying")

    def __init__(self):
        self.reads = self.writes = 0
        self.sizes = set()
        self.wvals = Counter()
        self.rvals = Counter()
        self.varying = False

    def add(self, op, val, size):
        self.sizes.add(size)
        if op == "w":
            self.writes += 1
            if not self.varying:
                self.wvals[val] += 1
        else:
            self.reads += 1
            if not self.varying:
                self.rvals[val] += 1
        if not self.varying and len(self.wvals) + len(self.rvals) > DISTINCT_CAP:
            self.varying = True
            self.wvals.clear()
            self.rvals.clear()


def behaviour(r):
    """One-line description of how a register is used."""
    if r.varying:
        kind = "pointer/position?" if r.reads >= r.writes else "DMA-written data?"
        return f"varying ({r.reads}r {r.writes}w) — {kind}"
    def vlist(counter):
        return " ".join(f"0x{v:x}" + (f"(x{c})" if c > 1 else "")
                        for v, c in counter.most_common(4))
    parts = []
    if r.wvals:
        tag = " <addr-like>" if any(addr_like(v) for v in r.wvals) else ""
        parts.append(f"W[{vlist(r.wvals)}]{tag}")
    if r.rvals:
        parts.append(f"R[{vlist(r.rvals)}]")
    return "  ".join(parts) or "(no values)"


def guess_role(off, r):
    """Best guess at a NEW register's data-plane role, for the summary."""
    if r.varying:
        return "DMA position / pointer (read)" if r.reads >= r.writes else "DMA-written region"
    if r.writes and any(addr_like(v) for v in r.wvals):
        return "ring base address (low32) — check off+4 for high32"
    if r.writes and not r.reads:
        v = r.wvals.most_common(1)[0][0]
        return "control / enable (clear)" if v in (0, 1, 2, 3) else "size / period / config"
    if r.reads and not r.writes:
        return "status (read-only)"
    return "?"


def main():
    ap = argparse.ArgumentParser(description="Profile Clarett BAR0 register activity")
    ap.add_argument("file", help="trace file, or - for stdin")
    ap.add_argument("--new-only", action="store_true",
                    help="show only registers outside the control-plane map (data-plane candidates)")
    ap.add_argument("--min", type=int, default=1, metavar="N",
                    help="hide offsets with fewer than N accesses (default 1)")
    args = ap.parse_args()

    fh = sys.stdin if args.file == "-" else open(args.file)
    regs = {}
    try:
        for line in iter(fh.readline, ''):
            m = RE_WRITE.search(line)
            if m:
                if int(m.group(1)) != 0:
                    continue
                regs.setdefault(int(m.group(2), 16), Reg()).add(
                    "w", int(m.group(3), 16), int(m.group(4)))
                continue
            m = RE_READ.search(line)
            if m:
                if int(m.group(1)) != 0:
                    continue
                regs.setdefault(int(m.group(2), 16), Reg()).add(
                    "r", int(m.group(4), 16), int(m.group(3)))
    except KeyboardInterrupt:
        pass

    new = []
    print(f"{'offset':>8}  {'':3} {'R':>7} {'W':>7}  behaviour / [known role]")
    for off in sorted(regs):
        r = regs[off]
        label = classify(off)
        if args.new_only and label is not None:
            continue
        if r.reads + r.writes < args.min:
            continue
        cls = "NEW" if label is None else "   "
        tail = f"   [{label}]" if label else ""
        print(f"  0x{off:04x}  {cls} {r.reads:>7} {r.writes:>7}  {behaviour(r)}{tail}")
        if label is None:
            new.append((off, r))

    if new:
        print(f"\n=== {len(new)} data-plane candidate register(s) (outside the control-plane map) ===")
        for off, r in new:
            print(f"  0x{off:04x}  {guess_role(off, r)}")
    else:
        print("\n(no registers outside the control-plane map — no data-plane activity in this trace)")


if __name__ == "__main__":
    main()
