#!/usr/bin/env python3
"""dma_bases.py — extract the live DMA ring bases from a streaming vfio trace and emit
ready-to-run QMP pmemsave commands for dumping the guest buffers.

The engine's block-0 (TX) and block-1 (RX) ring bases are programmed per-session at
0x210/0x214 and 0x310/0x314 (BASElo/BASEhi). Windows allocates these fresh each boot,
so they must be read from the current trace, not hard-coded. GPA = (BASEhi<<32)|BASElo.

Usage: python3 tools/dma_bases.py STREAM.log [domain] [size]
       (domain defaults to Windows10-custom; size in bytes, default 12288)
"""
import re
import sys

RE = re.compile(r'region0\+0x([0-9a-fA-F]+),\s*0x([0-9a-fA-F]+),')

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    log = sys.argv[1]
    domain = sys.argv[2] if len(sys.argv) > 2 else "Windows10-custom"
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 12288

    # take the LAST write seen to each base register (most recent arm)
    reg = {}
    with open(log) as f:
        for line in f:
            m = RE.search(line)
            if not m:
                continue
            off = int(m.group(1), 16)
            if off in (0x210, 0x214, 0x310, 0x314):
                reg[off] = int(m.group(2), 16)

    miss = [hex(o) for o in (0x210, 0x214, 0x310, 0x314) if o not in reg]
    if miss:
        print(f"missing base writes {miss} — capture a window that includes a stream (re)arm")
        sys.exit(2)

    for tag, lo, hi in (("TX/block0", 0x210, 0x214), ("RX/block1", 0x310, 0x314)):
        gpa = (reg[hi] << 32) | reg[lo]
        fn = f"/tmp/{'tx' if 'TX' in tag else 'rx'}_2pre.bin"
        print(f"# {tag}: BASEhi=0x{reg[hi]:x} BASElo=0x{reg[lo]:08x} -> GPA 0x{gpa:x} ({gpa} dec)")
        print(f'sudo virsh qemu-monitor-command "{domain}" '
              f'"{{\\"execute\\":\\"pmemsave\\",\\"arguments\\":'
              f'{{\\"val\\":{gpa},\\"size\\":{size},\\"filename\\":\\"{fn}\\"}}}}"')
        print()

if __name__ == "__main__":
    main()
