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


def load_init_blob(key):
    """Parse driver/clarett_init_<key>.h back into (blob bytes, [(opcode, off, len)])."""
    text = open(os.path.join(DRIVERDIR, f"clarett_init_{key}.h")).read()

    m = re.search(r"clarett_init_blob_%s\[\]\s*=\s*\{(.*?)\};" % key, text, re.S)
    blob = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)))

    m = re.search(r"clarett_init_seq_%s\[\]\s*=\s*\{(.*?)\};" % key, text, re.S)
    steps = [(int(op, 16), int(off), int(ln)) for op, off, ln in
             re.findall(r"\{\s*(0x[0-9a-fA-F]+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}", m.group(1))]
    return blob, steps


def band0_mux(key):
    """The model's band-0 routing table as [(src_pin, dst_pin)], padding entries dropped.

    Mirrors clarett_band0_mux() in the driver: SET_MUX payload is {u32 header, u32 entry[]} with
    header = band << 16, and each entry = (src_pin << 12) | dst_pin. entry == 0 is zero padding
    (dst 0 is not a valid pin); src == 0 with a real dst means a present-but-unrouted destination.
    """
    blob, steps = load_init_blob(key)
    for opcode, off, ln in steps:
        if opcode != FCP_SET_MUX or ln < 4:
            continue
        if int.from_bytes(blob[off:off + 4], "little") >> 16 != 0:
            continue
        data = blob[off + 4:off + ln]
        entries = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
        return [((e >> 12) & 0xfff, e & 0xfff) for e in entries if e]
    return []


# Pin naming is DIRECTION-SCOPED and PER-MODEL — the same number means different things as a source
# vs a destination (0x408 = S/PDIF in as a source, Monitor Out 1 as a destination), and the smaller
# models remap (the 2Pre reaches S/PDIF *input* at 0x186/0x187 where the 8PreX has S/PDIF *output*,
# and its two analogue inputs are 0x400/0x402, skipping 0x401). So names are derived from the pins
# the model actually presents, by position within each group, never by a fixed table.
def name_destinations(dsts):
    """dst pin -> (name, mixer_input_index or None), for the pins this model routes to."""
    out = OD()
    analogue = sorted(p for p in dsts if 0x400 <= p <= 0x409)
    # 8PreX-style: 0x408/0x409 are the monitor pair and the 0x400 block starts at Line Output 3.
    monitor_pair = 0x408 in dsts
    for pin in sorted(dsts):
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
            out[pin] = (f"PCM {pin - 0x600 + 1:02d}", None)
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


def name_sources(srcs):
    """src pin -> name. Pin 0 ("Off") is excluded: fcp-server rejects a router-pin <= 0."""
    out = OD()
    analogue = sorted(p for p in srcs if 0x400 <= p <= 0x407)
    for pin in sorted(p for p in srcs if p):
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
#
# Measured on a Clarett 4Pre, July 20 2026. Signal on analogue inputs 1-4 moved slots 0,1,2,3
# respectively — four independent readings, so the "analogue input N -> slot N-1" rule is solid.
# Inputs 1 and 2 additionally lit slots 18-23 (all six line outputs, via the mix they feed) and
# slots 28/29; inputs 3 and 4 lit nothing else, which is exactly right — the routing table patches
# only Analogue 1/2 into the mixer (400->300, 401->301), so 3/4 reach neither the buses nor the
# outputs. That also refutes reading 28-47 as PCM captures: 0x402 -> 0x602 would have lit a PCM
# slot for input 3, and none moved.
#
# ONLY the 4Pre is listed. The slot order is not transferable: the models differ in ADAT width
# (8 vs 16) and in their analogue pin blocks (the 2Pre's inputs are 0x400/0x402, skipping 0x401,
# and it reaches S/PDIF at 0x186/0x187), so any other model must be measured on its own hardware.
METER_SLOTS = {
    # Measured on a 2Pre: analogue in 1/2 -> slots 0/1, S/PDIF in 1/2 -> slots 2/3. Its router pins
    # are 0x400/0x402 (0x401 skipped) and S/PDIF is at 0x186/0x187, yet the slots are 0..3 — meter
    # slots are a COMPACT CHANNEL INDEX, unrelated to pin numbering.
    #
    # THE RULE, measured: slots are packed PER MODEL in category order — analogue inputs fill
    # 0..n-1, then S/PDIF, then ADAT. The 2Pre is now mapped end to end and confirms it exactly:
    # analogue 0-1, S/PDIF 2-3, ADAT 4-11. The 4Pre's S/PDIF at 8/9 (after its 8 analogue) is the
    # same rule, and its ADAT was then measured at exactly the predicted 10-17.
    #
    # METER_INFO answers 00 02 0c 00 on BOTH models. The 12 is exactly the 2Pre's input count
    # (2+2+8) but not the 4Pre's (8+2+8 = 18), so a single per-model count it is not. Reading the
    # bytes as {?, 2, 12} suggests 2 x 12 = 24 total slots, which would cover both — untested, and
    # it matters: fcp-server bounds peak-index by this value, so if the real bound is 24 rather
    # than 12 the 4Pre's ADAT becomes reachable too.
    "clarett-2pre": {
        0x400: (0, "measured"), 0x402: (1, "measured"),
        0x186: (2, "measured"), 0x187: (3, "measured"),   # S/PDIF in (optical)
        0x200: (4,  "measured"), 0x201: (5,  "measured"),  # ADAT 1-8, fed from an optical source
        0x202: (6,  "measured"), 0x203: (7,  "measured"),
        0x204: (8,  "measured"), 0x205: (9,  "measured"),
        0x206: (10, "measured"), 0x207: (11, "measured"),
    },
    "clarett-4pre": {
        # pin: (slot, provenance)
        0x400: (0,  "measured"), 0x401: (1,  "measured"),
        0x402: (2,  "measured"), 0x403: (3,  "measured"),
        # Inputs 5 and 8 were excited directly (finger-tap on a line input, so only ~30 counts —
        # under the tool's flag threshold, but they were the ONLY non-zero slots in the array).
        # 6 and 7 are bracketed between them and cannot be anywhere else.
        0x404: (4,  "measured-weak"), 0x405: (5,  "bracketed"),
        0x406: (6,  "bracketed"),     0x407: (7,  "measured-weak"),
        # Optical S/PDIF into the 4Pre (source byte @132 = Optical) lit 8 and 9 together.
        0x408: (8,  "measured"), 0x409: (9,  "measured"),
        # ADAT via the optical port (S/PDIF source back to RCA) lit 10-17 — exactly what the
        # packing rule predicted from the analogue and S/PDIF placements.
        0x200: (10, "measured"), 0x201: (11, "measured"),
        0x202: (12, "measured"), 0x203: (13, "measured"),
        0x204: (14, "measured"), 0x205: (15, "measured"),
        0x206: (16, "measured"), 0x207: (17, "measured"),
    },
    # PREDICTED, not measured — the one model here with no measurement on it. The packing rule is a
    # function of the input geometry alone, and the 8Pre's is identical to the 4Pre's (8 analogue at
    # 0x400-0x407, 2 S/PDIF at 0x408/0x409, 8 ADAT at 0x200-0x207), so the whole table is the 4Pre's.
    # Included rather than left out because every slot is inside the bound fcp-server enforces, so
    # the cost of being wrong is meters against the wrong channel names, not a discarded map — and
    # it makes the prediction falsifiable on first contact with the hardware (tools/fcp_meter_watch.c
    # against one input at a time; if it disagrees, the rule itself is wrong and wants revisiting).
    "clarett-8pre": {
        0x400: (0,  "predicted"), 0x401: (1,  "predicted"),
        0x402: (2,  "predicted"), 0x403: (3,  "predicted"),
        0x404: (4,  "predicted"), 0x405: (5,  "predicted"),
        0x406: (6,  "predicted"), 0x407: (7,  "predicted"),
        0x408: (8,  "predicted"), 0x409: (9,  "predicted"),
        0x200: (10, "predicted"), 0x201: (11, "predicted"),
        0x202: (12, "predicted"), 0x203: (13, "predicted"),
        0x204: (14, "predicted"), 0x205: (15, "predicted"),
        0x206: (16, "predicted"), 0x207: (17, "predicted"),
    },
}
# NO destination peak-index. fcp-server rejects any index >= the count the device reports from
# METER_INFO (0x001000, resp[0]), and a 4Pre rejected index 10 - so it exposes at most 10 meter
# slots, exactly the size of the analogue block (8 analogue + S/PDIF 1-2). The slots we measured
# downstream (18-23 line outputs, 28/29 mixer inputs) are real - GET_METER serves them when asked
# for 48 - but they are outside what METER_INFO advertises, and ONE out-of-range entry makes
# fcp-server discard the whole map (meter.c goto done), taking the measured analogue slots with it.
# ADAT (10-17, inferred) is dropped for the same reason.
METER_SLOTS_DST = {}

# per-model: mode_label, n_analogue (air on all), and per-input mode enum kind (see ENUM_LABELS)
MODELS = {
    "clarett-2pre":  dict(name="Clarett 2Pre",  mode_label="Level", init="2pre",
                          n_analogue=2, outputs=4,
                          modes={0: "li2", 1: "li2"}),
    "clarett-4pre":  dict(name="Clarett 4Pre",  mode_label="Level", init="4pre",
                          n_analogue=4, outputs=6,
                          modes={0: "li2", 1: "li2"}),
    "clarett-8pre":  dict(name="Clarett 8Pre",  mode_label="Level", init=None,
                          n_analogue=8, outputs=10,
                          modes={0: "li2", 1: "li2"},
                          # No capture for this model: routing is constructed instead, and the
                          # driver pushes the same table (see synth_band0_8pre).
                          synth_mux=lambda: synth_band0_8pre(),
                          synth_sources=lambda: synth_sources_8pre()),
    "clarett-8prex": dict(name="Clarett 8PreX", mode_label="Level", init="8prex",
                          n_analogue=8, outputs=10,
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
# (spec/clarett-control-plane.md §4). Those get the {name, value} form so the device bytes stay 1/2
# with no Mic entry — offering a Mic that the jack decides would be offering a setting that does
# nothing. REQUIRES the fcp-server patch that accepts the object form in input-controls (the same
# form global-controls.c already took).
ENUM_LABELS = {
    "mli3": ["Mic", "Line", "Inst"],
    "ml2":  ["Mic", "Line"],
    "li2":  [OD([("name", "Line"), ("value", 1)]), OD([("name", "Inst"), ("value", 2)])],
}


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
    members = OD()
    members["versionStageRelease"] = member(0, "uint32", nd=0, nc=0, note="PLACEHOLDER offset; see _todo")
    members["muteSwitch"] = member(24, "bool", nd=2, note="monitor mute @ 24; activate 2. Active-low: the device byte is 1 when UNMUTED, so the alsa-map marks it invert/invert-base 1 (needs the Stage-2 fcp-server patch)")
    members["dimSwitch"]  = member(28, "bool", nd=2, note="monitor dim @ 28; activate 2")
    members["air"]  = member(174, "bool",  shape=na, nd=7, note="per-input air @ 174+i; commit activate 7")
    members["mode"] = member(166, "uint8", shape=na, nd=6, note="per-input mode @ 166+i, byte 0=Mic/1=Line/2=Inst; commit activate 6")
    members["outputVolume"] = member(32, "uint8", shape=out_shape, nd=1, note="attenuation code 0..127; strided gains (pairs at base+{0,1}, pairs step 4); commit activate 1")

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
        phys_out.append(OD(name=f"Output {n+1}",
                           controls=OD(level=OD(index=out_index(n), member="outputVolume"))))

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
        src_pins = spec.get("synth_sources", lambda: None)() or {s for s, _ in pairs}
        src_names = name_sources(set(src_pins))
        dst_names = name_destinations({d for _, d in pairs})
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
    devmap["_provenance"] = ("Offsets from the snd-clarett driver + spec/clarett-control-plane.md: air @ 174+i, "
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
        "(tools/gen_fcp_maps.py parses clarett_init_<model>.h), so the router-pins are exactly what the "
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
        "all, so its band-0 table is CONSTRUCTED (driver/clarett_mux_8pre.h, emitted by this script and "
        "pushed by the driver once the model is detected): the capture half is the 4Pre's, whose input "
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
    alsamap["output-controls"] = OD(level=OD([("name", "Line %d Playback Volume"), ("type", "int"),
                                              ("min", -127), ("max", 0),
                                              ("db-min", -127), ("db-max", 0),
                                              ("invert", True)]))
    alsamap["output-link"] = []
    alsamap["global-controls"] = OD([
        # Infrastructure, not cosmetics: fcp-server writes its socket path into this control's TLV
        # and locks it, and that lock+SCKT TLV is how clients discover a running server. Without it
        # startup logs "Cannot write socket path TLV"/"Cannot lock control element" and no client
        # can connect. Keyed on the backing member; interface/name are what fcp-socket looks up.
        ("versionStageRelease", OD([("name", "Firmware Version"), ("interface", "card"),
                                    ("access", "readonly"), ("type", "int")])),
        # Active-low: the device byte is 1 when unmuted (the in-kernel control carries .invert = 1
        # for the same reason). invert-base 1 gives device = 1 - alsa. Dim, at offset 28, is NOT
        # inverted — verified against the driver, not assumed from its neighbour.
        ("muteSwitch", OD([("name", "Mute Playback Switch"), ("type", "bool"),
                           ("invert", True), ("invert-base", 1)])),
        ("dimSwitch",  OD(name="Dim Playback Switch",  type="bool")),
    ])

    if alsa_sources:
        alsamap["sources"] = alsa_sources
        alsamap["sinks"] = alsa_sinks

    with open(f"{OUTDIR}/fcp-alsa-map-{slug}.json", "w") as f:
        json.dump(alsamap, f, indent=2)
        f.write("\n")

    print(f"{slug}: {na} air, {sum(1 for m in modes.values() if m)} mode "
          f"({sorted(used_modes)}), {nout} outputs (shape [{out_shape}])")


# --- driver header for the constructed 8Pre table ------------------------------------------------
# Written from the same function the 8Pre's map is generated from, so the table the driver pushes and
# the destinations the map claims cannot drift apart. The other models need no such header: their
# driver-side table is the captured bring-up blob in clarett_init_<model>.h.
def emit_mux_header():
    pairs = synth_band0_8pre()
    lines = [
        "/* SPDX-License-Identifier: GPL-2.0-or-later */",
        "/*",
        " * Clarett 8Pre band-0 router table — GENERATED by tools/gen_fcp_maps.py, do not edit.",
        " *",
        " * Unlike clarett_init_<model>.h this is NOT a capture: no 8Pre boot has been traced. The",
        " * capture half is the 4Pre's captured table (identical input geometry); the output half is",
        " * authored, and every mixer input is present but unrouted. See synth_band0_8pre() for the",
        " * full provenance. Pins and their names are [XML], cross-checked against all three captured",
        " * models, where the XML's output pin list exactly matched the live band-0 destinations.",
        " *",
        f" * {len(pairs)} entries = 30 mixer inputs + 20 physical outputs + 20 capture channels: one per",
        " * destination, which is how every captured table is built.",
        " */",
        "",
        "static const struct clarett_mux_entry clarett_mux_band0_8pre[] = {",
    ]
    for i in range(0, len(pairs), 4):
        row = "".join("{ 0x%03x, 0x%03x }, " % p for p in pairs[i:i + 4]).rstrip()
        lines.append("\t" + row)
    lines += ["};", ""]

    with open(os.path.join(DRIVERDIR, "clarett_mux_8pre.h"), "w") as f:
        f.write("\n".join(lines))
    print(f"clarett-8pre: emitted driver/clarett_mux_8pre.h ({len(pairs)} entries)")


emit_mux_header()
