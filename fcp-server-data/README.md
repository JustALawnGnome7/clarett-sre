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

- `fcp-devmap-clarett-2pre.json` — **SKELETON.** A minimal, schema-correct
  device-map so fcp-server can get *past* the device-map step when the device's own
  `DEVMAP_READ` (`0x80000d`) is silent. Offsets/types are **provisional** (see the
  `_todo` inside) and must be validated against — ideally replaced by — a real
  `/tmp/fcp-devmap-clarett-2pre-*.json` dump once `DEVMAP_READ` works on hardware.

Not yet authored: the `fcp-alsa-map-clarett-*.json` control-naming maps (the next
thing fcp-server needs after the device-map). Without one, fcp-server exits right
after loading the device-map — which is the intended Stage-2/3 checkpoint.

## Use

```sh
sudo LOG_LEVEL=debug FCP_SERVER_DATA_DIR=/path/to/Clarett/fcp-server-data \
  ./fcp-server <card-number>
```

fcp-server searches `$FCP_SERVER_DATA_DIR`, then the current directory, then its
compiled-in `DATADIR`.
