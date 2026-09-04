# fcp-server data (Clarett Thunderbolt)

Device-map / control-map files for driving the Clarett Thunderbolt line with
Geoffrey Bennett's user-space [`fcp-server`](https://github.com/geoffreybennett/fcp-support),
paired with `snd-clarett` supplying the FCP hwdep.

These are keyed on the **model slug** the driver publishes at
`/proc/asound/card<N>/clarett` (`clarett-2pre`, `clarett-4pre`, `clarett-8pre`,
`clarett-8prex`) — because the whole line shares PCI id `1cb5:0002`, the slug, not
the PCI id, selects the per-model map. Stock fcp-server keys map filenames on the
USB product id, so this needs the `map_key` support on the `snd_clarett` branch.

Clean-room: authored from our own interface facts (`spec/provenance/clarett-control-plane.md`,
`driver/clarett.h`), never from any vendor device map.

## Where these live

**`tools/gen_fcp_maps.py` in this repo is the source of record.** The files are
generated, not hand-edited — including the `_note` / `_provenance` / `_limitations`
strings inside them, which come from the generator's own text. Edit the generator,
re-run it, then copy the result across.

They are also committed to **`fcp-support/data/`** on the `snd_clarett` branch, so
that a plain `make install` in that tree installs them to `$(DATADIR)` alongside the
Scarlett maps and the Clarett works with no second repository involved. Keeping the
two copies in step is a manual step of releasing:

```sh
python3 tools/gen_fcp_maps.py
cp fcp-server-data/fcp-*.json ../fcp-support/data/
```

Note the notes inside the shipped files are written for a reader of *that* tree:
they don't cite paths in this one, and they describe the maps rather than the
reverse-engineering behind them. The detail behind a given number lives here, in
`spec/provenance/`.

## Files

Each model is a **pair** (`fcp-devmap-<slug>.json` + `fcp-alsa-map-<slug>.json`) —
fcp-server needs both, and they cross-reference. All four Clarett models are covered:
`clarett-2pre`, `clarett-4pre`, `clarett-8pre`, `clarett-8prex`. The Red 8Line, which
the driver now also registers, deliberately has no pair — see below.

- **devmap** (`fcp-devmap-<slug>.json`) — `structs.APP_SPACE` members (offsets/types)
  plus the `device-specification` binding each per-channel control to a member, and
  the router sources/destinations. **This is the device's description, permanently:**
  the Thunderbolt Clarett does not answer `DEVMAP_READ` (`0x80000d`), so
  `fcp_devmap_read_from_file()`'s `DATADIR` lookup is the only way one is ever found.
  There is no device dump coming to replace it. Offsets are carried from the driver
  (air `174+i`, mode `166+i` with byte `0=Mic/1=Line/2=Inst`, gains strided at `32`,
  mute `24`, dim `73`).
- **alsa-map** (`fcp-alsa-map-<slug>.json`) — the presentation layer
  (names/types/ranges/enum labels), matching the driver's scarlett2-parity set.
  `device_name` is the devmap's own port name; `alsa_name` is what the control is
  called in ALSA, and the two deliberately differ for the analogue outputs — the
  devmap keeps the physical name (`Line Output 3`, `Monitor Output 1`) while ALSA
  sees `Analogue Output N`, because alsa-scarlett-gui only recognises a hardware
  output sink whose control name starts with `Analogue `, `S/PDIF ` or `ADAT `.

Cross-checked consistent: every referenced `member` resolves in the devmap with the
required keys and an in-range index. Per model this yields Line In air + mode
(Level/Mode) controls, the output-level controls, and Mute/Dim:

| slug | air | mode | outputs |
|------|-----|------|---------|
| `clarett-2pre`  | 1-2 | 1-2 (Line/Inst, no Mic)             | 4  |
| `clarett-4pre`  | 1-4 | 1-2 (Line/Inst, no Mic)             | 6  |
| `clarett-8pre`  | 1-8 | 1-2 (Line/Inst, no Mic)             | 10 |
| `clarett-8prex` | 1-8 | 1-2 (Mic/Line/Inst), 3-8 (Mic/Line) | 10 |

**The `clarett-8pre` pair is the one built without a capture of its own control
session.** Its map's routing comes from a band-0 table this script constructs (the
capture half is the 4Pre's, whose input geometry is identical; the output half is
authored), and its source list is the hardware's full inventory from the XML rather
than the routed-pins subset the other three get. That construction predates any 8Pre
hardware, but the unit has since been exercised extensively — its meter map was
measured, and the per-rate `peak-index-m`/`-h` compaction was measured on it.

Meter slots carry a `_peak-index-provenance` marker: `measured` (that destination
read directly on hardware), `stride` (filled between measured anchors in a
contiguous block), or `reinterpreted` (re-attributed from an earlier measurement
taken under different routing).

The device byte is `0=Mic/1=Line/2=Inst` line-wide, but **only the 8PreX can select Mic
in software** — it has separate XLR and ¼″ jacks per input, so something must choose the
path. The other three have a single combo XLR/TRS jack that auto-selects Mic when an XLR
is inserted, so their enum is Line/Inst only, carrying **explicit device values 1/2**
(`values: [{"name": "Line", "value": 1}, …]`). That form needs the fcp-server change
accepting `{name, value}` entries in `input-controls`; on a stock server the entries
parse as an index-valued enum and Line/Inst write 0/1 — i.e. Mic and Line.

## The Red 8Line map pair (Sep 4 2026)

`snd-clarett` registers the **Red 8Line** (slug `red-8line`, geometry `playback=64 capture=60` read off
real hardware), and it now has a pair: `fcp-devmap-red-8line.json` / `fcp-alsa-map-red-8line.json`,
generated by `build_red_8line()` — a builder of its own rather than a `MODELS` entry, because the Red's
offsets, widths and control set all differ from the Clarett's and the shared loop would emit
Clarett-shaped fields into a Red's config space. Keeping that loop untouched also keeps the four
Clarett pairs regenerating byte-identically, which is the regression test.

**What it exposes:** 14 analogue outputs (level, mute, dim, hardware-control enable); two preamps —
only Analogue 1-2 have them, Line 3-8 being line-level — with air, mode (Mic/Line/Inst), phantom,
high-pass, phase invert and stereo link; and the routing patchbay, 154 sources and 156 destinations.

**How trustworthy the offsets are.** Every one is cross-checked against the vendor's own `SET_DATA`
writes in `captures/red_8line.log`. Of the 72 config bytes the map claims, **61 were written by the
vendor** and the remaining 11 are indices inside arrays it did write; the only bytes outside any
confirmed array are the read-only Firmware Version placeholder. Output gain is **signed 16-bit dB at
1 dB/step over -112..0** — confirmed by the value the vendor wrote (`0xff90` = -112, the descriptor's
own minimum), so unlike the Clarett there is nothing to invert.

**Still missing, and why** (the file's own `_limitations` is authoritative):
- **The mic/line/inst preamp gains.** Offsets confirmed (`130/131/132 + 3i`, activate 9), but the
  descriptor gives no range or dB mapping and none has been measured. A guessed range on a mic preamp
  is not a cosmetic error. This is the first thing to add once measured.
- **All meter slots.** `peak-index` is absent everywhere: the `GET_METER` layout has never been
  observed on a Red, and slot order does not transfer between models, let alone product lines. Level
  meters will not display until `tools/fcp_meter_watch.c` is run on the hardware.
- **The mixer matrix.** Mixer Input destinations carry `mixer-input-index`, so routing is complete,
  but `MIX_INFO` dimensions have not been read from a Red.
- **`line-input-ref`** (offset 272, one bit per input, activate 21) — audible effect not established.

**HARDWARE-CONFIRMED (Sep 4 2026, ASRock X570 Creator).** fcp-server loads the pair and registers
**1252 controls** on a Red 8Line -- the first non-Clarett device this stack has ever driven -- and a
control write **manifests physically**: `Line In 1 Phantom Power Capture Switch` set from `amixer`
lit phantom on input 1 of the unit's front panel (user-confirmed), the Red's equivalent of the 2Pre
LED/relay confirmation. Air and high-pass wrote cleanly on the same input with input 2 unmoved, so
the per-input indexing is right too. Also confirmed live rather than inferred: output volumes read
back as real **signed** dB (`24..34 = 0`, `36/38 = -110`, `40..50 = -112`), and the router reads the
factory patch back coherently in both directions (Monitor 1/2 <- PCM 1/2, headphones <- PCM 9/10,
line outs 7-14 <- PCM 1-8). `Sync Status` reads Locked; the driver's own `Clock Source` offers all
seven Red sources.

**Two traps this run exposed, both now fixed and neither in the map's content:**
- The devmap **must** carry a top-level `enums` block. `init_global_controls()` hard-fails (-1)
  without one, so fcp-server exited 1 with `Cannot find enums in device map` before creating a
  single control. The requirement was already documented on the Clarett path and simply had not
  been carried into `build_red_8line()`.
- The repo Makefile's `install-maps` globbed `fcp-devmap-clarett-*.json`, so `make install`
  **silently skipped the Red pair while reporting success**. The glob now matches every model the
  generator emits.

`No meters found` is still logged at error level on every start. It is expected (no `peak-index`)
and non-fatal -- `add_meter_control()`'s return is ignored by its caller.

## What the maps deliberately don't cover

Each file's `_limitations` array is the authoritative list; in summary:

- **Output mute** — offset not identified. Master mute and dim are present.
- **Global `masterVolume`** — overlaps outputs 1/2 at offsets 32/33, so including it
  would have two controls driving one byte.
- **Firmware Version** — reads a placeholder offset, so the value is meaningless. It
  exists only because fcp-server requires the control for its socket-path TLV and
  lock handshake.
- **The source list is the factory-default patch, not the full inventory** — the
  8PreX routes only Mix C-F, so Mix A/B and Mix G-P aren't selectable, and only some
  PCM playback pins appear. Widening it means asserting pins the device hasn't been
  observed to accept: verify on the bench first (pick a destination, try a pin
  outside the list, confirm `GET_MUX` reflects it).
- **S/PDIF and ADAT controls, and the mux/mix routing sections** — deferred.

Two traps worth keeping in view: router pins are **direction-scoped and per-model**
(`0x408` is S/PDIF-in as a source but Monitor Out 1 as a destination), so a pin table
never transfers between models; and `router-pin` must be a **decimal** string,
because fcp-server parses it with `atoi()` and a hex string silently yields 0.

## Use

Installed via fcp-support's `make install`, which is how a normal setup gets them.
To run against this directory instead, without installing:

```sh
sudo LOG_LEVEL=debug FCP_SERVER_DATA_DIR=/path/to/Clarett/fcp-server-data \
  ./fcp-server <card-number>
```

fcp-server searches `$FCP_SERVER_DATA_DIR`, then the current directory, then its
compiled-in `DATADIR`.
