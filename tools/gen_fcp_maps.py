#!/usr/bin/env python3
"""Generate paired fcp-server devmap + alsa-map drafts for the Clarett Thunderbolt models.

Grounded in the snd-clarett driver: air @ 174+i, mode @ 166+i (device byte 0=Mic,1=Line,2=Inst),
output gains strided at 32 (pairs at base+{0,1}, pairs step by 4), master mute @ 24, dim @ 73.
fcp-server stores an enum's INDEX as the device byte (no value map), so mode enums are byte-identity
lists starting at Mic (index 0) — the combo-jack models hide Mic in-kernel, but exposing it here keeps
Line=1/Inst=2 byte-correct.
"""
import json, collections

import os
OUTDIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "fcp-server-data")
OD = collections.OrderedDict

# per-model: mode_label, n_analogue (air on all), and per-input mode enum ("mli3" | "ml2" | None)
MODELS = {
    "clarett-2pre":  dict(name="Clarett 2Pre",  mode_label="Level",
                          n_analogue=2, outputs=4,
                          modes={0: "mli3", 1: "mli3"}),
    "clarett-4pre":  dict(name="Clarett 4Pre",  mode_label="Level",
                          n_analogue=4, outputs=6,
                          modes={0: "mli3", 1: "mli3"}),
    "clarett-8pre":  dict(name="Clarett 8Pre",  mode_label="Level",
                          n_analogue=8, outputs=10,
                          modes={0: "mli3", 1: "mli3"}),
    "clarett-8prex": dict(name="Clarett 8PreX", mode_label="Mode",
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
    members = OD()
    members["versionStageRelease"] = member(0, "uint32", nc=0, note="PLACEHOLDER offset; see _todo")
    members["muteSwitch"] = member(24, "bool")
    members["dimSwitch"]  = member(73, "bool")
    members["air"]  = member(174, "bool",  shape=na, note="per-input air @ 174+i; WRITE offset, reads 0 on TB Clarett (see _todo)")
    members["mode"] = member(166, "uint8", shape=na, note="per-input mode @ 166+i, byte 0=Mic/1=Line/2=Inst; WRITE offset, reads 0 on TB Clarett (see _todo)")
    members["outputVolume"] = member(32, "uint8", shape=out_shape, note="attenuation code 0..127; strided gains (pairs at base+{0,1}, pairs step 4)")

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
        "air/mode use the config WRITE offsets (174+i / 166+i); on the TB Clarett these read back 0 "
        "(appspace does not mirror NVRAM preamp state), so initial/notified reads show 0 until a real "
        "read location is found. Writes are trace-confirmed to manifest.",
        "outputVolume is a strided array; physical-outputs index onto the real gains (pairs at base+{0,1}, "
        "pairs step by 4). Consequence: the alsa-map output names come out sparse (from index+1), a cosmetic "
        "artifact of the layout, to be fixed with the real devmap.",
        "Output mute offsets are not yet captured, so output mute is omitted (here and in the alsa-map).",
        "notify-client masks are guesses (the TB device does not expose the FCP notification word; the driver "
        "relays a wildcard ~0).",
    ]
    devmap["structs"] = OD(APP_SPACE=OD(members=members))
    devmap["device-specification"] = OD([("physical-inputs", phys_in), ("physical-outputs", phys_out)])
    devmap["enums"] = OD()

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
        "Add S/PDIF, ADAT, and the mux/mix routing sections (sources/sinks/output-group-sources) once the "
        "device's MUX/MIX capability + offsets are confirmed on the bench.",
        "Add output mute once its offset is captured (omitted; see the devmap _todo).",
        "Add global masterVolume + firmware-version once backing members/offsets are confirmed (masterVolume "
        "overlaps output 0/1 at 32/33, so it is left out of this slice to avoid double-driving one offset).",
        "Mode enums are byte-identity (index==device byte 0=Mic/1=Line/2=Inst) because fcp-server stores the "
        "enum index directly. The combo-jack models (2Pre/4Pre/8Pre) hide Mic in-kernel (auto-detected by the "
        "jack); it is exposed here to keep Line/Inst byte-correct. Confirm behaviour of selecting Mic on those.",
    ]
    alsamap["input-controls"] = input_controls
    alsamap["output-controls"] = OD(level=OD(name="Line %d Playback Volume", type="int",
                                             min=-127, max=0))
    alsamap["output-controls"]["level"]["db-min"] = -127
    alsamap["output-controls"]["level"]["db-max"] = 0
    alsamap["output-link"] = []
    alsamap["global-controls"] = OD([
        ("muteSwitch", OD(name="Mute Playback Switch", type="bool")),
        ("dimSwitch",  OD(name="Dim Playback Switch",  type="bool")),
    ])

    with open(f"{OUTDIR}/fcp-alsa-map-{slug}.json", "w") as f:
        json.dump(alsamap, f, indent=2)
        f.write("\n")

    print(f"{slug}: {na} air, {sum(1 for m in modes.values() if m)} mode "
          f"({sorted(used_modes)}), {nout} outputs (shape [{out_shape}])")
