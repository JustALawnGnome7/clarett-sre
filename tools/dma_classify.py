#!/usr/bin/env python3
"""dma_classify.py — classify a pmemsave dump of a stream ring base as FLAT AUDIO
or a DESCRIPTOR TABLE, and (for flat) report whether it is pre-seeded.

This automates the hand analysis from data-plane spec §9 that first told the two
buffer modes apart:

  * FLAT AUDIO — the engine's 0x210/0x310 point straight at the sample ring.
    Samples are 24-bit MSB-justified in 32-bit slots, so the LOW 16 bits of every
    word are zero and the values span a real +/- range (the 2Pre dump was ~100 %
    low-16-zero, TX != RX content).
  * DESCRIPTOR TABLE — 0x210/0x310 point at a list of 8-byte little-endian bus
    addresses, 0x100-aligned, high32 a small constant (the 8PreX dump: TX high=2,
    RX high=1), bit-0 wrap flag on the last entry. Here the LOW byte of every even
    word is 0x00 (alignment) and the odd words are a tiny constant.

Why it matters for the flat bring-up (spec §13): with a flat model in tree, dump the
2Pre TX/RX bases BEFORE the working arm and DURING the stream. The open §9 question is
whether the VM pre-seeds the sample area (does its TX ring hold playback audio at arm
time?) — 'flat, pre-seeded' vs 'flat, all-zero' is exactly what resolves why the old
flat attempt needed a non-null prefill to clock.

Usage:
  python3 tools/dma_classify.py DUMP.bin [--channels N] [--hex]
  # end to end, from a live stream trace:
  python3 tools/dma_bases.py TRACE.log Windows10   # emits the pmemsave lines; run them
  python3 tools/dma_classify.py /tmp/rx_2pre.bin --channels 14
"""
import argparse
import struct
import sys


def load_words(path):
    data = open(path, "rb").read()
    n = len(data) // 4
    return data, list(struct.unpack("<%dI" % n, data[: n * 4]))


def classify(words):
    """Return (verdict, scores dict). Heuristics from spec §9."""
    n = len(words)
    if n == 0:
        return "empty", {}
    nonzero = sum(1 for w in words if w)
    if nonzero == 0:
        return "all-zero", {"nonzero_frac": 0.0}

    # MSB-justified audio zeroes the low byte (24-bit) or low 16 bits (16-bit);
    # low8 catches both, low16 is a width hint (~1.0 => 16-bit samples).
    low8_zero = sum(1 for w in words if (w & 0xFF) == 0) / n
    low16_zero = sum(1 for w in words if (w & 0xFFFF) == 0) / n

    # Descriptor view: pair the words into u64 entries; the robust table signal is that
    # the HIGH 32 bits are a small constant (a bus-address high word / tag, {0,1,2,3}),
    # not that the low word is 0x100-aligned — real fragments pack at channels*64, which
    # is only 0x80-aligned for odd channel counts (2Pre RX 0x380). Audio high words vary
    # continuously and are rarely <= 3, so this separates the two regardless of alignment.
    entries = n // 2
    hi_small = 0
    hi_vals = {}
    for i in range(entries):
        lo, hi = words[2 * i], words[2 * i + 1]
        if hi <= 3 and (lo or hi):
            hi_small += 1
            hi_vals[hi] = hi_vals.get(hi, 0) + 1
    tbl_frac = hi_small / entries if entries else 0.0

    scores = {
        "nonzero_frac": nonzero / n,
        "low8_zero_frac": low8_zero,
        "low16_zero_frac": low16_zero,
        "table_entry_frac": tbl_frac,
    }

    # A table need not fill the dump: an oversized pmemsave catches unrelated guest
    # memory after the (short) table. Measure the LEADING run of small-high-word
    # entries — a clear table prefix is a table even if tbl_frac over the whole dump
    # is low.
    lead = 0
    while lead < entries:
        hi = words[2 * lead + 1]
        lo = words[2 * lead]
        if hi <= 3 and (lo or hi):
            lead += 1
        else:
            break
    scores["lead_table_run"] = lead

    # Descriptor table: most entries have a small constant high word (a handful of
    # distinct values — one per memory region Windows scattered the buffer across),
    # OR there is a clear leading table run before unrelated memory.
    if (tbl_frac >= 0.75 and len(hi_vals) <= 4) or lead >= 64:
        return "descriptor-table", scores
    if low8_zero >= 0.90:
        return "flat-audio", scores
    return "ambiguous", scores


def table_stats(words):
    """For a descriptor table: entries as (hi<<32)|lo, report the fragment stride
    (most common delta between consecutive low words within a contiguous run), the
    high-word tags, contiguous-run lengths (scatter-gather boundaries), and any low-bit
    wrap flags. Everything the driver needs to rebuild the table to match."""
    from collections import Counter

    # Only the leading table run (entries with a small high word); an oversized dump
    # catches unrelated memory past the table end.
    entries = []
    for i in range(len(words) // 2):
        hi, lo = words[2 * i + 1], words[2 * i]
        if hi > 3 or not (lo or hi):
            break
        entries.append((hi << 32) | lo)
    # low-bit flags: real fragment addresses are at least 4-aligned, so a set low
    # bit (0/1/2/3) is a flag (the wrap/IRQ marker on the last entry of a ring).
    flags = [(i, e & 0xF) for i, e in enumerate(entries) if e & 0x3]
    deltas = Counter()
    run, runs = 1, []
    tags = Counter()
    for i in range(1, len(entries)):
        tags[entries[i] >> 32] += 1
        d = (entries[i] & ~0xF) - (entries[i - 1] & ~0xF)
        if 0 < d < 0x10000:          # same-page forward step = the fragment stride
            deltas[d] += 1
            run += 1
        else:                        # jump to another region = scatter-gather boundary
            runs.append(run)
            run = 1
    runs.append(run)
    if entries:
        tags[entries[0] >> 32] += 1
    return {
        "n_entries": len(entries),
        "stride": deltas.most_common(1)[0][0] if deltas else 0,
        "stride_hist": dict(deltas.most_common(4)),
        "high_tags": dict(tags),
        "contig_runs": runs[:8],
        "flagged_entries": flags[:8],
    }


def audio_stats(words, channels):
    """For a flat-audio dump: peak and RMS as dBFS, treating words as 24-bit
    MSB-justified S32 (value = int32(word) >> 8, full-scale +/-2**23)."""
    import math

    peak = 0
    acc = 0.0
    for w in words:
        s = struct.unpack("<i", struct.pack("<I", w))[0] >> 8
        a = abs(s)
        if a > peak:
            peak = a
        acc += float(s) * s
    fs = float(1 << 23)
    rms = math.sqrt(acc / len(words)) if words else 0.0
    peak_db = 20 * math.log10(peak / fs) if peak else float("-inf")
    rms_db = 20 * math.log10(rms / fs) if rms else float("-inf")
    return peak, peak_db, rms_db


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--channels", type=int, default=0,
                    help="stream channel count (for flat-audio per-channel notes)")
    ap.add_argument("--hex", action="store_true", help="dump the first 8 u64 entries as hex")
    args = ap.parse_args()

    data, words = load_words(args.dump)
    verdict, scores = classify(words)
    print(f"{args.dump}: {len(data)} bytes, {len(words)} u32 words")
    print(f"  VERDICT: {verdict}")
    for k, v in scores.items():
        print(f"    {k:18s} {v:.4f}")

    if verdict == "descriptor-table":
        t = table_stats(words)
        print(f"    table entries      {t['n_entries']}")
        print(f"    fragment stride    0x{t['stride']:x} "
              f"({t['stride'] // 4} S32 words"
              + (f", = {t['stride'] // (args.channels * 4)} frames @ {args.channels}ch"
                 if args.channels else "") + ")")
        print(f"    stride histogram   { {hex(k): v for k, v in t['stride_hist'].items()} }")
        print(f"    high-word tags     { {hex(k): v for k, v in t['high_tags'].items()} }")
        print(f"    contiguous runs    {t['contig_runs']} (scatter-gather boundaries)")
        print(f"    flagged entries    {t['flagged_entries'] or 'none in dump (table longer than dump?)'}")

    if verdict == "flat-audio":
        peak, peak_db, rms_db = audio_stats(words, args.channels)
        print(f"    peak sample        {peak} ({peak_db:.1f} dBFS)")
        print(f"    rms                {rms_db:.1f} dBFS")
        print("    -> sample area holds real audio (pre-seeded)"
              if peak else "    -> flat but silent")
    elif verdict == "all-zero":
        print("    -> sample area is empty at capture time (NOT pre-seeded)")

    if args.hex:
        print("  first entries (u64 LE):")
        for i in range(min(8, len(words) // 2)):
            lo, hi = words[2 * i], words[2 * i + 1]
            print(f"    [{i:3d}] {hi:08x}_{lo:08x}")

    # Non-zero exit for ambiguous, so a scripted capture loop can flag it.
    sys.exit(0 if verdict in ("flat-audio", "descriptor-table", "all-zero") else 3)


if __name__ == "__main__":
    main()
