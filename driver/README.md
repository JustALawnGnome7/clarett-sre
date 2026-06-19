# snd-clarett — Focusrite Clarett 8PreX (Thunderbolt) ALSA driver

Status: **control plane only** (mixer-only sound card). PCM/streaming is not yet
implemented. Built from the clean-room notes in `../spec/`.

> **⚠️ Control changes do not physically take effect yet.** The driver brings up the
> device and every mixer write completes without error, but the hardware does not act
> on them (e.g. the front-panel Mute LED does not move) — this needs the **data plane**
> (audio engine streaming), which is not implemented. See "Known limitations". Also:
> the device-arming init replay only works on a **freshly power-cycled** device.

## What works

- PCI bring-up for `1cb5:0002` (BAR0 map, bus master, 32-bit DMA response buffer).
- FCP mailbox transport: `SET_DATA` / `DATA_CMD` / poll-for-completion.
- ALSA mixer controls (all confirmed single-byte fields):
  - `Mute` / `Dim` (monitor section, named to match the USB unit) and
    `Master Playback Volume` (monitor section gain)
  - 10 analogue output volumes (Monitor 1–2, Line 3–10), 1 dB/step, −127..0 dB TLV
  - per analogue input 1–8: `Air` switch + `Mode` enum (Mic/Line[/Inst])
- **Device session bring-up** at probe (`clarett_arm_device`): replays the 232-command
  vendor init from `clarett_init_seq.h` (`CONFIG_PUSH`×122, subsystem enables, 8 KB config
  sync, `SET_MIX`×16 + `SET_MUX`×3). Required on a **fresh** device — a self-booted 8PreX
  rejects `GET_DATA` until armed. (Re-running it on an already-armed device wedges the GET.)
- At probe the driver seeds its config shadow from the device (`GET_DATA(24,92)`)
  and **force-enables hardware Mute/Dim for Monitor Out 1-2** (bytes 72/73, command 3)
  so the global `Mute`/`Dim` actually act on those outputs — the master flag alone
  (offset 24/28) does nothing unless an output opts in via its enable bit.

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
3. Verify: `amixer -c <n> contents`, and test a control:
   ```sh
   amixer -c <n> sset 'Mute' toggle      # also 'Dim', 'Master Playback Volume', etc.
   ```
   (Note: writes complete but won't change the hardware yet — see the ⚠️ at the top.)

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
- **Control changes don't physically manifest** — the headline gap. After a correct
  bring-up, monitor `Mute`/`Dim` writes complete (`done=1, fcperr=0`) but nothing
  changes on the hardware; we match the vendor app byte-for-byte on the control plane.
  The missing piece is the **data plane**: the audio engine must be streaming (DMA-ring /
  clocking via non-mailbox registers) for control changes to take effect. This is the
  next milestone, not a control-plane bug.
- **Device bring-up replay is fresh-device-only.** `clarett_arm_device` arms a
  power-cycled device; re-running it on an already-armed device wedges `GET_DATA`
  (double-init). TODO: probe with a `GET` and skip the replay when already armed.
- **Packed bitfield controls** (per-output hardware gain/dim/mute enables): the
  Monitor Out 1-2 mute+dim enables are now set at probe (read-modify-write of bytes
  72/73 via `clarett_write_bits`), but they are not exposed as controls, and the
  gain enables (byte 52) and the Line 3-10 enables are still neither set nor exposed.
- **Settings are not persisted across a power cycle.** Each control commits live
  (`DATA_CMD{activate}`) but RAM-only; the device persists via a separate
  `DATA_CMD{5}` flash-commit (command 5), which the driver intentionally does not
  issue — auto-persisting every change would wear the flash. See
  `FCP_ACTIVATE_PERSIST` in `clarett.h` to add a deliberate "save" action.
- Single-card only; no module params for index/id.
