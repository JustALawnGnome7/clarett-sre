#!/usr/bin/env python3
"""
fcp_decode.py — turn QEMU `vfio_region_*` trace lines into structured FCP mailbox
transactions for the Focusrite Clarett 8PreX.

Input: a libvirt/QEMU log containing vfio_region_read / vfio_region_write lines, e.g.
  ...Z vfio_region_write  (0000:09:00.0:region0+0x8020, 0x80005000, 4)
  ...Z vfio_region_read   (0000:09:00.0:region0+0x100, 4) = 0x20000000

Output: one block per mailbox command (delimited by the doorbell), showing the decoded
FCP header (opcode / seq / size / error), the request payload, any response bytes the
guest read back, and which IRQ cause register signalled completion.

Register map is from spec/clarett-8prex-fcp-transport.md §8 (boot-init trace). Offsets are
constants below — tweak if later captures refine them.

Usage:
  ./tools/fcp_decode.py clarett_init_short.txt
  sudo tail -f /var/log/libvirt/qemu/Windows10-custom.log | ./tools/fcp_decode.py -
  ./tools/fcp_decode.py capture.txt --raw     # also echo non-mailbox accesses
"""
import argparse
import re
import sys

# --- BAR0 register map (confirmed from boot-init trace) -----------------------
MBOX_BASE   = 0x8020           # FCP request mailbox
OFF_CMD     = 0x00             #   +0  : cmd (bit31 = execute flag, low bits = opcode)
OFF_SIZESEQ = 0x04             #   +4  : size (lo16) | seq (hi16)
OFF_ERROR   = 0x08             #   +8  : error/status
OFF_PAD     = 0x0c             #   +12 : pad
OFF_DATA    = 0x10             #   +16 : data[]
MBOX_END    = MBOX_BASE + 0x100

DOORBELL    = 0x408            # write 1 = submit, 2 = ack/clear prior completion
CAUSE_REGS  = {0x100: "vec0", 0x200: "vec1", 0x300: "vec2", 0x400: "vec3"}
INFO_BASE   = 0x8000           # read-only firmware-info header (0x8000..0x801f)
EXEC_FLAG   = 0x80000000
DONE_BIT    = 0x20000000       # mailbox-done cause bit (seen in cause regs)

# Opcodes (low bits of cmd). CONFIRMED entries verified against stimulus; "?" still guesses.
OPCODE_NAMES = {
    0x001001: "GET_METER",      # CONFIRMED: GUI meter poll {offset,len}; == scarlett2 GET_METER
    0x800000: "GET_DATA",       # CONFIRMED: {u32 offset, u32 len}; response via DMA, not MMIO
    0x003001: "QUERY_3001?",    # seen after monitor GET_DATA; {u16, u8=2, u8 index}
    0x800001: "SET_DATA",       # CONFIRMED: {u32 offset, u32 len, data[len]}; == scarlett2
    0x800002: "DATA_CMD",       # CONFIRMED: {u32 activate} = XML control "command"; == scarlett2
    0x800005: "READ_SEG?",      # 0x008000xx class, low byte 5 (XML flash-command=5)
    0x0002:   "INIT_2?",
    0x5000:   "CONFIG_PUSH?",   # ~46x at init, 2-byte payload
    0x6000:   "GET_6.0?", 0x6001: "GET_6.1?", 0x6002: "GET_6.2?",
    0x7000:   "GET_7.0?", 0x7001: "GET_7.1?", 0x7002: "GET_7.2?", 0x7003: "GET_7.3?",
}

# --- line parsing -------------------------------------------------------------
RE_WRITE = re.compile(
    r'vfio_region_write\s+\([^)]*region(\d+)\+0x([0-9a-fA-F]+),\s*0x([0-9a-fA-F]+),\s*(\d+)\)')
RE_READ = re.compile(
    r'vfio_region_read\s+\([^)]*region(\d+)\+0x([0-9a-fA-F]+),\s*(\d+)\)\s*=\s*0x([0-9a-fA-F]+)')


def parse_line(line):
    """Return (op, region, offset, size, value) or None."""
    m = RE_WRITE.search(line)
    if m:
        return ("w", int(m.group(1)), int(m.group(2), 16), int(m.group(4)), int(m.group(3), 16))
    m = RE_READ.search(line)
    if m:
        return ("r", int(m.group(1)), int(m.group(2), 16), int(m.group(3)), int(m.group(4), 16))
    return None


def store_le(bmap, off, size, val):
    """Store `size` little-endian bytes of `val` into the byte map at `off`."""
    for i in range(size):
        bmap[off + i] = (val >> (8 * i)) & 0xff


def get_bytes(bmap, start, n):
    """Return list of n ints (or None for unseen bytes) from the byte map."""
    return [bmap.get(start + i) for i in range(n)]


def fmt_bytes(bs):
    return " ".join("??" if b is None else f"{b:02x}" for b in bs)


def fmt_words(bs):
    """Group bytes into little-endian u32 words for readability."""
    words = []
    for i in range(0, len(bs), 4):
        chunk = bs[i:i + 4]
        if any(b is None for b in chunk) or len(chunk) < 4:
            words.append("????????")
        else:
            words.append(f"{chunk[0] | chunk[1] << 8 | chunk[2] << 16 | chunk[3] << 24:08x}")
    return " ".join(words)


class Txn:
    __slots__ = ("n", "cmd", "opcode", "execd", "seq", "size", "error",
                 "data", "resp", "completions")

    def __init__(self, n, req_bytes):
        self.n = n
        cmd = u32(req_bytes, OFF_CMD)
        self.cmd = cmd
        self.execd = bool(cmd is not None and cmd & EXEC_FLAG)
        self.opcode = None if cmd is None else cmd & ~EXEC_FLAG
        ss = u32(req_bytes, OFF_SIZESEQ) or 0
        self.size = ss & 0xffff
        self.seq = (ss >> 16) & 0xffff
        self.error = u32(req_bytes, OFF_ERROR)
        self.data = get_bytes(req_bytes_dict(req_bytes), MBOX_BASE + OFF_DATA, self.size) \
            if self.size else []
        self.resp = {}          # offset -> value (guest reads after submit)
        self.completions = []   # (vecname, value)

    def render_brief(self):
        """One compact line per transaction — handy for tabulating fader sweeps."""
        name = OPCODE_NAMES.get(self.opcode, "")
        op = "????" if self.opcode is None else f"0x{self.opcode:06x}"
        s = f"#{self.n:<4} {op} {name:<12} seq={self.seq}"
        if self.size and all(b is not None for b in self.data[:min(8, self.size)]):
            if self.size >= 8:                       # data-class {offset, len, ...}
                a0 = self.data[0] | self.data[1] << 8 | self.data[2] << 16 | self.data[3] << 24
                a1 = self.data[4] | self.data[5] << 8 | self.data[6] << 16 | self.data[7] << 24
                s += f"  off=0x{a0:x} len=0x{a1:x}"
                if self.size > 8:
                    s += f" val={fmt_bytes(self.data[8:min(self.size, 8 + 16)])}"
            else:
                s += f"  data={fmt_bytes(self.data)}"
        elif self.size:
            s += f"  size={self.size}"
        return s

    def render(self):
        name = OPCODE_NAMES.get(self.opcode, "")
        op = "????" if self.opcode is None else f"0x{self.opcode:06x}"
        head = f"#{self.n:<4} op={op} {name:<14} seq={self.seq:<4} size={self.size:<3}"
        if self.error:
            head += f" err=0x{self.error:x}"
        lines = [head]
        if self.size:
            lines.append(f"      data bytes : {fmt_bytes(self.data)}")
            lines.append(f"      data words : {fmt_words(self.data)}")
            if self.size >= 8 and all(b is not None for b in self.data[:8]):
                a0 = self.data[0] | self.data[1] << 8 | self.data[2] << 16 | self.data[3] << 24
                a1 = self.data[4] | self.data[5] << 8 | self.data[6] << 16 | self.data[7] << 24
                lines.append(f"      if SET    : arg0/offset=0x{a0:x}  arg1/len=0x{a1:x}")
        if self.resp:
            rd = " ".join(f"+0x{o - MBOX_BASE:x}=0x{v:x}" for o, v in sorted(self.resp.items()))
            lines.append(f"      resp reads : {rd}")
        if self.completions:
            cp = " ".join(f"{v}=0x{val:x}" for v, val in self.completions)
            lines.append(f"      completion : {cp}")
        return "\n".join(lines)


# req_bytes is itself the byte map dict; small helpers keep Txn readable
def req_bytes_dict(bmap):
    return bmap


def u32(bmap, rel):
    bs = [bmap.get(MBOX_BASE + rel + i) for i in range(4)]
    if any(b is None for b in bs):
        return None
    return bs[0] | bs[1] << 8 | bs[2] << 16 | bs[3] << 24


def main():
    ap = argparse.ArgumentParser(description="Decode QEMU vfio_region_* traces into FCP transactions")
    ap.add_argument("file", help="trace file, or - for stdin")
    ap.add_argument("--raw", action="store_true", help="also echo non-mailbox register accesses")
    ap.add_argument("--brief", action="store_true", help="one compact line per transaction")
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(line_buffering=True)   # flush each line for live `tail -f` pipes
    except Exception:
        pass
    fh = sys.stdin if args.file == "-" else open(args.file)
    mbox = {}          # byte map of the request region
    cur = None         # open Txn awaiting completion/close
    n = 0
    hist = {}

    def flush():
        nonlocal cur
        if cur is not None:
            print(cur.render_brief() if args.brief else cur.render())
            hist[cur.opcode] = hist.get(cur.opcode, 0) + 1
            cur = None

    for line in iter(fh.readline, ''):   # readline avoids the read-ahead buffering of `for line in fh`
        p = parse_line(line)
        if not p:
            continue
        op, region, off, size, val = p
        if region != 0:
            continue

        # request-mailbox writes accumulate into the byte map
        if op == "w" and MBOX_BASE <= off < MBOX_END:
            store_le(mbox, off, size, val)
            continue

        # doorbell
        if op == "w" and off == DOORBELL:
            if val == 1:                       # submit -> snapshot a transaction
                flush()
                n += 1
                cur = Txn(n, dict(mbox))
            elif val == 2:                     # ack/clear prior completion
                flush()
            continue

        # completion: nonzero cause-register reads belong to the open txn
        if op == "r" and off in CAUSE_REGS and cur is not None and val != 0:
            cur.completions.append((CAUSE_REGS[off], val))
            continue

        # response: guest reads back from the mailbox region after submit
        if op == "r" and MBOX_BASE <= off < MBOX_END and cur is not None:
            cur.resp[off] = val
            continue

        if args.raw:
            tag = "W" if op == "w" else "R"
            print(f"      [{tag} +0x{off:x} = 0x{val:x}]")

    flush()

    print("\n--- opcode histogram ---")
    for opc, c in sorted(hist.items(), key=lambda kv: -kv[1]):
        ops = "????" if opc is None else f"0x{opc:06x}"
        print(f"  {ops:<10} {OPCODE_NAMES.get(opc, ''):<14} {c}")


if __name__ == "__main__":
    main()
