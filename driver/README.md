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
    `Master HW Playback Volume` — a **read-only** reflection of the hardware
    monitor-volume knob (offset 112, refreshed from the device on notifications),
    named/typed to match the scarlett2 driver's `Master HW Playback Volume`
  - per-output analogue volumes, 1 dB/step, −127..0 dB TLV, plus a per-output
    `Line Out NN Volume Control Playback Enum` (`SW`/`HW` — the `enable-hardware-gain`
    bit: `HW` follows the hardware monitor knob). As on scarlett2, an output's volume
    fader goes **read-only while its select is `HW`** (the knob owns the level)
  - per-output `Line NN Mute Playback Switch` (scarlett2 `MUTE_SWITCH`). Note the
    Clarett has no independent per-output mute: this is the `enable-hardware-mute`
    bit — the output is muted when this switch is **on and the global `Mute` is
    active** (the master flag alone does nothing until an output opts in)
  - per analogue input: `Air` switch + input-mode enum
  - `S/PDIF Source Capture Enum` (input) and `S/PDIF Output Mode Playback Enum`
    (`None`/`Optical`/`RCA`) on 4Pre/8Pre/8PreX
  - `Meter Source Capture Enum` (8PreX): which channel set the hardware meters
    show (`Analogue`/`S/PDIF`/`ADAT 1`/`ADAT 2`) — writes the per-band meter index
    tables + the source byte + `DATA_CMD{8}`, replaying FC's cycle
  - **routing (patchbay)**: a source-selection enum per destination — outputs
    (`… Playback Enum`) and PCM-capture / mixer-input (`… Capture Enum`), decoded
    from the arm blob's default `SET_MUX` matrix. **Writable**: changing one
    edits that destination's entry in each sample-rate band's payload (seeded
    verbatim from the blob) and resends all three `SET_MUX` commands — the arm's
    known-good matrix plus one delta, matching FC's routing-change cycle. Source
    names come from the confident pin ranges; destination names are 8PreX-accurate
    and approximate for smaller models pending per-model pin maps. **The mux writes
    are not yet hardware-verified — see the note in Known limitations.**
  - **mixer gain matrix**: `Mix X Input NN Playback Volume` for every (bus, input)
    slot (8PreX: 16 × 30 = 480), a 0.5 dB-step volume (−80..+12 dB, TLV) matching
    scarlett2. A change rewrites one 16-bit coefficient in that bus's `SET_MIX` row
    (seeded from the blob's all-unity default) and resends it. Same
    not-yet-hardware-verified caveat as the routing writes
  - **`Level Meter`**: a read-only 48-channel control (0..4095), snapshotted from
    the `GET_METER` heartbeat the driver already runs. Marked volatile so userspace
    re-reads live. Channels are the raw device meter order (scarlett2 reorders via a
    per-model meter map, not yet derived here). See `Meter Source` above for
    selecting which channel set the *hardware* meters display
- **Control names match the in-kernel scarlett2 driver.** For the models with a USB
  sibling (2Pre/4Pre/8Pre): inputs are `Line In N Air Capture Switch` /
  `Line In N Level Capture Enum`, and outputs are `Line NN (descr) Playback Volume`
  (e.g. `Line 01 (Monitor L) Playback Volume`), so `alsactl`/`alsa-scarlett-gui` see
  the same names as on the USB units. The **8PreX** follows the same scheme
  (`Line In N Air ...`, `Line NN (...) Playback Volume`) but keeps `Mode` in place of
  `Level` for its input-mode enum, since it exposes a richer Mic/Line/Inst mode than
  scarlett2's Line/Inst "Level".
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

## Model selection (auto-detected; `model=` overrides)

The entire Clarett Thunderbolt line shares PCI id `1cb5:0002` and presents a
byte-identical **pre-mailbox** surface — every MMIO register, config-space read, the
fw-info header, and even the dummy serial are the same across models (verified on real
2Pre/4Pre/8PreX hardware). But once armed, the device reports its own stream geometry:
`GET_7.1{band 0}` answers `{u16 playback_channels, u16 capture_channels}`, a pair unique
per model (live-confirmed `(4,14)` 2Pre, `(8,20)` 4Pre, `(28,28)` 8PreX). The bring-up
itself is model-agnostic (the same blob armed all three bench units), so the driver arms
first, asks second:

```sh
sudo insmod snd-clarett.ko                # auto-detects the model from the armed device
sudo insmod snd-clarett.ko model=8prex    # or force: model=2pre / model=4pre / model=8pre
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
   amixer -c <n> sset 'Mute' toggle      # also 'Dim', input Air/Mode, etc.
   ```
   (Note: writes complete but won't change the hardware yet — see the ⚠️ at the top.)

Never load this while the VM is using the device.

## Known limitations / TODO

- **Routing and mixer-gain writes are not hardware-verified.** The patchbay enums resend the whole
  `SET_MUX` matrix and the mixer-gain controls resend a `SET_MIX` row (both seeded verbatim from the
  arm blob, one entry/coefficient edited), matching FC's traced change cycles — but neither has been
  exercised on a device. Verify on the bench before relying on them. Also pending on routing:
  per-model destination pin maps (names are 8PreX-accurate), the full source space (the enum lists
  the sources present in the default matrix), and routing a source dropped at a higher sample-rate
  band into a destination that survives it.
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
- **`early_msi=` (default 1)**: enable MSI in the device's **config space** before any
  BAR access, matching the vendor's attach order. The cold trace shows Windows programs
  the MSI capability and sets the enable bit (`@0x4a=0xa5`: enabled, 4 vectors) *before*
  its first pre-mailbox BAR write; our old probe order left MSI disabled through the
  entire 232-command arm + seed — a device-visible pre-command-#0 config-state
  difference present in every walled run (including the Fedora-guest control), invisible
  to the BAR-only pre-mailbox replay. Set `0` for the old late enable (A/B).
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
  per-output mute enables (byte 72/73) are now exposed as `Line NN Mute Playback
  Switch`, and the gain enables (byte 52) as the SW/HW volume-control enums. The
  Monitor Out 1-2 mute+dim enables are still force-set at probe so the global
  `Mute`/`Dim` act on the monitors by default. The per-output **dim** enables are
  not individually exposed (scarlett2 has no per-output dim).
## Settings persistence & `alsactl` (device-owns-the-state)

This driver follows the same policy as the in-kernel scarlett2 / 4th-gen Scarlett
driver: **the device owns its settings.** They live in the interface's own NVRAM
and survive power cycles, reboots, and moving to another host — the driver does
not need to re-apply them.

- **The driver auto-persists changes, debounced.** Each control commits live
  (`DATA_CMD{activate}`) and then, ~2 s after the last change, a single
  `DATA_CMD{FCP_ACTIVATE_PERSIST}` (command 5) writes the config to NVRAM
  (`clarett_save_work` / `CLARETT_SAVE_DELAY_MS`). The debounce coalesces a burst
  (e.g. a volume drag) into one flash write, matching scarlett2's 2 s save and
  FC's own traced behaviour. A pending save is flushed at unload/reboot.
- **Disable `alsactl` restore for this card**, or it will overwrite the device's
  own stored state on every load. The `alsa-state`/`alsa-restore` services save
  ALSA control state to `/var/lib/alsa/asound.state` and replay it on card add —
  and since the device already restored the true state from its own NVRAM, a
  replayed snapshot just fights the hardware (worse: preamp Mode/Air don't read
  back reliably across a cold boot — see the limitation below — so the snapshot
  can be stale). This is the same issue the scarlett2 FAQ documents. If no other
  card needs the service:
  ```sh
  sudo systemctl mask alsa-state alsa-restore
  sudo systemctl stop alsa-state alsa-restore
  sudo rm /var/lib/alsa/asound.state
  ```
  To verify: toggle the `Inst`/`Air` of an input, power-cycle the unit, reload —
  the setting should stay as the device had it, not snap back to a saved state.
- **Known limitation: preamp Mode/Air display can be wrong after a cold boot.**
  The preamp config bytes (Mode `166+i`, Air `174+i`) *are* `GET_DATA`-readable and
  read back correctly at their write offset **within a powered session** — so the
  display is accurate after any control change, and across a warm `rmmod`/`insmod`.
  But on a **cold boot** the device restores the Mode/Air state to the *hardware*
  from NVRAM (front-panel LEDs are correct) while bringing the host-readable
  appspace up at a fixed default that does **not** mirror it. So right after a
  power cycle `alsamixer` may show a default (both Inst+Air) that disagrees with
  the LEDs, until you touch a control — from then on it tracks correctly. This is
  the device's *host-authored appspace* behaviour (the same reason `alsactl`
  restore fights it, above), not a driver bug; there is no reliable read of the
  true preamp state on a fresh boot from this region. The hardware is always
  correct regardless — the mismatch is display-only and self-heals on first touch.
  Diagnostic levers for any future dig: `seed_dump=1` (one-shot full `[0,256)`
  shadow dump at probe) and `put_trace=1` (log each control write's offset/value).
- Single-card only; no module params for index/id (but see `model=` above).
- **2Pre: experimental flat-buffer capture PCM** (`enable_pcm=1`). A RAM dump of the live
  2Pre stream showed it streams a **flat contiguous sample ring** at `0x210`/`0x310` with no
  descriptor table (unlike the 8PreX's scatter-gather list), with asymmetric TX 4ch / RX 14ch
  periods (`0x40` / `0xe0`). The driver uses a separate flat-buffer engine path (`flat_buffer`
  model flag) sharing the 8PreX's register arm + servicer. The ring/wrap length
  (`CLARETT_FLAT_FRAMES`, currently 1024 frames = the VM's observed 16 KB TX area) is a
  hypothesis pending hardware confirmation — capture may not yet clock or may wrap elsewhere.
