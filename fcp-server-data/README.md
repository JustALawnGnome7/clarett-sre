# fcp-server data (Clarett Thunderbolt)

Authored device-map / control-map files for driving the Clarett Thunderbolt line
with Geoffrey Bennett's user-space [`fcp-server`](https://github.com/geoffreybennett/fcp-support),
paired with `snd-clarett` loaded in transport mode (`in_kernel_controls=0`).

These are keyed on the **model slug** the driver publishes at
`/proc/asound/card<N>/clarett` (`clarett-2pre`, `clarett-4pre`, `clarett-8pre`,
`clarett-8prex`) — because the whole line shares PCI id `1cb5:0002`, the slug, not
the PCI id, selects the per-model map. (Requires the Stage-1 fcp-server patch that
reads the slug; stock fcp-server keys on the USB product id.)

Clean-room: authored from our own interface facts (`spec/clarett-control-plane.md`,
`driver/clarett.h`), never from any vendor device-map.

## Files

The 2Pre map is a **pair** — fcp-server needs both, and they cross-reference:

- `fcp-devmap-clarett-2pre.json` — **DRAFT** device-map: the `structs.APP_SPACE`
  members (offsets/types) plus the `device-specification` that binds each per-channel
  control to a member. Lets fcp-server create controls when the device's own
  `DEVMAP_READ` (`0x80000d`) is unavailable/silent. Offsets are carried from the
  driver (air `174+i`, mode `166+i`, gains `32,33,36,37`, mute `24`, dim `73`);
  see the `_todo` inside for caveats, and replace wholesale with a real
  `/tmp/fcp-devmap-clarett-2pre-*.json` dump once `DEVMAP_READ` works.
- `fcp-alsa-map-clarett-2pre.json` — **DRAFT** ALSA map: the presentation layer
  (control names/types/ranges/enum labels), matching the driver's scarlett2-parity
  control set. Scoped to a confident slice: preamp air/mode, output levels, master
  mute/dim.

Cross-checked consistent: every `member` the alsa-map/device-spec references resolves
in the devmap with the required keys. On a 2Pre this yields Line In 1-2 Air/Level,
four output-level controls, and Mute/Dim. Deferred (see each `_todo`): S/PDIF/ADAT,
the mux/mix routing sections, output mute, and global masterVolume/firmware-version.

## Use

```sh
sudo LOG_LEVEL=debug FCP_SERVER_DATA_DIR=/path/to/Clarett/fcp-server-data \
  ./fcp-server <card-number>
```

fcp-server searches `$FCP_SERVER_DATA_DIR`, then the current directory, then its
compiled-in `DATADIR`.
