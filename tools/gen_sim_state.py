#!/usr/bin/env python3
"""Generate an alsactl-style .state file for a Clarett TB model as fcp-server would present it.

    ./tools/gen_sim_state.py clarett-2pre > "Clarett 2Pre.state"
    alsa-scarlett-gui "Clarett 2Pre.state"

Name the file after the model exactly as the driver names the card ("Clarett 2Pre",
"Clarett 8PreX"): the simulated card takes its name from the filename, and alsa-scarlett-gui keys
the Thunderbolt Claretts' friendly port names (Line 3-4/Headphones, ...) on the card name.

alsa-scarlett-gui simulates a card from such a file (create_sim_from_file), which lets its
rendering of our control set be checked with no hardware attached — how the routing/mixer/levels
windows and the input Level enum were verified. The control set is DERIVED from the same
fcp-server maps the real device is driven by (names, types, enum items, ranges), so it tracks the
maps automatically -- except "Clock Source" and "Sync Status", which the maps do not carry and are
listed per model below. What it cannot reproduce is anything that only exists at runtime — TLVs
(so mixer dB readings are wrong here), meter labels, and the hwdep/socket driver-type path.
"""
import json, os, re, sys

slug = sys.argv[1] if len(sys.argv) > 1 else "clarett-2pre"
root = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "fcp-server-data")
amap = json.load(open(f"{root}/fcp-alsa-map-{slug}.json"))
dmap = json.load(open(f"{root}/fcp-devmap-{slug}.json"))

sources = [s["alsa_name"] for s in amap["sources"]]
sinks = [s["alsa_name"] for s in amap["sinks"]]
n_in = len([p for p in dmap["device-specification"]["physical-inputs"]])
n_out = len([p for p in dmap["device-specification"]["physical-outputs"]])
n_meter = 1 + max(
    s.get("peak-index", -1) for s in dmap["device-specification"]["sources"]
)

out = []
n = [0]


def ctl(iface, name, value, comment):
    n[0] += 1
    lines = [f"\tcontrol.{n[0]} {{", f"\t\tiface {iface}", f"\t\tname '{esc(name)}'"]
    if isinstance(value, list):
        lines += [f"\t\tvalue.{i} {v}" for i, v in enumerate(value)]
    else:
        lines.append(f"\t\tvalue {value}")
    lines.append("\t\tcomment {")
    lines += [f"\t\t\t{c}" for c in comment]
    lines += ["\t\t}", "\t}"]
    out.extend(lines)


def esc(s):
    """Escape for an alsa-lib single-quoted string: the Red's mixes run past Z ("Mix \\", ...)."""
    return s.replace("\\", "\\\\").replace("'", "\\'")


def q(s):
    """Quote an alsa-lib config value unless it is a plain word."""
    return s if re.fullmatch(r"[A-Za-z0-9_.+-]+", s) else f"'{esc(s)}'"


def enum_ctl(name, value, vals, access="read write"):
    ctl("MIXER", name, q(value),
        [f"access '{access}'", "type ENUMERATED", "count 1"] +
        [f"item.{j} {q(v)}" for j, v in enumerate(vals)])


# Clock + sync: on the real card neither comes from the maps. "Clock Source" is the one control
# snd-clarett owns (numid 1 on a live card); its items are the model's clock_srcs list in
# snd-clarett/clarett_main.c, copied here by hand -- keep the two in step. "Sync Status" (numid 2)
# is fcp-server's, added at runtime from the device's SYNC capability (fcp-support server/sync.c).
CLOCK_SOURCES = {
    "clarett-2pre": ["Internal", "S/PDIF", "ADAT"],
    "clarett-4pre": ["Internal", "S/PDIF", "ADAT"],
    "clarett-8pre": ["Internal", "S/PDIF", "ADAT"],
    "clarett-8prex": ["Internal", "S/PDIF", "ADAT 1", "ADAT 2", "Wordclock"],
    "red-8line": ["Internal", "Wordclock", "ADAT 1", "ADAT 2", "S/PDIF", "Dante", "Loop Sync"],
}
clocks = CLOCK_SOURCES[slug]
enum_ctl("Clock Source", clocks[0], clocks)
enum_ctl("Sync Status", "Locked", ["Unlocked", "Locked"], access="read volatile")

enum_items = ["Off"] + sources
items = [f"item.{i} {q(v)}" for i, v in enumerate(enum_items)]

# Routing sinks (mux)
for s in sinks:
    suffix = "Capture Enum" if s.startswith(("PCM", "Mixer")) else "Playback Enum"
    ctl("MIXER", f"{s} {suffix}", "Off",
        ["access 'read write'", "type ENUMERATED", "count 1"] + items)

def mix_label(index):
    """fcp-server's mix_output_label(): A..Z, then AA, AB, ... (bijective base 26)."""
    label = ""
    while index >= 0:
        label = chr(ord("A") + index % 26) + label
        index = index // 26 - 1
    return label


# Mixer matrix. The range is fcp-server's (mix.c): linear, unity 8192, topped at the map's
# "mixer-max-db" -- +6 dB (16345) on the Claretts -- or +12 dB (32613) when the map has no key.
mix_inputs = [s for s in sinks if s.startswith("Mixer Input")]
n_mix_out = len([s for s in sources if s.startswith("Mix ")])
mix_max_db = amap.get("mixer-max-db", 12)
mix_max = round(8192 * 10 ** (mix_max_db / 20))
for o in range(n_mix_out):
    for i, mi in enumerate(mix_inputs):
        ctl("MIXER", f"Mix {mix_label(o)} Input {i + 1:02d} Playback Volume", 0,
            ["access 'read write'", "type INTEGER", "count 1", f"range '0 - {mix_max}'",
             "dbmin -9999999", f"dbmax {mix_max_db * 100}"])

# Input and output controls. Which channels get which control comes from the devmap, not from the
# channel count: on the 8PreX inputs 1-2 and 3-8 carry *different* mode enums, and on the combo-jack
# models only inputs 1-2 have a mode at all.
def channel_controls(physical, control_configs):
    for i, chan in enumerate(physical):
        for key in chan["controls"]:
            cfg = control_configs.get(key)
            if not cfg:
                continue
            name = cfg["name"] % (i + 1)

            # bool-bitmap is a bool to ALSA; the bit addressing is fcp-server's business
            if cfg["type"] in ("bool", "bool-bitmap"):
                ctl("MIXER", name, "false",
                    ["access 'read write'", "type BOOLEAN", "count 1"])
            elif cfg["type"] == "enum":
                # "values" is either plain names or {name, value} objects (an explicit device
                # encoding); ALSA only ever sees the names, in order.
                vals = [v["name"] if isinstance(v, dict) else v for v in cfg["values"]]
                ctl("MIXER", name, q(vals[0]),
                    ["access 'read write'", "type ENUMERATED", "count 1"] +
                    [f"item.{j} {q(v)}" for j, v in enumerate(vals)])
            else:
                ctl("MIXER", name, cfg["max"],
                    ["access 'read write'", "type INTEGER", "count 1",
                     f"range '{cfg['min']} - {cfg['max']}'",
                     f"dbmin {cfg['db-min'] * 100}", f"dbmax {cfg['db-max'] * 100}"])


channel_controls(dmap["device-specification"]["physical-inputs"], amap["input-controls"])
channel_controls(dmap["device-specification"]["physical-outputs"], amap["output-controls"])

# Global controls
for key, cfg in amap["global-controls"].items():
    iface = "CARD" if cfg.get("interface") == "card" else "MIXER"
    access = "read" if cfg.get("access") == "readonly" else "read write"
    if cfg["type"] == "bool":
        ctl(iface, cfg["name"], "false", [f"access '{access}'", "type BOOLEAN", "count 1"])
    elif cfg["type"] == "enum":
        # Same shape as the per-channel enums above. Emitting these as INTEGER instead is not
        # merely cosmetic: alsa-scarlett-gui picks its "Digital I/O Mode" element by NAME prefix
        # ("S/PDIF Source" is the last fallback) and then reads an item name off it, so an
        # integer control under an enum's name dereferences an absent item-name table.
        vals = [v["name"] if isinstance(v, dict) else v for v in cfg["values"]]
        ctl(iface, cfg["name"], q(vals[0]),
            [f"access '{access}'", "type ENUMERATED", "count 1"] +
            [f"item.{j} {q(v)}" for j, v in enumerate(vals)])
    else:
        ctl(iface, cfg["name"], 1,
            [f"access '{access}'", "type INTEGER", "count 1", "range '0 - 65535'"])

# Level meter (kernel-owned, iface PCM, one value per measured slot)
ctl("PCM", "Level Meter", [0] * n_meter,
    ["access 'read volatile'", "type INTEGER", f"count {n_meter}", "range '0 - 4095'"])

print(f"state.{slug} {{")
print("\n".join(out))
print("}")
