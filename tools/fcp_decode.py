#!/usr/bin/env python3
"""
fcp_decode.py — turn QEMU `vfio_region_*` trace lines into structured FCP mailbox
transactions for the Focusrite Clarett line (2Pre by default).

Input: a libvirt/QEMU log containing vfio_region_read / vfio_region_write lines, e.g.
  ...Z vfio_region_write  (0000:09:00.0:region0+0x8020, 0x80005000, 4)
  ...Z vfio_region_read   (0000:09:00.0:region0+0x100, 4) = 0x20000000

Output: one block per mailbox command (delimited by the doorbell), showing the decoded
FCP header (opcode / seq / size / error), the request payload, any response bytes the
guest read back, and which IRQ cause register signalled completion.

Register map is from spec/provenance/clarett-fcp-transport.md §8 (boot-init trace). Offsets are
constants below — tweak if later captures refine them.

Usage:
  ./tools/fcp_decode.py captures/8prex_init_short.log
  sudo tail -f /var/log/libvirt/qemu/Windows10-custom.log | ./tools/fcp_decode.py -
  ./tools/fcp_decode.py capture.txt --raw       # also echo non-mailbox accesses
  ./tools/fcp_decode.py capture.txt --classify  # is this capture a fader move or a routing change?
  ./tools/fcp_decode.py attach.log --emit-init > driver/clarett_init_2pre.h  # 2Pre bring-up (default; --init-model 8prex/4pre for others)

The 8192-byte appspace persist write-back (a run of SET_DATA into config offset >=200) fires on
every control change and is collapsed to a single summary line by default; pass --show-appspace to
see the individual chunks.
"""
import argparse
import os
import re
import sys

# --- BAR0 register map (confirmed from boot-init trace) -----------------------
MBOX_BASE   = 0x8020           # FCP request mailbox
OFF_CMD     = 0x00             #   +0  : cmd (bit31 = execute flag, low bits = opcode)
OFF_SIZESEQ = 0x04             #   +4  : size (lo16) | seq (hi16)
OFF_ERROR   = 0x08             #   +8  : error/status
OFF_PAD     = 0x0c             #   +12 : pad
OFF_DATA    = 0x10             #   +16 : data[]
MBOX_END    = MBOX_BASE + 0x410   # data region holds up to ~1 KB payloads (SET_MUX, appspace)

DOORBELL    = 0x408            # write 1 = submit, 2 = ack/clear prior completion
CAUSE_REGS  = {0x100: "vec0", 0x200: "vec1", 0x300: "vec2", 0x400: "vec3"}
INFO_BASE   = 0x8000           # read-only firmware-info header (0x8000..0x801f)
EXEC_FLAG   = 0x80000000
DONE_BIT    = 0x20000000       # mailbox-done cause bit (seen in cause regs)
APPSPACE_BASE = 0xc8           # config offset 200: persistent-store write-back region (spec §12)
IRQ_BLOCK   = (0x100, 0x5ff)   # interrupt cause/summary register block (inclusive)
NOTIFY_BITS = ((0x00200000, "dim-mute"), (0x00400000, "monitor"))  # control-plane §11 async events

# Opcodes (low bits of cmd). CONFIRMED entries verified against stimulus; "?" still guesses.
OPCODE_NAMES = {
    0x001001: "GET_METER",      # CONFIRMED: GUI meter poll {offset,len}; == scarlett2 GET_METER
    0x002001: "GET_MIX",        # scarlett2 GET_MIX {u16 mix_num} -> coeffs
    0x002002: "SET_MIX",        # CONFIRMED: {u16 mix_num, u16 coeff[]}; routing via mixer matrix
    0x800000: "GET_DATA",       # CONFIRMED: {u32 offset, u32 len}; response via DMA, not MMIO
    0x003001: "GET_MUX?",       # scarlett2 GET_MUX value; seen after monitor GET_DATA at init
    0x003002: "SET_MUX",        # CONFIRMED: {u32 band<<16, u32 (src<<12|dst)[]}; routing matrix
    0x800001: "SET_DATA",       # CONFIRMED: {u32 offset, u32 len, data[len]}; == scarlett2
    0x800002: "DATA_CMD",       # CONFIRMED: {u32 activate} = XML control "command"; == scarlett2
    0x800005: "READ_SEG?",      # 0x008000xx class, low byte 5 (XML flash-command=5)
    0x0002:   "INIT_2?",
    0x5000:   "CONFIG_PUSH?",   # ~46x at init, 2-byte payload
    0x6000:   "GET_6.0?", 0x6001: "GET_6.1?", 0x6002: "GET_6.2?",
    0x7000:   "GET_7.0?", 0x7001: "GET_7.1?", 0x7002: "GET_7.2?", 0x7003: "GET_7.3?",
}

# --- bring-up-blob annotation (--annotate-init) -------------------------------
# Per-opcode decode status for the init replay tables. category is one of:
#   DECODED-SET  we understand the payload AND it writes device state -> keep, renderable
#   DECODED-GET  we understand it, but it is a read (response DMAed/ignored)
#   FORM-SET     shape known, per-item meaning opaque -> keep, NOT yet renderable
#   QUERY        pure read whose response the driver discards -> PRUNE CANDIDATE (issuance
#                may still be handshake-required; only a hardware A/B proves droppability)
#   QUERY-KEEP   a query we depend on elsewhere (do not treat as a prune candidate)
# name/note are documentation; nothing here is copied from vendor code (clean-room: these are
# black-box observations of a captured MMIO trace).
OPCODE_META = {
    0x800001: ("SET_DATA",     "DECODED-SET", "config write {off,len,data}"),
    0x800002: ("DATA_CMD",     "DECODED-SET", "activate {u32}"),
    0x002002: ("SET_MIX",      "DECODED-SET", "mixer coeffs {mix,u16[]}"),
    0x003002: ("SET_MUX",      "DECODED-SET", "routing {band,(src<<12|dst)[]}"),
    0x800000: ("GET_DATA",     "DECODED-GET", "config read {off,len} -> DMA"),
    0x002001: ("GET_MIX",      "DECODED-GET", "mixer read"),
    0x003001: ("GET_MUX",      "DECODED-GET", "routing read -> DMA"),
    0x005000: ("CONFIG_PUSH",  "FORM-SET",    "register config-item id {u16}; arms config space"),
    0x000001: ("INIT_1/SUBEN", "FORM-SET",    "subsystem enable {u16 id} (also CAP_READ)"),
    0x000002: ("INIT_2",       "FORM-SET",    "subsystem init (zero-len)"),
    0x004001: ("SUBSYS4_SET",  "FORM-SET",    "subsystem-4 setup"),
    0x004005: ("SUBSYS4_OP",   "FORM-SET",    "subsystem-4 op"),
    0x001000: ("COUNT_1",      "QUERY",       "subsystem-1 count query (response discarded)"),
    0x002000: ("COUNT_2",      "QUERY",       "subsystem-2 count query (response discarded)"),
    0x003000: ("COUNT_3",      "QUERY",       "subsystem-3 count query (response discarded)"),
    0x004000: ("COUNT_4",      "QUERY",       "subsystem-4 count query (response discarded)"),
    0x800005: ("READ_SEG",     "QUERY",       "fw-segment/identity read (response discarded)"),
    0x006000: ("GET_6.0",      "QUERY",       "identity/version (response discarded)"),
    0x006001: ("GET_6.1",      "QUERY",       "identity/version (response discarded)"),
    0x006002: ("GET_6.2",      "QUERY",       "identity/version (response discarded)"),
    0x006004: ("GET_6.4",      "QUERY",       "identity/version (response discarded)"),
    0x006005: ("GET_6.5",      "QUERY",       "identity/version (response discarded)"),
    0x007000: ("GET_7.0",      "QUERY",       "identity/count (response discarded)"),
    0x007001: ("GET_7.1",      "QUERY-KEEP",  "band0 {playback,capture} counts = model auto-detect"),
    0x007002: ("GET_7.2",      "QUERY",       "identity/count (response discarded)"),
    0x007003: ("GET_7.3",      "QUERY",       "identity/count (response discarded)"),
}
CATEGORY_ORDER = ("DECODED-SET", "DECODED-GET", "FORM-SET", "QUERY-KEEP", "QUERY", "UNKNOWN")

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


def fmt_u16s(bs):
    """Group bytes into little-endian u16 values (mixer coefficients)."""
    vals = []
    for i in range(0, len(bs), 2):
        chunk = bs[i:i + 2]
        if any(b is None for b in chunk) or len(chunk) < 2:
            vals.append("????")
        else:
            vals.append(f"{chunk[0] | chunk[1] << 8:04x}")
    return " ".join(vals)


# Mixer opcodes carry {u16 mix_num, u16 coeff[]} rather than the {offset,len,data}
# of the data class — render their payload accordingly.
MIX_OPCODES = (0x002001, 0x002002)
# Routing-matrix opcodes: payload structure not yet confirmed — show raw u32 words.
MUX_OPCODES = (0x003001, 0x003002)


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

    def data_offset_len(self):
        """For data-class commands, the {u32 offset, u32 len} prefix (or None)."""
        if self.size < 8 or any(b is None for b in self.data[:8]):
            return None
        off = self.data[0] | self.data[1] << 8 | self.data[2] << 16 | self.data[3] << 24
        ln = self.data[4] | self.data[5] << 8 | self.data[6] << 16 | self.data[7] << 24
        return off, ln

    def is_appspace(self):
        """One chunk of the full-config persist write-back (transport spec §8): a SET_DATA
        into the persistent-store region (offset >= 200). Fires on every control change."""
        if self.opcode != 0x800001:
            return False
        ol = self.data_offset_len()
        return ol is not None and ol[0] >= APPSPACE_BASE

    def mix_coeffs(self):
        """For SET_MIX/GET_MIX, return (mix_num, [u16 coeff, ...]) or None."""
        if self.opcode not in MIX_OPCODES or self.size < 2 \
                or any(b is None for b in self.data[:2]):
            return None
        mix = self.data[0] | self.data[1] << 8
        body = self.data[2:]
        coeffs = []
        for i in range(0, len(body) - 1, 2):
            a, b = body[i], body[i + 1]
            coeffs.append(None if a is None or b is None else a | b << 8)
        return mix, coeffs

    def mux_entries(self):
        """For SET_MUX (0x3002), return (band, [(dst_pin, src_pin), ...]) or None.
        Payload = {u32 band<<16, u32 entry[]}; each entry = (src_pin<<12) | dst_pin (12-bit pins).
        The entry table is zero-padded to a fixed per-band size (16 trailing zero words observed);
        a zero word has dst=0, not a valid destination pin, so trailing pads are stripped — the
        returned count is the real table size (band 0 = 86 = 28 out + 28 capture + 30 mixer in)."""
        if self.opcode != 0x003002 or self.size < 4 \
                or any(b is None for b in self.data[:4]):
            return None
        band = self.data[2] | self.data[3] << 8
        entries = []
        for i in range(4, self.size - 3, 4):
            b = self.data[i:i + 4]
            if any(x is None for x in b):
                break
            v = b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24
            entries.append((v & 0xfff, (v >> 12) & 0xfff))
        while entries and entries[-1][0] == 0:    # drop the trailing dst=0 zero-padding words
            entries.pop()
        return band, entries

    def get_mux_query(self):
        """For GET_MUX (0x3001), return (count, elem, band) or None. Request payload is
        {u16 count, u8 elem(=0x02), u8 band}; the response (routing data) arrives via DMA."""
        if self.opcode != 0x003001 or self.size < 4 or any(b is None for b in self.data[:4]):
            return None
        return (self.data[0] | self.data[1] << 8, self.data[2], self.data[3])

    def render_brief(self):
        """One compact line per transaction — handy for tabulating fader sweeps."""
        name = OPCODE_NAMES.get(self.opcode, "")
        op = "????" if self.opcode is None else f"0x{self.opcode:06x}"
        s = f"#{self.n:<4} {op} {name:<12} seq={self.seq}"
        if self.opcode in MIX_OPCODES and self.size >= 2 \
                and all(b is not None for b in self.data[:min(self.size, 2)]):
            mix = self.data[0] | self.data[1] << 8   # {u16 mix_num, u16 coeff[]}
            s += f"  mix={mix} coeffs={fmt_u16s(self.data[2:min(self.size, 2 + 32)])}"
        elif self.mux_entries() is not None:                 # SET_MUX
            band, entries = self.mux_entries()
            routed = sum(1 for d, sp in entries if sp)
            s += f"  band={band} entries={len(entries)} ({routed} routed)"
        elif self.get_mux_query() is not None:               # GET_MUX request
            count, elem, band = self.get_mux_query()
            s += f"  get band={band} count={count} elem=0x{elem:02x}"
        elif self.size and all(b is not None for b in self.data[:min(8, self.size)]):
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
            mux = self.mux_entries()
            gmq = self.get_mux_query()
            if self.opcode in MIX_OPCODES and self.size >= 2 \
                    and all(b is not None for b in self.data[:2]):
                mix = self.data[0] | self.data[1] << 8
                lines.append(f"      mix bus    : {mix}")
                lines.append(f"      coeffs(u16): {fmt_u16s(self.data[2:])}")
            elif mux is not None:
                band, entries = mux
                lines.append(f"      mux band   : {band}  ({len(entries)} entries, dst<-src pins)")
                routed = [f"0x{d:03x}<-0x{s:03x}" for d, s in entries if s]
                for i in range(0, len(routed), 8):
                    lines.append("      " + "  ".join(routed[i:i + 8]))
            elif gmq is not None:
                count, elem, band = gmq
                lines.append(f"      get mux    : band={band} count={count} elem=0x{elem:02x}"
                             f"  (response via DMA)")
            else:
                lines.append(f"      data words : {fmt_words(self.data)}")
            if self.opcode not in MIX_OPCODES and mux is None and self.size >= 8 \
                    and all(b is not None for b in self.data[:8]):
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


def _le(v, n):
    return bytes((v >> (8 * i)) & 0xff for i in range(n))


def deblob_typed(op, payload):
    """Decompose one bring-up step into a typed record for the byte-faithful de-blob.

    Returns (kind, human, reserialized_bytes). A typing is byte-faithful iff reserialized_bytes
    == payload — the caller asserts this. Well-understood commands type to small fields
    (CONFIG_PUSH id, GET_DATA range, mixer coeffs, routing words); genuinely opaque queries fall
    back to 'raw' (which trivially round-trips). This is the extraction the driver's authored
    tables + a builder will mirror, so a passing round-trip proves the driver refactor can be exact.
    """
    p = bytes(payload)

    def u16(o=0):
        return p[o] | p[o + 1] << 8

    def u32(o=0):
        return p[o] | p[o + 1] << 8 | p[o + 2] << 16 | p[o + 3] << 24

    if len(p) == 0:                                  # INIT_2, count/identity queries — op carries the meaning
        return ('none', f"op 0x{op:06x} (no payload)", b'')
    if op == 0x005000 and len(p) == 2:
        return ('push', f"CONFIG_PUSH id 0x{u16():04x}", _le(u16(), 2))
    if op == 0x000001 and len(p) == 2:
        return ('enable', f"SUBSYS_ENABLE id 0x{u16():04x}", _le(u16(), 2))
    if op == 0x800005 and len(p) == 8:
        return ('readseg', f"READ_SEG off={u32(0)} len={u32(4)}", _le(u32(0), 4) + _le(u32(4), 4))
    if op == 0x800000 and len(p) == 8:
        return ('getdata', f"GET_DATA off={u32(0)} len={u32(4)}", _le(u32(0), 4) + _le(u32(4), 4))
    if op == 0x800002 and len(p) == 4:
        return ('datacmd', f"DATA_CMD activate {u32(0)}", _le(u32(0), 4))
    if op == 0x004001 and len(p) == 4:
        return ('subsys4', f"SUBSYS4_SET {u32(0)}", _le(u32(0), 4))
    if op == 0x002002 and len(p) >= 2 and len(p) % 2 == 0:
        mix = u16(0)
        coeffs = [p[2 + 2 * i] | p[3 + 2 * i] << 8 for i in range((len(p) - 2) // 2)]
        rs = _le(mix, 2) + b''.join(_le(c, 2) for c in coeffs)
        return ('mix', f"SET_MIX bus {mix} ({len(coeffs)} coeffs)", rs)
    if op == 0x003002 and len(p) >= 4 and len(p) % 4 == 0:
        words = [u32(4 * i) for i in range(len(p) // 4)]
        rs = b''.join(_le(w, 4) for w in words)
        return ('mux', f"SET_MUX band {u16(2)} ({len(words) - 1} route words)", rs)
    if op == 0x800001 and len(p) >= 8:
        off, ln, data = u32(0), u32(4), p[8:]
        nz = len(data.rstrip(b'\x00'))
        rs = _le(off, 4) + _le(ln, 4) + data
        return ('setdata', f"SET_DATA off={off} len={ln} (hdr {nz}B + {len(data) - nz} zeros)", rs)
    return ('raw', f"raw op=0x{op:06x} {len(p)}B", p)


def deblob_check(path):
    """Round-trip validator for the byte-faithful de-blob: decompose every step to a typed record,
    re-serialize, and assert byte-identity. Report the typed/raw split so we know how much de-blobs."""
    blob, steps = parse_init_header(path)
    kinds, mismatches, raw_bytes, typed_bytes = {}, [], 0, 0
    for i, (op, off, ln) in enumerate(steps):
        payload = bytes(blob[off:off + ln])
        kind, _human, rs = deblob_typed(op, payload)
        kinds[kind] = kinds.get(kind, 0) + 1
        if rs != payload:
            mismatches.append((i, op))
        if kind == 'raw':
            raw_bytes += len(payload)
        else:
            typed_bytes += len(payload)
    total = len(steps)
    raw_steps = kinds.get('raw', 0)
    print(f"{path}")
    print(f"  {total} steps  |  typed {total - raw_steps}  raw {raw_steps}  |  "
          f"payload bytes: typed {typed_bytes}  raw {raw_bytes}")
    print(f"  byte-faithful round-trip: {'PASS' if not mismatches else 'FAIL ' + str(mismatches)}")
    for k, c in sorted(kinds.items(), key=lambda kv: -kv[1]):
        print(f"    {k:10} {c}")
    return not mismatches


def emit_deblob(path, suffix):
    """Emit the de-blobbed per-model bring-up: typed step list + mix/mux/writeback/raw tables.
    The step payloads are byte-identical to the original blob (asserted here; validated end-to-end
    by tools/test_deblob.c). Symbols are suffixed so the de-blobbed form coexists with the old blob
    during validation. Consumed by clarett_arm.h's clarett_arm_emit()."""
    blob, steps = parse_init_header(path)
    KIND = {'none': 'CARM_NONE', 'push': 'CARM_ID', 'enable': 'CARM_ID', 'datacmd': 'CARM_U32',
            'subsys4': 'CARM_U32', 'getdata': 'CARM_RANGE', 'readseg': 'CARM_RANGE',
            'mix': 'CARM_MIX', 'mux': 'CARM_MUX', 'setdata': 'CARM_WB', 'raw': 'CARM_RAW'}
    mix_rows, mux_bands, wb_pool, raw_pool, steplines = [], [], bytearray(), bytearray(), []

    for op, off, ln in steps:
        p = bytes(blob[off:off + ln])
        kind, _h, rs = deblob_typed(op, p)
        assert rs == p, f"round-trip fail at op 0x{op:06x}"     # never emit an unfaithful step
        ck = KIND[kind]
        if kind == 'none':
            steplines.append((op, ck, 0, 0, 'NULL', 0))
        elif kind in ('push', 'enable'):
            steplines.append((op, ck, p[0] | p[1] << 8, 0, 'NULL', 0))
        elif kind in ('datacmd', 'subsys4'):
            steplines.append((op, ck, int.from_bytes(p[:4], 'little'), 0, 'NULL', 0))
        elif kind in ('getdata', 'readseg'):
            steplines.append((op, ck, int.from_bytes(p[:4], 'little'),
                              int.from_bytes(p[4:8], 'little'), 'NULL', 0))
        elif kind == 'mix':
            bus = p[0] | p[1] << 8
            coeffs = [p[2 + 2 * i] | p[3 + 2 * i] << 8 for i in range((len(p) - 2) // 2)]
            idx = len(mix_rows)
            mix_rows.append(coeffs)
            steplines.append((op, ck, bus, 0, f'clarett_armmix{suffix}[{idx}]', len(coeffs)))
        elif kind == 'mux':
            words = [int.from_bytes(p[4 * i:4 * i + 4], 'little') for i in range(len(p) // 4)]
            idx = len(mux_bands)
            mux_bands.append(words)
            steplines.append((op, ck, 0, 0, f'clarett_armmux{suffix}_b{idx}', len(words)))
        elif kind == 'setdata':
            a, b, data = int.from_bytes(p[:4], 'little'), int.from_bytes(p[4:8], 'little'), p[8:]
            hdr = data[:len(data.rstrip(b'\0'))]
            o = len(wb_pool)
            wb_pool += hdr
            steplines.append((op, ck, a, b, f'clarett_armwb{suffix} + {o}' if hdr else 'NULL', len(hdr)))
        elif kind == 'raw':
            o = len(raw_pool)
            raw_pool += p
            steplines.append((op, ck, 0, 0, f'clarett_armraw{suffix} + {o}', len(p)))

    ncoef = len(mix_rows[0]) if mix_rows else 0
    assert all(len(r) == ncoef for r in mix_rows), "non-uniform mixer coeff counts"

    def hexrows(bs, per=12):
        return "\n".join("\t" + " ".join(f"0x{b:02x}," for b in bs[i:i + per])
                         for i in range(0, len(bs), per))

    print(f"/* Generated from a vendor MMIO bring-up capture ({suffix.lstrip('_')} model). Do not edit.")
    print(" * De-blobbed bring-up tables (see clarett_arm.h); payloads are byte-identical to the")
    print(" * vendor capture (verified offline against it). */")
    if mix_rows:
        print(f"static const u16 clarett_armmix{suffix}[{len(mix_rows)}][{ncoef}] = {{")
        for r in mix_rows:
            print("\t{ " + " ".join(f"0x{c:04x}," for c in r) + " },")
        print("};")
    for i, words in enumerate(mux_bands):
        print(f"static const u32 clarett_armmux{suffix}_b{i}[] = {{")
        print("\n".join("\t" + " ".join(f"0x{w:08x}," for w in words[j:j + 6])
                        for j in range(0, len(words), 6)))
        print("};")
    if wb_pool:
        print(f"/* Writeback fallback: capture-day config, normally OVERWRITTEN by echoed live device")
        print(f" * state during arm (only used if a live GET_DATA read fails). */")
        print(f"static const u8 clarett_armwb{suffix}[] = {{")
        print(hexrows(wb_pool))
        print("};")
    if raw_pool:
        print(f"/* Opaque query payloads (identity/version reads; responses discarded). */")
        print(f"static const u8 clarett_armraw{suffix}[] = {{")
        print(hexrows(raw_pool))
        print("};")
    print(f"static const struct clarett_arm_step clarett_arm{suffix}[] = {{")
    for op, ck, a, b, data, dlen in steplines:
        print(f"\t{{ 0x{op:06x}, {ck}, {a}, {b}, {data}, {dlen} }},")
    print("};")
    print(f"/* {len(steplines)} steps */")


def synth_txn(n, opcode, payload):
    """Build a Txn from a bring-up step's (opcode, payload bytes), so the trace decoders
    (mix_coeffs / mux_entries / data_offset_len) apply unchanged to the replay table."""
    mbox = {}
    store_le(mbox, MBOX_BASE + OFF_CMD, 4, opcode | EXEC_FLAG)
    store_le(mbox, MBOX_BASE + OFF_SIZESEQ, 4, len(payload) & 0xffff)
    store_le(mbox, MBOX_BASE + OFF_ERROR, 4, 0)
    store_le(mbox, MBOX_BASE + OFF_PAD, 4, 0)
    for i, b in enumerate(payload):
        mbox[MBOX_BASE + OFF_DATA + i] = b
    return Txn(n, mbox)


def parse_init_header(path):
    """Parse a generated clarett_init_<model>.h into (blob bytes, [(opcode, off, len), ...])."""
    text = open(path).read()
    mb = re.search(r'clarett_init_blob\w*\[\]\s*=\s*\{(.*?)\};', text, re.S)
    ms = re.search(r'clarett_init_seq\w*\[\]\s*=\s*\{(.*?)\};', text, re.S)
    if not mb or not ms:
        sys.exit(f"{path}: no clarett_init_blob/seq arrays found (is this a generated init header?)")
    blob = [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', mb.group(1))]
    steps = [(int(o, 16), int(off), int(ln))
             for o, off, ln in re.findall(r'\{\s*0x([0-9a-fA-F]+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}',
                                          ms.group(1))]
    return blob, steps


def annotate_detail(t):
    """Render the meaning of one bring-up step's payload (best-effort, per decode tier)."""
    op = t.opcode
    if op == 0x002002:                                   # SET_MIX
        mc = t.mix_coeffs()
        if mc is not None:
            mix, co = mc
            nz = sum(1 for c in co if c)
            return f"mix bus {mix}: {len(co)} coeffs, {nz} non-zero"
    if op == 0x003002:                                   # SET_MUX
        me = t.mux_entries()
        if me is not None:
            band, entries = me
            routed = sum(1 for _d, s in entries if s)
            return f"band {band}: {len(entries)} dests, {routed} routed"
    if op in (0x800000, 0x800001):                       # GET_DATA / SET_DATA
        ol = t.data_offset_len()
        if ol is not None:
            off, ln = ol
            if op == 0x800000:
                return f"off {off} len {ln} -> DMA"
            val = fmt_bytes(t.data[8:min(t.size, 8 + 12)])
            tail = " ..." if t.size > 8 + 12 else ""
            note = "  [appspace persist]" if off >= APPSPACE_BASE else ""
            return f"off {off} len {ln} val=[{val}{tail}]{note}"
    if op == 0x800002 and t.size >= 4 and all(b is not None for b in t.data[:4]):
        act = t.data[0] | t.data[1] << 8 | t.data[2] << 16 | t.data[3] << 24
        return f"activate {act}"
    if op == 0x005000 and t.size >= 2:                   # CONFIG_PUSH: the 2-byte id
        return f"config-item id 0x{t.data[0] | t.data[1] << 8:04x}"
    if op == 0x000001 and t.size >= 2:                   # INIT_1 / subsystem enable / CAP_READ
        return f"subsystem/category id 0x{t.data[0] | t.data[1] << 8:04x}"
    if t.size:
        tail = " ..." if t.size > 8 else ""
        return f"{t.size}B [{fmt_bytes(t.data[:min(t.size, 8)])}{tail}]"
    return "(no payload)"


def annotate_init(path, summary_only):
    """Classify every step of a generated init header by decode tier and render Tier-1 meaning."""
    blob, steps = parse_init_header(path)
    cat_steps = {c: 0 for c in CATEGORY_ORDER}
    cat_bytes = {c: 0 for c in CATEGORY_ORDER}
    push_ids, suben_ids = [], []
    print(f"# annotation of {path}  ({len(steps)} steps, {len(blob)} payload bytes)\n")
    if not summary_only:
        print(f"  {'#':>3}  {'opcode':<10} {'name':<14} {'category':<12} detail")
        print("  " + "-" * 78)
    for i, (op, off, ln) in enumerate(steps):
        payload = blob[off:off + ln]
        name, cat, _note = OPCODE_META.get(op, (OPCODE_NAMES.get(op, ""), "UNKNOWN", ""))
        if cat not in cat_steps:
            cat, cat_steps[cat], cat_bytes[cat] = "UNKNOWN", 0, 0
        cat_steps[cat] += 1
        cat_bytes[cat] += ln
        t = synth_txn(i, op, payload)
        if op == 0x005000 and ln >= 2:
            push_ids.append(payload[0] | payload[1] << 8)
        elif op == 0x000001 and ln >= 2:
            suben_ids.append(payload[0] | payload[1] << 8)
        if not summary_only:
            print(f"  {i:>3}  0x{op:06x}   {name:<14} {cat:<12} {annotate_detail(t)}")

    print("\n=== decode-tier summary ===")
    print(f"  {'category':<12} {'steps':>6} {'bytes':>7}   meaning")
    meaning = {
        "DECODED-SET": "understood + writes state (renderable; must keep)",
        "DECODED-GET": "understood read (response DMAed/ignored)",
        "FORM-SET":    "shape known, per-item meaning OPAQUE (must keep)",
        "QUERY-KEEP":  "query we depend on (model detect); keep",
        "QUERY":       "response discarded -> PRUNE CANDIDATE (needs hw A/B)",
        "UNKNOWN":     "opcode not in metadata table",
    }
    for c in CATEGORY_ORDER:
        if cat_steps[c]:
            print(f"  {c:<12} {cat_steps[c]:>6} {cat_bytes[c]:>7}   {meaning[c]}")
    total = sum(cat_steps.values())
    prune = cat_steps["QUERY"]
    print(f"  {'-'*12} {'-'*6} {'-'*7}")
    print(f"  {'TOTAL':<12} {total:>6} {sum(cat_bytes.values()):>7}")
    print(f"\n  prune candidates (QUERY): {prune}/{total} steps "
          f"({100*prune/total:.0f}%) — response-discarded queries, A/B on hardware to confirm droppable")
    if push_ids:
        uniq = sorted(set(push_ids))
        dup = len(push_ids) - len(uniq)
        print(f"  CONFIG_PUSH: {len(push_ids)} pushes, {len(uniq)} distinct ids"
              f"{f' ({dup} duplicate pushes)' if dup else ''}")
        print("    ids: " + " ".join(f"0x{x:04x}" for x in uniq))
    if suben_ids:
        print(f"  INIT_1/subsystem-enable ids: "
              + " ".join(f"0x{x:04x}" for x in suben_ids))


def main():
    ap = argparse.ArgumentParser(description="Decode QEMU vfio_region_* traces into FCP transactions")
    ap.add_argument("file", help="trace file, or - for stdin")
    ap.add_argument("--raw", action="store_true", help="also echo non-mailbox register accesses")
    ap.add_argument("--brief", action="store_true", help="one compact line per transaction")
    ap.add_argument("--show-appspace", action="store_true",
                    help="expand the appspace persist write-back (collapsed to one summary by default)")
    ap.add_argument("--mix-diff", action="store_true",
                    help="for SET_MIX, print only the coefficient slots that changed since the "
                         "previous write to that mix bus (ideal for fader sweeps)")
    ap.add_argument("--async", dest="async_", action="store_true",
                    help="surface async device->host signalling: interrupt-register reads outside any "
                         "mailbox transaction, and any read carrying a §11 notification mask")
    ap.add_argument("--classify", action="store_true",
                    help="tag each control write MIX-level (SET_MIX) / MUX-route (SET_MUX) / "
                         "CONFIG-set (SET_DATA), skipping GET/poll/appspace noise, and print a "
                         "per-capture verdict — disambiguates a fader move from a routing change")
    ap.add_argument("--emit-init", action="store_true",
                    help="emit a C device-bring-up replay table from this capture: every non-meter "
                         "command up to the first monitor-mute write (the bulk config read/writeback "
                         "included). Redirect to driver/clarett_init_<model>.h.")
    ap.add_argument("--emit-deblob", action="store_true",
                    help="input is a generated clarett_init_<model>.h; emit the de-blobbed bring-up "
                         "(typed step list + mix/mux/writeback/raw tables, see clarett_arm.h). "
                         "Use --init-model for the symbol suffix. Redirect to tools/arm-tables/clarett_arm_<model>.h.")
    ap.add_argument("--deblob-check", action="store_true",
                    help="input is a generated clarett_init_<model>.h; decompose every step to a typed "
                         "record, re-serialize, and verify byte-identity (the byte-faithful de-blob "
                         "foundation). Reports the typed/raw split.")
    ap.add_argument("--annotate-init", action="store_true",
                    help="input is a generated clarett_init_<model>.h; classify every replay step "
                         "by decode tier (DECODED-SET / DECODED-GET / FORM-SET / QUERY), render the "
                         "Tier-1 meaning, and print prune-candidate + CONFIG_PUSH-id rollups.")
    ap.add_argument("--summary", action="store_true",
                    help="with --annotate-init, print only the decode-tier rollup (skip the per-step table)")
    ap.add_argument("--init-model", default="2pre",
                    help="symbol suffix for --emit-init (default '2pre' -> clarett_init_blob_2pre / "
                         "clarett_init_seq_2pre); e.g. '8prex' for the 8PreX. Per-model headers coexist. "
                         "Pass '' for an unsuffixed symbol.")
    args = ap.parse_args()

    if args.emit_deblob:              # offline: emit the de-blobbed per-model tables
        emit_deblob(args.file, f"_{args.init_model}" if args.init_model else "")
        return

    if args.deblob_check:             # offline: round-trip validate the byte-faithful de-blob
        deblob_check(args.file)
        return

    if args.annotate_init:            # offline: input is a generated init header, not a trace
        annotate_init(args.file, args.summary)
        return

    try:
        sys.stdout.reconfigure(line_buffering=True)   # flush each line for live `tail -f` pipes
    except Exception:
        pass
    fh = sys.stdin if args.file == "-" else open(args.file)
    mbox = {}          # byte map of the request region
    cur = None         # open Txn awaiting completion/close
    n = 0
    hist = {}
    appspace = []      # consecutive appspace write-back chunks awaiting a collapsed summary
    prev_mix = {}      # mix_num -> last-seen coeff list, for --mix-diff / --classify
    cls_keys = {"MIX": set(), "MUX": set(), "CONFIG": set()}  # --classify tallies
    init_cmds = []     # --emit-init: collected (opcode, payload-bytes) in capture order
    stop_emit = [False]   # set once the monitor-mute write is reached (= end of init)

    def collect_init(t):
        """For --emit-init: keep every non-meter command for the replay table, stopping at the
        first monitor-mute SET_DATA (= the user's test action, end of init). The bulk config
        read/writeback IS included now — it is part of arming config apply."""
        if stop_emit[0] or t.opcode == 0x001001:       # already done, or a meter poll
            return
        if t.opcode == 0x800001:                        # SET_DATA: stop at the monitor mute
            ol = t.data_offset_len()
            if ol is not None and ol[0] == 0x18:
                stop_emit[0] = True
                return
        if t.size > 1024 or any(b is None for b in t.data):
            return
        init_cmds.append((t.opcode, list(t.data)))

    def classify_line(t):
        """For --classify: return (category, key, detail) for a control write, else None.
        MIX = SET_MIX (level), MUX = SET_MUX (routing), CONFIG = a SET_DATA control write.
        GET_*/DATA_CMD/appspace/meter polls return None (not a user-visible state change)."""
        if t.opcode == 0x002002:                       # SET_MIX — a mix-bus level write
            mc = t.mix_coeffs()
            if mc is None:
                return None
            mix, coeffs = mc
            old = prev_mix.get(mix)
            prev_mix[mix] = coeffs
            if old is None:                            # no baseline yet (e.g. a full rewrite)
                nz = sum(1 for c in coeffs if c)
                return ("MIX", mix, f"bus={mix} (baseline, {nz} non-zero slots)") if nz else None
            diffs = [f"slot {i}: 0x{old[i]:04x}->0x{coeffs[i]:04x}"
                     for i in range(min(len(old), len(coeffs))) if old[i] != coeffs[i]]
            return ("MIX", mix, f"bus={mix} " + "; ".join(diffs)) if diffs else None
        if t.opcode == 0x003002:                       # SET_MUX — a routing-matrix write
            me = t.mux_entries()
            if me is None:
                return None
            band, entries = me
            routed = sum(1 for _d, s in entries if s)
            return ("MUX", band, f"band={band} entries={len(entries)} ({routed} routed)")
        if t.opcode == 0x800001 and not t.is_appspace():   # SET_DATA — a single control write
            ol = t.data_offset_len()
            if ol is None:
                return None
            off, ln = ol
            val = fmt_bytes(t.data[8:min(t.size, 8 + min(ln, 8))])
            return ("CONFIG", off, f"off={off} len={ln} val=[{val}]")
        return None

    def mix_diff_line(t):
        """One line showing which coefficient slots changed vs the previous write to this bus."""
        mc = t.mix_coeffs()
        if mc is None:
            return None
        mix, coeffs = mc
        old = prev_mix.get(mix)
        prev_mix[mix] = coeffs
        if old is None:                       # first sight of this bus: list the non-zero slots
            nz = ", ".join(f"slot {i}=0x{c:04x}" for i, c in enumerate(coeffs) if c)
            return f"#{t.n:<4} mix={mix} initial: {nz or 'all zero'}"
        diffs = [f"slot {i}: 0x{old[i]:04x}->0x{coeffs[i]:04x}"
                 for i in range(min(len(old), len(coeffs))) if old[i] != coeffs[i]]
        return f"#{t.n:<4} mix={mix} change: {'; '.join(diffs)}" if diffs else None

    def emit_appspace_summary():
        if not appspace:
            return
        spans = [t.data_offset_len() for t in appspace]
        total = sum(ln for _, ln in spans)
        lo, hi = min(o for o, _ in spans), max(o for o, _ in spans)
        print(f"#...  [appspace persist write-back: {len(appspace)}x SET_DATA, {total} bytes "
              f"@ off 0x{lo:x}..0x{hi:x}, seq {appspace[0].seq}-{appspace[-1].seq}]"
              f"  (--show-appspace to expand)")
        appspace.clear()

    def flush():
        nonlocal cur
        if cur is None:
            return
        hist[cur.opcode] = hist.get(cur.opcode, 0) + 1
        if args.emit_init:              # collect the bring-up replay table
            collect_init(cur)
            cur = None
            return
        if args.classify:               # tag control writes; suppress GET/poll/appspace noise
            res = classify_line(cur)
            if res is not None:
                cat, key, detail = res
                label = {"MIX": "MIX-level ", "MUX": "MUX-route ", "CONFIG": "CONFIG-set"}[cat]
                print(f"#{cur.n:<4} {label}  {detail}")
                cls_keys[cat].add(key)
            cur = None
            return
        if args.mix_diff and cur.opcode in MIX_OPCODES:
            line = mix_diff_line(cur)   # sweep mode: just the changed coefficient slot(s)
            if line is not None:
                emit_appspace_summary()
                print(line)
        elif not args.show_appspace and cur.is_appspace():
            appspace.append(cur)        # defer: collapse the run into one summary line
        else:
            emit_appspace_summary()     # a non-appspace txn ends any pending run
            print(cur.render_brief() if args.brief else cur.render())
        cur = None

    for line in iter(fh.readline, ''):   # readline avoids the read-ahead buffering of `for line in fh`
        p = parse_line(line)
        if not p:
            continue
        op, region, off, size, val = p
        if region != 0:
            continue

        # async device->host signalling: notification-masked reads (any time), or interrupt-block
        # reads that occur with no mailbox transaction in flight (i.e. not the steady meter poll)
        if args.async_ and op == "r" and off < INFO_BASE:
            # notification bits are only meaningful in the cause registers (0x100-0x400);
            # 0x500 is the summary/mask reg (constant 0xff0000) and must not be flagged
            bits = [nm for m, nm in NOTIFY_BITS if val & m] if off in CAUSE_REGS else []
            if bits or (cur is None and IRQ_BLOCK[0] <= off <= IRQ_BLOCK[1] and val):
                note = "  <-- NOTIFICATION: " + "+".join(bits) if bits else ""
                print(f"      [async R +0x{off:x} = 0x{val:x}{note}]")

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
    emit_appspace_summary()      # close any write-back run at EOF

    if args.emit_init:
        blob, rows = [], []
        for op, data in init_cmds:
            rows.append((op, len(blob), len(data)))
            blob.extend(data)
        src = args.file if args.file != "-" else "stdin"
        suffix = f"_{args.init_model}" if args.init_model else ""
        model_arg = f" --init-model {args.init_model}" if args.init_model else ""
        print(f"/* Generated by tools/fcp_decode.py --emit-init{model_arg} from {src}. Do not edit.")
        print(" * Device bring-up replayed at probe; struct clarett_init_step is in clarett.h. */")
        print(f"static const u8 clarett_init_blob{suffix}[] = {{")
        for i in range(0, len(blob), 12):
            print("\t" + " ".join(f"0x{b:02x}," for b in blob[i:i + 12]))
        print("};")
        print(f"static const struct clarett_init_step clarett_init_seq{suffix}[] = {{")
        for op, off, ln in rows:
            print(f"\t{{ 0x{op:06x}, {off}, {ln} }},")
        print("};")
        print(f"/* {len(rows)} commands, {len(blob)} payload bytes */")
        return

    if args.classify:
        m, x, cf = (len(cls_keys[k]) for k in ("MIX", "MUX", "CONFIG"))
        print("\n=== classification summary ===")
        print(f"  MIX-level  : {m} bus(es)   {sorted(cls_keys['MIX']) if m else ''}")
        print(f"  MUX-route  : {x} band(s)   {sorted(cls_keys['MUX']) if x else ''}")
        print(f"  CONFIG-set : {cf} offset(s) {sorted(cls_keys['CONFIG']) if cf else ''}")
        if x and m:
            verdict = ("ROUTING change — SET_MUX (per band) + a SET_MIX rewrite "
                       "(add/remove/source assignment, NOT a fader move)")
        elif x:
            verdict = "ROUTING change — SET_MUX only"
        elif m:
            verdict = "MIX-level change — SET_MIX only (fader/level move; no routing)"
        elif cf:
            verdict = "CONFIG control write — mute/dim/gain/air/mode; not mix or route"
        else:
            verdict = "no mix / route / config write seen in this capture"
        print(f"  verdict    : {verdict}")
        return

    print("\n--- opcode histogram ---")
    for opc, c in sorted(hist.items(), key=lambda kv: -kv[1]):
        ops = "????" if opc is None else f"0x{opc:06x}"
        print(f"  {ops:<10} {OPCODE_NAMES.get(opc, ''):<14} {c}")


if __name__ == "__main__":
    main()
