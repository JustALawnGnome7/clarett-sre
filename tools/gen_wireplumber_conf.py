#!/usr/bin/env python3
"""Generate the WirePlumber card-naming drop-in from the driver's model table.

The drop-in has to name every model the driver can register, matching each one by the exact
string the driver puts in card->shortname. That string is clarett_model.name in
driver/clarett_main.c, so this generator reads it from there rather than keeping a second
copy: a hand-maintained list drifts the moment a model is added, and the failure is silent
(the device simply displays under the generic pci.ids name).

  tools/gen_wireplumber_conf.py            regenerate wireplumber/51-clarett-naming.conf
  tools/gen_wireplumber_conf.py --check    exit 1 if the file is stale (for CI / pre-commit)
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "driver", "clarett_main.c")
OUT = os.path.join(ROOT, "wireplumber", "51-clarett-naming.conf")

# static const struct clarett_model clarett_2pre = { .name = "Clarett 2Pre", ...
MODEL_DEF = re.compile(
    r"static\s+const\s+struct\s+clarett_model\s+(\w+)\s*=\s*\{(.*?)\n\};", re.S)
MODEL_NAME = re.compile(r"\.name\s*=\s*\"([^\"]+)\"")
# The detect-model candidate list, which the driver keeps in smallest-to-largest order.
DETECT_LIST = re.compile(
    r"static\s+const\s+struct\s+clarett_model\s*\*\s*const\s+models\[\]\s*=\s*\{(.*?)\}", re.S)

HEADER = """\
# Focusrite Clarett Thunderbolt card naming for WirePlumber.
#
# GENERATED FILE — do not edit. Regenerate with tools/gen_wireplumber_conf.py,
# which reads the model names out of the driver's clarett_model table so this
# file cannot drift from the names the driver actually registers.
#
# Why this is needed. 1cb5:0002 IS in pci.ids, as the whole-line name "Clarett":
#
#     1cb5  Focusrite Audio Engineering Ltd
#         0002  Clarett
#
# udev turns that into ID_MODEL_FROM_DATABASE, libspa-alsa turns that into
# device.product.name, and WirePlumber's alsa.lua prefers device.product.name
# over api.alsa.card.name when it derives device.description:
#
#     d = d or properties["device.product.name"]
#           or properties["api.alsa.card.name"]
#           or properties["alsa.card_name"]
#
# So every unit in the line displays as "Clarett" — "Clarett Multichannel" once
# the profile description is appended — with no way to tell a 2Pre from an
# 8PreX, or to tell two units apart when both are plugged in.
#
# This is an override of a name WirePlumber PREFERS, not a name it lacks, so no
# choice of card name in the driver can fix it: device.product.name always wins.
# Nor can pci.ids be made more specific — every model in the line reports the
# same subsystem ID, so there is nothing for a per-model entry to key on.
#
# The driver already names each card per model (card->shortname == the model
# name == api.alsa.card.name). All this file does is promote that into
# device.description, which is the field GNOME and most UIs display. Matching on
# the card name is also what distinguishes multiple units plugged in at once.
#
# Install: `sudo make install-wireplumber` from the repo root, which places this
# in $PREFIX/share/wireplumber/wireplumber.conf.d/ (PREFIX=/usr/local by
# default; /usr/local/share is in the XDG_DATA_DIRS default, which WirePlumber's
# config lookup honours). Packages should build with PREFIX=/usr.
# /etc/wireplumber/wireplumber.conf.d/ is read too, but leave it for the user's
# own overrides. Then `systemctl --user restart wireplumber`, or replug.

monitor.alsa.rules = [
"""

RULE = """\
  {{
    matches = [ {{ api.alsa.card.name = "{name}" }} ]
    actions = {{ update-props = {{ device.description = "{name}" }} }}
  }}
"""


def model_names(text):
    """The driver's model names, in the order clarett_detect_model() lists them.

    That array is maintained smallest-to-largest, which is the order we want to emit. Any
    model defined but missing from it is appended in definition order rather than dropped --
    silently omitting a model is the exact failure this generator exists to prevent.
    """
    by_ident = {}
    for ident, body in MODEL_DEF.findall(text):
        m = MODEL_NAME.search(body)
        if m:
            by_ident[ident] = m.group(1)
    if not by_ident:
        sys.exit(f"{SRC}: no clarett_model definitions found -- has the table moved?")

    ordered = []
    m = DETECT_LIST.search(text)
    if m:
        for ident in re.findall(r"&(\w+)", m.group(1)):
            if ident in by_ident and by_ident[ident] not in ordered:
                ordered.append(by_ident[ident])
    for ident, name in by_ident.items():
        if name not in ordered:
            ordered.append(name)
    return ordered


def render(names):
    return HEADER + "".join(RULE.format(name=n) for n in names) + "]\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="do not write; exit 1 if the drop-in is out of date")
    args = ap.parse_args()

    names = model_names(open(SRC).read())
    want = render(names)

    if args.check:
        have = open(OUT).read() if os.path.exists(OUT) else None
        if have != want:
            print(f"{os.path.relpath(OUT, ROOT)} is out of date "
                  f"-- run tools/gen_wireplumber_conf.py", file=sys.stderr)
            return 1
        print(f"{os.path.relpath(OUT, ROOT)}: up to date ({len(names)} models)")
        return 0

    with open(OUT, "w") as f:
        f.write(want)
    print(f"{os.path.relpath(OUT, ROOT)}: {len(names)} models ({', '.join(names)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
