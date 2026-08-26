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
fcp-server needs both, and they cross-reference. All four models are covered:
`clarett-2pre`, `clarett-4pre`, `clarett-8pre`, `clarett-8prex`.

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
