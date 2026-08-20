# snd-clarett — internals & reverse-engineering notes

Maintainer / contributor documentation. For build and usage, see [README.md](README.md).

This is a clean-room driver: built only from Focusrite's device XML descriptors, black-box
MMIO captures, and the public `scarlett2`/FCP documentation. No vendor driver code was
disassembled or copied. Encodings are per-model — values are never assumed to carry across
models.

## The control-plane crossing (the "manifestation wall")

Confirmed on hardware: preamp Mode and Air move a 2Pre's relays and front-panel LEDs,
monitor Mute/Dim act on the outputs, the SW/HW selector hands an output to the front-panel
knob, and `GET_DATA` returns real data — including the knob's own position, live. Encodings
are verified by reading the device's bytes back, not by watching the hardware, since a relay
clicks the same for a right and a wrong value.

This was not always so. For most of this project's life every write completed and nothing
happened — a wall attributed to an "off-wire / below-driver" boundary after four independent
methods agreed. **That attribution was wrong**, and the negative results it produced are worth
distrusting before you trust any negative result here. The eliminations behind it were all
real: the vendor's MMIO, config space, DMA footprint, and cold-boot behaviour genuinely did
match ours on every host-visible surface. What was wrong was the conclusion drawn from them —
that the difference must therefore be off-wire.

It was a race we had introduced: the trailing doorbell ack (`0x408=2`) was sent as soon as the
command completed, before the device's response DMA had landed, which the device answers by
refusing the session outright. Every vendor trace had been captured under MMIO trapping at
~20 µs per access, under which the response had always landed long before the ack — so the
traces could not show a precondition they always satisfied. Gating the ack on the response
actually arriving fixed it. **Lesson: be suspicious of every write whose meaning is an
acknowledgement, and characterize failures by their onset, not their endpoint.**

## Bring-up and probe

By default the probe **does not arm** the device. A previously-armed unit self-arms across a
power cycle — its config reads, input metering, and control writes all work with no host bring-up
(the arm state is flash-persisted). So probe waits for that flash-persisted session to answer
(`clarett_detect_model()` polls `GET_7.1` for up to `wait_ready_ms`), detects the model from it,
and leaves the device's own routing untouched.

If the device never answers within the settle budget — a cold Thunderbolt attach can race device
readiness (command #0's response may not land) — probe **fails loudly** (`-ENODEV`, no card)
rather than registering a placeholder, so the condition gets attention. A used device usually
just needs a reload; a genuinely virgin/never-armed unit (which rejects `GET_DATA` until armed)
must opt in with **`force_arm=1`**.

Only under `force_arm=1` does `clarett_arm_device()` replay the de-blobbed typed init table
(per-model `clarett_arm_<model>.h`: a `CONFIG_PUSH` burst, subsystem enables, an 8 KB config sync,
and `SET_MIX` + `SET_MUX`). It arms with the id_table default, detects the real model, and
re-applies its routing — while **preserving any live routing** a configured device already holds
(`clarett_band0_routed()` reads band-0 first; if it is populated, the `SET_MUX`/`SET_MIX` steps
are skipped).

At probe the driver also seeds its config shadow from the device (`GET_DATA(24,92)`) and
**force-enables hardware Mute/Dim for Monitor Out 1-2** (bytes 72/73, command 3) so the global
`Mute`/`Dim` actually act on those outputs — the master flag alone (offset 24/28) does nothing
unless an output opts in via its enable bit.

## The userspace FCP (hwdep) model

This driver follows the in-kernel FCP model used by the 4th-gen Scarlett (`sound/usb/fcp.c`):
**a minimal kernel driver that exposes a hwdep interface**, with Geoffrey Bennett's userspace
`fcp-server` implementing the controls. There is no in-kernel mixer — the control layer was
removed once the hwdep path worked on hardware (see git history if you need the old control
set; the encodings it carried live on in the device maps).

The hwdep ABI is complete: `PVERSION`, `CMD`, `INIT`, `SET_METER_MAP`/`SET_METER_LABELS`, and
the notification `read()`/`poll()` relay.

- `CMD` maps straight onto the mailbox — the FCP wire packet *is* our mailbox packet, and the
  opcodes are ours.
- `INIT` runs `INIT_1`/`INIT_2` with the opcodes `fcp-server` passes and returns the
  firmware-info block in `step2[]`. `step0` is zero-filled (its USB `STEP0` class request has
  no mailbox equivalent; `fcp-server` ignores it), and `c->seq` is *not* reset — the in-kernel
  `GET_METER` heartbeat shares it, and the device only echoes seq.
- `SET_METER_MAP`/`SET_METER_LABELS` let `fcp-server` create and drive `Level Meter`: it
  installs a channel→raw-slot map (the control's `.get` polls `GET_METER` and projects through
  it) plus an optional `FCP_CHANNEL_LABELS` TLV.
- The notification relay blocks until the device signals a change (the `0x400` cause).
  **Adaptation:** the USB FCP device carries a precise notification bitmask in its interrupt
  message; this Thunderbolt device only signals *that* something changed, so we deliver an
  all-categories event (`~0`) and `fcp-server` re-reads every notifiable control — broad but
  correct, and the device gives us nothing narrower. In fact the `0x400` signal is a **periodic
  heartbeat at ~13.4 Hz**, not a change event (measured — the rate is identical idle, under
  load, and while a control is being turned): it says "re-read me" on a fixed cadence and never
  says what changed, capping front-panel tracking at one update per ~75 ms. Wakes are
  **rate-limited** to one per `notify_ms` so a burst collapses into one re-read. It must be a
  rate limit (fire one interval after the *first* of a burst) and not a debounce (one interval
  after the *last*): against a source that never goes idle, a debounce never fires at all — it
  was a debounce once, and userspace saw exactly one notification per session.

### Device maps

The device does not self-describe — `DEVMAP_INFO` returns size 0, so `fcp-server`'s
device-provided map path yields nothing. The maps in `fcp-server-data/` are authored instead,
keyed on the model slug the driver publishes at `/proc/asound/cardN/clarett` (the whole line
shares PCI id `1cb5:0002`, so the id cannot select a model). They currently require local
`fcp-server` patches — model-slug keying, inverted-value support, and chunked `MUX_READ` — see
`fcp-server-data/README.md`.

The in-kernel `GET_METER` heartbeat still runs, since the device appears to need it to apply
control writes; its response is discarded (`fcp-server` polls the meter itself). That
"heartbeat needed to apply writes" hypothesis predates the wall crossing and is pending
re-audit.

## Model detection

The entire Clarett Thunderbolt line shares PCI id `1cb5:0002` and presents a byte-identical
**pre-mailbox** surface — every MMIO register, config-space read, the fw-info header, and even
the dummy serial are identical across models (verified on real 2Pre/4Pre/8PreX hardware). But
from its flash-persisted (self-armed) state the device reports its own stream geometry:
`GET_7.1{band 0}` answers `{u16 playback_channels, u16 capture_channels}`, a pair unique per
model (live-confirmed `(4,14)` 2Pre, `(8,20)` 4Pre, `(28,28)` 8PreX). Probe reads this directly
to detect the model — no host bring-up needed, since a used device self-arms from flash. (Under
`force_arm=1` the model-agnostic bring-up runs first and detection follows.)

**Detection is the only path — there is no override, by design.** The id_table's 2Pre exists
solely as the stand-in for `force_arm`'s bring-up, which has to run before the device can be
asked; nothing model-dependent may be sized before `GET_7.1` answers. If the device does not
answer, or answers with a geometry no `clarett_model` claims, probe **fails with `-ENODEV` and
registers no card**, logging the raw `playback=/capture=` pair. It does not fall back to a
plausible model: channel counts, DMA ring and descriptor geometry, fragment strides, routing and
mixer tables, and the meter layout are all derived from `c->model`, so a wrong model is not a
mislabel but a card streaming the wrong width into wrongly strided rings. Adding genuinely new
hardware is a `clarett_model` entry keyed on the logged pair — not a module parameter.

Because the PCI id is shared line-wide, the detected model is published for userspace at
`/proc/asound/card<N>/clarett` as a stable, greppable slug — the key `fcp-server` uses to
select its per-model control map, since the PCI id cannot:

```
model: Clarett 8PreX
slug: clarett-8prex
```

Slugs: `clarett-2pre` / `clarett-4pre` / `clarett-8pre` / `clarett-8prex`. Unlike `card->id`,
the slug is never mangled for uniqueness, so it is a reliable contract.

There is no Thunderbolt-DROM auto-detect of any kind. The model name *does* live in the DROM,
but the entire Clarett line is **Thunderbolt 2** (discontinued before any TB3 model), and
whether such a unit is enumerated as a kernel-managed TB router — rather than merely
firmware-tunnelled onto the PCI bus — is **host-firmware dependent**: an ASRock X570 Creator
lists none of the Clarett Thunderbolt units in `boltctl`, while an HP EliteBook 840 G5 lists and
authorizes all of them (see the driver README). So a `device_name` under `/sys/bus/thunderbolt`
may exist on some hosts and not others, which makes it useless as a contract even where it is
present. The per-model slug below is the thing userspace should key on.

### Per-model descriptors

The **4Pre** descriptor is built from the device XML and cross-checked against a live capture:
the input/output control map is `[XML]` (Analogue 1-2 Line/Inst + Air, 3-4 Air-only, 5-8 none;
six output gains @ 32/33/36/37/40/41), while the channel counts (8 playback / 20 record), the
bring-up replay, the stream-routing ids, and the Analogue-1 toggle are `[TRACE]`-confirmed.

The **8Pre** (distinct from the 8PreX) gained a bring-up capture, so it can arm like the other
models under `force_arm=1`: `clarett_arm_8pre.h` carries its bring-up (hardware-verified on an
8Pre), arming config access and its own captured default routing. Its input/output layout is from
the XML: combo XLR/TRS jacks (Mic is auto-detected by the jack, so the software mode is
Line/Inst only, on inputs 1-2; 3-8 are air-only) unlike the 8PreX's separate ports, outputs
matching the 8PreX (10 gains), and `(20, 20)` streams for detection. Its stream-routing ids are
derived from the model-independent source-id enumeration (equal to the 4Pre's, whose input
layout is identical), not captured.

PCM streaming on the 8Pre is now confirmed on hardware, which validates its `(20, 20)` stream
geometry. Still **predicted**, and worth checking against hardware, are its routing sources and
meter `peak-index` values — routing from the 4Pre's identical input geometry, meters from the
measured packing rule: confirm that `MUX_READ` reads the routing back as pushed, and that one
excited input at a time lights the predicted meter slot.

## PCM data plane

Full-duplex capture and playback run over one shared descriptor-ring engine, driven by the
`0x300` period servicer (`clarett_pcm.c`). Hardware-confirmed on the 2Pre, 4Pre, 8Pre, and 8PreX. The
long-standing one-ring-pass stall turned out to be the descriptor-table **format** — a missing
periodic RX IRQ marker, the flag that advances the counted `0x300` period — not a wall.

Caveats: low-latency streaming can suffer periodic audible skips traced to a
Thunderbolt-triggered platform SMI — a firmware/BIOS
issue, not the driver (the autonomous DMA rides through it; the click is the audio server
re-preparing). There is no default route, so playback is silent until a PCM source is wired to
a physical output in the router.

## Reverse-engineering / test setup

During RE the Clarett is passed through to a Windows VM via `vfio-pci`, and the vendor driver's
MMIO accesses are traced from the host. To test this driver on the **host**, the device must be
free for `snd-clarett` to claim:

1. Shut down the Windows VM.
2. Unbind from `vfio-pci` and let `snd-clarett` bind:
   ```sh
   echo 0000:09:00.0 | sudo tee /sys/bus/pci/drivers/vfio-pci/unbind
   sudo insmod snd-clarett.ko
   echo 0000:09:00.0 | sudo tee /sys/bus/pci/drivers/snd-clarett/bind   # if not auto-bound
   ```
   (Also remove any `vfio-pci.ids=` / `driver_override` or modprobe binding that re-grabs the
   device at boot.)
3. Verify with `amixer -c <n> contents`, then test a control:
   ```sh
   amixer -c <n> sset 'Mute' toggle      # also 'Dim', input Air/Mode, etc.
   ```
   These take physical effect (see the crossing note above).

Never load this driver while the VM is using the device.

## Internals & known limitations

- **Routing writes take effect; the mixer-gain matrix is less exercised.** Control writes
  manifest physically (Mode/Air, Mute/Dim, SW/HW gain are hardware-confirmed). Routing lives in
  the device maps where `fcp-server` builds the patchbay enums and the mixer matrix; reads decode
  correctly against the device's own tables (`PCM 01 Capture Enum` reads `Analogue 1` for
  `400 600`), and re-routing a source to a PCM capture is reflected in that channel's meter — but
  the full mixer-gain matrix has had less systematic testing. Also pending: for the captured
  models the source list is only the pins the factory-default matrix routes, not the device's
  full source inventory (the 8Pre map, not table-derived, lists all of them).
- **The config shadow is write-through.** `clarett_set_data()` keeps a shadow of the config
  space so the probe-time monitor-enable write can do a correct read-modify-write. It is seeded
  from the device at probe (`GET_DATA(24,92)`); bytes the device never reports back stay at their
  written value. Nothing reads it for display any more — `fcp-server` reads the device directly.
- **Mailbox completion is polled**, not MSI-driven. MSI *is* enabled, but for **async
  notifications** (vec0 / cause `0x400`) and stream period events: a front-panel button raises
  the dim-mute/monitor mask, and the ISR → workqueue re-reads the monitor region and
  `snd_ctl_notify()`s the monitor controls. The GET-response layout is decoded (16-byte echoed
  FCP header + requested bytes at +16), so the handler updates the monitor shadow to reflect
  physical button changes. Polling the mailbox DONE bit keeps it from racing the read-to-clear
  cause register against the ISR.
- **GET-response DMA buffer address** is programmed at `0x410` (low32) / `0x414` (high32) —
  `0x414` = address-high confirmed (hardcoding the trace's `0x2` faulted the IOMMU; the driver
  uses `upper_32_bits(resp_dma)`).
- **Packed bitfield controls** (per-output hardware gain/dim/mute enables). The gain enables
  (byte 52 + 4·(out/2), bit out%2) are exposed by the device maps as the SW/HW volume-control
  selector — hardware-confirmed: an output set to HW follows the front-panel knob. The Monitor
  Out 1-2 mute+dim enables are force-set at probe so the global `Mute`/`Dim` act on the monitors
  by default; the rest of the mute enables (byte 72/73) are **not** exposed, and neither are the
  dim enables. These are *enables* — whether the master section reaches an output — not per-output
  mute: this hardware has no per-output mute at all (one `<mute>` in every model's descriptor).

## Module parameters

The module carries a number of parameters, mostly diagnostic/experimental levers from the
reverse-engineering work. Run `modinfo snd-clarett.ko` for the complete, authoritative list.
The operationally relevant ones:

- `enable_pcm` (default on) — register the PCM devices; `0` for a mixer-only card.
- `enable_midi` — register the DIN MIDI rawmidi.
- `notify_ms` — rate limit for the front-panel notification relay.
- `max_rate` — override the highest advertised sample rate for all models (`48000`/`96000`/`192000`).
  `0` (default) uses each model's hardware-confirmed cap: single speed (44.1/48) everywhere, plus
  double/quad on models where the high-rate data plane is confirmed (the 2Pre, to 192 kHz). Set it to
  test double/quad speed on a not-yet-confirmed model — the stream width is rate-independent, so verify
  with a known tone (correct pitch on the analogue channel) before trusting a rate on an ADAT model.
- `force_arm` (default off) — run the vendor bring-up at probe. Only needed for a
  virgin/never-armed unit; used devices self-arm from flash. With it off, probe waits for the
  flash-persisted session and fails loudly if the device never becomes ready, rather than arming.
- `wait_ready_ms` (default 2000) — settle budget to wait for the flash-persisted session to
  answer at probe before giving up. A cold Thunderbolt attach can race device readiness.

Several parameters are A/B levers retained from localizing the control-plane crossing (e.g.
`resp_prefill`, `premailbox_reads`, `early_msi`, `mmio_dilate_us`) and from the data-plane
bring-up; their `MODULE_PARM_DESC` strings carry the detail.

## Settings persistence (internals)

The device owns its settings — they live in the interface's NVRAM and survive power cycles,
reboots, and moving to another host, so the driver does not re-apply them. This mirrors the
in-kernel scarlett2 / 4th-gen Scarlett policy.

- **The driver auto-persists changes, debounced.** Each control commits live
  (`DATA_CMD{activate}`), and ~2 s after the last change a single
  `DATA_CMD{FCP_ACTIVATE_PERSIST}` (command 5) writes the config to NVRAM (`clarett_save_work`
  / `CLARETT_SAVE_DELAY_MS`). The debounce coalesces a burst (e.g. a volume drag) into one flash
  write, matching scarlett2's 2 s save and FC's own traced behaviour. A pending save is flushed
  at unload/reboot.
- **`alsactl` restore must be disabled for this card** (see README.md), or it overwrites the
  device's own stored state on every load — the device already restored the true state from its
  NVRAM, so a replayed snapshot just fights the hardware. This is the same issue the scarlett2
  FAQ documents.
- **Preamp Mode/Air display can be wrong after a cold boot.** The preamp config bytes (Mode
  `166+i`, Air `174+i`) *are* `GET_DATA`-readable and read back correctly at their write offset
  **within a powered session** — so the display is accurate after any control change, and across
  a warm `rmmod`/`insmod`. But on a **cold boot** the device restores Mode/Air to the *hardware*
  from NVRAM (front-panel LEDs correct) while bringing the host-readable appspace up at a fixed
  default that does **not** mirror it. So right after a power cycle `alsamixer` may show a default
  (both Inst+Air) that disagrees with the LEDs, until you touch a control — from then on it tracks
  correctly. This is the device's host-authored appspace behaviour (the same reason `alsactl`
  restore fights it), not a driver bug; there is no reliable read of the true preamp state on a
  fresh boot from this region. The hardware is always correct — the mismatch is display-only and
  self-heals on first touch. Diagnostic levers for any future dig: `seed_dump=1` (one-shot full
  `[0,256)` shadow dump at probe) and `put_trace=1` (log each control write's offset/value).
- Single-card only; no module params for index/id.
