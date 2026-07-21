#!/usr/bin/env python3
"""Generate an alsactl-style .state file for a Clarett TB model as fcp-server would present it.

    ./tools/gen_sim_state.py clarett-2pre > "Clarett 2Pre TB.state"
    alsa-scarlett-gui "Clarett 2Pre TB.state"

alsa-scarlett-gui simulates a card from such a file (create_sim_from_file), which lets its
rendering of our control set be checked with no hardware attached — how the routing/mixer/levels
windows and the input Level enum were verified. The control set is DERIVED from the same
fcp-server maps the real device is driven by (names, types, enum items, ranges), so it tracks the
maps automatically; what it cannot reproduce is anything that only exists at runtime — TLVs
(so mixer dB readings are wrong here), meter labels, and the hwdep/socket driver-type path.
"""
import json, sys, os

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
    lines = [f"\tcontrol.{n[0]} {{", f"\t\tiface {iface}", f"\t\tname '{name}'"]
    if isinstance(value, list):
        lines += [f"\t\tvalue.{i} {v}" for i, v in enumerate(value)]
    else:
        lines.append(f"\t\tvalue {value}")
    lines.append("\t\tcomment {")
    lines += [f"\t\t\t{c}" for c in comment]
    lines += ["\t\t}", "\t}"]
    out.extend(lines)


def q(s):
    return f"'{s}'" if " " in s or "/" in s else s


enum_items = ["Off"] + sources
items = [f"item.{i} {q(v)}" for i, v in enumerate(enum_items)]

# Routing sinks (mux)
for s in sinks:
    suffix = "Capture Enum" if s.startswith(("PCM", "Mixer")) else "Playback Enum"
    ctl("MIXER", f"{s} {suffix}", "Off",
        ["access 'read write'", "type ENUMERATED", "count 1"] + items)

# Mixer matrix
mix_inputs = [s for s in sinks if s.startswith("Mixer Input")]
n_mix_out = len([s for s in sources if s.startswith("Mix ")])
for o in range(n_mix_out):
    for i, mi in enumerate(mix_inputs):
        ctl("MIXER", f"Mix {chr(ord('A') + o)} Input {i + 1:02d} Playback Volume", 0,
            ["access 'read write'", "type INTEGER", "count 1", "range '0 - 32613'",
             "dbmin -9999999", "dbmax 1200"])

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

            if cfg["type"] == "bool":
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
    else:
        ctl(iface, cfg["name"], 1,
            [f"access '{access}'", "type INTEGER", "count 1", "range '0 - 65535'"])

# Level meter (kernel-owned, iface PCM, one value per measured slot)
ctl("PCM", "Level Meter", [0] * n_meter,
    ["access 'read volatile'", "type INTEGER", f"count {n_meter}", "range '0 - 4095'"])

print(f"state.{slug} {{")
print("\n".join(out))
print("}")
