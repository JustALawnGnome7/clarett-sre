# fcp-server data (Clarett Thunderbolt)

Device-map / control-map files for driving the Clarett Thunderbolt line with
Geoffrey Bennett's user-space [`fcp-server`](https://github.com/geoffreybennett/fcp-support),
paired with `snd-clarett` supplying the FCP hwdep.

These are keyed on the **model slug** the driver publishes in the card's ALSA
components string as `Clarett:<slug>` (`clarett-2pre`, `clarett-4pre`, `clarett-8pre`,
`clarett-8prex`, `red-8line`, `red-16line`, `red-4pre`, `red-8pre`; see `alsactl info <card>`) — because the whole line shares PCI id `1cb5:0002`, the slug, not
the PCI id, selects the per-model map. Stock fcp-server keys map filenames on the
USB product id, so this needs the `map_key` support on the `snd_clarett` branch.

Clean-room: authored from our own interface facts (`spec/provenance/clarett-control-plane.md`,
`snd-clarett/clarett.h`), never from any vendor device map.

## Where these live

**`tools/gen_fcp_maps.py` in this repo is the source of record.** The files are
generated, not hand-edited — including the `_note` / `_provenance` / `_limitations`
strings inside them, which come from the generator's own text. Edit the generator,
re-run it, then copy the result across.

**`fcp-support/data/`** is the copy that ships -- **at the moment only on the `snd_clarett`
branch of our fork** ([JustALawnGnome7/fcp-support](https://github.com/JustALawnGnome7/fcp-support/tree/snd_clarett)).
A plain `make install` in that tree installs them to `$(DATADIR)` alongside the Scarlett maps,
so the Clarett works with no second repository involved. Upstream fcp-support ships none of
them, nor the fcp-server patches they depend on; getting both upstream is the eventual aim.
Nothing installs from this directory. Keeping the two copies in step is a manual step of
releasing:

```sh
python3 tools/gen_fcp_maps.py
cp fcp-server-data/fcp-*.json ../fcp-support/data/
```

A regenerated map that has not been copied across is not installed, and nothing says so --
diff `fcp-support/data/` against `fcp-server-data/` when something that was fixed stops
being fixed.

Note the notes inside the shipped files are written for a reader of *that* tree:
they don't cite paths in this one, and they describe the maps rather than the
reverse-engineering behind them. The detail behind a given number lives here, in
`spec/provenance/`.

## Files

Each model is a **pair** (`fcp-devmap-<slug>.json` + `fcp-alsa-map-<slug>.json`) —
fcp-server needs both, and they cross-reference. All four Clarett models are covered:
`clarett-2pre`, `clarett-4pre`, `clarett-8pre`, `clarett-8prex`, and four Reds: `red-8line`
(hardware-confirmed), and `red-16line`, `red-4pre`, `red-8pre` (derived from their descriptors,
untested) — see below.

`sim/` holds an interface-simulation preview of each untested model (`Red 16Line.state`, …), for
alsa-scarlett-gui to render with no device attached: `alsa-scarlett-gui "sim/Red 8Pre.state"`.
`gen_fcp_maps.py` regenerates them from the maps on every run (via `tools/gen_sim_state.py`), so
they cannot fall behind. They lack what only exists at runtime — meter labels (so the Levels
window groups meters by routing category), TLVs, real values. Replace one with an `alsactl store`
of a real unit once there is one.

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
contiguous block), `reinterpreted` (re-attributed from an earlier measurement
taken under different routing), `band0` (the destination's index in the vendor's band-0 routing
table; see the Red section), or `synth` (the same, in a band-0 table synthesised for a model with
no capture; see the derived-Red section).

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
high-pass, phase invert and stereo link; the routing patchbay, 154 sources and 156 destinations;
and the same global controls as the Clarett maps, under the same names: monitor Mute and Dim,
the read-only Master HW dial, Meter Source, and the S/PDIF input/output connector selects.

Mute and dim are bits 0 and 1 of one byte (124), so each is a masked switch. **Dim needs the
fork's masked-switch fix in fcp-server** (`control-utils.c`: a set switch writes its whole mask);
without it, turning dim on writes 0.

**Outputs 1-6 belong to the three front-panel knobs.** Monitor 1/2, Headphones 1 and Headphones 2
take their level, mute and dim from their knob group (gain 112/116/120, mute/dim 124/126/128); the
device pins their own gain bytes at 0 dB (a -40 dB write to Monitor 1, on SW, read back 0). So the
map binds those outputs' faders and mute/dim to the group fields and gives them no SW/HW toggle.
Writing a group gain from software is per the descriptor and not yet hardware-verified.

Line Outputs 1-8 (outputs 7-14) have real per-output gains and a SW/HW byte each (90+i), written 0/1
exactly as FC does -- a plain `bool`, not the Clarett's `bool-bitmap` (whose bytes pack two outputs).
**On HW the Red mirrors the monitor knob into their stored gain itself** (observed), so unlike the
Clarett it needs no driver-side follow.

Two naming rules the GUI imposes: mix buses past Z are `Mix AA`…`Mix AF`, matching fcp-server's own
mixer control names, and the two ADAT ports reach ALSA as one flat `ADAT 1`-`16` run (the devmap
keeps the descriptor's `ADAT 1.1`…`2.8`), because alsa-scarlett-gui numbers ports by the first
integer in the name.

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
- **RedNet Control's mixer ceiling.** The map sets no `mixer-max-db`, so fcp-server's +12 dB
  default applies, on purpose: the hardware is measured exactly linear up to +12 dB (unity
  0x2000, measured/expected 1.00000 at every step from -18 to +12 dB). What the vendor's own fader
  tops out at is unread, because every `SET_MIX` in the vendor capture wrote zero.
- **Unconfirmed on hardware:** Meter Source (268) comes from the descriptor alone.
- **Dante output meters.** Deliberately absent -- see Level meters below.
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
- This repo's former Makefile's `install-maps` globbed `fcp-devmap-clarett-*.json`, so
  `make install` **silently skipped the Red pair while reporting success**. The Makefile has
  since been removed; fcp-support's `install-data` globs every map in its `data/`.

**Level meters (hardware-confirmed).** On every Clarett a destination's `GET_METER` slot is exactly its
index in the band-0 `SET_MUX` table the vendor programs at bring-up -- all four measured layouts, no
exception. The 2Pre's slots 16-17 and the 4Pre's 26-27, long read as dark, are those models' loopback
destinations, so every model meters its loopback pair: measured live on the 2Pre (PCM 13-14 follow
audio when loopback is fed) and the 4Pre (PCM 19-20, slots 26-27). They had only ever looked
dark because nothing was feeding loopback when they were watched. The Red's 156 slots are read off its
own band 0 (provenance `band0`) and were confirmed on the unit by routing an input signal through every
kind of destination. An ALSA INTEGER control holds at most **128 values**, so the driver splits a longer
map across several `Level Meter` controls (same name, index 0, 1, ...) and alsa-scarlett-gui joins
them; the Red's 156 channels, Dante outputs included, take two. An input meter in the GUI borrows the
level of a metered destination it is routed to, so input meters cost no channels. The unit reports only
58 meter slots, so this needs fcp-server's `METER_SLOT_LIMIT` at 255 (it was 128, which discarded the
whole map).

**Changing a model's meter count needs a driver reload.** The driver creates the `Level Meter`
control(s) from the first map fcp-server sends, and an ALSA control cannot change its channel count
while it exists. A map with more or fewer metered destinations is refused until the module is
reloaded; fcp-server logs only `Cannot set meter map: Invalid argument` and keeps the old meters, while
the kernel log names the cause (`meter map geometry changed ... reload the module`). Release the card
(PipeWire, `fcp-server@N`, `alsa-state.service`), reload `snd-clarett`, and let fcp-server start again.

## The Red 16Line, 4Pre and 8Pre map pairs (Oct 9 2026) — derived, untested

Built by the same Red builder (`build_red()`) with **no capture and no hardware**. The Red range is
two pairs of twins sharing a geometry and router layout: 8Line/4Pre (64/60) and 16Line/8Pre (64/64),
the Pre model of each pair having preamps on inputs 1-4 / 1-8 where the Line model has 1-2. The
16Line's basis, which the Pre models share:

- **Control set = the Red 8Line's, extended.** The 16Line descriptor is the 8Line's plus eight
  line inputs (source pins `0x40a`-`0x411`, backwards within each four like the rest) and eight
  line outputs (`0x40e`-`0x415`), whose gain/mute-dim/hardware-control fields continue the
  8Line's strides (gain `24+2n`, mute/dim `68+n`, hw `90+n`, n = 14..21). Every shared control
  sits at the 8Line's offset, so the 8Line's hardware-confirmed control set carries over, and the
  map has 22 outputs instead of 14. Meter Source gains "Analogue Inputs/Outputs 9-16" (1/3).
- **Router tables and meter slots are SYNTHESISED** (`synth_red_bands()`, provenance `synth`). The
  rule: capture records minus loopback, then the outputs in descriptor order, then the loopback
  pair, then the mixer inputs (32, or 30 at quad), each speed dropping what the descriptor's
  `pin-m`/`pin-h` drop. It reproduces the Red 8Line's captured band 0/1/2 tables exactly, and
  `gen_fcp_maps.py` re-checks that on every run (`check_red_bands()`), so a rule change that
  broke the 8Line would fail generation. 168 destinations: PCM 1 = slot 0, Monitor Output 1 = 62,
  Mixer Input 01 = 136.
- **One known risk in the slots:** the descriptor lists Dante 29-32 as capture records at pins
  `0x000`-`0x003`, past the 64 capture channels. They are treated as absent. If the vendor
  programs them, every output, loopback and mixer-input slot is 4 higher than the map says. The
  first hardware check: route a signal to Monitor Output 1 and see which meter moves.
- **The Pre models** (`red-4pre`, `red-8pre`): a field-by-field comparison of each Pre descriptor
  with its Line twin finds every Line control at the same offset, plus the extra preamps continuing
  the per-input strides (gains `130+3i`, phantom `154+i`, mode `162+i`, air `170+i`, high-pass
  `178+i`, phase `186+i`, stereo link `194+i`). Inputs 3 and up are Mic/Line only (an `ml2` mode
  control beside the `mli3` of inputs 1-2, the 8PreX's two-enum pattern), and their gain `select`
  spans two bytes. One stereo-link control per preamp pair, each assumed to behave as the 8Line's
  194/195 does (one switch in two bytes) — unverified. The 4Pre numbers inputs 1-4 forwards (pin
  `0x400` = Analogue 1), and both Pre models name their line outputs from 3, as their descriptors
  do. Their loopback pairs sit elsewhere in the capture order (`0x60a`/`0x612`), which the band
  synthesis takes from the model facts. The `0x000`-`0x003` risk above applies to the 8Pre too.
- **Telling the twins apart is the driver's job** (`clarett_unit_tb_gen()`): the Pre models are
  Thunderbolt 2 units, the Line models Thunderbolt 3, and the endpoint's immediate upstream bridge
  is always the unit's own controller. Before that, a Pre unit would have registered as its Line
  twin and loaded these maps' Line counterparts.

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
