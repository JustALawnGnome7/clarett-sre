#!/usr/bin/env python3
"""Generate paired fcp-server devmap + alsa-map drafts for the Clarett Thunderbolt models.

Grounded in the snd-clarett driver: air @ 174+i, mode @ 166+i (device byte 0=Mic,1=Line,2=Inst),
output gains strided at 32 (pairs at base+{0,1}, pairs step by 4), master mute @ 24, dim @ 73.
fcp-server stores an enum's INDEX as the device byte (no value map), so mode enums are byte-identity
lists starting at Mic (index 0) — the combo-jack models hide Mic in-kernel, but exposing it here keeps
Line=1/Inst=2 byte-correct.
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
    # Measured on a 2Pre, July 20 2026: analogue in 1 -> slot 0, analogue in 2 -> slot 1. Note its
    # router pins are 0x400/0x402 (0x401 is skipped on this model) yet the slots are 0/1 — so meter
    # slots are a COMPACT CHANNEL INDEX, not derived from the pin number. Nothing else is listed:
    # S/PDIF/ADAT would need a digital source to excite, and both models answer METER_INFO with the
    # same 00 02 0c 00, so the "12" in it is a family constant rather than this model's channel
    # count and cannot be used to infer where the remaining inputs sit.
    "clarett-2pre": {
        0x400: (0, "measured"), 0x402: (1, "measured"),
    },
    "clarett-4pre": {
        # pin: (slot, provenance)
        0x400: (0,  "measured"), 0x401: (1,  "measured"),
        0x402: (2,  "measured"), 0x403: (3,  "measured"),
        # Same block, same order — inferred, not measured (no signal source on 5-8 at the time).
        0x404: (4,  "inferred"), 0x405: (5,  "inferred"),
        0x406: (6,  "inferred"), 0x407: (7,  "inferred"),
        0x408: (8,  "inferred"), 0x409: (9,  "inferred"),   # S/PDIF in, continuing the block
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

# per-model: mode_label, n_analogue (air on all), and per-input mode enum ("mli3" | "ml2" | None)
MODELS = {
    "clarett-2pre":  dict(name="Clarett 2Pre",  mode_label="Level", init="2pre",
                          n_analogue=2, outputs=4,
                          modes={0: "mli3", 1: "mli3"}),
    "clarett-4pre":  dict(name="Clarett 4Pre",  mode_label="Level", init="4pre",
                          n_analogue=4, outputs=6,
                          modes={0: "mli3", 1: "mli3"}),
    "clarett-8pre":  dict(name="Clarett 8Pre",  mode_label="Level", init=None,
                          n_analogue=8, outputs=10,
                          modes={0: "mli3", 1: "mli3"}),
    "clarett-8prex": dict(name="Clarett 8PreX", mode_label="Mode", init="8prex",
                          n_analogue=8, outputs=10,
                          modes={0: "mli3", 1: "mli3",
                                 2: "ml2", 3: "ml2", 4: "ml2",
                                 5: "ml2", 6: "ml2", 7: "ml2"}),
}
ENUM_LABELS = {"mli3": ["Mic", "Line", "Inst"], "ml2": ["Mic", "Line"]}

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
            ctrls["mode3" if mt == "mli3" else "mode2"] = OD(index=i, member="mode")
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
    if spec["init"]:
        pairs = band0_mux(spec["init"])
        src_names = name_sources({s for s, _ in pairs})
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
            alsa_sinks.append(OD([("device_name", nm), ("alsa_name", nm)]))

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
    if "mli3" in used_modes:
        input_controls["mode3"] = OD(name=f"Line In %d {ml} Capture Enum", type="enum",
                                     values=ENUM_LABELS["mli3"])
    if "ml2" in used_modes:
        input_controls["mode2"] = OD(name=f"Line In %d {ml} Capture Enum", type="enum",
                                     values=ENUM_LABELS["ml2"])

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
        "The Clarett 8Pre has NO bring-up blob in the driver (it needs an 8Pre capture), so it gets no "
        "routing/mixer sections at all — same gap as its in-kernel routing controls.",
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
        "Mode enums are byte-identity (index==device byte 0=Mic/1=Line/2=Inst) because fcp-server stores the "
        "enum index directly. The combo-jack models (2Pre/4Pre/8Pre) hide Mic in-kernel (auto-detected by the "
        "jack); it is exposed here to keep Line/Inst byte-correct. Confirm behaviour of selecting Mic on those.",
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
