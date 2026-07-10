#!/usr/bin/env python3
"""resp_dump.py — annotate and diff pmemsave dumps of the FCP GET-response buffer.

Purpose (manifestation-wall §7 test 2): decide whether Focusrite Control's host side ever
puts content into the 0x410 response buffer that is NOT a device-written response — i.e. a
request mirror / token / ack the device DMA-reads. Every dump state must be explainable as:

    [16-byte echoed FCP header][size bytes of response payload] over older response residue

Anything else is a candidate host seed.

Response header layout (fcp-transport spec §8, clarett.h FCP_RESP_*):
    +0  u32 echo    = 0x80000000 | opcode   (CMD_EXEC_FLAG | the request's opcode)
    +4  u16 size    response payload length
    +6  u16 seq     echoed request sequence number
    +8  u32 status  0x03 = SUCCESS on a device-written response
    +12 u32 pad
    +16 payload

Discriminator for a HOST-WRITTEN REQUEST MIRROR (the key §7 signature): the header shape
is the same, but +8 holds the request's zeroed error word (0x0, not 0x03) while size > 0
and the payload is request-shaped (e.g. GET_DATA: {u32 offset, u32 len} = 8 bytes).

Usage:
    resp_dump.py DUMP.bin [DUMP2.bin ...]      annotate each dump
    resp_dump.py --diff DUMP1.bin DUMP2.bin... diff consecutive dumps (sorted by name),
                                               annotating every state transition
"""
import argparse
import sys

CMD_EXEC = 0x80000000
OPCODES = {
    0x000000: "INIT_1", 0x000001: "SUB_ENABLE", 0x000002: "INIT_2", 0x000003: "REBOOT",
    0x001001: "GET_METER",
    0x800000: "GET_DATA", 0x800001: "SET_DATA", 0x800002: "DATA_CMD", 0x800005: "READ_SEG",
    0x003001: "QUERY_3001",
    0x005000: "CONFIG_PUSH",
    0x006000: "GET_6.0", 0x006001: "GET_6.1", 0x006002: "GET_6.2", 0x006003: "SET_CLOCK",
    0x006004: "GET_6.4", 0x006005: "GET_6.5",
    0x007000: "GET_7.0", 0x007001: "GET_7.1", 0x007002: "GET_7.2", 0x007003: "GET_7.3",
    0x004005: "CMD_4005",
}


def le32(b, off):
    return int.from_bytes(b[off:off + 4], "little")


def le16(b, off):
    return int.from_bytes(b[off:off + 2], "little")


def nonzero_extents(b, start=0):
    """Yield (offset, length) runs of nonzero bytes from `start`."""
    runs, i, n = [], start, len(b)
    while i < n:
        if b[i]:
            j = i
            while j < n and b[j]:
                j += 1
            runs.append((i, j - i))
            i = j
        else:
            i += 1
    return runs


def hexline(b, off, length, width=16):
    out = []
    for base in range(off, off + length, width):
        chunk = b[base:min(off + length, base + width)]
        out.append(f"    0x{base:04x}: {' '.join(f'{x:02x}' for x in chunk)}")
    return "\n".join(out)


def annotate(b, name, verbose=False):
    print(f"== {name} ({len(b)} bytes)")
    echo, size, seq = le32(b, 0), le16(b, 4), le16(b, 6)
    status, pad = le32(b, 8), le32(b, 12)

    if echo == 0:
        print("  header: EMPTY (echo=0) — buffer never written, or zeroed")
        shape = "empty"
    elif echo & CMD_EXEC:
        op = echo & ~CMD_EXEC
        opname = OPCODES.get(op, f"op 0x{op:06x}")
        # +8 is the FCP error/status word. Working sessions write 0x00 (scarlett2
        # convention: 0 = OK); our walled device writes 0x03 on every response —
        # a refusal code, not "success" (S0 pmemsave finding, July 9 2026).
        kind = {0x00: "response, error=0 (WORKING-session style)",
                0x03: "response, error=3 (the walled-device refusal code)"}.get(
               status, f"*** error=0x{status:x} — unknown code, inspect ***")
        print(f"  header: {opname} seq={seq} size={size} status=0x{status:02x} pad=0x{pad:x}"
              f"  -> {kind}")
        if size:
            print(f"  payload (+0x10, {min(size, 64)} of {size} bytes):")
            print(hexline(b, 16, min(size, 64)))
        shape = f"resp/err{status}"
    else:
        print(f"  header: NOT AN ECHO (0x{echo:08x}) — *** unexplained content, inspect ***")
        shape = "ALIEN"

    covered = 16 + size if echo else 0
    tail = nonzero_extents(b, max(covered, 16))
    if tail:
        print(f"  nonzero beyond header+size (residue from an older/larger response, or a seed):")
        for off, length in tail:
            print(f"    extent 0x{off:04x}+0x{length:x}")
            if verbose or shape in ("REQUEST?", "ALIEN") or off >= 0x800:
                print(hexline(b, off, min(length, 64)))
    elif echo:
        print("  tail: clean (all zero beyond the response)")
    return shape


def diff(a, b, na, nb):
    changed = [(i, a[i], b[i]) for i in range(min(len(a), len(b))) if a[i] != b[i]]
    if not changed:
        print(f"-- {na} -> {nb}: identical")
        return
    # group into extents
    exts, start, prev = [], changed[0][0], changed[0][0]
    for i, _, _ in changed[1:]:
        if i != prev + 1:
            exts.append((start, prev - start + 1))
            start = i
        prev = i
    exts.append((start, prev - start + 1))
    print(f"-- {na} -> {nb}: {len(changed)} bytes changed in {len(exts)} extent(s): "
          + ", ".join(f"0x{o:04x}+0x{l:x}" for o, l in exts))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dumps", nargs="+", help="pmemsave dump files (4096 bytes each)")
    ap.add_argument("--diff", action="store_true",
                    help="diff consecutive dumps (sorted by filename) and annotate transitions")
    ap.add_argument("--ladder", action="store_true",
                    help="treat each file as concatenated 4096-byte records (doorbell_ladder.gdb "
                         "output): one summary line per record — record N = response to command N-1")
    ap.add_argument("-v", "--verbose", action="store_true", help="hexdump every extent")
    args = ap.parse_args()

    if args.ladder:
        for f in args.dumps:
            with open(f, "rb") as fh:
                blob = fh.read()
            print(f"== {f}: {len(blob) // 4096} records")
            for n in range(0, len(blob) // 4096):
                b = blob[n * 4096:(n + 1) * 4096]
                echo, size, seq = le32(b, 0), le16(b, 4), le16(b, 6)
                status = le32(b, 8)
                if echo == 0:
                    print(f"  [{n:3d}] (empty — no response yet)")
                    continue
                op = echo & ~CMD_EXEC
                pay = " ".join(f"{x:02x}" for x in b[16:16 + min(size, 16)]) if size else ""
                print(f"  [{n:3d}] {OPCODES.get(op, f'op 0x{op:06x}'):<14} seq={seq:<4d} "
                      f"err=0x{status:x} size={size:<5d} {pay}")
        return

    files = sorted(args.dumps)
    data = {}
    for f in files:
        with open(f, "rb") as fh:
            data[f] = fh.read()

    if args.diff and len(files) > 1:
        # annotate distinct states once, then show the transition sequence
        seen = {}
        for f in files:
            key = data[f]
            if key not in seen:
                seen[key] = f
                annotate(data[f], f, args.verbose)
                print()
        print(f"{len(seen)} distinct state(s) across {len(files)} dumps\n")
        for x, y in zip(files, files[1:]):
            diff(data[x], data[y], x, y)
    else:
        for f in files:
            annotate(data[f], f, args.verbose)
            print()


if __name__ == "__main__":
    main()
