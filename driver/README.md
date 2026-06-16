# snd-clarett — Focusrite Clarett 8PreX (Thunderbolt) ALSA driver

Status: **control plane only** (mixer-only sound card). PCM/streaming is not yet
implemented. Built from the clean-room notes in `../spec/`.

## What works

- PCI bring-up for `1cb5:0002` (BAR0 map, bus master, 32-bit DMA response buffer).
- FCP mailbox transport: `SET_DATA` / `DATA_CMD` / poll-for-completion.
- ALSA mixer controls (all confirmed single-byte fields):
  - `Master Playback Switch` (mute), `Monitor Dim Playback Switch`,
    `Master Playback Volume` (monitor section gain)
  - 10 analogue output volumes (Monitor 1–2, Line 3–10), 1 dB/step, −127..0 dB TLV
  - per analogue input 1–8: `Air` switch + `Mode` enum (Mic/Line[/Inst])

## Build

```sh
make                       # against the running kernel
sudo insmod snd-clarett.ko
```

`make KDIR=/path/to/kernel` to build against another tree.

## ⚠️ The device must NOT be bound to vfio-pci

During RE the Clarett is passed through to the Windows VM via `vfio-pci`. To test
this driver on the **host**, the device must be free for `snd-clarett` to claim:

1. Shut down the Windows VM.
2. Unbind from vfio-pci and let snd-clarett bind, e.g.:
   ```sh
   echo 0000:09:00.0 | sudo tee /sys/bus/pci/drivers/vfio-pci/unbind
   sudo insmod snd-clarett.ko
   echo 0000:09:00.0 | sudo tee /sys/bus/pci/drivers/snd-clarett/bind   # if not auto-bound
   ```
   (Also remove any `vfio-pci.ids=`/driver_override or modprobe binding that
   re-grabs the device at boot.)
3. Verify: `aplay -l` / `amixer -c <n> contents`, and test a control:
   ```sh
   amixer -c <n> sset 'Master Playback' mute     # then unmute, move volumes, etc.
   ```

Never load this while the VM is using the device.

## Known limitations / TODO

- **No PCM** — data-plane DMA streaming is not reverse-engineered yet.
- **Mixer "get" returns a shadow**: write-through on put, and the **monitor bytes
  are refreshed from the DMAed GET response on a front-panel notification**, so
  those reflect live hardware. Other bytes stay write-through and default to
  0 dB / unmuted / Mic / Air-off at probe, which may not match the hardware.
- **Mailbox completion is polled**, not MSI-driven. MSI *is* enabled, but only
  for **async notifications** (vec0 / cause `0x400`): a front-panel button raises
  the §11 dim-mute/monitor mask, and the ISR → workqueue re-reads the monitor
  region and `snd_ctl_notify()`s the monitor controls. The GET-response layout is
  decoded (16-byte echoed FCP header + requested bytes at +16), so the handler
  updates the monitor shadow to reflect physical button changes.
- The GET-response DMA buffer address is programmed at `0x410` (low32) / `0x414`
  (high32) — `0x414` = address-high confirmed (hardcoding the trace's `0x2` faulted
  the IOMMU; the driver uses `upper_32_bits(resp_dma)`).
- **Firmware init handshake** (boot `INIT_2` + `0x5000/0x6000/0x7000` sequence)
  is not replayed; not understood yet.
- **Packed bitfield controls** (per-output hardware gain/dim/mute enables) are
  not implemented (need read-modify-write of shared bytes).
- Single-card only; no module params for index/id.
