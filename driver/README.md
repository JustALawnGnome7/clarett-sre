# snd-clarett — Focusrite Clarett (Thunderbolt) ALSA driver

Supported: **Clarett 8PreX**, **Clarett 8Pre**, **Clarett 4Pre**, and **Clarett 2Pre** (one module,
per-model descriptor).

Status: **control plane only** (mixer-only sound card). PCM/streaming is not yet
implemented (and is gated off for the 2Pre until its stream geometry is captured).
Built from the clean-room notes in `../spec/`.

> **⚠️ Control changes do not physically take effect.** The driver brings up the
> device and every mixer write completes without error, but the hardware does not act
> on them (e.g. the front-panel Mute LED does not move) and `GET_DATA` reads come back
> empty — the device backend stays **dormant for our driver**. This is **not** a
> control-plane protocol bug (our FCP traffic matches the vendor byte-for-byte) and
> **not** a missing data plane (the vendor moves the same LEDs at idle with no stream):
> it is a proven **off-wire / below-the-driver** boundary — see "Known limitations" and
> `../spec/clarett-manifestation-wall.md`. Also: the device-arming init replay only
> works on a **freshly power-cycled** device.

## What works

- PCI bring-up for `1cb5:0002` (BAR0 map, bus master, 32-bit DMA response buffer).
- FCP mailbox transport: `SET_DATA` / `DATA_CMD` / poll-for-completion.
- ALSA mixer controls (all confirmed single-byte fields):
  - `Mute` / `Dim` (monitor section, named to match the USB unit) and
    `Master Playback Volume` (monitor section gain)
  - 10 analogue output volumes (Monitor 1–2, Line 3–10), 1 dB/step, −127..0 dB TLV
  - per analogue input 1–8: `Air` switch + `Mode` enum (Mic/Line[/Inst])
- **Device session bring-up** at probe (`clarett_arm_device`): replays the 232-command
  vendor init from `clarett_init_8prex.h` (`CONFIG_PUSH`×122, subsystem enables, 8 KB config
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

## Model selection (`model=`)

The entire Clarett Thunderbolt line shares PCI id `1cb5:0002` **and** presents a
byte-identical PCIe interface — every MMIO register, FCP query response, config-space
read, and even the dummy serial is the same across models (verified on real 8PreX and
2Pre hardware). So the driver **cannot auto-detect the model** and takes a parameter:

```sh
sudo insmod snd-clarett.ko                # defaults to model=2pre
sudo insmod snd-clarett.ko model=8prex    # or model=4pre / model=8pre / model=2pre
```

The **4Pre** descriptor is built from the device XML and cross-checked against a live capture:
the input/output control map is `[XML]` (Analogue 1-2 Line/Inst + Air, 3-4 Air-only, 5-8 none;
six output gains @ 32/33/36/37/40/41), and the channel counts (8 playback / 20 record), the
bring-up replay, the stream-routing ids, and the Analogue-1 toggle are `[TRACE]`-confirmed.

The **8Pre** (distinct from the 8PreX) is **control-plane only** — built from the XML, but with no
8Pre capture available it has no bring-up replay or stream ids, so it registers its mixer but will
not arm config access or stream PCM until an 8Pre boot is captured. It uses combo XLR/TRS jacks (Mic
is auto-detected by the jack, so the software mode is Line/Inst only, on inputs 1-2; 3-8 are air-only),
unlike the 8PreX's separate ports; outputs match the 8PreX (10 gains).

There is no auto-detect of any kind. The model name *does* live in the Thunderbolt
DROM, but the entire Clarett line is **Thunderbolt 2** (discontinued before any TB3
model), and TB2 units are firmware-tunnelled rather than enumerated as kernel-managed
TB routers — so they never expose a `device_name` on `/sys/bus/thunderbolt`, and there
is nothing for userspace to key on either. Set the model explicitly; to make it
persistent:

```sh
echo 'options snd_clarett model=2pre' | sudo tee /etc/modprobe.d/snd-clarett.conf
```

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

- **No sustained PCM** — the data-plane engine *is* reverse-engineered and clocks (arms,
  DMAs a burst, descriptors correct, PTR advances) but stalls after one ring pass at the
  **same off-wire/below-driver wall** as the control plane (`../spec/clarett-data-plane.md`).
  Experimental capture PCM is opt-in (`enable_pcm=1`); see the 2Pre note below.
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
  A/B lever `resp_prefill=` (-1/0..255): fill the response buffer with a byte before
  every command submit — `0` mirrors FC's freshly-zeroed common buffer, `170` (0xAA)
  restores the §5a emptiness marker, `-1` (default) leaves it untouched between
  commands (baseline). Probes whether the device reads/reacts to this buffer's
  contents (the only host address it knows at init).
- **`premailbox_reads=` (default 1)**: replay the vendor driver's exact pre-mailbox
  BAR0 **read** sequence at attach (caps, `0x4/0x8`, serial, `0x514`, `0x58c`, all
  four cause blocks, the full `0x8000–0x801c` fw-info header) before the first FCP
  command. The cold gdb ladder (2026-07-10) showed the working device answers
  `error=0` from mailbox command #0, so the accept-vs-refuse gate is set *before* the
  mailbox opens; pre-mailbox writes already match FC, so this read set is the sole
  remaining host-visible pre-mailbox difference. Set `0` for the old read-minimal
  probe to A/B whether the reads flip `GET_DATA` to `error=0`.
- **Control changes don't physically manifest — a proven below-driver boundary** (the
  headline gap). After a correct bring-up, monitor `Mute`/`Dim` writes complete
  (`done=1, fcperr=0`) and `GET_DATA` returns empty (`size=0`) — the device backend is
  dormant for our driver even though our FCP traffic matches the vendor app byte-for-byte.
  The earlier "needs the **data plane** (streaming)" theory is **disproven** (the vendor
  moves the same LEDs at idle, no stream). The differentiator has been localized to
  **off-wire bus-master DMA / transport below the driver**, invisible to every host-side
  software trace, and confirmed by **four independent methods**: Windows/vfio MMIO
  (byte-identical on every surface), macOS DTrace of the working kext (device returns rich
  real data to identical `GET_DATA` requests that return empty for us), our Linux replay,
  and WinDbg kernel-debug of the working Windows driver (its entire init DMA footprint is
  attribute-equivalent to ours — cached-coherent, 64-bit, nothing extra programmed to the
  device at init). It is **not** a control-plane protocol bug and **not** fixable from the
  driver's observable surface; the remaining leads (TB/PCIe bus analyzer, vendor-binary
  disassembly) are excluded. Full analysis: `../spec/clarett-manifestation-wall.md`.
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
- Single-card only; no module params for index/id (but see `model=` above).
- **2Pre: experimental flat-buffer capture PCM** (`enable_pcm=1`). A RAM dump of the live
  2Pre stream showed it streams a **flat contiguous sample ring** at `0x210`/`0x310` with no
  descriptor table (unlike the 8PreX's scatter-gather list), with asymmetric TX 4ch / RX 14ch
  periods (`0x40` / `0xe0`). The driver uses a separate flat-buffer engine path (`flat_buffer`
  model flag) sharing the 8PreX's register arm + servicer. The ring/wrap length
  (`CLARETT_FLAT_FRAMES`, currently 1024 frames = the VM's observed 16 KB TX area) is a
  hypothesis pending hardware confirmation — capture may not yet clock or may wrap elsewhere.
