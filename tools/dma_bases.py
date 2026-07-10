#!/usr/bin/env python3
"""dma_bases.py — extract the live DMA base addresses from a vfio trace and emit
ready-to-run QMP pmemsave commands for dumping the guest buffers.

Three host-buffer bases exist (the device's complete DMA-address surface):
  0x410/0x414  FCP GET-response buffer   (programmed at driver init — any boot trace)
  0x210/0x214  block-0 (TX) ring base    (programmed at stream start only)
  0x310/0x314  block-1 (RX) ring base    (programmed at stream start only)
Windows allocates these fresh each boot, so they must be read from the current trace,
not hard-coded. GPA = (BASEhi<<32)|BASElo. Groups absent from the capture window are
skipped with a note (a boot-only trace has no stream bases; that is normal).

Usage: python3 tools/dma_bases.py TRACE.log [domain] [size]
       (domain defaults to Windows10; size in bytes, default 12288 for the
        ring dumps; the response buffer is always dumped as 4096)
"""
import re
import sys

RE = re.compile(r'region0\+0x([0-9a-fA-F]+),\s*0x([0-9a-fA-F]+),')

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    log = sys.argv[1]
    domain = sys.argv[2] if len(sys.argv) > 2 else "Windows10"
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 12288

    # take the LAST write seen to each base register (most recent programming)
    reg = {}
    with open(log) as f:
        for line in f:
            m = RE.search(line)
            if not m:
                continue
            off = int(m.group(1), 16)
            if off in (0x210, 0x214, 0x310, 0x314, 0x410, 0x414):
                reg[off] = int(m.group(2), 16)

    groups = (("RESP/0x410", 0x410, 0x414, "resp", 4096),
              ("TX/block0",  0x210, 0x214, "tx",   size),
              ("RX/block1",  0x310, 0x314, "rx",   size))

    found = 0
    for tag, lo, hi, stem, sz in groups:
        if lo not in reg or hi not in reg:
            print(f"# {tag}: not programmed in this capture window "
                  f"(normal for a boot-only trace if it is a stream base)")
            print()
            continue
        found += 1
        gpa = (reg[hi] << 32) | reg[lo]
        fn = f"/tmp/{stem}_2pre.bin"
        print(f"# {tag}: BASEhi=0x{reg[hi]:x} BASElo=0x{reg[lo]:08x} -> GPA 0x{gpa:x} ({gpa} dec)")
        print(f'sudo virsh qemu-monitor-command "{domain}" '
              f'"{{\\"execute\\":\\"pmemsave\\",\\"arguments\\":'
              f'{{\\"val\\":{gpa},\\"size\\":{sz},\\"filename\\":\\"{fn}\\"}}}}"')
        print()

    if not found:
        print("no DMA base writes found — is this a vfio_region_* trace of a driver bring-up?")
        sys.exit(2)

if __name__ == "__main__":
    main()
