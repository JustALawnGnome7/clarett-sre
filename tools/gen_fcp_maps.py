#!/usr/bin/env python3
"""Generate paired fcp-server devmap + alsa-map drafts for the Clarett Thunderbolt models.

Grounded in the snd-clarett driver: air @ 174+i, mode @ 166+i (device byte 0=Mic,1=Line,2=Inst),
output gains strided at 32 (pairs at base+{0,1}, pairs step by 4), master mute @ 24, dim @ 73.
Mode enums carry the device byte explicitly where it is not the enum index, so the combo-jack models
offer Line/Inst only (bytes 1/2) and just the 8PreX offers Mic — see ENUM_LABELS.
"""
import json, collections, re

import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTDIR = os.path.join(ROOT, "fcp-server-data")
DRIVERDIR = os.path.join(ROOT, "driver")
OD = collections.OrderedDict

FCP_SET_MUX = 0x003002


def load_mux_bands(key):
    """The model's SET_MUX band tables from the de-blobbed driver/clarett_arm_<key>.h.
    Each clarett_armmux_<key>_b<i>[] is {u32 header (band << 16 | ...), u32 entry[]}."""
    text = open(os.path.join(DRIVERDIR, f"clarett_arm_{key}.h")).read()
    bands = {}
    for m in re.finditer(r"clarett_armmux_%s_b(\d+)\[\]\s*=\s*\{(.*?)\};" % key, text, re.S):
        bands[int(m.group(1))] = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{8})", m.group(2))]
    return bands


def band0_mux(key):
    """The model's band-0 routing table as [(src_pin, dst_pin)], padding entries dropped.

    SET_MUX payload is {u32 header, u32 entry[]} with header = band << 16, and each entry =
    (src_pin << 12) | dst_pin. entry == 0 is zero padding (dst 0 is not a valid pin); src == 0
    with a real dst means a present-but-unrouted destination. Mirrors clarett_arm_emit(CARM_MUX).
    """
    for words in load_mux_bands(key).values():
        if not words or words[0] >> 16 != 0:		# band 0 has header >> 16 == 0
            continue
        return [((e >> 12) & 0xfff, e & 0xfff) for e in words[1:] if e]
    return []


# Pin naming is DIRECTION-SCOPED and PER-MODEL — the same number means different things as a source
# vs a destination (0x408 = S/PDIF in as a source, Monitor Out 1 as a destination), and the smaller
# models remap (the 2Pre reaches S/PDIF *input* at 0x186/0x187 where the 8PreX has S/PDIF *output*,
# and its two analogue inputs are 0x400/0x402, skipping 0x401). So names are derived from the pins
# the model actually presents, by position within each group, never by a fixed table.
def name_destinations(dsts, slug=None):
    """dst pin -> (name, mixer_input_index or None), for the pins this model routes to."""
    out = OD()
    analogue = sorted(p for p in dsts if 0x400 <= p <= 0x409)
    # 8PreX-style: 0x408/0x409 are the monitor pair and the 0x400 block starts at Line Output 3.
    monitor_pair = 0x408 in dsts
    # Capture pins: use the model's record/loopback layout (loopback pins named, records renumbered).
    cap = capture_names(slug) if slug else {}

    # Order capture (PCM) sinks by their DISPLAY number, not register pin: the loopback pins are
    # numbered last (8PreX PCM 27/28, 2Pre PCM 13/14, ...) but sit mid-block by pin (0x60a/0x60b,
    # between PCM 10 and 11), so a pin-order list drops them mid-column in the routing window. Sort by
    # the PCM number so they land at the END of the PCM block; non-PCM destinations keep _dest_rank.
    def dest_key(pin):
        if 0x600 <= pin <= 0x61f and pin in cap:
            band, sub, _ = _dest_rank(pin)
            return (band, sub, int(cap[pin].rsplit(None, 1)[-1]))
        return _dest_rank(pin)

    for pin in sorted(dsts, key=dest_key):
        if 0x186 <= pin <= 0x187:
            out[pin] = (f"S/PDIF Output {pin - 0x186 + 1}", None)
        elif 0x200 <= pin <= 0x20f:
            out[pin] = (f"ADAT Output {pin - 0x200 + 1}", None)
        elif 0x300 <= pin <= 0x31f:
            out[pin] = (f"Mixer Input {pin - 0x300 + 1:02d}", pin - 0x300)
        elif pin in (0x408, 0x409):
            out[pin] = (f"Monitor Output {pin - 0x408 + 1}", None)
        elif 0x400 <= pin <= 0x407:
            n = analogue.index(pin) + (3 if monitor_pair else 1)
            out[pin] = (f"Line Output {n}", None)
        elif 0x600 <= pin <= 0x61f:
            out[pin] = (cap.get(pin, f"PCM {pin - 0x600 + 1:02d}"), None)
    return out


def alsa_sink_name(name):
    """Device name -> ALSA control name for a routing destination.

    alsa-scarlett-gui classifies a routing sink by its control name, and only accepts a hardware
    output whose name starts with "Analogue ", "S/PDIF " or "ADAT " (alsa.c is_elem_routing_snk).
    The physical names above ("Line Output 3", "Monitor Output 1" — what the front panel and the
    device's own port list call them) are kept in the devmap, and the analogue ones are presented
    to ALSA under the scarlett2 convention, which is what the USB Clarett+ 2Pre also reports.
    Numbering is preserved: the monitor pair is 1/2 and the 0x400 block continues from there.
    """
    for prefix in ("Line Output ", "Monitor Output "):
        if name.startswith(prefix):
            return "Analogue Output " + name[len(prefix):]
    return name


# Presentation order for the routing window: the GUI lists each Hardware group in the order the pins
# appear in the map, and pin order alone would show ADAT before Analogue (sources) and S/PDIF before
# Analogue (outputs). Sort by category rank so the conventional Analogue -> S/PDIF -> ADAT order (matching
# the USB Clarett) comes out; Mix and PCM are separate GUI groups so their rank only fixes internal order.
def _source_rank(pin):
    if 0x400 <= pin <= 0x407:                 return (0, pin)  # Analogue
    if pin in (0x408, 0x409) or 0x186 <= pin <= 0x187: return (1, pin)  # S/PDIF
    if 0x200 <= pin <= 0x20f:                 return (2, pin)  # ADAT
    if 0x300 <= pin <= 0x30f:                 return (3, pin)  # Mix
    if 0x600 <= pin <= 0x61f:                 return (4, pin)  # PCM
    return (9, pin)


def _dest_rank(pin):
    if pin in (0x408, 0x409):                 return (0, 0, pin)  # Monitor outputs (8PreX) — before Line
    if 0x400 <= pin <= 0x407:                 return (0, 1, pin)  # Line/Analogue outputs
    if 0x186 <= pin <= 0x187:                 return (1, 0, pin)  # S/PDIF output
    if 0x200 <= pin <= 0x20f:                 return (2, 0, pin)  # ADAT output
    if 0x300 <= pin <= 0x31f:                 return (3, 0, pin)  # Mixer input
    if 0x600 <= pin <= 0x61f:                 return (4, 0, pin)  # PCM
    return (9, 0, pin)


# Capture (record-output) layout, from each model's vendor XML <record-outputs>. The capture stream is a
# contiguous pin block 0x600.. whose entries are physical record inputs with a Loopback PAIR inserted at a
# model-specific position (2Pre after input 3, the others after input 9). So a plain "PCM = pin - 0x600 + 1"
# naming mislabels the two loopback pins as PCM and shifts every record channel after them. CAPTURE_CHANNELS
# is the total (records + 2 loopback); LOOPBACK_PINS are the two inserted pins.
CAPTURE_CHANNELS = {"clarett-2pre": 14, "clarett-4pre": 20, "clarett-8pre": 20, "clarett-8prex": 28}
LOOPBACK_PINS    = {"clarett-2pre": (0x604, 0x605), "clarett-4pre": (0x60a, 0x60b),
                    "clarett-8pre": (0x60a, 0x60b), "clarett-8prex": (0x60a, 0x60b)}

def capture_record_pins(slug):
    """Ordered capture pins that are physical record channels (loopback excluded). Index i is record
    input i -> PCM (i+1) -> meter slot i."""
    loop = LOOPBACK_PINS[slug]
    return [p for p in range(0x600, 0x600 + CAPTURE_CHANNELS[slug]) if p not in loop]

def capture_names(slug):
    """capture pin -> display name. Every capture channel is a PCM input: the physical records are
    numbered PCM 1..N by input order, and the inserted loopback pair continues that numbering
    (2Pre -> PCM 13/14, 4Pre/8Pre -> PCM 19/20, 8PreX -> PCM 27/28) rather than forming a separate
    "Loopback" category. The loopback pins sit mid-block by register order but are numbered as if
    appended after the records."""
    loop = LOOPBACK_PINS[slug]
    n_records = CAPTURE_CHANNELS[slug] - len(loop)
    out, n = OD(), 0
    for p in range(0x600, 0x600 + CAPTURE_CHANNELS[slug]):
        if p in loop:
            out[p] = f"PCM {n_records + loop.index(p) + 1:02d}"
        else:
            n += 1
            out[p] = f"PCM {n:02d}"
    return out


def name_sources(srcs):
    """src pin -> name. Pin 0 ("Off") is excluded: fcp-server rejects a router-pin <= 0."""
    out = OD()
    analogue = sorted(p for p in srcs if 0x400 <= p <= 0x407)
    for pin in sorted((p for p in srcs if p), key=_source_rank):
        if 0x186 <= pin <= 0x187:
            out[pin] = f"S/PDIF {pin - 0x186 + 1}"
        elif 0x200 <= pin <= 0x20f:
            out[pin] = f"ADAT {pin - 0x200 + 1}"
        elif 0x300 <= pin <= 0x30f:
            out[pin] = f"Mix {chr(ord('A') + pin - 0x300)}"
        elif pin in (0x408, 0x409):
            out[pin] = f"S/PDIF {pin - 0x408 + 1}"
        elif 0x400 <= pin <= 0x407:
            out[pin] = f"Analogue {analogue.index(pin) + 1}"
        elif 0x600 <= pin <= 0x61f:
            out[pin] = f"PCM {pin - 0x600 + 1}"
    return out

# --- GET_METER slot map (peak-index) -------------------------------------------------------------
# fcp-server needs a "peak-index" on a source/destination to build its meter map: the raw GET_METER
# slot that channel's level lands in. USB FCP devices read these from the device's own devmap; the
# Clarett TB line serves none (DEVMAP_INFO returns size 0), so they have to be MEASURED — put signal
# on one channel at a time and see which slot moves (tools/fcp_meter_watch.c).
# ---------------------------------------------------------------------------------------------------
# SOURCE peak-indexes: NONE. The Clarett meters DESTINATIONS ONLY.
#
# This table used to carry twelve "physical input" slots per model. That reading was wrong, and wrong in
# a way that could not be caught by the measurements that produced it: the device's default routing sends
# Analogue 1 -> PCM 1, S/PDIF -> PCM 3/4 and ADAT -> PCM 5-12, so "signal on Analogue 1 lights slot 0" is
# predicted identically by "slot 0 meters the input" and "slot 0 meters PCM 1". Every historical
# measurement was taken under default routing and so could not distinguish them.
#
# Re-routing tells them apart, and it was measured on a 2Pre July 23 2026: with signal on Analogue 1,
# moving that input from PCM 1 to PCM 3 moved the meter from slot 0 to slot 2. The slot follows the PCM
# CHANNEL, not the input. So slots 0..n-1 are the record/capture destinations.
#
# The whole 48-slot array is destinations, which is why it is exactly 48 on a 2Pre:
#   PCM 01-12 (12 record channels; the 2 loopback channels are NOT metered)
#   Line Output 1-4 (12-15), S/PDIF Output 1-2 (16-17, dark - no router destination on this unit)
#   Mixer Input 01-30 (18-47)
# The record-channel count is what sets the base for everything after it: 12 on the 2Pre, 18 on the
# 4Pre/8Pre (capture_channels minus the 2 loopback pins).
#
# 4Pre: RESOLVED July 24 2026 on hardware (tools/fcp_meter_watch, graded-level attribution — each test
# destination fed a distinct PCM playback source at a distinct level, with all default routing zeroed so
# every lit slot maps to exactly one destination by its level). It is HYPOTHESIS (b):
#   [PCM 01-18 record: 0-17][Line Output 1-6: 18-23][S/PDIF Output 1-2: 24-25][2 unidentified: 26-27]
#   [Mixer Input 01-30: 28-57]
# Directly measured, each alone: Analogue Output 1 -> 18, Analogue Output 6 -> 23, S/PDIF Output 1 -> 24,
# Mixer Input 01 -> 28, Mixer Input 30 -> 57 (the last from a run-2/run-3 differential; the rest from the
# clean zeroed run). Outputs starting at 18 (not 20) is what killed hypothesis (a): the loopback pair
# (PCM 19-20) is NOT metered, exactly as on the 2Pre, so the record block is 18 wide and the 2-slot
# residual is a genuine pair of reserved/unidentified slots at 26-27 (analogous to the 2Pre's dark S/PDIF
# 16-17 — present in the array, unlit on this unit, mapped to no destination). Loopback-via-PCM-playback
# was separately confirmed unmetered here: PCM 01 <- PCM 1 added no slot. The physical-input record slots
# 0-17 keep their historical "reinterpreted" provenance (this session drove destinations from PCM
# playback, which does not exercise the input meters). The old "26+ (28/29 seen)" note was contaminated
# by live default routing; the zeroed run supersedes it.
METER_SLOTS = {}

# Destination (and mixer-input) peak-indexes. These sit past what METER_INFO advertises: fcp-server
# used to reject any index >= that count, and ONE out-of-range entry discards the whole meter map
# (meter.c goto done), so this table was empty. That bound is now treated as a floor - fcp-server
# raises it to cover the map when the device reports no slot count (see fcp-support server/meter.c,
# max_peak_index) - which is justified because the count is provably not the array size: the 2Pre
# answers 00 02 0c 00 (2 x 12 = 24) and yet reads a real, identifiable signal at slot 47.
# REQUIRES that fcp-support change; without it these entries take the whole map down.
METER_SLOTS_DST = {
    # Measured on a 2Pre (July 23 2026): a -6 dBFS tone played through PCM 1-2 -> Line Output 1-2 lit
    # slots 12/13 at exactly 2047 (= -6 dBFS of 4095), and NOTHING at 14+. That the PCM streams themselves
    # were driven (4-ch tone) but no PCM slot lit means the device meters the physical OUTPUTS, not the
    # PCM sources. So the outputs pack 12..15, immediately after the 12 input slots (0-11) — all inside the
    # 24-slot METER_INFO bound (resp = 00 02 0c 00 -> 2*12), so fcp-server accepts them. Re-routing the
    # same tone to Line Output 3-4 then lit 14/15, confirming the packing: all four are measured.
    #
    # THE FULL LAYOUT, measured on the 2Pre July 23 2026 (tools/fcp_meter_watch.c, one route at a time):
    #   [all physical inputs][all physical outputs: line, then S/PDIF][Mixer Input 01-30]
    # 2Pre: inputs 0-11, Line Output 1-4 at 12-15, S/PDIF Output at 16-17, mixer inputs 18-47 = 48 slots,
    # exactly the array GET_METER serves. Confirmed by routing one input to Mixer Input 01/02/05/13/30 and
    # watching slots 18/19/22/30/47 rise, each alone. The meter sits PRE-MIX: slot 18 stayed lit with the
    # Mix A gain pulled to -inf, which is what distinguishes it from a mix-bus meter.
    # 16/17 are never lit on this unit because its router has NO S/PDIF output destination (confirmed by
    # dumping the live mux table with tools/fcp_mux_probe: 0x600-0x60d, 0x400-0x403, 0x300-0x31d only).
    # The 4Pre follows the same rule with a twist: 18 record, line outputs 18-23, S/PDIF 24-25, then a
    # 2-slot reserved gap (26-27) before mixer inputs at 28-57 — so its mixer base is 12 + n_out + 2, not
    # 12 + n_out. See the RESOLVED July 24 note above.
    "clarett-2pre": {
        # PCM 01-12, the physical record channels, at slots 0-11 (loopback pins 0x604/0x605 sit BETWEEN
        # record inputs 3 and 4 and are NOT metered, so the record pins skip them — capture_record_pins).
        # PCM 1 -> slot 0 and PCM 3 -> slot 2 were measured directly by re-routing one input between them;
        # the rest follow the stride and were each seen lit under default routing.
        **{pin: (i, "measured" if i in (0, 2) else "stride")
           for i, pin in enumerate(capture_record_pins("clarett-2pre"))},
        1024: (12, "measured"), 1025: (13, "measured"),  # Line Output 1-2 (Monitor L/R)
        1026: (14, "measured"), 1027: (15, "measured"),  # Line Output 3-4
        # Mixer Input 01-30 at pins 0x300.. -> slots 18.. (01/02/05/13/30 measured, rest by the stride)
        **{0x300 + i: (18 + i, "measured" if i in (0, 1, 4, 12, 29) else "stride")
           for i in range(30)},
    },
    # 4Pre: full layout measured July 24 2026 (see the RESOLVED note above). Directly lit, each alone:
    # Line Output 1 -> 18, Line Output 6 -> 23, S/PDIF Output 1 -> 24, Mixer Input 01 -> 28,
    # Mixer Input 30 -> 57. The record block (0-17) keeps its historical "reinterpreted" provenance —
    # this session drove destinations from PCM playback, which does not exercise the input meters.
    "clarett-4pre": {
        # PCM 01-18 physical record channels at slots 0-17 (loopback pins 0x60a/0x60b sit between record
        # inputs 9 and 10 and are NOT metered; the record pins skip them). Outputs start at slot 18.
        **{pin: (i, "reinterpreted") for i, pin in enumerate(capture_record_pins("clarett-4pre"))},
        # Line Output 1-6 at 18-23 (1 and 6 measured, rest by stride)
        **{0x400 + i: (18 + i, "measured" if i in (0, 5) else "stride") for i in range(6)},
        # S/PDIF Output 1-2 at 24-25 (Output 1 measured; slots 26-27 are an unidentified reserved pair)
        0x186: (24, "measured"), 0x187: (25, "stride"),
        # Mixer Input 01-30 at 28-57 (01 and 30 measured, rest by stride)
        **{0x300 + i: (28 + i, "measured" if i in (0, 29) else "stride") for i in range(30)},
    },
    # 8Pre: FULL 70-slot layout, MEASURED on hardware Aug 6 2026 (tools/fcp_meter_watch). It is the 8PreX
    # pattern scaled down — the 8Pre's output inventory matches the 8PreX EXACTLY except ADAT is 8-wide (one
    # optical port) not 16, and the record block is 18 (capture 20 - 2 loopback) not 26. Like the 8PreX (and
    # unlike the 4Pre) LOOPBACK IS METERED, packed right after the outputs and before the mixer.
    #   0-17   PCM 01-18 record   (0x600.. minus loopback)
    #   18-19  Monitor Output 1-2 (0x408/0x409)
    #   20-27  Line Output 3-10   (0x400-0x407)
    #   28-29  S/PDIF Output 1-2  (0x186/0x187)
    #   30-37  ADAT Output 1-8    (0x200-0x207)
    #   38-39  PCM 19-20 = loopback pins (0x60a/0x60b) — MEASURED metered (route to Loopback 1 lit slot 38)
    #   40-69  Mixer Input 01-30  (0x300-0x31d)
    # Anchors read directly: Mixer In 01 -> 40 (PCM 1 routed in) and Mixer In 30 -> 69 (PCM 2), delta 29
    # confirming the +1 stride; loopback 38-39 confirmed. The base 40 (not 38) proved the two loopback slots
    # sit before the mixer, exactly the 8PreX packing. The output block (18-37) is transitively confirmed:
    # loopback landing at 38 places the outputs' end at 37 and the record block's end at 17.
    "clarett-8pre": {
        **{pin: (i, "stride") for i, pin in enumerate(capture_record_pins("clarett-8pre"))},
        0x408: (18, "stride"), 0x409: (19, "stride"),           # Monitor Output 1-2
        **{0x400 + i: (20 + i, "stride") for i in range(8)},    # Line Output 3-10
        0x186: (28, "stride"), 0x187: (29, "stride"),           # S/PDIF Output 1-2
        **{0x200 + i: (30 + i, "stride") for i in range(8)},    # ADAT Output 1-8
        0x60a: (38, "measured"), 0x60b: (39, "stride"),         # PCM 19-20 = loopback pins (metered)
        **{0x300 + i: (40 + i, "measured" if i in (0, 29) else "stride")
           for i in range(30)},                                 # Mixer Input 01-30
    },
    # 8PreX: FULL 86-slot layout, MEASURED on hardware (July 29-30 2026, tools/fcp_meter_watch, one
    # destination at a time; "measured" = read directly, "stride" = filled between measured anchors).
    # Two ways it differs from the 2Pre/4Pre: the analogue outputs pack in CATEGORY order (Monitor before
    # Line), not pin order; and LOOPBACK IS METERED (slots 54-55, after the outputs) where the 2Pre/4Pre
    # leave those slots dark. Anchors read: PCM 01->0, Monitor Out 1->26, S/PDIF Out 1->36, ADAT Out 1->38,
    # ADAT Out 16->53, PCM 27 (loopback pin)->54, Mixer In 01->56, Mixer In 30->85.
    #   0-25   PCM 01-26 record         (0x600.. minus loopback)
    #   26-27  Monitor Output 1-2       0x408/0x409
    #   28-35  Line Output 3-10         0x400-0x407
    #   36-37  S/PDIF Output 1-2        0x186/0x187
    #   38-53  ADAT Output 1-16         0x200-0x20f
    #   54-55  PCM 27-28 (loopback pins) 0x60a/0x60b
    #   56-85  Mixer Input 01-30        0x300-0x31d
    "clarett-8prex": {
        **{pin: (i, "measured" if i == 0 else "stride")
           for i, pin in enumerate(capture_record_pins("clarett-8prex"))},
        0x408: (26, "measured"), 0x409: (27, "stride"),		# Monitor Output 1-2
        **{0x400 + i: (28 + i, "stride") for i in range(8)},	# Line Output 3-10
        0x186: (36, "measured"), 0x187: (37, "stride"),		# S/PDIF Output 1-2
        **{0x200 + i: (38 + i, "measured" if i in (0, 15) else "stride")
           for i in range(16)},					# ADAT Output 1-16
        0x60a: (54, "measured"), 0x60b: (55, "stride"),		# PCM 27-28 = loopback pins (metered on the 8PreX)
        **{0x300 + i: (56 + i, "measured" if i in (0, 29) else "stride")
           for i in range(30)},					# Mixer Input 01-30
    },
}

# per-model: mode_label, n_analogue (air on all), and per-input mode enum kind (see ENUM_LABELS)
MODELS = {
    # pcm_out = PCM playback channel count (GET_7.2 / driver playback_channels). The router exposes one
    # PCM source pin per playback channel (0x600 + i), but a captured band-0 table only routes a handful
    # by default, so the full range is added explicitly rather than harvested (see the src_pins union).
    "clarett-2pre":  dict(name="Clarett 2Pre",  mode_label="Level", init="2pre",
                          n_analogue=2, outputs=4, pcm_out=4, mix_out=16,
                          modes={0: "li2", 1: "li2"}),
    "clarett-4pre":  dict(name="Clarett 4Pre",  mode_label="Level", init="4pre",
                          n_analogue=4, outputs=6, pcm_out=8, mix_out=16,
                          modes={0: "li2", 1: "li2"}),
    "clarett-8pre":  dict(name="Clarett 8Pre",  mode_label="Level", init=None,
                          n_analogue=8, outputs=10, pcm_out=20, mix_out=16,
                          modes={0: "li2", 1: "li2"},
                          # No capture for this model: the fcp-server map's routing is constructed
                          # instead (synth_band0_8pre). The driver arms its own captured routing from
                          # clarett_arm_8pre.h, so nothing driver-side consumes this table.
                          synth_mux=lambda: synth_band0_8pre(),
                          synth_sources=lambda: synth_sources_8pre()),
    "clarett-8prex": dict(name="Clarett 8PreX", mode_label="Level", init="8prex",
                          n_analogue=8, outputs=10, pcm_out=28, mix_out=16,
                          modes={0: "mli3", 1: "mli3",
                                 2: "ml2", 3: "ml2", 4: "ml2",
                                 5: "ml2", 6: "ml2", 7: "ml2"}),
}
# Mode enum per kind. The device byte is always 0=Mic, 1=Line, 2=Inst (line-wide encoding).
#
# The 8PreX has SEPARATE XLR and 1/4" jacks per input, so software picks the path and Mic is a real
# setting: its enums start at Mic and are plain lists, whose index IS the device byte.
#
# The 2Pre/4Pre/8Pre have a single combo XLR/TRS jack per input that cannot take both plugs at once,
# so the hardware selects Mic itself when an XLR is inserted and software only chooses Line vs Inst
# (spec/provenance/clarett-control-plane.md §4). Those get the {name, value} form so the device bytes stay 1/2
# with no Mic entry — offering a Mic that the jack decides would be offering a setting that does
# nothing. REQUIRES the fcp-server patch that accepts the object form in input-controls (the same
# form global-controls.c already took).
ENUM_LABELS = {
    "mli3": ["Mic", "Line", "Inst"],
    "ml2":  ["Mic", "Line"],
    "li2":  [OD([("name", "Line"), ("value", 1)]), OD([("name", "Inst"), ("value", 2)])],
}

# <meter-source> [XML]: which input bank the front-panel hardware meter bridge displays. The selected
# value is written to config offset 184 and committed with DATA_CMD activate 8. Device values are
# non-contiguous (1/2/4/8), so the {name, value} enum form is required. Only the 8PreX offers a real
# choice; the 4Pre/8Pre XML has Analogue only (a one-option control, not worth exposing) and the 2Pre
# has no <meter-source> element at all — so this table is the gate for the whole control.
METER_SOURCE = {
    "clarett-8prex": [OD([("name", "Analogue"), ("value", 1)]),
                      OD([("name", "S/PDIF"),   ("value", 2)]),
                      OD([("name", "ADAT 1"),   ("value", 4)]),
                      OD([("name", "ADAT 2"),   ("value", 8)])],
}

# <spdif-mode> [XML]: which physical connector S/PDIF uses. The XML models it as one control backed by
# two 2-bit fields, both activate 4: <input> @132 (connector captured) and <output> @124 (connector
# driven). The in-kernel scarlett2 driver (ALSA, no fcp-server) handles the sibling USB Clarett/Clarett+
# line and exposes only ONE control ("S/PDIF Source Capture Enum") writing only the input offset
# (SCARLETT2_CONFIG_SPDIF_MODE, 0x9e on the USB map); it never touches the output offset and cannot set
# the two connectors independently. We deliberately go further and expose BOTH as independent enums —
# input as scarlett2's "S/PDIF Source Capture Enum" (so alsa-scarlett-gui still recognises it) and
# output as "S/PDIF Source Playback Enum". That second name required two companion changes, both in
# alsa-scarlett-gui (fcp-server creates either enum generically, no change): its routing-sink parser
# (alsa.c is_elem_routing_snk) had to stop treating an "S/PDIF ... Playback Enum" whose name carries no
# channel number as a router sink (it aborted the GUI), and its Device Settings tab
# (config-device-settings.c) now renders both dropdowns. Enum values are scarlett2's exactly:
# None=0 / Optical=1 / RCA=2 (contiguous, index == device byte). The 2Pre is optical-only (its
# <spdif-mode> offers Optical alone), so it gets neither control, matching scarlett2, which omits it
# there. [mixer_scarlett2.c, driver/clarett.h, spec/clarett-interface.md]
SPDIF_SOURCE_ENUM = [OD([("name", "None"),    ("value", 0)]),
                     OD([("name", "Optical"), ("value", 1)]),
                     OD([("name", "RCA"),     ("value", 2)])]
SPDIF_SOURCE = {"clarett-4pre", "clarett-8pre", "clarett-8prex"}


# --- Clarett 8Pre band-0 router table (no capture exists for this model) -------------------------
#
# Every other model's routing comes from its captured bring-up blob. There is no 8Pre capture, and
# without a band-0 table naming the 8Pre's own destinations the device is left holding the 2Pre's
# routing after the arm, whereupon fcp-server refuses to create ANY routing control.
#
# So this table is CONSTRUCTED, and its two halves have very different provenance:
#
#  * The capture half is the 4Pre's captured table verbatim. The two models have identical input
#    geometry — 8 analogue at 0x400-0x407, S/PDIF at 0x408/0x409, 8 ADAT at 0x200-0x207, 20 capture
#    channels — so the 4Pre's own vendor-configured block applies unchanged, loopback included.
#
#  * The output half is AUTHORED. The captured tables are user sessions, not factory defaults (the
#    8PreX's routes analogue inputs straight to its ADAT outputs), so there is nothing to copy. It
#    follows the 4Pre's pattern of mixes landing on the analogue outputs in order, and leaves the
#    digital outputs and all 30 mixer inputs unrouted — the same src=0 the vendor tables use for
#    destinations they aren't feeding. Nothing is fed into the mixer, so the device starts silent
#    rather than making up a patch that might surprise someone's monitors.
#
# Destination pins and their names are [XML] (vendor-reference/Devices/Clarett 8Pre.xml), which was
# cross-checked against all three captured models: for each of them the XML's output pin list is
# exactly the set of physical destinations in the live band-0 table.
def synth_band0_8pre():
    pairs = []

    # Capture: analogue -> PCM 1-8, S/PDIF -> 9/10, loopback -> 11/12, ADAT -> 13-20 [4Pre TRACE]
    pairs += [(0x400 + i, 0x600 + i) for i in range(8)]
    pairs += [(0x408, 0x608), (0x409, 0x609)]
    pairs += [(0x600, 0x60a), (0x601, 0x60b)]
    pairs += [(0x200 + i, 0x60c + i) for i in range(8)]

    # Physical outputs [AUTHORED]: Mix A/B -> the monitor pair, Mix C-J -> Line Output 3-10
    pairs += [(0x300, 0x408), (0x301, 0x409)]
    pairs += [(0x302 + i, 0x400 + i) for i in range(8)]
    pairs += [(0, 0x186), (0, 0x187)]                       # S/PDIF out, unrouted
    pairs += [(0, 0x200 + i) for i in range(8)]             # ADAT out, unrouted

    # Mixer inputs [AUTHORED]: all 30 present, none fed
    pairs += [(0, 0x300 + i) for i in range(30)]

    return pairs


# The full source set, which a band-0 table cannot give: it records the source currently patched to
# each destination, so an unrouted source simply doesn't appear (which is why the 2Pre's map offers
# 2 of its 16 mix buses and 2 of its 4 playback streams). fcp-server validates destinations against
# the live table but not sources, so the 8Pre — the one model whose map isn't blob-derived — lists
# every source the hardware has. [XML] Clarett 8Pre.xml: 8 analogue, 2 S/PDIF, 8 ADAT, 20 playback,
# plus the 16 mix buses shared line-wide.
def synth_sources_8pre():
    return ([0x400 + i for i in range(8)] + [0x408, 0x409] +
            [0x200 + i for i in range(8)] +
            [0x600 + i for i in range(20)] +
            [0x300 + i for i in range(16)])


def mode_key(kind):
    """alsa-map control-type key for a mode enum kind: by arity, so a model's kinds never collide
    (the 8PreX has a 3-way and a 2-way; every other model has only one kind)."""
    return f"mode{len(ENUM_LABELS[kind])}"

def out_index(n):        # physical output n (0-based) -> array index onto the strided gain region
    return (n // 2) * 4 + (n % 2)

def member(offset, typ, shape=None, nd=0, nc=1, note=None):
    m = OD(offset=offset, type=typ)
    if shape is not None:
        m["array-shape"] = [shape]
    m["notify-device"] = nd
    m["notify-client"] = nc
    if note:
        m["_note"] = note
    return m

for slug, spec in MODELS.items():
    na, nout, ml = spec["n_analogue"], spec["outputs"], spec["mode_label"]
    modes = spec["modes"]
    out_shape = out_index(nout - 1) + 1

    # ---- devmap ----
    # notify-device is the DATA_CMD{activate} the device needs to COMMIT a write (fcp-server issues
    # it via fcp_data_notify after the write). From the snd-clarett driver: air=7, mode=6, gain=1,
    # monitor mute/dim=2. Without it the SET_DATA stages but never manifests.
    # notify-client is the mask fcp-server re-reads a control on when the DEVICE announces a change.
    # It costs a mailbox round trip per control per notification, and this device notifies steadily,
    # so it is set only where the device can actually change the value behind us: mute, dim and the
    # monitor gain (the front-panel knob). Air/mode/SW-HW are host-owned — there is no front-panel
    # control for any of them on this line, so nothing but us ever writes them — and the firmware
    # version is static. See spec/provenance/clarett-control-plane.md and the config-ownership notes.
    members = OD()
    members["versionStageRelease"] = member(0, "uint32", nd=0, nc=0, note="PLACEHOLDER offset; see _todo")
    members["muteSwitch"] = member(24, "bool", nd=2, note="monitor mute @ 24; activate 2. 1 = muted, 0 = unmuted (trace-confirmed, and confirmed on a 2Pre) - no inversion, same as dim @ 28")
    members["dimSwitch"]  = member(28, "bool", nd=2, note="monitor dim @ 28; activate 2")
    # The monitor section's 8-bit gain, which is where the front-panel knob's position shows up:
    # one of the three monitor bytes (24/28/112) the device refreshes on a front-panel notification.
    # Read-only — the knob is the master and software cannot override it.
    members["monitorVolume"] = member(112, "uint8", nd=2, note="monitor gain @ 112; attenuation code, same encoding as the output gains; read-only reflection of the front-panel knob")
    members["air"]  = member(174, "bool",  shape=na, nd=7, nc=0, note="per-input air @ 174+i; commit activate 7")
    members["mode"] = member(166, "uint8", shape=na, nd=6, nc=0, note="per-input mode @ 166+i, byte 0=Mic/1=Line/2=Inst; commit activate 6")
    members["outputVolume"] = member(32, "uint8", shape=out_shape, nd=1, note="attenuation code 0..127; strided gains (pairs at base+{0,1}, pairs step 4); commit activate 1")
    # SW/HW per output: enable-hardware-gain, one BIT per output, two outputs to a byte, the bytes
    # stepping by 4 exactly as the gains do — byte 52 + 4*(out/2), bit out%2 [XML, all four models].
    # Set = the monitor knob drives that output (HW); clear = the software fader does (SW). One
    # member per byte, since a control addresses a bit within one member.
    for pair in range((nout + 1) // 2):
        members[f"hwGainEnable{pair}"] = member(52 + 4 * pair, "bool", nd=3, nc=0,
                                                note=f"enable-hardware-gain bits for outputs "
                                                     f"{2*pair+1}/{2*pair+2} @ {52 + 4*pair}, "
                                                     f"bit 0/1; commit activate 3")

    # <meter-source> [XML]: front-panel meter-bridge bank selector (8PreX only). Value written to
    # offset 184, committed with DATA_CMD activate 8. Host-owned (nc=0): no front-panel control
    # changes it behind us.
    if slug in METER_SOURCE:
        members["meterSource"] = member(
            184, "uint8", nd=8, nc=0,
            note="front-panel meter-bridge source @ 184; commit activate 8; "
                 "enum Analogue=1/S/PDIF=2/ADAT 1=4/ADAT 2=8 [XML]")

    # <spdif-mode> [XML]: S/PDIF connector select, exposed as two independent enums (input @132,
    # output @124), both commit activate 4. Host-owned (nc=0): nothing changes them behind us. See the
    # SPDIF_SOURCE comment above for the scarlett2 relationship and the alsa-scarlett-gui companions.
    if slug in SPDIF_SOURCE:
        members["spdifSourceInput"] = member(
            132, "uint8", nd=4, nc=0,
            note="S/PDIF input connector @ 132; commit activate 4; enum None=0/Optical=1/RCA=2 "
                 "(scarlett2 SCARLETT2_CONFIG_SPDIF_MODE input offset)")
        members["spdifSourceOutput"] = member(
            124, "uint8", nd=4, nc=0,
            note="S/PDIF output connector @ 124; commit activate 4; enum None=0/Optical=1/RCA=2 "
                 "(XML <spdif-mode><output>; scarlett2 does not expose this)")

    phys_in = []
    for i in range(na):
        ctrls = OD()
        ctrls["air"] = OD(index=i, member="air")
        mt = modes.get(i)
        if mt:
            ctrls[mode_key(mt)] = OD(index=i, member="mode")
        phys_in.append(OD(name=f"Analogue {i+1}", controls=ctrls))

    phys_out = []
    for n in range(nout):
        phys_out.append(OD(name=f"Output {n+1}", controls=OD([
            # "index" addresses the member: a byte index for the gain, a BIT index for the enable.
            # The control's NAME comes from the output's position here, not from index — which
            # needs the fcp-server patch that separates the two, or the strided gains come out
            # named "Line 1, Line 2, Line 5, Line 6".
            ("level",   OD(index=out_index(n), member="outputVolume")),
            ("hw-gain", OD(index=n % 2, member=f"hwGainEnable{n // 2}")),
        ])))

    # ---- routing / mixer tables, recovered from the model's own bring-up blob ----
    # The band-0 SET_MUX the driver replays at arm IS the device's live routing table, so the pins it
    # names are exactly the ones GET_MUX will report back — which matters because fcp-server aborts
    # ALL mux controls if a devmap destination's router-pin is absent from the live table.
    # router-pin is a STRING parsed with atoi(), i.e. decimal (fcp-server's error message prints it
    # as 0x%s, which is misleading — a device-provided devmap may well use another convention).
    dev_sources, dev_dests, alsa_sources, alsa_sinks = [], [], [], []
    pairs = band0_mux(spec["init"]) if spec["init"] else spec.get("synth_mux", lambda: [])()
    if pairs:
        # Sources: from the table for a captured model (all it can tell us), from the hardware's own
        # full list where we have one (see synth_sources_8pre).
        src_pins = set(spec.get("synth_sources", lambda: None)() or {s for s, _ in pairs})
        # Always expose the full PCM playback range: a captured band-0 table only routes a few PCM pins
        # by default (the 4Pre default routes only PCM 1/2/7/8), but every playback channel is a valid
        # router source. Add 0x600 .. 0x600+pcm_out-1 so all of PCM 1..pcm_out are selectable.
        src_pins |= {0x600 + i for i in range(spec.get("pcm_out", 0))}
        # Same story for the mix-bus outputs (Mix A..): a captured band-0 table only routes the buses
        # the default patch uses (the 2Pre routes only A/B, the 4Pre A-F), but every Clarett Plus model
        # has the full set of mix buses — [XML] <mixes num="16"> on the 2Pre/4Pre/8Pre alike — and each
        # is a valid router source. Add 0x300 .. 0x300+mix_out-1 so all of Mix A..P are selectable.
        # (Focusrite Control's own GUI surfaces only A-J, a subset, as it does 18 of the 30 mix inputs.)
        src_pins |= {0x300 + i for i in range(spec.get("mix_out", 0))}
        src_names = name_sources(src_pins)
        dst_names = name_destinations({d for _, d in pairs}, slug)
        assert len(set(src_names.values())) == len(src_names), f"{slug}: duplicate source name"
        assert len(set(n for n, _ in dst_names.values())) == len(dst_names), f"{slug}: dup sink name"

        meter_src = METER_SLOTS.get(slug, {})
        meter_dst = METER_SLOTS_DST.get(slug, {})

        for pin, nm in src_names.items():
            entry = OD([("name", nm), ("router-pin", str(pin))])
            if pin in meter_src:
                slot, how = meter_src[pin]
                entry["peak-index"] = slot
                entry["_peak-index-provenance"] = how
            dev_sources.append(entry)
            alsa_sources.append(OD([("device_name", nm), ("alsa_name", nm)]))
        for pin, (nm, mix_idx) in dst_names.items():
            entry = OD([("name", nm), ("router-pin", str(pin))])
            if pin in meter_dst:
                slot, how = meter_dst[pin]
                entry["peak-index"] = slot
                entry["_peak-index-provenance"] = how
            if mix_idx is not None:
                # Marks this destination as a mixer input; fcp-server addresses the SET_MIX matrix
                # as mix_output * mix_input_count + mixer-input-index (dims come from MIX_INFO).
                entry["mixer-input-index"] = mix_idx
            dev_dests.append(entry)
            alsa_sinks.append(OD([("device_name", nm), ("alsa_name", alsa_sink_name(nm))]))

    devmap = OD()
    devmap["_note"] = (f"DRAFT — hand-authored device-map for the {spec['name']} (Thunderbolt), paired with "
                       f"fcp-alsa-map-{slug}.json for fcp-server. Loaded from file so fcp-server can create "
                       "controls when the device's own DEVMAP_READ (0x80000d) is unavailable/silent. NOT a "
                       "substitute for the device-provided devmap: offsets/types are carried from the "
                       "snd-clarett driver (see _todo). Replace with the /tmp/fcp-devmap-" + slug + "-*.json "
                       "dump once DEVMAP_READ works.")
    devmap["_schema"] = ("structs.<STRUCT>.members.<name> = { offset:int, type:string, notify-device:int, "
                         "notify-client:int, array-shape?:[int] }. device-specification.physical-{inputs,"
                         "outputs}[] = { name, controls: { <type>: { index:int, member:string } } }; read "
                         "offset = member.offset + index*width, and index+1 fills the %d in the alsa-map name.")
    devmap["_provenance"] = ("Offsets from the snd-clarett driver + spec/provenance/clarett-control-plane.md: air @ 174+i, "
                             "mode @ 166+i (0=Mic/1=Line/2=Inst), output gains strided at 32, mute @ 24, dim @ 73. "
                             "Clean-room: authored from our own interface facts, not any vendor devmap.")
    devmap["_todo"] = [
        "Replace wholesale with the device-provided devmap once DEVMAP_READ answers.",
        "versionStageRelease.offset is a PLACEHOLDER (real firmware-version location unknown).",
        "notify-device carries the DATA_CMD{activate} the device needs to COMMIT a write (fcp-server "
        "sends it via DATA_NOTIFY after the write): air=7, mode=6, gain=1, monitor mute/dim=2. Without "
        "it the write stages but does not manifest. muteSwitch is active-low (device byte 1 = unmuted), "
        "handled in the alsa-map with invert/invert-base 1.",
        "air/mode reads at 174+i / 166+i returned non-zero, plausible values on hardware (contradicting "
        "the earlier 'reads back 0' caveat); still unverified whether they track physical state live.",
        "outputVolume is a strided array; physical-outputs index onto the real gains (pairs at base+{0,1}, "
        "pairs step by 4). Consequence: the alsa-map output names come out sparse (from index+1), a cosmetic "
        "artifact of the layout, to be fixed with the real devmap.",
        "Output mute offsets are not yet captured, so output mute is omitted (here and in the alsa-map).",
        "peak-index is present for the 4Pre ONLY, and each entry carries _peak-index-provenance: "
        "\"measured\" (analogue in 1-4 -> slots 0-3, one signal source at a time), \"block-measured\" "
        "(slots 18-23 lit together as the six line outputs, so the set is measured but the order "
        "within it is not), or \"inferred\" (continuing the pin block: analogue 5-8, S/PDIF, ADAT). "
        "Other models have NO peak-index and so get no Level Meter: slot order is not transferable "
        "across this line (ADAT width differs 8 vs 16, and the 2Pre's analogue pins are 0x400/0x402 "
        "skipping 0x401), so each model must be measured with tools/fcp_meter_watch.c. Also unverified: "
        "fcp-server rejects any peak-index >= the slot count the device reports via fcp_meter_info(), "
        "and we have assumed 48 from the request payload rather than reading that answer back.",
        "notify-client masks are guesses (the TB device does not expose the FCP notification word; the driver "
        "relays a wildcard ~0).",
        "sources/destinations are derived from the band-0 SET_MUX table in the driver's bring-up blob "
        "(tools/gen_fcp_maps.py parses clarett_arm_<model>.h), so the router-pins are exactly what the "
        "device reports via GET_MUX after our arm. Pin meaning is DIRECTION-SCOPED and per-model: 0x408 "
        "is S/PDIF-in as a source but Monitor Out 1 as a destination, the 2Pre reaches S/PDIF input at "
        "0x186/0x187 (where the 8PreX has S/PDIF output), and its analogue inputs are 0x400/0x402, "
        "skipping 0x401. Never copy a pin table between models.",
        "router-pin is emitted as a DECIMAL string because fcp-server parses it with atoi(); if a "
        "device-provided devmap ever turns up using hex strings, that parse silently yields 0.",
    ]
    devmap["structs"] = OD(APP_SPACE=OD(members=members))
    devmap["device-specification"] = OD([("physical-inputs", phys_in), ("physical-outputs", phys_out)])
    if dev_sources:
        devmap["device-specification"]["sources"] = dev_sources
        devmap["device-specification"]["destinations"] = dev_dests
    # REQUIRED, not decoration: fcp-server's create_global_control() hard-fails (-1) if
    # enums.eDEV_FCP_USER_MESSAGE_TYPE.enumerators is absent, so an empty "enums" silently kills
    # EVERY global control (mute, dim, Firmware Version). Only eMSG_FLASH_CTRL is read from it: it
    # supplies the notify-device activate for `save: true` controls == our DATA_CMD persist (5).
    devmap["enums"] = OD([
        ("eDEV_FCP_USER_MESSAGE_TYPE",
         OD(enumerators=OD([("eMSG_FLASH_CTRL", 5)]))),
    ])

    with open(f"{OUTDIR}/fcp-devmap-{slug}.json", "w") as f:
        json.dump(devmap, f, indent=2)
        f.write("\n")

    # ---- alsa-map ----
    input_controls = OD()
    input_controls["air"] = OD(name="Line In %d Air Capture Switch", type="bool")
    used_modes = set(m for m in modes.values() if m)
    for kind in sorted(used_modes):
        input_controls[mode_key(kind)] = OD(name=f"Line In %d {ml} Capture Enum", type="enum",
                                            values=ENUM_LABELS[kind])

    alsamap = OD()
    alsamap["_note"] = (f"DRAFT — hand-authored ALSA control map for the {spec['name']} (Thunderbolt), paired "
                        f"with fcp-devmap-{slug}.json. Presentation layer: control names/types/ranges/enum "
                        "labels (matching the driver's scarlett2-parity set); offsets + bindings live in the "
                        "devmap. Scoped to a confident slice: preamp air/mode, output levels, master mute/dim.")
    alsamap["_provenance"] = ("Names/types from the snd-clarett in-kernel control set + the shipped "
                              "fcp-alsa-map-821d.json schema. Clean-room: no vendor map.")
    alsamap["_todo"] = [
        "Routing sources/sinks are generated from the model's band-0 SET_MUX bring-up table. LIMITATION: "
        "the SOURCE list is the set of pins that table actually routes, which is the factory-default "
        "patch, NOT the device's full source inventory — e.g. the 8PreX only ever routes Mix C-F, so "
        "Mix A/B and Mix G-P are absent as selectable sources, and only some PCM playback pins appear. "
        "Widening it means asserting pins we have not observed the device accept; verify on the bench "
        "(pick a destination, try a pin outside the list, confirm GET_MUX reflects it) before adding.",
        "The Clarett 8Pre is the exception to the note above, in both directions. It has no capture at "
        "all, so its band-0 routing is CONSTRUCTED (the driver arms its own captured routing from "
        "clarett_arm_8pre.h): the capture half is the 4Pre's, whose input "
        "geometry is identical, and the output half is authored. Its source list is therefore not "
        "table-derived either — it is the hardware's full inventory from the XML, which is what the "
        "other models' lists should eventually become. Its meter peak-index values are PREDICTED from "
        "the packing rule, not measured. NOTHING here has run against 8Pre hardware.",
        "output-group-sources / output-link are not emitted: those drive the Scarlett 4th gen's output "
        "group controls, which this line does not appear to have.",
        "Add output mute once its offset is captured (omitted; see the devmap _todo).",
        "Add global masterVolume + firmware-version once backing members/offsets are confirmed (masterVolume "
        "overlaps output 0/1 at 32/33, so it is left out of this slice to avoid double-driving one offset).",
        "Output level uses \"invert\": true, which REQUIRES the Stage-2 fcp-server patch (value_invert in "
        "struct control_props; device = invert-base - alsa). The device stores unsigned attenuation "
        "(0 = unity, 127 = -127 dB) where the Scarlett 4th gen stores signed dB. On an unpatched "
        "fcp-server the key is ignored, every reading lands outside [-127, 0], and the resulting "
        "snd_ctl_elem_write EINVAL repeats on every notification refresh.",
        "Firmware Version reads the versionStageRelease PLACEHOLDER offset, so its displayed value is "
        "meaningless — it exists because fcp-server needs the control for its socket-path TLV + lock "
        "handshake. Point it at the real firmware-version location once that is found in the appspace.",
        "Mode: the device byte is 0=Mic/1=Line/2=Inst line-wide, but only the 8PreX (separate XLR + 1/4\" "
        "jacks) can select Mic in software. The combo-jack models (2Pre/4Pre/8Pre) auto-select Mic from the "
        "jack, so their enum is Line/Inst carrying explicit values 1/2 — which REQUIRES the fcp-server patch "
        "accepting {name, value} entries in input-controls; on an unpatched server the names parse as an "
        "index-valued enum and Line/Inst would write 0/1.",
    ]
    alsamap["input-controls"] = input_controls
    # The Scarlett 4th gen (fcp-alsa-map-821d) stores volume as a signed dB byte, so its map can say
    # min=-127/max=0 directly. The Clarett stores an UNSIGNED ATTENUATION MAGNITUDE (0 = unity,
    # 127 = -127 dB), which runs backwards. "invert" (our Stage-2 fcp-server patch) maps
    # device = invert-base - alsa, base 0 — so the ALSA side keeps the natural ascending dB range
    # and a valid DB_MINMAX TLV, and alsamixer behaves like every other volume control.
    alsamap["output-controls"] = OD([
        ("level", OD([("name", "Line %d Playback Volume"), ("type", "int"),
                      ("min", -127), ("max", 0),
                      ("db-min", -127), ("db-max", 0),
                      ("invert", True)])),
        # SW/HW. A boolean carrying scarlett2's control name, which ends in "Enum" because on the
        # USB models it is one; alsa-scarlett-gui keys its SW/HW button on that exact substring and
        # renders it as a two-state toggle either way, so matching the name is what makes the
        # control appear. Value 1 = HW = the monitor knob drives the output, which is the bit's own
        # sense, so no inversion.
        ("hw-gain", OD([("name", "Line Out %d Volume Control Playback Enum"),
                        ("type", "bool-bitmap")])),
    ])
    alsamap["output-link"] = []
    alsamap["global-controls"] = OD([
        # Infrastructure, not cosmetics: fcp-server writes its socket path into this control's TLV
        # and locks it, and that lock+SCKT TLV is how clients discover a running server. Without it
        # startup logs "Cannot write socket path TLV"/"Cannot lock control element" and no client
        # can connect. Keyed on the backing member; interface/name are what fcp-socket looks up.
        ("versionStageRelease", OD([("name", "Firmware Version"), ("interface", "card"),
                                    ("access", "readonly"), ("type", "int")])),
        # NOT inverted: 1 = muted, straight through. Was marked invert/invert-base 1 on the strength
        # of the in-kernel control's .invert = 1, which was itself wrong - it inverted mute while
        # leaving dim, an identical 1-bit field two bytes along, alone. The trace settles it
        # (8prex_monitor_mutedim.log): muting in FC writes 01 to offset 24 and unmuting writes 00,
        # exactly as dim writes 01/00 to offset 28. Confirmed on a 2Pre, where the inverted version
        # made the GUI's mute button unmute the device. Upstream's Scarlett map says the same thing
        # by saying nothing, and alsa-scarlett-gui reads value 1 as muted.
        ("muteSwitch", OD([("name", "Mute Playback Switch"), ("type", "bool")])),
        # The front-panel monitor knob, read-only. scarlett2's name for the same thing, which
        # alsa-scarlett-gui matches exactly to draw it as the "HW" dial next to the output faders -
        # the partner of the per-output SW/HW toggles, since it is what drives every output set to
        # HW. Same attenuation encoding as the output gains, so the same invert.
        ("monitorVolume", OD([("name", "Master HW Playback Volume"), ("type", "int"),
                              ("access", "readonly"),
                              ("min", -127), ("max", 0),
                              ("db-min", -127), ("db-max", 0),
                              ("invert", True)])),
        ("dimSwitch",  OD(name="Dim Playback Switch",  type="bool")),
    ])
    # Front-panel meter-bridge source selector (8PreX). Non-contiguous device values (1/2/4/8) need
    # the {name, value} enum form, which global-controls.c accepts. Bound to the meterSource devmap
    # member (offset 184, commit activate 8).
    if slug in METER_SOURCE:
        alsamap["global-controls"]["meterSource"] = OD([
            ("name", "Meter Source Enum"), ("type", "enum"),
            ("values", METER_SOURCE[slug]),
        ])
    # S/PDIF connector select (4Pre/8Pre/8PreX): two independent enums bound to the spdifSourceInput/
    # Output devmap members (offsets 132/124, both commit activate 4). "S/PDIF Source Capture Enum" is
    # scarlett2's name; "S/PDIF Source Playback Enum" is the output partner. alsa-scarlett-gui renders
    # both in its Device Settings tab (config-device-settings.c).
    if slug in SPDIF_SOURCE:
        alsamap["global-controls"]["spdifSourceInput"] = OD([
            ("name", "S/PDIF Source Capture Enum"), ("type", "enum"),
            ("values", SPDIF_SOURCE_ENUM),
        ])
        alsamap["global-controls"]["spdifSourceOutput"] = OD([
            ("name", "S/PDIF Source Playback Enum"), ("type", "enum"),
            ("values", SPDIF_SOURCE_ENUM),
        ])

    if alsa_sources:
        alsamap["sources"] = alsa_sources
        alsamap["sinks"] = alsa_sinks

    with open(f"{OUTDIR}/fcp-alsa-map-{slug}.json", "w") as f:
        json.dump(alsamap, f, indent=2)
        f.write("\n")

    print(f"{slug}: {na} air, {sum(1 for m in modes.values() if m)} mode "
          f"({sorted(used_modes)}), {nout} outputs (shape [{out_shape}])")
