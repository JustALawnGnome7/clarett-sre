# Clarett 8PreX — Linux ALSA Driver (Reverse-Engineering Project)

Clean-room reverse engineering of the **Focusrite Clarett 8PreX** (Thunderbolt
audio interface) to build a native Linux ALSA driver. This file is the portable
project memory: it captures state, key facts, and conventions so any fresh
session (or contributor) can continue without the original chat history.

## Goal & status

Build an in-kernel ALSA driver for the Clarett line (2Pre/4Pre/8PreX).
**THE MANIFESTATION WALL IS CROSSED (July 16 2026 — `spec/provenance/clarett-manifestation-wall.md` §8).**
The year-defining "off-wire/below-driver wall" was a **timing artifact of the measurement
apparatus**: every "known-good" vendor trace ran under x-no-mmap MMIO trapping (~20 µs/access),
under which the device's asynchronous response DMA had always landed before the trailing doorbell
ack (`0x408=2`); our native-speed replay acked ~µs after DONE, **before the response landed** — a
protocol violation the device answers with a blanket `err=3` session refusal from command #0 (which
masqueraded as an attach-time gate). Gating the ack on the response actually landing
(`clarett_resp_wait` in `clarett_mailbox.c` + pre-submit response-header zero; levers
`gated_ack`/`resp_trace`/`mmio_dilate_us`) arms the session.
- **Control plane** — **WORKS ON REAL HARDWARE**: full 232-command arm + seed answer `err=0` with
  real data (seq echoed, CONFIG_PUSH port names, 8 KB config read full, serial/fw answered), and
  **control writes manifest physically** — user-confirmed Mode/Air toggles from alsamixer move the
  2Pre's front-panel LEDs and switch its relays. **Attribution matrix CLOSED 3/3 (July 16, fresh DC
  power-cycle each): two gated runs arm clean, the levers-off control run walls (seed `-5`) — the
  landed-gated ack + pre-submit header zero are now the unconditional default cycle** (`gated_ack`
  lever retired; `resp_trace` kept as telemetry). **PENDING:** re-audit the shadow/`GET_DATA`
  refresh paths and the `meter_poll_ms` "heartbeat" hypothesis (both written for a walled device).
- **Data plane** (PCM DMA streaming) — **extensively traced and reverse-engineered**
  (boot→stream captures + guest-RAM dumps). The engine **plumbing is validated** — arms
  cleanly, DMAs a burst, descriptors correct (no IOMMU faults), PTR advances — **but won't
  sustain past one ring pass** (flags period 0, the `0x300` counter never advances).
  **Its "same below-BAR wall" attribution is now VOID** — retest on an armed session; the stall
  may be the same ack-timing class on the stream cause blocks, or may resolve outright.
  Details: `spec/provenance/clarett-data-plane.md`.
- **How the wall was crossed (method lesson — carry this):** the wall had been "confirmed
  below-driver" by four independent methods (Windows/vfio MMIO, macOS DTrace, our Linux replay,
  WinDbg of `FocusritePCIe.sys`) — every host-visible surface, warm and cold, matched the vendor
  byte-for-byte, and clean-room RE was declared at its terminus. All those negatives were **true
  facts but the localization was wrong**: byte-identical traffic under a time-dilating instrument
  is not identical behavior. The traces couldn't show that the vendor's trailing ack was (in
  effect) conditioned on the response DMA having landed, because under trapping it always had
  (≥242 µs after submit in every capture). The exercise that found it: walk the transaction cycle
  asking, for each host action, "is this valid the instant the previous MMIO completes, or is it
  semantically conditioned on something the device does asynchronously?" — and be suspicious of
  every write whose meaning is an acknowledgement. Characterize failures by their **onset**
  (`resp_trace` per-command telemetry), not their endpoint.
- **Historical eliminations that remain true** (kept in `manifestation-wall.md` §§1–7, macOS/WinDbg
  plans): vendor init DMA footprint == ours (2×{16 KB descriptor CB + 2 MB sample MDL} + 4 KB
  response CB, nothing extra programmed at init, no mailbox pointer-push); cold boot == warm on all
  three surfaces; **firmware-over-DMA disproven** (FPGA self-boots from flash); config space
  byte-for-byte; MSI ordering/counts matched; environment ruled out (Fedora-guest passthrough);
  `0x400` is a 2-bit command-phase register, not an event queue. Still excluded: bus analyzer
  (user ruled out), disassembling the vendor driver/kext (clean-room no-go).

## Method (how the RE is done)

The device is PCIe-passed-through (`vfio-pci`) to a Windows 10 VM on a Linux host
running Focusrite Control. We trace the Windows driver's MMIO accesses to the
device's single 64 KB BAR0 from the host by disabling the vfio BAR mmap so every
access traps into QEMU:

- libvirt domain XML: `xmlns:qemu` on `<domain>`; `<hostdev>` aliased `ua-clarett`;
  `x-no-mmap=true` via `<qemu:override>` (NOT `-set` — fails on JSON `-device`);
  `-trace enable=vfio_region_*` via `<qemu:commandline>`.
- Stock Fedora QEMU has **no runtime trace events** → must run a **custom
  trace-enabled QEMU build** (`--enable-trace-backends=log`), pointed to via
  `<emulator>`, with `-L .../pc-bios`. SELinux: set `security_driver="none"` in
  `/etc/libvirt/qemu.conf` for the dev box.
- Trace lands in `/var/log/libvirt/qemu/<domain>-custom.log` (UTC timestamps —
  compare with `date -u`, not the GNOME clock).
- `tools/fcp_decode.py` parses `vfio_region_*` lines into structured FCP
  transactions. Use `--brief` for one-line-per-command; pipe a live `tail -f`.

Workflow per control: predict the FCP payload from the device XML, toggle ONE
control in Focusrite Control, find the matching mailbox transaction in the trace.

## Hardware facts

- PCI ID **1cb5:0002**, class Multimedia audio controller.
- **Single 64 KB MMIO BAR0** = entire register interface (control mailbox + DMA
  control). Audio samples move by bus-master DMA, not through the BAR.
- **4 MSI vectors**; MSI-driven (`DisINTx+`). PCIe Gen1 x1. **Dummy serial AND dummy firmware-version
  words** — an 8Pre and an 8PreX report byte-identical `serial`/`fw app`/`fpga`, so none of the three
  identifies a unit or a model (Aug 21 2026; see the detection bullet under Driver limitations).
  **A Red 8Line reports the same three constants too** (Sep 2 2026) — so this holds across product
  *lines*, not just within the Clarett line. See the Red range section below.
- FPGA-based Thunderbolt front-end (firmware has App + FPGA segments).

## Red range — first hardware contact (Red 8Line, Sep 2 2026)

**The Sep 2 raw captures were lost to a host crash (both lived in `/tmp`) — but a full replacement
exists: `captures/red_8line.log` (Sep 4 2026, 44 MB, 516k lines, 16074 FCP transactions, 5526 of them
non-meter), a complete vendor session covering bring-up AND streaming.** Write every Red trace straight
into `captures/`, never `/tmp`.

- **`GET_7.1` geometry = `playback=64 capture=60`.** `snd_clarett` bound on the shared PCI id, read it,
  and refused to register. That is the pair a `struct clarett_model` entry needs, and it closes the
  "read the Red's GET_7.1 counts" TODO in [[focusrite-red-8line-incoming]].
  **This was also the first real-world exercise of the unknown-geometry `-ENODEV` path**, previously
  listed as untested under the model-detection bullet: it fired correctly and logged the raw pair to
  key on, exactly as designed.
- **★ REGISTER `0x000` DISTINGUISHES RED FROM CLARETT — the first host-visible register that does,
  and it needs NO mailbox transaction.**
  | | `0x000` |
  |---|---|
  | Clarett 2Pre / 4Pre / 8Pre / 8PreX — 13 captures, many sessions | `0x032003fd` |
  | Red 8Line — two independent traces, taken under two *different* Windows drivers | `0x042003fc` |

  `XOR = 0x07000001` → bits **[26:24]** go `3 → 4`, plus **bit 0**. The nibble reads like a
  family/generation field, but that is inference from two data points.
  **Limits, both important:** one Red unit only (may not hold for Red 4Pre/8Pre/16Line), and it does
  **not** separate models *within* a family — all four Claretts share `0x032003fd` — so
  `GET_7.1` geometry remains the model-level discriminator and `0x000` is at best a family gate.
  This is a genuine correction to the standing "the pre-mailbox surface is identical across the line"
  claim: identical across the *Clarett* line, yes; across *lines*, no.
- **The identity constants extend to the Red line**: `0x8000 = 0x04061973` (fw app),
  `0x8004 = 0x18101966` (fpga), serial `0x10`/`0x14` = `0x5678abcd`/`0x1234` →
  `0x000012345678abcd`. Byte-identical to every Clarett, so nothing may key off them — see Hardware facts.
- **The whole `0x8000`–`0x801c` fw-info header is byte-identical to the 2Pre's**, including
  `0x8010 = 0x400`, `0x8014 = 0x410`, `0x8018 = 0x20`. **HYPOTHESIS, unproven:** that block is a
  self-description of the transport — doorbell block base, response-DMA pointer register, DONE bit —
  which is what a driver would read to adapt across models. It cannot be confirmed adaptive precisely
  *because* it is constant on every unit seen; disproving it needs a device whose transport differs.
- **The MIDI transport ports to the Red line**: `0x510`/`0x514` (`0x847`), `0x58c`, and `0x500`
  reading its documented idle `0xff0000` all appear on the Red exactly as in the 2Pre decode.
- **The vendor driver writes `0x414 = 2`** — it places the response DMA buffer above 4 GB
  (`0x2_5b6ff000`), an independent confirmation that `0x414` is a real 64-bit address high word.
- **★★ THE BRING-UP IS DE-BLOBBED — `tools/arm-tables/arm_red_8line.h` (Sep 4 2026).** From
  `captures/red_8line.log` via `fcp_decode.py --emit-init` → `--emit-deblob`, byte-faithful round-trip
  (`--deblob-check` PASS, 391 steps: 377 typed / 14 raw). Three `SET_MUX` bands of **157/141/99**
  entries against the 8PreX's 103/87/79; band 0 gives **156 routed pairs, 156 distinct destinations,
  109 distinct sources**, with analogue in `0x400-0x407` → capture `0x600-0x607` (note the reversal
  within each group of four: `0x403→0x600`, `0x402→0x601`, …). **This clears blocker (1) on the Red map
  pair** — see `fcp-server-data/README.md`; the sole remaining blocker is authoring a Red control model
  in `gen_fcp_maps.py` (16-bit gains, Red offsets, richer preamps), which needs no new measurement.
- **★★★ THE RED'S CONTROL PLANE WORKS END TO END — fcp-server DRIVES IT AND A WRITE MANIFESTS
  PHYSICALLY (Sep 4 2026, ASRock X570 Creator).** `fcp-server@4` loads the map pair and registers
  **1252 controls**; `amixer -c 4 cset name='Line In 1 Phantom Power Capture Switch' 1` **lit phantom
  power on input 1 of the unit's front panel** (user-confirmed, then reverted). That is the Red's
  equivalent of the 2Pre LED/relay confirmation and it closes the loop the whole map-authoring
  exercise was for. Air and high-pass wrote cleanly on the same input with input 2 unmoved, so the
  per-input indexing is right as well.
  - **Both mandatory capability gates PASS on a non-Clarett device** — no "does not support required
    INIT/DATA category". The flash-persisted session really is enough on the Red too.
  - **★ THE MIXER MATRIX CAME UP UNAIDED — 32 mixes x 32 inputs, 1024 gain controls.** fcp-server
    reads `MIX_INFO` from the device and builds the grid; the map contributes only
    `mixer-input-index` on the Mixer Input destinations. This **retires the "MIX_INFO dimensions have
    not been read from a Red" limitation** — they never had to be, and now they are known anyway.
  - **Output volumes confirmed live as SIGNED dB**, not inferred from the one vendor write:
    `offset 24..34 = 0`, `36/38 = -110`, `40..50 = -112`.
  - **The router reads the factory patch back coherently in both directions** — Monitor 1/2 <- PCM 1/2,
    headphones <- PCM 9/10, line outs 7-14 <- PCM 1-8, 155 selectable sources per destination. Pin ->
    name resolution is right as a source *and* as a destination, which is the thing a direction-scoped
    pin table most easily gets wrong.
  - `Sync Status` = Locked; the driver's own `Clock Source` offers all seven Red sources.
  - **TWO TRAPS, both fixed, neither in the map's content.** (1) **The devmap MUST carry a top-level
    `enums` block** — `init_global_controls()` hard-fails (-1) without one, so the server exited 1 on
    `Cannot find enums in device map` before creating a single control. The requirement was already
    documented on the Clarett path in `gen_fcp_maps.py` and simply had not been carried into
    `build_red_8line()`. (2) **The repo Makefile's `install-maps` globbed `fcp-devmap-clarett-*.json`,
    so `make install` SILENTLY SKIPPED the Red pair while reporting success** — which is why its copy
    in `/usr/local/share/fcp-server` had to be made by hand. (That Makefile is gone since Sep 25 2026;
    fcp-support installs the maps.)
  - ~~Mixes past Z render as `Mix [` …~~ **FIXED:** fcp-server names them `Mix AA`…`AF` (fork
    `b446906`), and the map's router sources use the same letters (Sep 25 2026).
  - **★★ METER SLOTS SOLVED WITHOUT A MEASUREMENT SWEEP (Sep 25 2026): a destination's `GET_METER`
    slot == its index in the band-0 `SET_MUX` table of the vendor bring-up.** Checked against all four
    measured Clarett layouts with zero exceptions — and it explains their two old mysteries: the 2Pre's
    "dark S/PDIF" slots 16-17 and the 4Pre's "unidentified" 26-27 are the LOOPBACK destinations'
    positions. The Red's 156 slots were read off its band 0 and **user-confirmed on hardware** (every
    routing tried lit the expected meter). **And LOOPBACK IS METERED on the 2Pre (Oct 8 2026):** with
    loopback fed from playback PCM 1/2, PCM 13-14 (slots 16-17) move with the audio. The old "loopback
    unmetered on the 2Pre/4Pre" reading came from watching those slots with nothing feeding loopback.
    Maps now meter the 2Pre pair ("measured") and the 4Pre's 26-27 (measured Oct 9 2026: loopback fed from
    playback PCM 1, then PCM 2, lit slot 26, then 27, and nothing else).
    **The per-rate bands 1/2 follow the same rule (Oct 2 2026):** `gen_fcp_maps.py`
    now derives `peak-index-m/-h` for every model from them, and it is hardware-confirmed on the Red,
    8PreX and 4Pre (see `spec/provenance/clarett-rate-aware-plan.md` item 1). It also CONFIRMED and fixed
    the suspected 2Pre/4Pre error: the old ranking skipped the loopback slots, so every mixer-input meter
    read 2 low above 48 kHz.
  - **★ THE LEVEL METER CAN HOLD ONLY 128 CHANNELS, AND THE DRIVER USED TO OVERFLOW THE KERNEL PAST
    THAT.** An ALSA INTEGER control's value array is 128 entries; `clarett_hwdep_meter_get()` wrote one
    per mapped channel and the meter-map ioctl accepted 255, so the Red's first 156-channel map wrote
    ~100 bytes past `snd_ctl_elem_value` on every read (alsa-lib then asserted in the GUI). Fixed: the
    ioctl refuses > `CLARETT_METER_MAX_CHANNELS`. ~~The Red map meters 124 destinations — the 32 Dante
    outputs go unmetered.~~ **Since Oct 7 2026 the driver splits a longer map across several `Level Meter`
    controls (same name, index 0, 1, ...; at most 128 each) and alsa-scarlett-gui joins them, so the Red
    meters all 156, Dante included** (hardware-confirmed Oct 8 2026: PCM 1-2 routed to every reachable stereo
    pair, every meter moved as expected). Input meters cost nothing
    (the GUI borrows a routed destination's level).
    The unit reports 58 slots, so fcp-server's `METER_SLOT_LIMIT` had to rise 128 -> 255.
  - ~~Still open for the map: the preamp gain ranges, the mixer ceiling, Meter Source on hardware.~~
    Meter Source done Sep 25 2026; **preamp gains + stereo link done Oct 2 2026** (clarett-sre
    `cd04b43`, fcp-support `6144a74`): 0.99 dB/code, 0..63 (64+ acts as 63, 1-7 act as 0) measured for
    Mic and Inst; Line mode's input is the DB25 and stays unmeasured. One fader per input follows the
    mode byte through fcp-server's new input-control `select`. Bytes 194/195 are ONE link switch; a
    linked pair keeps its gain OFFSET (a write moves the partner by the same delta), Air does not
    follow. Detail in [[red-8line-open-items]] 7d. ~~Still open: the mixer ceiling.~~ **Hardware side MEASURED Oct 9 2026:** playback -> Mixer Input -> Mix ->
    capture, coefficient stepped 1024..32613: unity 0x2000, gain EXACTLY linear to +12 dB (measured/expected
    1.00000 at every step, residual at the 24-bit floor), near the 16-bit top (+12.04 dB). The map keeps
    fcp-server's +12 dB default on purpose (operator's call). Still unread: RedNet Control's own fader top
    (needs a VM trace with a fader at its top; FC's on the Clarett is +6 dB).
  - **The front-panel red input (clip) indicator is NOT on the mailbox (Oct 2 2026).** It latches on
    a real overload and is cleared by that input's front-panel select button (user-found). Full
    read-only snapshots (all 16 KB of config space via GET_DATA, all 156 `GET_METER` words, the
    0x6002/4/5 status replies) taken clear -> red -> cleared differed in NO config byte and no status
    reply; meter words only tracked unrelated playback, and their upper 16 bits read zero throughout.
    The descriptor has no clip field either. So host software can neither see nor clear it; a GUI
    clip-hold would have to be synthesised from the meters. Untested: a meter read DURING clipping,
    and whether Focusrite Control has a clear command (needs a VM trace).
  - **★★ DANTE WORKS END TO END (Oct 2 2026, laptop host, no Dante Controller).** Rig: a gigabit PoE
    switch with the Red, an Audinate AVIO-DAI2 (2-ch analogue-in adapter) and the host's wired port.
    - **Network:** nothing on that switch serves DHCP, so every device sits on IPv4 link-local
      (169.254/16) and the host must too. A NetworkManager profile with `ipv4.method link-local`
      (+ `ipv6.method link-local`) works; `ipv4.link-local fallback` on a DHCP profile does NOT — it
      gets an address but still fails activation at the 45 s DHCP timeout and re-cycles.
    - **Control:** `netaudio` (chris-ritsen/network-audio-controller, 0.3.14) needs
      `--interface <wired-if>` (env `NETAUDIO_INTERFACE`), else it browses mDNS on Wi-Fi and finds
      nothing. The Red advertises as `Red8Line-<mac3>`: 32 TX / 32 RX, 48 kHz PCM24, 1 ms, and it is
      the PTP **clock leader** (the AVIO follows).
    - **netaudio reports every subscription to the Red as FAILED, but it is applied.** The Red's ARC
      version is **2.8.12**; netaudio accepts only 2.7.41/2.8.1/2.8.9/2.8.15, so its read-back after
      the write throws (`fresh readback was unavailable: unsupported ARC protocol version '2.8.12'`)
      and `subscription list` shows nothing for the Red. `netaudio -n 'Red*' flow receiver-list`
      reads the Red fine and shows the flow (`Subscribed (unicast)`). An upstream fix is a version-table
      entry; not filed.
    - **Factory routing already carries Dante RX 1-32 on capture PCM 27-58.** Discriminator with no
      signal: an unsubscribed Dante channel reads EXACTLY zero, a subscribed AVIO input carries its
      converter noise (-112.7 dBFS, 98 % non-zero).
    - **Analogue loop, sample-exact over a minute:** PCM 1 or 2 -> Monitor L/R -> cable -> AVIO in 1
      -> Dante -> Red RX 1 -> capture 27. A 60 s 1 kHz tone (period exactly 48 samples, so each sample
      must equal the one 48 earlier) came back with ZERO dropouts or repeats on both monitor sides;
      residual at the noise floor. Analogue offset Red monitor out -> AVIO in = **6 dB** (same at -30 dB
      and unity; AVIO input level setting unread — netaudio's `channel gain` failed with "no
      established gain adapter").
    - **Two traps that faked failures.** (1) The Red's monitor MUTE (front panel) silences the jack
      while the router meter still shows signal — the meter is pre-level; no map control exposes that
      mute. (2) PipeWire's default sink was the Red, so a desktop sound landed on PCM 1 mid-test (a
      150 ms 2-7 kHz chirp on top of an unbroken tone). Move the default sink off the Red for tests.
- **★★★ THE RED 8LINE STREAMS — FIRST AUDIO EVER OFF A NON-CLARETT DEVICE, AND IT NEEDED NO CODE CHANGE
  (Sep 4 2026, ASRock X570 Creator).** `arecord -D hw:5,0 -c 60 -f S32_LE -r 48000` runs and exits 0.
  - **Clock is correct: 47997.4 Hz measured against 48000 nominal, −0.01 %** (`hw_ptr` delta over a
    3.55 s span containing no re-trigger). Same order as the 2Pre's +0.09 %, i.e. unlocked-crystal
    offset, so `CLARETT_CTR_FRAMES=16` is exact on this model too. 44.1 kHz also runs.
  - **The audio is real and lands where the de-blobbed router says it should.** Capture channels 0-7
    carry an ADC noise floor at ~−105 dBFS (96 % non-zero, low byte always zero = 24-bit MSB-justified);
    channels 8-59 are exactly zero, being Dante/ADAT/S-PDIF/loopback with nothing connected. That is
    precisely `arm_red_8line.h` band 0's `0x400-0x407 → 0x600-0x607` — **the first cross-validation of a
    de-blobbed routing table against live audio**, and it came from the same capture.
  - **Why nothing had to change**, which is the load-bearing part: every register `clarett_engine_arm()`
    writes matches the vendor's Red values byte-for-byte; `clarett_period_bytes()` predicts `0x400`/`0x3c0`
    unaided; and the page-safe fragment rule covers the Red without special-casing (TX `64*4*16` =
    `0x1000`, exactly one page; RX `0xf00` is not pow2 and slots up to `0x1000`). The 2 MB contiguous
    DMA allocation — double the 8PreX's — succeeded.
  - **★ THE OVERRUNS ARE THE ASROCK PLATFORM FREEZE, NOT THE RED AND NOT THE BUFFER (corrected on
    measurement).** I first attributed 0.37-0.83 overruns/s to `max_buffer`'s 128-frame pin being too
    tight for a 60-channel client. **Raising it to 2048 changed nothing** (buffer/period confirmed
    2048/512 in `hw_params`), and the driver telemetry names the real cause outright:
    `readmax=42047us` — **a single MMIO read taking 42 ms** — with `gapmax` 45-52 ms and occasional
    `0x300 read returned ~0` blackouts. No buffer size fixes a 42 ms stall.
    **A ≥42 ms servicer gap appears in 153 of 153 windows (median 42.6 ms, max 52.7 ms)**, and the same
    is true of the 8PreX's telemetry on this box before the Red ever streamed — so it is the host, not
    the model and not two-devices-at-once. **This is 20x more frequent than the "~42 ms every 40-58 s"
    recorded for this board in [[clarett-playback-skipping]]**, which is an open discrepancy worth
    re-characterising: same fault, far higher rate, cause of the change unknown.
    **Why 2048 was not enough is arithmetic**: 2048 frames = 42.67 ms of runway against a 42.6 ms median
    freeze and a 52.7 ms worst case — exactly marginal. **`max_buffer=4096` (85.3 ms, the full ring)
    CLEARS IT COMPLETELY — 30 s at 60 channels with ZERO client overruns (confirmed).** The fault is not
    fixed, it is absorbed, and the distinction is visible in the telemetry: that same clean run still
    logged `gapmax` 46-60 ms (worst 60.3 ms, i.e. WORSE than the runs that failed) and the device still
    raised 3 unacked periods, while `readmax` stayed at 48-60 us — the servicer was stalled between
    polls rather than caught inside a `readl`. **So on this board a Red needs `max_buffer=4096`; 2048
    would have failed against the measured 60 ms tail anyway.**
    **Method note (my error, twice over):** I inferred "not the SMI" from the overrun *rate* being too
    high for the documented cadence, and "narrower streams are clean" from 2-second runs — far too short
    to sample a fault this intermittent. The driver already publishes `readmax`/`gapmax` telemetry that
    answers this directly. **Read the servicer telemetry before theorising about a stream fault.**
  - ~~**Still untested on the Red:** playback, any rate above 48 kHz, and the digital-loopback ramp.~~
    **ALL DONE.** Playback and the ramp landed Sep 25 2026 (see [[red-8line-open-items]]).
    **96 and 192 kHz VERIFIED Oct 2 2026** and `max_rate = 192000` set (snd-clarett `90d8244`): capture
    clock 95980 / 192104 Hz, and the ramp arrived sample-exact through channels live at each speed
    (playback 61 -> capture 51 at 96k, 31 -> 31 at 192k). Per the descriptor and vendor bands 1/2,
    the Red drops playback 63-64 + capture slots 53-60 at double speed and playback 37-64 + capture
    33-60 at quad; ADAT re-pins, Dante keeps 32 at 96k and 16 at 192k. ~~**OPEN from the same runs:** a
    playback START LOSS when aplay joins a running engine (112 frames at 96k, 800 at 192k) and a stale
    64-frame (= `tx_guard`) burst about one period after playback stops (192k only so far).~~
    **FIXED Oct 2 2026 (snd-clarett `f9e9b26`, all models)** — see [[red-8line-open-items]] item 7.
- **★★ THE VENDOR STREAMED, so the capture carries the Red's DATA PLANE too — unplanned and the most
  valuable part.** From `tools/bar_profile.py`:
  - **`0x0204 = 0x40` (64) and `0x0304 = 0x3c` (60) are the per-direction CHANNEL COUNTS** — an
    independent confirmation of `GET_7.1`'s `playback=64 capture=60` from a register that needs no
    mailbox transaction at all.
  - **`0x0208 = 0x400` and `0x0308 = 0x3c0` are the SIZE/IRQ-period registers, and OUR EXISTING FORMULA
    PREDICTS BOTH EXACTLY.** `clarett_period_bytes(ch) = ch * 4 * 4` (channels x 4 bytes x 4 frames)
    gives 64 -> `0x400` and 60 -> `0x3c0`. That formula was derived entirely from Clarett models
    (8PreX 28ch -> `0x1c0`, 2Pre 4ch -> `0x40` / 14ch -> `0xe0`) and it lands on the Red's vendor values
    with no adjustment — the strongest evidence yet that the data-plane engine is line-wide, not
    per-model. (Read `64 x 16` at first: arithmetically the same number, but it is the IRQ period in
    bytes, NOT the descriptor fragment — the fragment is `ch * 4 * 16` = `0x1000`/`0xf00`.)
  - **`0x0214 = 0x0314 = 0x0414 = 2`** — descriptor rings *and* the response buffer all placed above
    4 GB. Third independent confirmation these are real 64-bit address high words.
  - Written 14x each at arm: `0x0108=0x10`, `0x010c=0x1e70700`, `0x0110=7` then `0`, `0x020c=1`,
    `0x0210`/`0x0310` = the two ring bases. `0x0300` cause values `0x8000000c`/`0x80000018`/`0x80000024`
    — the counter advancing in steps of `0xc`.
  - `0x0218`/`0x021c` and `0x0318`/`0x031c` are the live ring pointers (read 505x each).
- **`0x000 = 0x042003fc` confirmed on a THIRD independent trace** (29 reads, all identical), under a
  third Windows driver stack. The family register holds; see the `0x000` table above.
- **Vendor init on the Red** (identical under both Windows drivers, apart from the DMA address):
  `0x800005 {off=0, len=8}`, then `GET_7.1` bands **0, 1 AND 2** — ours reads band 0 only — then a
  **4.99 s** idle poll of `0x100`/`0x300`/`0x200`/`0x400`/`0x500` (the four MSI cause blocks + MIDI
  status). Whether the vendor needs all three bands to identify a unit is open, and is a candidate
  explanation for band 0 alone yielding a pair no Clarett entry matches.
- **Topology**: the Red 8Line's internal Thunderbolt controller is a **JHL6540 Alpine Ridge 4C**
  (the Claretts carry a DSL2210 Port Ridge). `boltctl` names it `Focusrite Red16Line`, generation
  Thunderbolt 3, 20 Gb/s = 2 lanes × 10 — consistent with the DROM-superset note in
  [[focusrite-red-8line-incoming]]. Its IOMMU group holds the internal bridge `08:00.0` (bound to
  `pcieport`) **plus** the endpoint; that does NOT block vfio (the group-viability test exempts PCI
  bridges, `hdr_type != NORMAL`), but `reset_method` is **`bus` only, no FLR**. The host also cannot
  assign an I/O window to `08:00.0` (`can't assign; no space`, ×4) — benign, the endpoint has no I/O BAR.

### RESOLVED by hot-plug — Windows will not BOOT with the Red 8Line attached, but takes it fine at runtime (Sep 4 2026)

A Clarett 8PreX in the **identical** slot, guest address and domain config boots fine, which is the
control that makes this the device's problem and not the rig's.
- **Ruled out by that A/B and by the trace:** `boltctl`/the TB tunnel (device enumerates, BAR assigned,
  `vfio-pci ... reset done` ×4); the IOMMU group; the bus-reset-only path; guest addressing; the
  VM/NVRAM/failed-boot counter; and `kvmvapic.bin: Failed to open file`, which is **present in
  successful boots too** and is therefore not a cause.
- **With the Clarett Thunderbolt driver installed:** the driver ran the 4 commands above, idled ~5 min,
  then Windows saved config space, wrote Command ← 0, cleared BAR0 and rebooted → *Automatic Repair*
  loop. Suspected cause: Windows binds the Clarett driver on the **shared PCI id `1cb5:0002`** and it
  chokes on a device it does not know — the same collision `snd_clarett` hits, except ours refuses in
  one line.
- **After installing RedNet Control 2** (which uninstalled the Clarett Thunderbolt driver): the
  device-level traffic is *identical*, but Windows gets further — the guest display driver loads
  (mode `1280x800 → 1400x1050`) and it does not reboot — then hangs with **`vcpu.0.time` pinned at
  ~100 %, `vcpu.1/2/3.time` at exactly 0 (the APs never started, so it never reached SMP init), and
  disk I/O frozen**.
- **★ THE PENDING CONTROL RAN AND PASSED (Sep 4 2026):** the VM boots normally with **no `<hostdev>`
  at all**, so the RedNet Control 2 / Dante install is exonerated and the boot failure really is the
  8Line's. **And the workaround is total** — hot-plugged after Windows is up, the device attaches,
  Windows binds a driver and runs a full control + streaming session with no instability. Only
  *boot-attach* fails.
- **★★ THE HOT-PLUG RIG, AS BUILT AND PROVEN (Sep 4 2026) — this is how the Red capture was taken.**
  Keep the hostdev out of the persistent domain and `virsh attach-device <dom> hostdev.xml --live` once
  Windows is up. A driver stall then costs a device, not a boot loop; the capture is *cleaner* because
  driver init is isolated at a known wall-clock instant; and detach/re-attach repeats an init capture
  with no reboot. Re-check the source BDF after every physical replug — `09:00.0` and `1a:00.0` have
  both been the Red, and on a two-unit host **both endpoints are `1cb5:0002` bound to `snd_clarett`**,
  so identify by `readlink -f /sys/bus/pci/devices/<bdf>` / the registered card name, never by order.
  ```xml
  <hostdev mode='subsystem' type='pci' managed='yes'>
    <source><address domain='0x0000' bus='0x1a' slot='0x00' function='0x0'/></source>
    <alias name='ua-clarett'/>
  </hostdev>
  ```
  `managed='yes'` does the `snd_clarett` → `vfio-pci` swap and rebinds on detach.
- **★★ MMIO TRACING A HOT-PLUGGED DEVICE NEEDS `-global`, NOT `<qemu:override>` — the alias-keyed
  override is SILENTLY INERT on the hotplug path, and an untraced capture looks identical to a traced
  one until you check.** libvirt applies `<qemu:override>` when it builds the command line, so a device
  added later never sees it: `x-no-mmap` reads `false`, BAR0 is mapped through, and `vfio_region_*`
  produces **nothing** while `vfio_pci_*_config` keeps working (config space is never mmap'd) — so the
  log looks alive. Fix, in `<qemu:commandline>`, applied at device *creation* and therefore to
  hot-plugged devices too:
  ```xml
  <qemu:arg value='-global'/><qemu:arg value='vfio-pci.x-no-mmap=true'/>
  ```
  Needs a VM restart (command-line arg), which is free now that the guest boots with no hostdev.
  **Always verify before trusting a capture:**
  `virsh qemu-monitor-command <dom> --pretty '{"execute":"qom-get","arguments":{"path":"/machine/peripheral/ua-clarett","property":"x-no-mmap"}}'`
  **Dead end, do not retry:** out-of-band `device_del` + `device_add x-no-mmap=true` via QMP. libvirt
  watches `DEVICE_DELETED` and immediately reverses its `managed` setup — the host driver is rebound and
  `/dev/vfio/<group>` disappears, so the re-add dies with `Could not open '/dev/vfio/55'`.
- **Scrub the trace before committing it:** libvirt's first log line embeds `hostname:`, which the
  repo rule forbids. `sed -i '1s/hostname: [^ ]*/hostname: <redacted>/'`.
- **★ METHOD — "stuck at the TianoCore screen" was a red herring twice over, and the MMIO trace alone
  could not tell.** `virsh screenshot` showed OVMF had *already* handed off (`BdsDxe: starting
  Boot0004 "Windows Boot Manager"`); the logo persists only because Windows has not switched video
  modes yet, so a Windows-stage hang looks exactly like a firmware-stage one. A later screenshot
  showed *Automatic Repair*. **`virsh domstats <dom> --vcpu --block` is the discriminator for how far
  a guest actually got** — frozen `block.0.rd.reqs` with one vCPU at 100 % means spinning, not working.
  Both conclusions I drew from the trace before screenshotting ("not the device", then "it booted")
  were wrong.
  **★★ CORRECTION (Sep 4 2026) — "AP `vcpu.N.time` of exactly 0 means Windows never reached SMP init"
  IS WRONG ON THIS RIG, and that half of the RedNet hang diagnosis is withdrawn.** The domain runs
  `-smp 4,sockets=4,cores=1,threads=1` — four separate SOCKETS — and Windows 10 client editions cap
  physical sockets (Home 1, Pro 2), so vcpu1-3 read exactly 0 **on a completely healthy boot**.
  Measured on a guest that was demonstrably fine (disk writes advancing, vcpu0 at ~5 %): libvirt
  reported `vcpu.1/2/3.time=0`, and `/proc/<qemu>/task/*/stat` confirmed the AP threads had genuinely
  consumed zero — so it is not even a reporting artifact, it is the steady state. The *other* half
  (vcpu0 pinned at ~100 % with frozen disk I/O) still stands as a hang signal. Fixing the topology to
  `sockets=1,cores=4` would give that guest its other three cores back.

## Protocol — FCP (Focusrite Control Protocol)

Same protocol family as the in-kernel `scarlett2`/`fcp` drivers. The USB Clarett
class is in `scarlett2`; the **Thunderbolt Clarett is not** — but the protocol
ports, so `scarlett2` + the USB Clarett XML are a verified interpretation
reference. **Encodings are per-model — never copy opcodes/offsets/enums across
models.** The 8PreX's own numbers come from `vendor-reference/Devices/Clarett 8PreX.xml`.

### Transport (confirmed from boot-init trace)
- **FCP request mailbox @ BAR0 `0x8020`**: header = `cmd`@+0 (bit31 = execute
  flag | opcode), `size|seq`@+4 (size low16, seq high16, seq increments),
  `error`@+8, pad@+12, `data[]`@+0x10. Matches scarlett2 header layout.
- **Doorbell @ `0x408`**: write `1` = submit, `2` = ack/clear prior completion.
- **Completion**: poll IRQ cause reg `0x100` for DONE bit `0x20000000`. Cause regs
  `0x100/0x200/0x300/0x400` = one block per MSI vector (read-to-clear).
- **GET responses arrive via DMA, NOT the BAR.** Device DMAs results into a host
  buffer whose bus address is programmed at `0x410` (low32) / `0x414` (high32).
  → MMIO traces can't see GET payloads; the driver allocates its own buffer.
- Other regs: `0x000` caps, `0x010/0x014` serial, `0x104` IRQ enable
  (`0xf000003f`), `0x8000..0x801f` read-only fw-info header.

### Opcodes
- **Confirmed (== scarlett2 values):** `GET_DATA=0x800000 {u32 off,u32 len}`,
  `SET_DATA=0x800001 {u32 off,u32 len,data}`, `DATA_CMD=0x800002 {u32 activate}`,
  `GET_METER=0x001001` (GUI polls continuously — the trace "noise").
- **Device-specific init-only:** `0x5000` (config push), `0x6000-2`, `0x7000-3`,
  `0x0002`. Not decoded; not replayed by the driver (works without so far).
- **`MUX_READ=0x003001`** — routing read-back, decoded on hardware July 20 2026 (transport spec §8):
  request `{u8 offset, u8 pad, u8 count, u8 mux_num}`, **reply capped at 28 entries (112 B)** whatever
  `count` says, and `offset` is a **flat** entry index crossing band boundaries. Callers must window.
- **Open:** a 1 KB bulk `SET_DATA`; the init handshake.

### The control-plane model (the key result)
A config write = `SET_DATA{offset, len, value}` then `DATA_CMD{activate}`, where
`offset`/`len`/`value` and `activate` come straight from the XML per control
(`offset-bytes`, `bits`, and `command`). The **encoding** is confirmed against FC's
live traffic on master mute (offset 24, activate 2) and master volume (stereo,
offsets 32/33, activate 1) — i.e. our bytes match FC's byte-for-byte. **Not** verified
end-to-end: replayed by our driver these writes complete (`done=1`) but do not manifest
(the manifestation wall), so the encoding is proven correct, the physical effect is not.

### Output gain encoding (confirmed)
7-bit **attenuation** code = |dB| exactly, linear 1 dB/step: `0x00`=0 dB (unity)
… `0x7f`=−127 dB (floor). ALSA: `DECLARE_TLV_DB_SCALE(tlv,-12700,100,0)`, value
`v`(0..127) → device code `127 − v`.

## Repository layout

```
spec/clarett-interface.md           Clean device & protocol specification (distilled): device, transport,
                                    control protocol, data plane, bring-up, per-model tables. No provenance
                                    notes or failed-experiment history — that lives in spec/provenance/.
spec/provenance/                    The RE lab notebook: the evidence trail behind the clean spec — full
                                    elimination records, wall narratives, per-experiment provenance tags,
                                    and cross-platform plans. Kept as the clean-room audit log.
  clarett-control-plane.md          Authored control-plane spec (offsets, opcodes, enums, pins, mixer,
                                    routing). Provenance-tagged.
  clarett-fcp-transport.md          Mailbox/transport framing; confirmed reg map.
  clarett-data-plane.md             PCM-DMA RE: method, recovered register/descriptor maps, and the
                                    validated-but-won't-sustain engine (boot→stream traced; below-BAR wall).
  clarett-manifestation-wall.md     The wall: full elimination record (§§1–7) + §8 THE CROSSING —
                                    trailing-ack-vs-response-DMA race; landed-gated ack arms the session.
  clarett-macos-dtrace-plan.md      DTrace of the working macOS driver (device runs on the M1): RUN and
                                    exhausted (§5d) — confirmed the wall, blocked inside the stripped kext.
  clarett-windbg-plan.md            RUN (§5e): WinDbg of the working Windows driver's init DMA — vendor's
                                    driver-level DMA is attribute-equivalent to ours; wall confirmed below-driver.
  clarett-rate-aware-plan.md        The rate-aware control-plane work: (1) per-rate meters, (2) per-rate
                                    router pins for the re-pinning second ADAT port (8PreX, Red), (3) the
                                    0x006004 sync word -- ALL DONE Oct 2 2026, with results. Its "Also
                                    open" list remains; 8PreX ADAT 2 and Wordclock are verified.
  clarett-packaging.md              DKMS + akmod packaging, Secure Boot and licensing: the full record
                                    (summarised under Build & test).
  clarett-buffer-latency.md         The ALSA buffer-ceiling work, tx_guard, PipeWire's adaptation, the
                                    digital-loopback ramp method (summarised under Driver limitations).
  clarett-address-ack-handshake.md  G. D. Bennett's record of the response-address acknowledgement
                                    (0x400 bit0 after the 0x414 write), with the Oct 10 2026 addendum
                                    measuring it on every model after the port to main.
  clarett-opcode-inference.md       G. D. Bennett: what the init-only opcodes are (0x007001 =
                                    STREAM_INFO per speed band, 0x005000 a per-port descriptor read,
                                    flash FLASH_INFO/SEGMENT_INFO/READ). Main still uses the old
                                    GET_7.x / CONFIG_PUSH names.
snd-clarett/                          GIT SUBMODULE -> github.com/JustALawnGnome7/snd-clarett (PUBLIC; fresh
                                      history, the dated RE trail stays here). Out-of-tree module
                                      `snd-clarett` (hwdep transport + PCM + MIDI). Was `driver/`.
  clarett.h, clarett_main.c (PCI probe + data-plane engine), clarett_mailbox.c (FCP transport),
  clarett_hwdep.c (the FCP hwdep ABI — the only control surface), clarett_pcm.c (capture PCM,
  enable_pcm=1), Makefile, README.md
  dkms.conf                           THE version (PACKAGE_VERSION) — the Makefile parses it out and
                                      compiles it in as MODULE_VERSION. Bump it here and nowhere else.
  packaging/*.spec                    Fedora RPM: snd-clarett-kmod.spec (kmodtool -> akmod + per-kernel
                                      kmod) and snd-clarett-dkms.spec. Driven by `make rpm-akmod` /
                                      `make rpm-kmod`; the by-hand recipe is in each header.
  wireplumber/51-clarett-naming.conf  HAND-MAINTAINED WirePlumber drop-in: one rule per model promoting
                                      the driver's card name (api.alsa.card.name == clarett_model.name)
                                      into device.description, so GNOME shows "Clarett 2Pre" not the
                                      generic "Clarett Multichannel". Covers Clarett 2Pre/4Pre/8Pre/8PreX
                                      and Red 4Pre/8Pre/8Line/16Line — the Reds other than the 8Line in
                                      advance of their driver entries, so a new model's name MUST be
                                      spelled as its rule has it. Shipped by both specs (kmod -common,
                                      dkms) and `make dkms-install`; `make wireplumber-install` for the
                                      insmod route (PREFIX default /usr/local).
fcp-server-data/*.json                Authored devmap + alsa-map pairs per model: the control set
                                      userspace (fcp-server) builds. See its README.
fcp-server-data/sim/*.state           GENERATED alsa-scarlett-gui simulation previews of the Red models
                                      not yet run on hardware; gen_fcp_maps.py rewrites them each run.
tools/arm-tables/arm_<model>.h        The de-blobbed vendor bring-up (typed step lists + the
                                      clarett_arm_emit() builder in clarett_arm.h). <model> carries the
                                      product line -- arm_clarett_8prex.h, arm_red_8line.h -- and the
                                      arrays inside match (arm_red_8line_mux_b0), so no variable wears a
                                      bare vendor prefix. Lived in the driver tree while it
                                      replayed the bring-up; the driver no longer arms, so these are
                                      kept ONLY as input to gen_fcp_maps.py (it parses the SET_MUX
                                      bands for the router pins). Regenerate with
                                      fcp_decode.py --emit-deblob.
tools/gen_fcp_maps.py                 Generates every map pair (names, routing/mixer tables from
                                      the de-blobbed bring-up tables tools/arm-tables/arm_<model>.h,
                                      measured meter peak-index). The Red 16Line/4Pre/8Pre have no
                                      capture: their tables come from synth_red_bands(), a rule
                                      re-checked against the Red 8Line's captured tables every run.
tools/gen_sim_state.py                Map -> alsactl .state file, so alsa-scarlett-gui can render our
                                      control set with no hardware attached.
tools/gen_ucm.py                      Generates the Clarett 8Pre/8PreX and Red 8Line ALSA UCM profiles in
                                      ucm2/Clarett/ (2Pre/4Pre and Clarett.conf are
                                      hand-written). Names = alsa-scarlett-gui Routing page, hyphens.
                                      Devices are at most 8 channels wide (SplitPCMDevice's limit).
ucm2/                                 ALSA UCM profiles, ON HOLD: kept here, not in snd-clarett, until
                                      they are fit to ship (the Red profile hangs WirePlumber: ACP's
                                      profile build doubles per device). Test via ALSA_CONFIG_UCM2.
tools/fcp_decode.py                   vfio_region_* trace -> FCP transaction decoder.
                                      (--brief, --mix-diff, --async, --show-appspace, --classify).
tools/bar_profile.py                  vfio_region_* -> per-register activity profile; flags offsets
                                      outside the control-plane map (data-plane reg discovery).
tools/notify_correlate.py             vfio_region_* -> correlates 0x400 notify-cause transitions with
                                      the mailbox command around each (proved 0x400 = command-phase reg).
tools/dma_bases.py                    vfio_region_* -> the live DMA base GPAs + ready-to-run QMP pmemsave
                                      commands to dump the guest ring buffers.
tools/dma_classify.py                 pmemsave dump -> classifies it flat-audio / descriptor-table /
                                      all-zero (automates the §9 buffer-mode analysis; flags pre-seeding).
tools/fcp_*.c                         Bench tools driving the hwdep directly (stop fcp-server first —
                                      it holds the hwdep exclusively). fcp_cfg_read: GET_DATA a config
                                      byte range, the only way to see what actually reached the device;
                                      fcp_meter_watch: which meter slot a channel moves; fcp_mux_probe:
                                      MUX_READ windowing; fcp_cap_read: the per-category CAP_READ bytes
                                      + a GET_DATA probe (diagnoses fcp-server's "does not support
                                      required INIT category" — unarmed device vs zero capabilities);
                                      fcp_cmd: ANY opcode with request bytes from the command line,
                                      response hex-dumped — the first thing to reach for on an
                                      undecoded query (what found the STREAM_INFO bands, §9 of
                                      spec/provenance/clarett-opcode-inference.md); fcp_flash_dump:
                                      lists the flash segments and dumps one (each chunk read twice
                                      until two agree, since long FLASH_READ runs return stale data).
                                      Its author reports the REAL serial lives in the App_Env segment
                                      (offset/value not recorded) — the lead for the dummy-serial TODO.
vendor-reference/Devices/*.xml        Focusrite's device descriptors (RE source material).
captures/*.log                        Trace captures (vfio_region_* logs, guest-RAM dumps, decoded
                                      dumps) + working notes (insmod/session notes; former .txt now .log).
```

## Build & test

```sh
make -C snd-clarett               # builds snd-clarett/snd-clarett.ko
sudo insmod snd-clarett/snd-clarett.ko   # auto-binds 1cb5:0002
sudo make -C ../fcp-support install      # our fork, snd_clarett branch: fcp-server + udev/systemd + maps
sudo make -C snd-clarett wireplumber-install   # per-model names in PipeWire/GNOME
```
- **★ THE DRIVER IS A SUBMODULE (Sep 18 2026).** Clone with `--recurse-submodules` (or run
  `git submodule update --init` after a plain clone, then `git -C snd-clarett switch main`, since the
  update leaves a detached HEAD). A driver change is committed IN the submodule,
  then the pointer is bumped here in a second commit; **push `snd-clarett` first**, or the clarett-sre
  commit points at an object GitHub does not have. The public repo started from one commit (no RE
  history, deliberately), so nothing under `snd-clarett/` may lean on clarett-sre-only material —
  `CLAUDE.md`, `spec/`, `captures/`, `tools/`, "see git history" — as if the reader had it; name the
  clarett-sre project instead, as its README does for the device maps.
- **★ PACKAGING — DKMS + Fedora akmod, both verified on hardware, including across real kernel upgrades
  and with Secure Boot (akmod route). Full record: `spec/provenance/clarett-packaging.md`.**
  `make -C snd-clarett` / `modules_install` are dev-only. Supported routes:
  `sudo make -C snd-clarett dkms-install`, or on Fedora `make -C snd-clarett rpm-akmod`
  (`rpm-kmod KVER=<ver>` for one kernel); the RPM targets build and STOP, printing the `dnf install`
  line, because that transaction needs reading before yes (the partial-kernel trap below). Things to carry:
  - **`snd-clarett/dkms.conf` `PACKAGE_VERSION` is THE version** — the Makefile compiles it in as
    `MODULE_VERSION`; a build that bypasses the Makefile reports `0.0.0-unknown` on purpose.
  - **★★ `dnf install dkms` (and `akmods`) can install a PARTIAL KERNEL that boots broken** (800x600,
    no Wi-Fi/10GbE): their `(kernel-devel-matched if kernel-core)` dep pulls `kernel-core` without
    `kernel-modules`, and `kernel-install` makes it the default. Check every package removed at the old
    version has a counterpart installed at the new one; upgrade as `dnf upgrade kernel kernel-devel`.
    Recovery: boot the previous kernel, install `kernel-<ver> kernel-modules-<ver>
    kernel-modules-extra-<ver>` version-pinned, `dracut -f --kver <ver>`.
  - **Never install both routes**: akmod uses `extra/snd-clarett/snd-clarett.ko.xz`, DKMS a flat
    `extra/snd-clarett.ko.xz`. `modinfo -n snd-clarett` names the winner.
  - The akmod rebuild for a new kernel happens at BOOT (`akmods.service`), by design. Both routes sign
    the module with a locally generated key; the "module verification failed" taint just means the MOK
    is not enrolled. Secure Boot needs `mokutil --import /etc/pki/akmods/certs/public_key.der`
    (a symlink — check with `readlink -e`), and on HP laptops the BIOS toggle "Enable MS UEFI CA key"
    (`mokutil --db | grep -i Microsoft` showing only Windows CAs is the tell).
  - The Makefile detects kbuild by `obj`, NOT `KERNELRELEASE` (DKMS sets that and calls the Makefile
    directly). kmodtool traps (the `-common` subpackage, empty debug package, spec staged into
    `%{_specdir}`) are fixed in the spec; a bare `rpmbuild -bb` on the kmod spec cannot work outside
    RPM Fusion's build farm — pass `--define 'buildforkernels akmod'` or `--define "kernels $(uname -r)"`.
  - **License GPL-2.0-only** (`LICENSE` md5 `b234ee4d69f5fce4486a80fdaf4a4263`);
    `MODULE_LICENSE("GPL")` is correct alongside it — do not "fix" it.
  - **OPEN before a 0.1.0 tag:** the specs have no `%changelog`, because entries are dated and
    `snd-clarett/` is under the no-dates rule. Decide whether a release date is exempt.
- **★ USERSPACE INSTALL IS fcp-support's, AND END USERS NEVER TOUCH THIS REPO (Sep 25 2026, operator's
  call).** The intent is that fcp-support ships the maps. **AT THE MOMENT only our fork does**
  (github.com/JustALawnGnome7/fcp-support, `snd_clarett` branch): the maps are in its `data/`, and its
  `sudo make install` deploys them with fcp-server, the udev rule and the systemd template. Upstream
  fcp-support has neither the maps nor the fcp-server patches they need (`map_key`, `invert`,
  `mixer-max-db`, …), so an upstream checkout installs a server that cannot drive the Clarett — check
  the branch before blaming anything else. This repo GENERATES the maps (`tools/gen_fcp_maps.py` ->
  `fcp-server-data/`, the source of record), then they are copied into the fork and committed there —
  see `fcp-server-data/README.md`. The top-level Makefile, which only installed maps, was removed as
  redundant (and a stale copy installed last would silently win). The WirePlumber drop-in ships with
  the driver (`snd-clarett/`).
- **PREFIX is `/usr/local` everywhere — don't qualify it.** fcp-support and alsa-scarlett-gui both
  default there, so a bare `sudo make install` in each is correct and consistent. The prefix must
  agree because fcp-server compiles its DATADIR in (`-DDATADIR=$(PREFIX)/share/fcp-server`), so maps
  installed under the other prefix are invisible to it. **Never leave both prefixes populated:**
  systemd (`/usr/local/lib/systemd/system` before
  `/usr/lib/systemd/system`) and udev (`/usr/local/lib/udev/rules.d` first) prefer `/usr/local`, so a
  stale `/usr/local` install silently shadows a freshly built `/usr` one — the unit keeps launching
  the old binary and nothing reports an error. `sudo make uninstall PREFIX=<old>` in fcp-support
  before switching, and check with `systemctl cat 'fcp-server@*'` that `ExecStart` is the binary you
  just built. The WirePlumber drop-in reaches `/usr/local/share` via `XDG_DATA_DIRS`, not via
  WirePlumber's own prefix — and it does so whether or not the variable is set, because
  `/usr/local/share/:/usr/share/` is the XDG *default* that WirePlumber's config lookup falls back to.
  (Don't read a set `XDG_DATA_DIRS` in the systemd user manager as the reason it works: on this box
  that is flatpak's `profile.d` rewriting it, which is incidental.) Packages should use `PREFIX=/usr`;
  `/etc/wireplumber/wireplumber.conf.d/` is read too but belongs to the user's own overrides.
- **★ `Clarett.conf` — WHY THE CARD WAS INVISIBLE TO JUCE APPS (Sep 10 2026, 8PreX in TONE3000).
  SCRAPPED FROM snd-clarett FOR NOW (Sep 24 2026, operator's call)** — no route installs it any more
  (Makefile `alsa-install`, both specs, `dkms-install` all dropped), so JUCE apps are back to not listing
  the card unless the per-user asoundrc fallback below is used. The file is in snd-clarett's history if
  it comes back; the findings below still stand. JUCE lists ALSA devices from name hints and skips `default:`/`sysdefault:`/`plughw:`,
  while bare `hw:` is hidden by `defaults.namehint.showall off`. Other cards survive through
  `front:CARD=…`, which alsa-lib creates only for drivers with a `cards/<driver>.conf` — none existed
  for `Clarett`, so the card's only hint was `sysdefault:` and it vanished from the list. Verified
  before shipping via `ALSA_CONFIG_DIR` on a copy of `/usr/share/alsa`: `arecord -L`/`aplay -L` list
  `front:CARD=C8PreX,DEV=0` as "Clarett 8PreX, Clarett 8PreX / Front output / input", and the open
  resolves to the hardware (stock: "Unable to find definition"). **A per-user asoundrc PCM of
  `type hw` with a hint does NOT list** — alsa-lib omits standalone hw definitions; `type empty`
  wrapping `hw:` does, and is the fallback where the card config cannot be installed. Still exclusive:
  needs PipeWire's card profile `off`, or `pw-jack` + the app's JACK backend.
  **Separately, and NOT the driver:** TONE3000's libasound segfaults in `snd_pcm_ioplug_poll_revents`
  are a JUCE use-after-free — `ALSAThread::close()` waits 400 ms, then `snd_pcm_close()`s while the
  audio thread is still in I/O on the PipeWire ALSA plugin. Still present in upstream JUCE master.
  Read the coredump (`coredumpctl info`) before blaming the driver for an application crash.
- **★ WHY THE WIREPLUMBER DROP-IN IS UNAVOIDABLE (Aug 20 2026) — and the old comment's reason was
  WRONG.** It claimed the Thunderbolt units "have no pci.ids entry, so WirePlumber falls back to the
  ALSA driver string". They do have one: `1cb5:0002` is listed as the whole-line name `Clarett`. The
  real chain is udev `ID_MODEL_FROM_DATABASE` → libspa-alsa `device.product.name` → WirePlumber's
  `alsa.lua`, which prefers `device.product.name` **over** `api.alsa.card.name` when deriving
  `device.description` (`/usr/share/wireplumber/scripts/monitors/alsa.lua`, the
  `d = d or properties["device.product.name"] or properties["api.alsa.card.name"] or ...` chain).
  So we are overriding a name WirePlumber *prefers*, not supplying one it lacks. Two consequences:
  **no card name the driver picks can ever win** (so this cannot be fixed driver-side), and **pci.ids
  cannot be made per-model either — every model in the line reports the same subsystem ID**
  (user-confirmed), so there is nothing for a per-model entry to key on. `update-props` takes literal
  values only (no interpolation of `api.alsa.card.name`), so one generic rule keyed on
  `alsa.driver_name` is impossible and the per-model list is mandatory. **The list is now
  hand-maintained in `snd-clarett/wireplumber/` (Sep 18 2026)** — the generator that parsed it out of
  `clarett_main.c` was dropped rather than published with the driver, and the rules cover every
  Clarett and Red model up front, so adding a model means spelling its `clarett_model.name` exactly as
  the rule does (`clarett.h` says so at the field).
- **Mixer-only**: `aplay -l` shows nothing (no PCM yet). Use `amixer -c N
  contents` / `alsamixer -c N`.
- **Device must be free of `vfio-pci`** to test on the host (stop the VM, unbind).
- To unload, release the card first: `sudo systemctl stop alsa-state.service`
  (and PipeWire/WirePlumber if they hold `/dev/snd/controlC*`), then `rmmod`.
- Bare-metal test box: handle Thunderbolt auth (`boltctl authorize`) and Secure
  Boot (unsigned module needs SB off or a signed MOK).
- **TB2 enumeration is HOST-FIRMWARE DEPENDENT, not a property of the device** (revises the old
  "TB2 units are never enumerated by `boltctl`, so security must be disabled"). An **HP EliteBook
  840 G5** lists the Clarett Thunderbolt units in `boltctl` and puts them on the PCI bus on
  `boltctl authorize <uuid>` with the security level left at *PCIe and DisplayPort - User
  Authorization* — no *No Security*, and no need to clear *Require BIOS PW to change Thunderbolt
  Security Level*; its *Thunderbolt PCIe Hot plug Mode* was *Native + Lower Power Mode* (*Legacy Mode*
  untested), which also disables Thunderbolt S4 boot. Other vendors seem to call that setting
  *Thunderbolt BIOS Assist Mode*. The **ASRock X570 Creator** lists none of them, which is where the
  old blanket claim came from — so disabling security is the FALLBACK for such boards, not the
  starting advice. Confirmed line-wide, not per model. Consequence for detection: a DROM `device_name`
  under `/sys/bus/thunderbolt` may exist on some hosts and not others, so it stays unusable as a
  contract (the per-model slug remains the one to key on). **The `pci=assign-busses,realloc,hpbussize=0x10`
  kernel arguments are ASRock-specific too** — not needed on the EliteBook, which enumerates the Clarett
  with stock parameters. Try stock first; those arguments work around firmware that under-allocates bus
  numbers behind the Apple TB3→TB2 adapter's bridge, they are not a device requirement.
- **A TB4 HOST NEEDS AN INTERMEDIATE DOCK for the Apple TB3→TB2 adapter — and the dock's controller must
  be DISCRETE, not its Thunderbolt generation (Aug 19 2026; corrected Aug 21 2026).** An **HP EliteBook
  640 G11** (integrated Thunderbolt 4, Core Ultra) will **not** enumerate a Clarett through the Apple
  adapter plugged in directly; putting a dock between host and adapter makes it work. **The dock does NOT
  have to be TB3: an HP Thunderbolt Dock G4 — a TB4 dock, Goshen Ridge `[8086:0b26]` — carries the adapter
  fine (user-confirmed both directions on the same dock: direct fails, via-dock works).** So the earlier
  "needs a TB3 dock" was too narrow. The property that matters is that the link terminates at a **discrete**
  Thunderbolt controller which re-originates a downstream port still speaking legacy TB1/TB2, so the
  handshake happens dock↔adapter and never host↔adapter; Intel's **integrated** TB4 host ports (Meteor
  Lake / Core Ultra) dropped that. Discrete TB3 (Alpine Ridge) and discrete TB4 (Goshen Ridge) both work.
  (The 840 G5 above has a discrete TB3 host controller, which is why it takes the adapter directly.)
  Secure Boot off, stock `pci=` parameters, no security-level change needed.
  **CONFIRMED both docks are real and distinct (Aug 21 2026, `boltctl list` on the host):** an
  `HP Thunderbolt 3Dock` (`generation: Thunderbolt 3`, the Aug 19-20 rig, matching the `DSL6540 Alpine
  Ridge` chain recorded then) and an `HP Thunderbolt Dock G4` (`generation: USB4`, today's, Goshen Ridge)
  are both stored and have both carried the adapter. So discrete TB3 and discrete TB4 are each
  INDEPENDENTLY confirmed, not one inferred from the other.
  **`boltctl` also confirms the link rates directly:** the Dock G4 negotiates `40 Gb/s = 2 lanes * 20 Gb/s`
  while the Clarett 8Pre behind it negotiates `10 Gb/s = 1 lanes * 10 Gb/s` and reports
  `generation: Thunderbolt 1`. Since the dock's own link is 4x faster, the 10 Gb/s is definitively the
  **device's** TB1-class controller and not a dock penalty — an independent confirmation of the DSL2210
  reading below, from a source that needs no PCIe topology reasoning.
  **`boltctl` saying `authorized` is NOT enough — `lspci -nn -d 1cb5:` is the check that the PCIe tunnel
  actually came up**, and behind a dock plus a legacy adapter that is where a chain falls down.
  Bridge chain also identifies the device's own Thunderbolt controller: host root port → **dock switch**
  (upstream bridge + several downstream bridges) → **DSL2210 "Port Ridge 1C"** (upstream + 2 downstream
  bridges: one carries the endpoint, the other is EMPTY — Port Ridge's own layout, not a thru jack; the
  Claretts are single-port, only the Reds have a daisy-chain port) → the `1cb5:0002` endpoint. So the
  **10 Gb/s single-lane link is the Clarett's own TB1-class controller, not a dock penalty**, and it refines
  "FPGA-based Thunderbolt front-end" above: the TB layer is an Intel DSL2210 and the FPGA is a PCIe endpoint
  *behind* it. **The DSL2210 is IN THE CLARETT, not the Apple adapter (re-checked Oct 9 2026, 8PreX chained
  behind the Red 8Line):** `/sys/bus/thunderbolt/devices` lists every Thunderbolt router by its DROM — host,
  `HP Thunderbolt Dock G4`, `Focusrite Red16Line` (gen 3, the Red's JHL6540), `Focusrite Clarett8PreX`
  (gen 1, vendor 0x12 device 0xb) — and the adapter is NOT a router at all (its chip, a TI CD3211, is a port
  controller). So the endpoint's immediate upstream bridge is always the unit's OWN controller, on any host,
  through a dock or another unit's chain port: the basis of Red twin detection (Alpine Ridge = Line model).
  Irrelevant for bandwidth (worst case in the line, 8PreX 28in+28out S32 @192 kHz, is ~344 Mb/s both
  directions).
- **★ A `BadDLLP` AER STORM ON THIS RIG IS THE DOCK'S OWN NIC LEG, NOT THE AUDIO CHAIN — and the ONLY way
  to tell is the sysfs parentage (Aug 21 2026, EliteBook 640 G11 + HP Dock G4 + Apple adapter + 8Pre).**
  `pcieport 0000:03:04.0: ... [ 7] BadDLLP`, ~45/s sustained, 37.6k cumulative, surviving a clean reboot
  with everything attached. **Measured per-port after a reboot: `03:00.0`/`03:01.0`/`03:02.0`/`03:03.0` all
  read `BadDLLP=0` while `03:04.0` alone read 12621 — and the Clarett hangs off `03:01.0`,** so its leg is
  provably clean (`05:00.0` also runs at its full rated 2.5 GT/s x4). `03:04.0` leads to the dock's
  internal `I225-LMvP` Ethernet. Nothing to do with the device, the adapter, or the driver.
  - **The check that resolves it in one line:** `readlink -f /sys/bus/pci/devices/<clarett bdf>` prints the
    literal parent chain, naming which downstream port the device is actually behind. Then read
    `aer_dev_correctable` on *every* sibling port — a reboot gives a zeroed baseline, so one climbing port
    stands out immediately.
  - **Two dead ends worth not repeating.** (1) Reasoning from the `lspci` listing ORDER: the ports and the
    device appear adjacent, which suggests but does not establish parentage. (2) Comparing `LnkSta` against
    `LnkCap`, or comparing the two ends of a link against each other: **PCIe over a Thunderbolt tunnel is
    SYNTHESISED, not negotiated over wire**, so the two ends legitimately disagree (here `03:04.0` read
    5 GT/s x1 against `05:00.0`'s 2.5 GT/s x4) and a mismatch proves nothing either way.
  - **Meaning:** correctable = the link layer retransmitted and nothing was lost. The cost is retransmit
    latency, which on this project would surface as stream jitter, never as corrupt audio — so the arbiter
    is the servicer telemetry (`late`/`gapmax`/`badreads`), not the AER count. The real cost here is **log
    noise**: 6050 AER lines can bury a `stream-badread`, a `WARN_ON_ONCE` splat or a probe error in
    `journalctl -k`. If it interferes, mask the correctable reporting on that one device; do **not** use
    `pci=noaer`, which blinds AER machine-wide. If a storm ever does land on the audio leg, reseat, try the
    other port, and test `pcie_aspm=off` (L0s/L1 exit on a marginal link is a common source).
- **★ THE THUNDERBOLT FABRIC WEDGES AFTER HEAVY PLUG CYCLING — REPLUG THE DOCK, NOT THE DEVICE
  (Aug 21 2026, EliteBook 640 G11 + Dock G4 + Apple adapter + 8Pre; seen twice in one session).**
  Signature: the unit IS found and named, then torn down before it can be authorized, so **`boltctl
  list` shows nothing and `lspci -d 1cb5:0002` is empty** — which looks like the device failing to
  negotiate, and is not:
  ```
  thunderbolt 0-301: new device found ... Focusrite Clarett8Pre
  thunderbolt 0000:00:0d.2: 301:1: hop deactivation failed for hop 1, index 8
  thunderbolt 0000:00:0d.2: PCIe Down path activation failed: -107      (-ENOTCONN)
  thunderbolt 0000:00:0d.2: 301:2: PCIe tunnel activation failed, aborting
  thunderbolt 0-301: device disconnected
  ```
  A **stale path is left in the HOST controller's fabric state** (`0000:00:0d.2`, the NHI): the driver
  tries to deactivate hop 1 before building the new tunnel, that fails, and the PCIe path is never
  created. It retries on a ~5 min cadence, failing identically each time. **Because the state is
  host-side, power-cycling the interface and reseating the adapter change NOTHING** — the fix is to
  **unplug the dock from the host** for ~30 s (confirmed sufficient), or reboot. Avoid
  `modprobe -r thunderbolt`: it drops the dock too and is likelier to wedge things further.
  Provoked by hot-swapping units (8PreX <-> 8Pre) and repeated abrupt power cycles — i.e. by exactly
  the kind of bring-up testing this project does. **Consequence for measurement: check the chain is
  stable before trusting ANY probe-timing result** — a device that vanishes 574 ms after appearing
  fails readiness for reasons unrelated to the driver, and one capture was lost to precisely that.
- **★ THE PLATFORM SMI IS CONFIRMED PLATFORM-SPECIFIC (Aug 19 2026) — the driver is exonerated on
  independent hardware.** The retest this section used to call for has RUN, on the EliteBook 640 G11 above.
  The 2Pre streamed **292 s / 13,694 periods with ZERO SMI-class events**: `gapmax` pinned at nominal,
  `readmax` 25–77 µs, `stepmax` exactly one period throughout, **`badreads=0`**. The ASRock produces a
  ~42 ms freeze every 40–58 s (5–7 expected in that window) — none here. **The ~44 s all-`0xffffffff` MMIO
  stall did not reproduce either**, over a window that should have held ~6, which argues against the
  device-side flash-commit hypothesis in `clarett-periodic-mmio-stall`: the same unit on different host
  firmware never blanks. Both faults follow the ASRock, not the device. Two further notes on this host:
  it is the first run of the data plane behind an **Intel IOMMU** (`dmar0`/`dmar1`, DMA remapping on) with
  no DMAR faults; and the Core Ultra is **hybrid**, so the SCHED_FIFO servicer can land on an LP-E core —
  check `ps -o psr` on it before blaming the driver for missed deadlines. Still not done on this host: a
  listening test (all evidence so far is telemetry) and a native hwlat run (no VM needed there — the plan
  in `spec/provenance/clarett-smi-hwlat-vm-plan.md` applies with the VM step dropped).
- **★ LOG POLICY — the default load is ONE `dev_info` line (Aug 20 2026).** The RE instrumentation that
  used to print unconditionally is now `dev_dbg`, so a healthy driver prints:
  ```
  snd_clarett 0000:0a:00.0: Clarett 2Pre: serial ... fw app 0x... fpga 0x...; FCP hwdep, PCM 4/14ch, MIDI
  ```
  and **nothing at all while streaming**. The rules: `info` = the probe summary; `warn` = degraded but
  working (short MSI count, a subsystem that failed to register, a seed failure); `dev_dbg` = everything
  whose audience is this project. Demoted: `pre-mailbox causes`/`pre-mailbox regs`, `resp buffer dma addr`,
  the 4/4 `MSI:` line (a SHORT count is now a `dev_warn`), `FCP hwdep created`, `MIDI registered`,
  `PCM registered`, `descriptor rings:`, `stream-handshake:`, `engine armed:`, `stream-ev[0..7]`.
  Still `info` unconditionally: `stream-badread[n]`, and the probe `dev_err` when the device never
  becomes ready. Everything else already sat behind an opt-in param (`stream_probe`,
  `error_probe`, `seed_dump`, `resp_trace`, `tx_trace`, `rekick`, `arm_pre`, `tx_tone`).
  - **`stream-svc:` telemetry is now anomaly-gated, not unconditional** (`clarett_svc_log`). The 2-second
    window line prints at `info` only when that window had `late`, an `overrun`, a `badread`, or a
    `rekick`; otherwise `dev_dbg`. Same rule for the per-stream `stream-svc: stopped` summary, which now
    also carries the **run-wide** worst case (`run gapmax=/readmax=/late=/stepmax=`) so a demoted window
    line loses nothing. Rationale: `enable_pcm` defaults on and PipeWire holds a PCM open permanently
    ([[clarett-stream-gated-behaviour]]), so the old line wrote to the kernel log every 2 s for as long
    as the machine was up. **A dropout still announces itself** — the ~42 ms platform freeze trips `late`.
  - **Getting the detail back (no reload; all of it is runtime-switchable):**
    ```sh
    # everything, including the ~24 Hz mailbox trace — very noisy
    echo 'module snd_clarett +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
    # just the servicer telemetry (the usual want): match one statement by format
    echo 'format "stream-svc:" +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
    # or per file / per function
    echo 'file clarett_main.c +p'  | sudo tee /sys/kernel/debug/dynamic_debug/control
    ```
    or load with `insmod snd-clarett.ko dyndbg='+p'` to catch probe-time lines. `-p` turns them off again.
- Mailbox has a per-command trace (op/seq/cause/done/fcperr) at **`dev_dbg`** — off by default;
  enable via dynamic debug when diagnosing the mailbox (info-level would flood at the ~24 Hz meter
  poll). The notify re-read failure log is `dev_warn_ratelimited` (a walled device retries the
  config-change notification indefinitely, so an un-limited warn would flood).

## Driver limitations / TODO

- **★★★ THE 128-FRAME PIN BROKE A REAL JUCE APP — REPLACED BY A PER-STREAM PERIOD RULE (Sep 10 2026,
  8PreX, TONE3000; `CLARETT_MAX_PERIODS` in `clarett_pcm.c`).** Answers retest item 4 below: it did not
  survive real use. TONE3000 (JUCE 9.0.1's ALSA backend) garbled audio at every buffer above 64 samples.
  JUCE calls `set_periods_near(4)` BEFORE `set_period_size_near(block)`, so under the pin every block of 64
  or more was granted **period 32 / buffer 128** while JUCE kept reading and writing whole blocks (its UI
  still said 64; `/proc` said 32/128 — the two are different quantities). It also links playback to
  capture, never prefills, and sets `stop_threshold` to the boundary, so playback runs one block behind the
  hardware by design and holds only while block < buffer: slack 64 at a 64 block, 0 at 128, negative at
  256+ — a lap overwritten every cycle, heard as garbling and never reported as an xrun. TONE3000's UI
  marks 128 "(recommended)", i.e. the default setting was the broken one.
  - **Fix:** `max_buffer` defaults to 0 (the ring) again. Its latency job — alsa-lib resolves
    BUFFER_SIZE with set_last, so an app that pins only the period gets the ceiling — is now done per
    stream by two hw rules: buffer ≤ max(4 × period, `CLARETT_MIN_BUFFER_FRAMES`) and the converse
    (period ≥ buffer/4 above the floor). A 16-frame period still gets 8 periods (the floor). `max_buffer`
    survives as an optional hard cap; setting it to 128 reproduces the old pin exactly, which is how the
    baseline below was taken.
  - **Negotiation, measured (JUCE sequence | period-only):** 16 → 32/128 | 16/128; 32 → 32/128;
    64 → 64/256; 128 → 128/512; 256 → 256/1024; 512 → 512/2048; 1024 → 1024/4096. Live TONE3000 at a
    128 block: period 128 / buffer 512 (was 32/128).
  - **JUCE-exact duplex loop** (JUCE's negotiation and sw params, streams linked, no prefill, read N then
    write N; 15 s each, non-RT client; "lost" = cycles where the hardware had lapped the write position):
    | block | 64 | 128 | 256 | 512 | 1024 |
    |---|---|---|---|---|---|
    | old pin (`max_buffer=128`) | 2 (freeze) | **5624 / 5625** (no freeze) | **2804 / 2805** | **1394 / 1395** | — |
    | period rule | 2 (freeze) | 0 | 10 (freeze) | 3 (freeze) | 0 |
    Every loss under the rule sits in an ASRock ≥42 ms freeze window, per leg, from the `stream-svc`
    timestamps; every freeze-free leg was clean, and 1024 (an 85 ms buffer) rode a 63 ms freeze with
    nothing lost. A well-behaved prefilled client likewise xruns only in freeze windows (64: 1 freeze →
    1/1; 128: 1 → 3/3; 256: 4 → 9/9 capture/playback). On this board only a buffer above the freeze —
    1024-frame blocks — is glitch-free; that is the platform, not the driver.
  - **Harness lessons (each cost a wrong reading first):** a read-then-write client that neither links
    nor prefills underruns every cycle whatever the driver does, because its lead drains to zero each
    cycle; one that recovers an xrun without re-prefilling collapses into that state after the first
    freeze; and with debug output on, the `stream-svc:` line count is every 2 s window, not anomalies —
    count windows with gapmax ≥ 40 ms instead. Mirror the real client before counting xruns.
  - **TONE3000 by ear on the new module: 64, 128, 256 and 512 all clean (user-confirmed)**, with round-trip
    latency rising with the buffer as it should (4 periods of the block).
  - **PipeWire, re-checked on the new module:** takes the card and streams normally, negotiating period
    256 / buffer 1024 — exactly what its node requests (`api.alsa.period-size = 256`,
    `api.alsa.period-num = 4`, `api.alsa.disable-tsched = true`), so the rule is not what sets PipeWire's
    geometry, and the old ring default would grant the same. **No config file sets those values** — none
    of the user drop-ins (`51-alsa-pro-audio.conf` only sets channel positions for a USB Clarett node),
    nor anything under `/etc`, `/usr/local` or `/usr/share` for WirePlumber or PipeWire — so PipeWire
    derives them itself for the pro-audio profile, most likely from `52-clarett-noidle.conf`'s
    `node.latency = "512/48000"` (a period of half the latency; UNVERIFIED). **Consequence on the
    ASRock:** a 1024-frame (21 ms) buffer is below the ~42-60 ms freeze, and it caps that drop-in's
    `api.alsa.headroom = 3072` at the buffer size, so desktop playback through PipeWire will still skip at
    freezes. A user-config matter, not a driver one.
  - **★ FOUND WHILE TESTING, OPEN — A DEVICE-SIDE ENGINE WEDGE.** Mid-sweep, after roughly 90 arm/stop
    cycles on the ASRock, every stream began failing with EIO about 110 ms after start (ALSA's wait
    timeout). The handshake answered `err=0` throughout, and `engine armed` was identical to a working
    arm, but the engine never fetched a single descriptor: `stream-svc: stopped (periods=0 ... ptr0=0x0
    ptr1=0x0)`, where healthy streams stop at `ptr0=0x11 ptr1=0x4-0x5` and even the vendor's failing arms
    prefetch `0xe`/`0x3`. The control plane stayed healthy (every FCP status 0). **It survived a clean
    module reload and cleared only on a power cycle of the unit**, and an identical re-run of the sweep did
    not reproduce it. Not attributed to the period rule: that code only shapes negotiation and cannot reach
    the device, and no driver state survives a reload. If it recurs, load with `dyndbg=+p` and compare the
    failing arm against the last good one; note whether it follows a platform freeze during an arm or
    teardown.
- **★★ THE "EXCESSIVE POLLING CAUSED THE SKIPS" CONCLUSION DOES NOT REPRODUCE ON A FREEZE-FREE HOST
  (Sep 17 2026, 2Pre, ASUS ROG Zephyrus laptop / Intel, behind the HP TB3 dock).** The July 23 2026
  cluster that blamed control traffic for playback skips — `70afc2c` "rate-limit meter polling — the
  real source of the playback skip", `e580497` "suppress the notification relay while streaming (the
  real skip fix)", `4077d63` "real-time priority … (fixes GUI-load skips)" — took ALL of its evidence
  on the ASRock X570 Creator, **eight days before the firmware SMI was root-caused there** (July 31,
  [[clarett-playback-skipping]]), and that session then disproved polling as the freeze trigger. The
  two commits also contradict each other within ten minutes: `70afc2c` names the GUI's meter reads as
  the cause, `e580497` opens with "the GUI does not even read meters, so the meter poll was not the
  flood".
  - **New lever `notify_while_streaming`** (`clarett_main.c`, bool, runtime-writable, default 0 =
    today's behaviour) turns off the `stream_on` gate on the 0x400 notification relay, so it can be
    A/B'd without a reload. The gate's comment and `DEVELOPMENT.md` now say the mailbox load is real
    but the skips are unproven.
  - **Measured, digital-loopback ramp (playback 3 → capture 13 in the router, silent on a 2Pre since
    PCM 3/4 feed no output and no mixer input; it arrives on ALSA capture ch5, see the mapping item
    below), fcp-server + alsa-scarlett-gui with the Levels window open, non-RT clients, 120 s/leg:**
    | period/buffer | legs | ramp breaks | device period overruns | relays/s | mailbox cmds/s |
    |---|---|---|---|---|---|
    | 64/256 gate ON | 1 | 0 | 0 | ~0 | 50 |
    | 64/256 gate OFF | 1 | 0 | 0 | 8.0 | 96 |
    | 32/128 gate ON | 4 | 0, 0, 0, 0 | 8, 2, 34, 26 | ~0 | 50-51 |
    | 32/128 gate OFF | 4 | **1 xrun**, 0, 0, 0 | 38, 40, 18, 76 | 5.5-24 | 88-144 |
    **22.5 M consecutive ramp samples across the eight tight legs with one one-buffer repeat+skip
    pair** (14.96 s into one gate-off leg, i.e. a non-RT client starving, not corruption), `late=0`
    and `badreads=0` everywhere, no fcp-server timeouts either way, `periods` exact. So relaying
    mid-stream costs 2-3x the mailbox traffic (the extra is fcp-server's GET_DATA re-reads) and buys
    the audio nothing measurable. Unacked-period flags average higher with the gate off (43 vs 17.5)
    but the ranges overlap — noise at this sample size.
  - **Host check in passing: this laptop is freeze-free** — `gapmax` 1.6-2.4 ms against 1.33/0.67 ms
    nominal, `readmax` ≤ 770 us, no window ≥ 5 ms in 59 windows/leg. The ASRock reaches `readmax`
    42 ms. Third Thunderbolt platform after the ASRock and the EliteBook 640 G11.
  - **Default NOT flipped, deliberately.** The July symptom was on the **8PreX** (28ch, ~3x the
    notifiable controls and the per-period copy work), which has not been re-tested on a freeze-free
    host; and the storm is partly self-inflicted — the `interrupt-driven-mailbox` branch fixes an
    inverted notify mask, after which the relay would fire on real changes only and the gate becomes
    moot. Fix the cause, then drop the gate. Verdicts on the rest of that cluster: `18cccf0`/`cd6db04`
    (cause-register ownership) are real races and stay whatever the SMI did; `4ecfd6d` (debounce that
    never fired) is a real bug fix; `70afc2c` (meter cache) and `7fb4840`/`03ec348` (notify coalescing,
    trimmed notify-client mask) are cheap and correct as efficiency, with wrong skip attributions.
- **★ FIXED Oct 9 2026 IN THE DRIVER — THE CLARETT CAPTURE STREAM PUT THE LOOPBACK PAIR MID-STREAM, TWO
  CHANNELS AWAY FROM THE ROUTER'S NAMES.** The device sends capture in router-pin order, loopback pair
  after the fixed inputs and before ADAT (2Pre at position 4, 4Pre/8Pre/8PreX at 10) — so S/MUX only ever
  removes a tail and the loopback keeps one position at every rate (`rx_live_{mid,high}` add up exactly as
  fixed + 2 + surviving ADAT on all four). The router (and our maps) number that pair LAST (PCM 19-20 on
  a 4Pre/8Pre), so a recording app found loopback on ch 11/12 and router PCM 11-18 on ch 13-20.
  **The names are the operator's and stay; the driver reorders instead:** `clarett_model.rx_loopback_at`
  + `clarett_rx_drain()` move the pair to the end of each frame and shift the rest down two, and the
  S/MUX dead-tail blank moves down two with them. Reds are untouched (their maps name capture in stream
  order). **Hardware-verified with the digital-loopback ramp on the 4Pre and 8Pre at 48/96/192 kHz:**
  "PCM 19" -> ch 19, "PCM 20" -> ch 20, "PCM 11" (ADAT 1) -> ch 11, all sample-exact, removed channels
  silent. **8PreX verified the same way (Oct 9 2026):** "PCM 27"/"PCM 28" -> ch 27/28, "PCM 11" -> ch 11
  at 48/96/192 kHz, removed channels (19-26 at 96k, 15-26 at 192k) silent. 2Pre (position 4) not yet run. A renaming of the
  maps to stream order was tried first and REJECTED by the operator: capture names stay records PCM
  01..N, loopback last, never called "Loopback"; fix channel order in the driver, never by renaming.
  The original 2Pre finding (Sep 17 2026, measured twice): Routing the router's `PCM 13 Capture Enum`
  from playback `PCM 3` delivered the ramp on **ALSA capture channel 5**; routing `PCM 05 Capture Enum`
  (normally ADAT 1) from playback `PCM 4` delivered it on **channel 7**. Both legs confirmed by a
  per-channel scan, with analogue 1/2 on channels 1/2 as expected. Consistent with the 2Pre's capture
  stream being ordered analogue 1-2, S/PDIF 1-2, the two spare/loopback slots, then ADAT 1-8 — i.e. the
  authored map's PCM destination names are shifted against the stream past channel 4. (The 8Pre's
  earlier ADAT capture test, "ch12-19" at 48k, is channels 13-20 counted from 1: the same layout.)
- **★★ THE ALSA BUFFER USED TO BE PINNED TO THE 4096-FRAME RING (83 ms of playback latency at a
  16-frame period). Full record, measurements and method: `spec/provenance/clarett-buffer-latency.md`.**
  The buffer is now any power of two from `CLARETT_MIN_BUFFER_FRAMES` (128) up to the ring (pow2 so it
  divides the ring). What still matters from that work:
  - **alsa-lib resolves BUFFER_SIZE with `set_last` (the MAXIMUM)** while every other parameter gets
    the minimum, so an app that pins only the period is handed the driver's ceiling. Making a small
    buffer legal changes nothing without a ceiling, hence `max_buffer`, now done per stream by
    `CLARETT_MAX_PERIODS` (see the ★★★ bullet above).
  - **`delay` in `/proc/asound/cardN/pcm*/sub*/status` is the measured latency**; what an app displays
    is what it requested. And `owner_pid` there says WHICH client holds the PCM: the driver offers one
    playback and one capture substream, so a DAW on `hw:N,0` locks PipeWire out completely (that was the
    whole "PipeWire plays nothing" episode). Use `pw-jack` to have both.
  - **PipeWire adapts to a lower ceiling by shrinking its ALSA period, not its graph quantum** (quantum
    stayed 1024 on 2Pre and 8Pre), and its unconstrained pick is not deterministic.
  - **`tx_guard` is not a latency term** (a write deadline, `delay` identical at 64/128/256). With an
    ordinary client every guard from 16 to 96 is equally clean; the 16/32 "catastrophe" came from a
    synthetic short-lead client and is unresolved (retest item 2). Default and clamp floor unchanged.
  - Servicer CPU is flat across buffer sizes (~11-12 %, dominated by MMIO polling), so don't argue
    against small periods on CPU grounds, despite `clarett_tx_fill` copying `ring - guard` every tick.
  - **★ THE DIGITAL-LOOPBACK RAMP — reuse it for any playback-glitch question.** Route a playback
    channel back into a capture destination in the router (`PCM nn Capture Enum` accepts PLAYBACK
    sources), pick a channel that feeds no physical output (8Pre: PCM 7-10; check per model), play a
    sample counter x256 (24-bit-lossless) and check that consecutive captured samples differ by exactly
    256. `skip/repeat B` with B == the ALSA buffer is an xrun; a non-multiple of the step is corruption.
    Drive both legs with `aplay`/`arecord` on `hw:N,0`, never PipeWire (it opens the capture side too).
  - **Retest list:** items 1, 3 and 4 are done (128 frames clean duplex at 64/60ch with RT clients on the
    EliteBook). Still open: 2 (`tx_guard` short-lead client on a clean host), 5 (analogue round-trip
    latency, measured), 6 (60 s cadence-4 duplex re-run; rates other than 48 kHz for these tests).
- **★ LOW-LATENCY FLOOR = `dyn_period` cadence 4 (64-frame period, 1.33 ms), FULL DUPLEX (Aug 19 2026,
  2Pre).** 60 s of simultaneous 14ch capture + 4ch playback: `gapmax` 1369–1557 µs against 1333 nominal,
  `stepmax` exactly one period throughout (**no coalescing**), `late`/`overrun`/`badreads` all 0,
  `periods=45000` exact. Capture alone at the same cadence measured 1401–1687 µs, so **the TX fill costs
  nothing measurable.** That is ~2.7 ms round trip before converter latency — inside the <5 ms
  amp-modeling target, on a host without the SMI freeze. **Cadence 1 (16 frames) is the HARDWARE floor,
  not the usable one:** it runs, but the engine flags ~0.6 period overruns/s, the servicer coalesces 1–2
  periods per poll, and `CLARETT_TX_GUARD_FRAMES` (64) is *four periods* wide there — so the TX guard, not
  the period, would set playback latency anyway. Cadence sweep at 48 kHz (1/4/16/64 = 16/64/256/1024
  frames) is in `CLARETT_CTR_OVERRUN`'s comment in `clarett.h`.
- **`0x300` bit 30 = PERIOD OVERRUN (`CLARETT_CTR_OVERRUN`), identified Aug 19 2026.** The device sets it
  on an event raised while the *previous* period had not been acknowledged. Proof: across two cadence-1
  runs, every 2-second window with `stepmax=0x1` had zero flags and every window with `stepmax=0x2` had at
  least one — **59 of 59 windows, no exceptions** — plus a cliff across cadences (36/36/0/0/0 at
  1/1-repeat/4/16/64) that rules out both a constant per-event rate and a time-based source. **The servicer
  used to DISCARD every event carrying it:** the counter was masked with `0x7fffffff`, which keeps bit 30,
  so a valid counter of `0x1a` read as `0x4000001a` and failed the `>= CLARETT_CTR_MOD` range test. Fixing
  the mask also **halved worst-case servicer latency** (`gapmax` 917–1220 → 467–754 µs) because the reject
  path did `usleep_range(100, 200)` per drop — the misread was self-inflicting the run's worst latency at
  exactly the cadence low-latency work needs. Telemetry: `overrun=` in the 2-second line; `badreads=` now
  means only genuinely unusable samples (all-ones dead-link reads), and `badbits=` is their cumulative OR.
- **Servicer `late=` is PERIOD-RELATIVE now (`clarett_tick_late_us()`), not a fixed 16 ms.** The old
  constant was calibrated when the period was always ~5.3 ms; `dyn_period` makes nominal span 0.33 ms to
  tens of ms, so a fixed threshold was wrong in *both* directions — at a 1024-frame period every healthy
  tick counted as late (so the documented `late=[1-9]` stall grep fired continuously), and at cadence 1 the
  same 16 ms is 48 periods and would flag nothing. Threshold is 3/2 × nominal (deliberately low: 3× of a
  1024-frame period is 64 ms, *above* the 42–48 ms platform freeze it exists to catch), floored at 2 ms.
- **Concurrent duplex prepare used to ORPHAN a servicer kthread — fixed Aug 19 2026, hardware-confirmed.**
  `clarett_pcm_prepare()` decided `arm = !c->stream_on` under `pcm_lock`, but `stream_on` was published
  only at the *end* of `clarett_engine_arm()`, with `clarett_stream_handshake()`'s ~20 mailbox commands in
  between — a **milliseconds-wide** window in which two prepares both believed they were first. Both armed,
  and both called `clarett_engine_run()`, whose unconditional `c->stream_svc = kthread_run(...)` overwrote
  the first thread's handle; since the loop exits only on `kthread_should_stop()` and `engine_stop()` can
  only stop the handle it still has, **the first servicer became unstoppable — and on `rmmod` it keeps
  executing module text while devres frees it: a panic.** PipeWire spaces its two prepares widely enough to
  have hidden this; `arecord & aplay` reproduces it every time, and a DAW opening duplex would too. Fixed by
  claiming the arm under `pcm_lock` plus a `WARN_ON_ONCE` guard in `clarett_engine_run()`. Log signature of
  the bug: two `engine armed` lines microseconds apart, two `stream-svc` lines per window, one `stopped` —
  **but as of the Aug 20 log cleanup those three are `dev_dbg`, so a recurrence announces itself by the
  `WARN_ON_ONCE` splat instead** (enable the old signature with `dyndbg` if you need to see it directly).
  Detect a live orphan with `ps -eLo pid,tid,comm,cls,rtprio | grep clarett-svc` (must be zero with no
  stream running) — there is no userspace way to stop it, so **reboot, do not `rmmod`**.
  **When two servicers ran, `gapmax`/`late` were GARBAGE** (up to 998 ms, hundreds of late ticks): they are
  per-thread locals, and with two threads racing on a read-to-clear `0x300` each sees a random subset of
  events. The frame clock was fine throughout — read-to-clear still delivered each event exactly once.
- **★ CONCURRENT DUPLEX *CLOSE* WEDGED THE CLOSING PROCESS UNKILLABLY — fixed Aug 20 2026,
  hardware-confirmed. Fixing prepare did NOT fix teardown; this is the same bug at the other end.**
  `clarett_pcm_detach()` makes its "last one out" test **after** dropping `pcm_lock`
  (`mutex_unlock(); if (!c->pcm_sub && !c->pcm_play_sub) clarett_engine_stop(c);`), so when `arecord` and
  `aplay` end a timed duplex run in the same instant, both observe both substreams NULL and **both** call
  `clarett_engine_stop()` — which serialised nothing: both read the same `c->stream_svc` and both called
  `kthread_stop()` on it. The first wins; **the second calls `kthread_stop()` on an already-exited,
  already-reaped task and blocks forever on a completion nothing will signal again.**
  - **Diagnostic signature (deliberately counter-intuitive): the servicer's `stopped` line DOES appear** —
    the winner ran to completion — *while a process sits in `kthread_stop()`*. Stacks:
    `aplay D+ kthread_stop ← clarett_engine_stop ← clarett_pcm_close ← snd_pcm_release`;
    `arecord DN snd_pcm_release` (queued behind it); `arecord D+ snd_pcm_open` (every later open queued
    behind that). `D` state means SIGINT **and** SIGKILL are ignored, so `timeout -s INT` cannot recover it
    and Ctrl-C does nothing. **Reboot; do NOT `rmmod`** (a thread wedged inside the module + devres free =
    panic). Get the evidence first: `ps -eLo pid,tid,stat,wchan:32,comm` and `sudo cat /proc/<pid>/stack`.
  - **Fix:** claim the teardown under `pcm_lock` — take `stream_on` **and** the servicer handle together,
    so exactly one caller proceeds and the loser returns at the `stream_on` test. `kthread_stop()` stays
    **outside** the lock, and that is mandatory, not stylistic: the servicer calls `clarett_pcm_tick()`,
    which takes `pcm_lock`, so stopping it under the lock trades this hang for a deadlock.
  - **Verified:** 10/10 consecutive simultaneous `arecord &` / `aplay &` 5 s duplex runs with no survivor,
    then a clean `rmmod`. Before the fix it hung on the first collision. **The stream itself was never
    implicated** — the run that exposed this clocked 44994 periods at cadence 4 with `late=2 overrun=4`
    before teardown hung, which is a *passing* stream result.
  - **★ METHOD (now twice in two days): audit every "am I the first/last one here?" decision for whether
    it is evaluated under the lock that guards the state it reads.** Both instances were latent for months
    and surfaced only when two processes hit the same instant — prepare hid behind PipeWire spacing its
    opens, close simply won the race on every earlier run (including the day before).
  - **Test-hygiene traps from the same session, both of which briefly faked a result:** (1) `insmod`
    reporting `File exists` means the test ran against a **stale** module — always confirm a *silent*
    `insmod` right after `make clean && make`; (2) **unknown module parameters are IGNORED with a warning,
    not rejected**, so `insmod snd-clarett.ko force_arm=1` returning rc=0 proves nothing about removal —
    `modinfo` is the check, and the kernel log's `unknown parameter 'X' ignored` is the runtime proof.
- **Data plane: capture PCM clocks on hardware, stalls after one ring pass.** `clarett_pcm.c` (on by
  default, `enable_pcm`) registers a per-model S32_LE capture + playback device (up to 28ch; 44.1–192 kHz,
  see the sample-rate bullet below), driven by the persistent `0x300` servicer
  (`clarett_pcm_tick` → `snd_pcm_period_elapsed`). Hardware-confirmed this session:
  - The engine clocks via the PCM path (248-period burst, `ctr=0x1b3`) — requires (a) one **contiguous**
    buffer for both rings, (b) **full-duplex** arming (silent dummy TX on block 0; block-1-only won't
    clock and hangs `activate=5`), and (c) a **`0xAA` RX pre-fill before arming** (KEY: the lone diff
    that made it clock; likely a write-visibility/`dma_wmb` effect, not the content).
  - Servicer ACKs `0x300` from `prepare()` (engine stalls in ms if unserviced from arm); `trigger` only
    gates `period_elapsed` via `pcm_running`.
  - **THE WALL — root cause found and fixed in tree, hardware-confirmation pending (July 23 2026, spec
    §14).** `ctr=0` (engine reads our table, fires periods, consumes nothing) was our descriptor **table
    format**. `pmemsave` of the live 2Pre `0x210`/`0x310` (via `tools/dma_bases.py` + `dma_classify.py`)
    recovered the real format and exposed three bugs: **(1)** fragment stride is `channels·4·16` with NO
    alignment rounding (RX 14ch = `0x380`, not our `lcm`-doubled `0x700`; the `0x100`-alignment rule was
    false — vendor RX is `0x80`-aligned); **(2)** the RX ring carries a **periodic IRQ flag (bit1) every
    ~14 descriptors**, and consuming an IRQ-flagged descriptor is what raises the counted `0x300` period —
    we set only a single wrap flag on the last entry, so the counter never advanced (**the `ctr=0`
    cause**); **(3)** SIZE reg (4 frames) / fragment (16 frames) / IRQ period were conflated. All fixed:
    `clarett_frag_bytes` drops `lcm`, `clarett_build_rings` sets the periodic RX marker, the PCM period
    advances `clarett_irq_period_frames()` per event. **Test:** `enable_pcm=1`, `arecord -c14`,
    watch `stream-svc: ctr=` advance past the `0x1b3`/`0` one-pass wall — the window line is `dev_dbg` now,
    so turn it on first: `echo 'format "stream-svc:" +p' | sudo tee /sys/kernel/debug/dynamic_debug/control`.
  - Eliminated earlier this session (spec §13): arm ritual/timing (`arm_pre`/`arm_settle_ms`), TX content
    (`tx_tone`), and **`0x214`/`0x314` settled as a real 64-bit address high word** (`base_hi=2` faults at
    `0x2_ffe00000`; closes the `dma_bits` ambiguity). **Flat-buffer hypothesis FALSIFIED** — a flat ring
    faults dereferencing zeroed contents as pointers, proving the engine wants a table (the 2Pre "flat
    audio" dump was the fragment buffers). `flat_buffer` false on all models; `force_flat` param re-tests.
    Levers: `rekick`/`arm_pre`/`tx_tone`/`base_hi`/`force_flat`.
  - **Playback (TX) WORKS on the 2Pre (July 23 2026).** Full-duplex PCM (1 playback 4ch + 1 capture 14ch)
    sharing the one engine: whichever direction prepares first arms it, the other attaches at the shared
    `pcm_frames` clock. Each 0x300 tick drains RX→capture-ALSA (behind the write ptr) and refills
    TX←playback-ALSA (ahead of the read ptr, past `CLARETT_TX_GUARD_FRAMES` so the current DMA read is
    never torn); `pcm_lock` serialises the copies vs `hw_free`. TX plays silence when no playback stream is
    attached. **Confirmed audible** via `aplay` once **PCM 1 is routed to Analogue Output 1** in the router
    (alsa-scarlett-gui) — there is NO default route, so playback is silent until a PCM source is wired to a
    physical output (a mixer-config step, not a DMA problem). Simultaneous duplex stress not yet stressed.
  - **8PreX PLAYBACK WORKS — TX fragments must be page-safe too (July 30 2026, hardware-confirmed; spec
    data-plane §16).** 8PreX playback was garbled and folded 28ch→4 (a tone on PCM 1 also drove PCM 5/9/… —
    every output ≡ its source mod 4) while capture was clean. Everything the device reads was proven
    byte-identical to the vendor (registers, descriptor table, source-ids, handshake, arm, AND the 28-ch
    interleaved sample layout — confirmed by dumping the vendor's TX sample fragments *and* our live TX
    ring; fill clock perfect via `tx_trace`). **Root cause = the exact TX analog of the §15 RX drift:** the
    TX fragment `channels·4·16` is page-safe only when a power of two. 2Pre (`0x100`)/4Pre (`0x200`) are —
    which hid the bug — but **8Pre (`0x500`)/8PreX (`0x700`) straddle the 4 KB page**, and the device's
    per-fragment TX *read* mis-frames across the boundary into 4-channel groups. **Fix:** mirror RX slotting
    for TX — `c->tx_slot` = fragment rounded up to pow2 (`0x700→0x800`), descriptors strided by the slot,
    slot-aware fill `clarett_tx_fill` (mirror of `clarett_rx_drain`); ALSA buffer / per-period math stay on
    the LOGICAL contiguous size. Lever `tx_frag_pad` mirrors `rx_frag_pad`. No change for 2Pre/4Pre
    (fragment already pow2). Diagnostic `tx_trace` (per-period 0x218/0x318 ptr + `pcm_frames`) kept.
    8Pre playback VERIFIED Oct 9 2026: digital-loopback ramp sample-exact at 48/96/192 kHz (480k/480k/960k
    frames, zero breaks, no start loss). Not yet tested: simultaneous duplex stress.
  - **Sample rates 44.1/48/88.2/96/176.4/192 kHz — CAPTURE hardware-confirmed on ALL FOUR models (Aug 12
    2026).** A tone into Analogue 1 reads the correct, stable pitch at 96k and 192k with the full stream
    width and no glitches on 2Pre/4Pre/8Pre/8PreX (this was also the first 8Pre capture confirmation) —
    **no SMUX shrink**, so the fixed per-model channel count stays correct at every rate. Nearly free: the
    transport was already rate-agnostic (PCM prepare sends `SET_CLOCK{rate, Internal}` with the negotiated
    rate) and the whole data-plane geometry is in frames, with the servicer self-calibrating off the
    measured `0x300` counter delta — so `CLARETT_CTR_FRAMES=16` and the descriptor layout are unchanged at
    any rate; only the ALSA advertisement had pinned 48k. Per-model `clarett_model.max_rate` (all four =
    192000) gates the advertised `.rates` mask (`clarett_rate_caps` in `clarett_pcm.c`: 44.1/48 always,
    +88.2/96 double, +176.4/192 quad). The `max_rate` module param that overrode it for testing is
    REMOVED (Oct 2 2026, every model now at 192000): test a new model's rates by raising its field and
    rebuilding. **ADAT S/MUX at double/quad speed is DOCUMENTED in the vendor XML** — `<adat>`
    `pin`/`pin-m`/`pin-h` = the value at single/double(mid)/quad(high) speed, `0x0` = channel gone, giving
    textbook **8→4→2 channels per ADAT port** at 1x/2x/4x (analogue/S-PDIF have no override, present at all
    rates). The stream width genuinely does not shrink — but the "SMUX'd-away channels go silent" half of
    that claim was **WRONG, disproven on hardware Aug 14 2026**; see the S/MUX bullet below. Still untested
    (not blockers, none affect the audio path): HS *playback* re-verified only
    on the 2Pre (clean 96k tone) and the 8Pre (ramp sample-exact at 96k and 192k, Oct 9 2026). The rate-dependent LEVEL METERS that used
    to be listed here are no longer a caveat — they are a CONFIRMED BUG; see the meter bullet below.
  - **ADAT capture + S/MUX HARDWARE-CONFIRMED at single, double AND quad speed, and the S/MUX-removed
    channels carry junk that the driver must blank — FIXED (Aug 14 2026, 8PreX -> 8Pre).** Rig: **8PreX
    ADAT Out 1 -> 8Pre ADAT In** (both Thunderbolt), tones 233/311/419/523/631/743/857/971 on ADAT 1-8,
    8PreX master (Internal), 8Pre slaved (`clock_source` = ADAT). Quad speed needs THIS pair: the
    Clarett 8Pre **USB** has `pin-h="0x0"` on every ADAT *output* (no ADAT out at 176.4/192 kHz), while the
    TB units keep ADAT Out 1.1-1.2 — an earlier USB-source attempt could only reach 96k and read `Unlocked`
    at 192k, correctly. Results, all purity 1.000: **48k = ADAT 1-8 on capture ch12-19; 96k = ADAT 1-4 on
    ch12-15; 192k = ADAT 1-2 on ch12-13** — the XML `pin-m`/`pin-h` 8->4->2 prediction, proven end to end.
    Per-model `clarett_model.rx_live_{mid,high}` = leading capture channels the device fills at 2x/4x
    (2Pre 10/8, 4Pre 16/14, 8Pre 16/14, 8PreX 20/16 of widths 14/20/20/28), derived from the [XML]
    `<record-outputs>` `pin-m`/`pin-h` overrides — `0x0` = gone at that speed *and above* (the cascade is
    confirmed by the `<routing num/num-m/num-h>` deltas), and the dead set is a contiguous tail on every
    model. 8Pre's 16/14 are now hardware-verified; the other three are XML-derived.
    **What the dead channels actually contain (two wrong guesses before the right answer):** they are NOT
    silent. First they read as a frozen 32-frame loop of stale full-scale audio — the DMA ring is allocated
    once at probe and reused, so they replayed the previous stream. Blanking the ring at `prepare` cut that
    to a **sparse residue the engine actively writes: one non-zero sample every 32 frames, an impulse train
    at ~-25 dBFS**, and only into channels dropped at the *immediately preceding* speed tier (ADAT 5-8 at
    double, ADAT 3-4 at quad); channels dropped a full tier earlier stayed exactly zero. So the engine does
    keep touching those slots and a one-shot ring blank cannot hold. **Fix as landed:** `clarett_set_rx_live()`
    latches the live/dead byte split at `prepare`, and **`clarett_rx_drain()` blanks the dead tail per period**
    on the frames handed to ALSA. Costs one small memset per frame (16 B/frame on the 8Pre at 96k).
    Stream-start glitches are the ADAT receiver locking (first ~0.3 s), not a defect.
  - **FIXED Oct 2 2026 (per-rate maps + fcp-server's band switch, hardware-confirmed; rate-aware plan item 1).**
    **WAS AN OPEN BUG — the GET_METER slot array COMPACTS at high speed, so fcp-server's meter map is wrong above
    the first S/MUX-removed destination (Aug 14 2026, 8Pre, hardware-measured).** The meters sit at ROUTER
    DESTINATIONS, and a meter's slot is **its position in THAT RATE's destination table** — so every
    destination S/MUX removes shifts everything after it down. Measured with `tools/fcp_meter_watch.c` while
    an 8PreX fed ADAT into the 8Pre, one probe below the first removal and one above it:
    | | ADAT in (-> PCM 11-18) | Mixer Input 01 |
    |---|---|---|
    | 48k | slots 10-17 | **40** |
    | 96k | slots 10-13 | **32** |
    | 192k | slots 10-11 | **28** |
    A model built from the [XML] `pin-m`/`pin-h` removals **predicts all three exactly** (8Pre loses PCM 15-18
    + ADAT Out 5-8 = 8 slots at double, plus PCM 13-14 + ADAT Out 3-4 = 12 at quad; 70 -> 62 -> 58 live
    slots): Monitor Output 1 `18->14->12`, ADAT Output 1 `30->26->24`, Mixer Input 01 `40->32->28`. Note the
    ADAT INPUT meters do NOT move — they sit at slots 10-13, *below* the first removal (PCM 15 = slot 14) —
    which is why partial tests looked reassuring; the shift only appears when you probe above it.
    **Consequence:** fcp-server's `peak-index` is a single layout, so at 96k on an 8Pre EVERY meter above
    slot 13 (all outputs, S/PDIF, ADAT, all 30 mixer inputs) displays another channel's level in
    alsa-scarlett-gui. **Fix, easy half:** `tools/gen_fcp_maps.py` can emit `peak-index-m`/`peak-index-h`
    computed from the XML with no new measurement. **Hard half:** *fcp-server has no idea what sample rate
    the device is at* — clock/rate is not a config byte (`<clocking>` has no `offset-bytes`), so it would
    have to learn the rate from `/proc/asound/cardN/pcm*/sub*/hw_params` or a new FCP query. That is a design
    decision, not a patch. Separately, the vendor XML `<hardware-meters>` `meters-l/m/h` at `@136/146/156`
    (`METER_TABLE_[LMH]_OFFSET`) are the FRONT-PANEL bridge tables — a different mechanism, already written
    per-band by `clarett_meter_source_follow` on the 8PreX; 2Pre/4Pre/8Pre use the flash-persisted ones.
  - **"Clock Source" is an ALSA control, and the ONE control this driver owns** (`clarett_add_clock_control`
    in `clarett_pcm.c`; per-model lists in `clarett_main.c`: Internal/S/PDIF/ADAT everywhere, plus ADAT 2 and
    Wordclock on the 8PreX). **alsa-scarlett-gui needs NO changes** — `iface-mixer.c` already renders any
    element named `Clock Source` as a drop-down next to Sync Status. It cannot go through fcp-server:
    the source is not a config byte (so it cannot be a devmap global-control) AND `SET_CLOCK`'s payload is
    `{rate, source}` while fcp-server has no notion of the sample rate — the same gap that blocks the
    per-rate meter fix. The driver already sends SET_CLOCK at every arm and knows the rate, so it owns this.
    Backed by the `clock_source[]` module param, so control and sysfs are ONE value (a sysfs write bypasses
    the control's change notification). Changing it while idle sends SET_CLOCK immediately so Sync Status
    updates live; while streaming the change is deferred to the next arm rather than re-clocking mid-stream.
  - **`clock_source` is PER-CARD** (`module_param_array`, indexed by ALSA card number, runtime-writable,
    default Internal everywhere). A two-Clarett ADAT rig needs one master and one slave, so a scalar
    parameter would have slaved both. It is **not a config-space byte** — `<clocking>` has
    no `offset-bytes`, it exists only in the SET_CLOCK payload — so it CANNOT become an fcp-server devmap
    global-control; a GUI control needs a new fcp-server clocking category over `0x006003`/`0x006004`.
    `Sync Status` already exists via the SYNC capability and is the external-lock indicator — read it by
    **name**, never numid (fcp-server renumbers every control on restart).
  - **Clock-source enums — MEASURED, and the 2Pre XML is WRONG (Aug 14 2026).** `Internal=24`, `ADAT=0`,
    **`S/PDIF=3` on every model INCLUDING the 2Pre**, whose [XML] claims 4. Method: an 8PreX fed one optical
    port (switchable between ADAT and S/PDIF via its `S/PDIF Source Playback Enum`), reading the 2Pre's
    `Sync Status` per value, with an invalid value 7 as the negative control and the captured audio proving
    the source was really on the wire each time:
    | value | S/PDIF on wire | ADAT on wire | conclusion |
    |---|---|---|---|
    | 0 | — | Locked | ADAT |
    | **3** | **Locked** | **Unlocked** | **S/PDIF — tracks that source and only that source** |
    | 4 | Locked | Locked | NOT source-specific; locks to whatever is present |
    | 7 | Unlocked | Unlocked | rejected, so Sync genuinely discriminates |
    So the 2Pre's `option="4"` is something looser (any external / optical), not a per-model S/PDIF
    encoding: **there is no per-model split here** and an earlier `CLARETT_CLOCK_SPDIF_2PRE` was reverted.
    Verified: Internal (every stream arms with it), ADAT=0 (8Pre at 48/96/192 kHz), S/PDIF=3 (8Pre over RCA
    coax, 2Pre over TOSLINK). **REOPENED Oct 2 2026: the 0x006004 sync word is a bitfield (bit 0 lock, bit 1
    a read-to-clear "changed" latch) and fcp-server showed `!!word`, so an unlocked device with the latch
    set read Locked — very likely the cause below. fcp-server now reports bit 0 on these cards, and with it
    **ADAT 2 = 1 IS VERIFIED** (Red ADAT Out into one 8PreX port at a time: port 2 fed locks on 1 not 0, port
    1 fed the reverse, S/PDIF control unlocked throughout). WORDCLOCK = 2 VERIFIED Oct 9 2026 on the 8PreX AND the Red 8Line (BNC each way; the other sources
    unlocked, and the lock drops when the cable is pulled; results in the rate-aware plan). Same day, the Red's
    Loop Sync = 5, ADAT 2 = 1 and Dante = 4 too: EVERY Red 8Line source verified. Dante is the exception to
    "Sync Status is the probe" — a Dante module with no leader elects itself and stays Locked even with the
    Ethernet pulled; the sample clock (~3 ppm shift with an AVIO leading) is what discriminates.** Original finding: **8PreX ADAT 2=1 is UNVERIFIABLE by this method and stays OPEN:** on the 8PreX
    `Sync Status` does NOT reliably track the selected source — feeding one ADAT port from an 8Pre, the
    invalid control value 7 read `Locked` in 2 of 3 trials, while value 1 locked with EITHER port fed and
    value 0 locked ONLY with port 2 fed. Those are mutually inconsistent, so no port mapping can be claimed
    (a tempting "the XML labels are inverted" reading fitted 3 of 4 cells and was dropped when the control
    failed). Likely the 8PreX reports a lock if EITHER ADAT receiver has locked, independent of the
    SET_CLOCK selection — which would also make Sync useless as a probe on any two-ADAT-port model. Note the
    2Pre/8Pre results above are NOT affected: their negative control held in every run. (Wordclock=2: verified
    Oct 9 2026, see above.) **Anchor every such test on a negative control and re-check it per run** —
    the control is what separates a finding from a pattern fitted to noise.
    **METHOD TRAP — the audio path is NOT a probe for clock source.** S/PDIF and ADAT keep arriving on their
    capture channels whatever the clock source says, *even while Sync reads Unlocked* — the router does not
    care. An earlier reading of "the tones landed, so the enum selected S/PDIF" was therefore invalid;
    `Sync Status` is the only signal that distinguishes these values. Two other things that looked like
    signal and were not: the idle-ADC noise floor (an invalid value runs the converters too), and a single
    `Locked` reading taken right after another `Locked` leg (re-test from a known-Unlocked state).
  - **Attaching to an already-armed engine wedged the stream — FIXED July 24 2026, hardware-confirmed
    (commit `5f4bbcb`).** `clarett_pcm_pointer()` reported the *absolute* engine frame clock mod
    `buffer_size`, correct only for the direction that armed the engine (`prepare()` reset `pcm_frames`
    solely on the arming path). ALSA zeroes `hw_ptr` at every prepare, so any other attach — the second
    direction, or **the same one re-preparing after an xrun** — got a first `.pointer` return of wherever
    the free-running engine happened to be, which the core reads as a huge `hw_ptr` jump and xruns within
    a tick. Recovery re-prepares, lands somewhere else arbitrary, xruns again: **self-perpetuating**, with
    the only escape being a close of every substream so `clarett_engine_stop()` ran. That is why "a module
    reload clears it" kept being the recorded remedy. **Fix:** each direction records where it joined the
    shared clock (`pcm_base`/`play_base`); `.pointer`, the trigger's period index and the tick's period
    accounting are all relative to it. Consequence handled: ALSA buffer offset and hardware ring offset
    now differ by a constant rotation, which the copies had assumed away — `clarett_ring_copy()` and
    `clarett_rx_drain()` take separate source/destination positions that wrap independently.
    **Diagnostic that found it:** `cat /proc/asound/card*/pcm*/sub*/status` — `state: XRUN` with `avail`
    *exceeding* `buffer_size`, a fresh `trigger_time` on every look, and `hw_ptr` at a different multiple
    of the 256-frame hardware period each time. Healthy steady state is `RUNNING` with
    `appl_ptr - hw_ptr == delay` ≈ one period. Note the engine telemetry looks **perfect** throughout
    (`late=0`, periods advancing) — this failure is entirely above the DMA layer.
- Mixer **"get" returns a shadow**: write-through on put, and the **monitor bytes
  (24/28/112) are refreshed from the DMAed GET response on a notification**, so
  those reflect live hardware. **GET-response layout decoded** (16-byte echoed FCP
  header + data at +16; `resp[16+i] == config[off+i]`; guard on the echoed cmd at
  +0). Other bytes stay write-through. See transport spec §8.
- **Async notifications implemented** (MSI **vec0** / cause `0x400`): the ISR detects
  the §11 dim-mute/monitor mask, a workqueue re-reads the monitor region and
  `snd_ctl_notify()`s the monitor controls. **Mailbox completion is still polled**
  (the ISR deliberately leaves the `0x100` cause to the poll to avoid a race).
  - **The relay is gated off while streaming (`stream_on`), so the monitor knob used to freeze for the
    duration of any stream — FIXED July 24 2026, hardware-confirmed.** The gate is necessary: vec0 also
    fires per audio period and `0x400` reads its idle `0x3` each time, and the relay is a *wildcard*
    (the FCP notify word is not exposed), so fcp-server answers each period by re-reading EVERY
    control — mailbox flood, audible skips. Its premise ("front-panel moves during playback are rare")
    died with `enable_pcm` defaulting on: PipeWire adopts the card and holds a PCM open permanently, so
    the gate was closed essentially always. **Fix:** `clarett_monitor_poll()` — the meter worker, which
    already issues `GET_METER` at ~24 Hz during streaming, also does one `GET_DATA{24,92}` per tick,
    memcmps it against `c->mon_snap`, and relays **only on a real change**. Idle costs one command per
    tick and relays nothing. Lever `monitor_poll=0` restores the frozen behaviour. **Only the monitor
    region is covered** — any other self-changing control still won't update mid-stream (believed moot
    on the 2Pre: no front-panel Mode/Air on the Clarett TB units; the 8PreX front panel is unenumerated).
    **Method note:** `cat /proc/asound/card*/pcm*/sub*/status` is the one-line check for "is something
    holding a stream open" — this whole symptom was one `RUNNING` on `pcm0p`.
- **★ MODEL AUTO-DETECTION IS THE ONLY PATH — the `model=` parameter is REMOVED (Aug 20 2026).** The
  device decides, or no card registers. `clarett_pick_model()` and the `model=` charp param are gone;
  the id_table's 2Pre is now only a placeholder until `GET_7.1` answers. **The former fallback now fails
  the probe with `-ENODEV`:** a device that answers with a geometry matching no `clarett_model`
  (previously "unrecognized stream geometry; override with model=. Assuming 2Pre"). The
  collapse/not-ready `-ENODEV` was already there and is unchanged.
  **Why refusing beats guessing:** channel counts, DMA ring + descriptor geometry, fragment strides,
  routing/mixer tables and the meter layout are all sized from `c->model`, so a wrong model is not a
  cosmetic mislabel — it is a card streaming the wrong width into wrongly strided rings. **Adding new
  hardware (e.g. the Red 8Line) is now a `clarett_model` entry, not a load-time flag** — the probe
  error prints the raw `playback=/capture=` pair to key it on. One subtlety fixed in passing: the
  readiness poll runs `clarett_detect_model` *quietly* and breaks immediately on a valid-but-unmatched
  reply, so the non-quiet re-run is now gated on `!det` rather than `collapsed` — otherwise the
  unmatched case printed nothing at all and the `-ENODEV` referenced a pair that was never logged.
  **Tradeoff accepted:** the 8Pre/8PreX geometry pairs are XML-derived, so if one is wrong that model
  now fails to register where `model=` could previously force it up. The failure is loud and the fix
  is a one-line table edit against the logged pair.
- **Bring-up ("arm") is OPT-IN, not automatic (Aug 12 2026 — supersedes the July 23 "probe ALWAYS
  arms" design).** Firmware *code* self-boots from flash, and — the decisive finding — a
  *previously-armed* unit fully self-arms across a genuine power cycle: config reads, input metering,
  **and control writes** all work with **no host bring-up** (hardware-confirmed device-wide — 2Pre + 8Pre
  loaded with no arm: model auto-detected, meters live, Inst/Line relay switching). So the ~232-command
  replay is a **no-op on any used device**, and its `SET_MUX`/`SET_MIX` steps would only *reset the user's
  routing* to the vendor default. **Default probe now arms NOTHING:** it polls `clarett_detect_model`
  (GET_7.1, quietly) until the flash-persisted session answers, detects the model from it, and leaves
  routing untouched. **Since Oct 10 2026 it asks AT ONCE and retransmits:** a lost first command is
  re-sent every `ready_retry_ms` (250) as seq 0 (the mailbox resets to 0 after any unanswered command) until answered, within
  `ready_timeout_ms` (10 s); `settle_ms` (default now 0) is only an optional quiet period. If the
  budget runs out, probe **fails loudly (`-ENODEV`, no card registered)** — power-cycle to retry. See
  the COLD-ATTACH REFUSAL entry below; the "unrecoverable wedge" it describes was a sequence-number
  problem.
  - **★★★ RESOLVED Oct 10 2026 — THE "WEDGE" IS A LOST FIRST MESSAGE, AND RETRANSMITTING IT WITH THE
    SAME SEQUENCE NUMBER RECOVERS IT.** A Clarett asked too early raises DONE but never DMAs the
    response; it then keeps expecting that command's seq and refuses (err=3, stale echoed seq) anything
    sent with a later one. Every recovery attempt ever made — retries, mailbox resets, quiet waits +
    re-init — went out with the driver's NEXT seq, which is why "nothing recovers it". Measured on the
    8Pre with a forced early touch (settle 300 ms), retry 3 s later: ack + next seq **0/1**; ack + same
    seq **5/5**; same seq, no ack **3/3** — so the seq is the key and the ack is not needed.
    **Fix (snd-clarett):** the mailbox advances `seq` only when the device answered the command as
    ours, and **resets it to 0 after an unanswered one** (operator's choice, Oct 10 2026, over "reuse the
    lost seq": identical at probe, where the lost command is always seq 0; NOT YET SEPARATED — see the
    next-session test of a nonzero first seq; 8PreX re-checked on this rule: 381 ms, attempt 2); probe asks at once and retransmits every 250 ms (10 s
    budget). **Attach times, power-cycle
    each:** 8Pre **6/6 in 378-389 ms** after enable, always attempt 2 (first lost, retransmission
    answered — so even 250 ms retransmits stay recoverable); Red **3/3 in 21-29 ms**, attempt 1. Was 3 s
    for both. Note the 8Pre answers a retransmission at ~0.38 s although a *first* command at 0.5-0.6 s
    was lost in the bisection: the "variable readiness" below was variable loss of the first message.
    **8PreX the same (Oct 10 2026): 6/6 in 379-390 ms, attempt 2 every time.** **4Pre is DIFFERENT
    (Oct 10 2026, chained behind the Red): 3/3 in 15-27 ms, attempt 1 — it never lost its first
    command**, despite the same double Thunderbolt appearance and the same ~140-165 ms enable after the
    second one. So the lost first message is an 8Pre/8PreX trait, not a Clarett-line one, and the 4Pre
    is no use for the seq reuse-vs-0 test. Still untried on the new probe: 2Pre.
    **★ RETRY TUNED Oct 10 2026 (8Pre, power-cycle each, 3 per value): the device is never "not ready
    yet" — it drops the FIRST command and answers the next one whenever it comes.** `ready_retry_ms`
    100/50/25/0 gave 227-237/177-183/145-155/123-130 ms, attempt 2 every time; at 0 the retransmission
    was answered ~1 ms after the lost one's 100 ms response deadline (~120 ms after enable). So the
    floor was the response deadline, not readiness. New probe-only `ready_resp_ms` (each attempt's
    response deadline; normal commands keep `resp_timeout_ms` 100): 20/10/5 ms gave 41-49/33-42/28-31 ms,
    attempt 2 every time, no late landings. **Defaults now `ready_retry_ms=0`, `ready_resp_ms=20`**
    (~45 ms 8Pre attach, was ~380). The remaining ~20 ms before the first command is the pre-mailbox
    init. 20 kept over 5 as margin (a healthy response lands in ~150 us; a host stall that outlasts it
    costs one harmless retry). **Re-run on the new defaults:** 8PreX 3/3 in 47-51 ms (attempt 2; was
    379-390); Red 8Line 3/3 in 21-31 ms (attempt 1, unchanged). Not re-run: 4Pre (answers at once, so
    unaffected). A "never landed" line logged ~100 ms after a `Link Down` is the in-flight command
    at power-off (seen for GET_METER and GET_DATA), not an attach fault.
    **★★★ ROOT CAUSE FOUND Oct 10 2026 — THE "LOST FIRST COMMAND" WAS OURS: main sent it before the
    device acknowledged the response-buffer address.** The device answers the `0x414` (address high
    word) write with `0x400` bit0; main slept a fixed ~3.22 ms there. Ported from the unmerged
    `interrupt-driven-mailbox` branch (commit `8bb5122`, Geoffrey D. Bennett, which measured 2-11 ms on a
    2Pre/4Pre) as lever `addr_ack_ms` (the vec0 ISR completes `addr_acked` on bit0 while init waits).
    **8PreX, 3 power cycles, addr_ack_ms=500: acknowledged after 12.76-12.97 ms, first command answered
    every time (attempt 1, zero lost), 28-40 ms enable->registered.** So the 8PreX acks just after
    main's first command (see the correction below). Consequences: the retransmit/`ready_resp_ms` work is
    a fallback, not the fix; the seq reuse-vs-0 question is moot AT PROBE (it still matters for a
    mid-session lost response); the Oct 9 "no register signals readiness" never polled `0x400`.
    **Confirmed and made the DEFAULT (`addr_ack_ms=500`; a missing ack warns and falls through to the
    retransmit loop):** 8Pre 3/3 acked after 12.64-12.98 ms, attempt 1, 33-41 ms; **Red 8Line 3/3 acked
    after 131-166 us** (~100x faster), attempt 1, 21-25 ms. Credited to Bennett in DEVELOPMENT.md and the
    snd-clarett commit. **4Pre 3/3 acked after 10.71-11.06 ms**, attempt 1, 30-36 ms. **Correction:**
    main's first command went out ~11.5 ms after the `0x414` write, not 3.22 ms (3.22 ms sleep + cause
    sweep + a further 8.22 ms sleep + header reads), so the earlier "4Pre/Red ack within 3.22 ms" was
    wrong: the Red acks at ~0.15 ms, the 4Pre at ~11 ms (half a millisecond inside the old timing),
    8Pre/8PreX at ~12.6-13 ms (1-1.5 ms outside it). **Warm sysfs rebind with the wait, 3 each, attempt 1
    every time: 4Pre acked after 2.10-2.17 ms (vs ~11 ms cold), Red 0.18-0.34 ms;** registered 11-14 ms
    after the ack. This EXPLAINS the old "a manual rebind never fails, the automatic probe does"
    asymmetry: a warm unit acks ~5x faster, well inside main's ~11.5 ms. Not yet run with the wait:
    2Pre; an 8Pre/8PreX rebind. **UNTESTED side effect:** the seq rule applies to
    EVERY command, so after a response lost mid-session (e.g. the ASRock MMIO blackout) the NEXT command
    — usually a different opcode — goes out as seq 0. Only a same-opcode retry at probe is measured. It may
    keep the session in step (possibly relevant to the session-collapse bug) or may not; watch for it.
  - **★★ COLD-ATTACH REFUSAL — MITIGATED, NOT DIAGNOSED (Aug 21 2026, 8Pre, EliteBook 640 G11 behind
    the Dock G4). [Superseded by the RESOLVED entry above.]** `settle_ms` (default **3000**) leaves the device untouched after attach, before the
    pre-mailbox init. **This is an observation, not a root cause.**
    **Observed:** an in-probe first touch ~140 ms after enumeration fails reliably; a first touch at 1 s
    or later has never failed. 3 s is margin over the only failing point measured, and is
    **hardware-confirmed on the probe path** — `enabling device` 17:40:59.141 → model line
    17:41:02.201, 3.06 s, first attempt. (That check mattered: every 1 s data point came from a manual
    sysfs bind, and binds never fail, so it was not obvious the number transferred to the probe path.) When it fails, the
    command completes (DONE raised) but never DMAs a response; the ack is correctly withheld (acking an
    unlanded response is what caused the wall), and the device then answers that command in place of
    every later one — stale `rseq`, blanket `err=3`. **Nothing recovers that**, which is why the fix is
    a don't-touch window and not a retry.
    **RULED OUT on hardware — do not retry any of these:**
    | hypothesis | killed by |
    |---|---|
    | recover the wedge by retrying | tight polling at 2/10/180 s budgets; 25 s spacing; replaying the init every 5 s (13 attempts); both combined |
    | reset the mailbox (`0x510`/`0x500` + DMA addr) | 38 resets, refused identically |
    | the response is merely late | `resp_timeout_ms=3000`: 3.002 s elapsed with nothing, next command answered in 84 us |
    | device wake time from power-up | a 1 s delay after enumeration passes 4/4 |
    | unstable/flapping enumeration | 3/3 flapped runs PASSED |
    **THE UNEXPLAINED ASYMMETRY — start here if it resurfaces:** a manual sysfs bind has **never** failed
    (5/5, including at 1 s) while the automatic probe failed consistently with no settle. Same device,
    same timing window, different invocation path. That is not a timing question.
    - Every probe pays the wait, including a reload or sysfs rebind: unbinding disables the PCI device
      (`clarett_remove` + devres) and re-enabling brings it back in whatever state a fresh attach is in.
      An attempt to skip it for devices present at module load was **reverted** — a rebind 32 s after a
      good registration failed on its first command, command register visibly going `0000 -> 0002`.
    - **★ METHOD — six hypotheses died in one session, each killed by the next measurement.** Every
      experiment varied HOW WE RETRY; none varied WHETHER WE TOUCH IT AT ALL, because the first attempt
      looks free (on a warm device it always succeeds). Two ingredients were also tested only separately,
      never together. And three drafted conclusions were withdrawn when the operator supplied a step
      absent from the pasted log — a power cycle done to free a busy `rmmod`, and an `rmmod` that had
      failed. **Reconstruct the operator's actions, not just the kernel log, before attributing a
      recovery.** Also: **the rig cannot resolve this further** — manual power cycles, one run at a time,
      on a chain that flaps unpredictably, cannot distinguish 4/4 from 4/5. Characterising the remaining
      asymmetry needs scripted power control and run counts, not more one-off bisection.
    - **★★ CHARACTERISED Oct 9 2026 (laptop, Red 8Line + Clarett 8Pre daisy-chained behind it; scripts
      `settle_bisect.sh`/`settle_attach.sh`/`retry_test.sh` in that session's scratchpad, wait_ready_ms=0
      so one attempt decides). Times are from the PCI enable unless stated.**
      | test | result |
      |---|---|
      | Red, sysfs rebind, settle 1000 -> 0 ms | all pass, incl. 0 |
      | Red, power-cycle attach, settle 0 ms | **6/6 pass** (enable comes ~480 ms after TB discovery; first command answered ~25 ms after enable) |
      | 8Pre, power-cycle attach, bisection | fail 500, 625; pass 656, 687, 750, 1000 |
      | 8Pre, power-cycle attach, 1000 ms x5 | **4/5** — the fail was at 1.168 s after the 2nd TB appearance, where a pass had been at 1.170 s |
      | 8Pre, plain 3000 ms (recovery attaches) | 5/5 pass |
      | 8Pre, 3000 ms spent READ-polling 16 side-effect-free regs every 10 ms | 2/3 pass; the fail refused at 3.1 s |
      | 8Pre, refused at 300 ms, then the 30 s-untouched + fresh-init retry x4 (100 s) | **still refused** — power cycle needed |
      | 8Pre, 500 ms, vendor command order (`READ_SEG{0,8}` first, as `FocusritePCIe.sys` opens) | **0/5** — `READ_SEG` itself never got its response and wedged the mailbox |
      **Conclusions:** (1) The **Red needs no settle**; the **Claretts do**, and their readiness VARIES
      per power-up (656 ms has passed, 1000 ms has failed) — so there is no threshold to tune, only margin;
      **3000 stays**. (2) **A Clarett appears on Thunderbolt TWICE at power-up** (`new device found` ×2,
      ~1.7-1.8 s apart); the PCI enable follows the second by ~165 ms. The Red appears once. pciehp shows
      why: the Clarett's PCIe link comes up for ~230 ms, drops, and comes back ~1.5 s later for good. (3) **No
      register signals readiness**: `0x000`/`0x004`/`0x008`/serial/`0x514`/`0x8000-0x801c`/`0x8020`/`0x8024`
      hold their final values from the first read after enable (0 changes over 3 s), long before the
      mailbox can answer. (4) Reads during the window MAY hurt (1 of 3 polled attaches failed at 3 s vs
      0 of 5 unpolled) — unproven at n=3; the polling option was not kept. (4b) **Command order does not matter:** any first
      mailbox command sent before the Clarett is ready wedges it, the vendor's opener included. The vendor
      captures show no readiness check either (34 BAR accesses, ~20 ms, then `READ_SEG`); Windows gets away
      with it in every capture only because the guest driver first touches the unit 9 s to 10 min after
      power-up. (5) **The retry recovers nothing**
      and it held the device for 100 s, which held back pciehp's removal and the re-attach that a power
      cycle triggers (the old "async probe does not stall hotplug" claim was false). Retry + `wait_ready_ms`
      removed; a refusal now fails at once. (6) The "never-armed unit" scenario is believed not to exist
      (operator's assessment): a unit arms itself at power-up provided nothing reads it too early.
      The rebind-never-fails asymmetry above is only PARTLY explained: a rebind skips the device's
      power-up, and the Red rebinds fine at 0 ms; but the Aug record has an 8Pre rebind failing with no
      wait, and no Clarett rebind was tested Oct 9.
  - **★ Aug 20 2026: `force_arm` and the whole bring-up replay are REMOVED from the driver.** The
    working assumption is now that every unit in the field has been through Focusrite Control at least
    once and therefore self-arms; nothing observed on hardware has contradicted it. Deleted with it:
    `clarett_arm_device()`, `clarett_apply_model_routing()`, `clarett_band0_routed()`, the
    `rearm_geometry` and `inject_clock` params, and `clarett_model.arm_seq`/`n_arm_steps` (41 → 38
    module params). **The four `arm_clarett_<model>.h` tables MOVED to `tools/arm-tables/` rather than
    being deleted** — `gen_fcp_maps.py` parses their `SET_MUX` bands for the router pins, so deleting
    them would have silently broken map generation (verified: the regenerated maps are byte-identical
    after the move). `tools/fcp_cap_read.c` still dumps the capability bytes. Transport §8.
    **If a virgin/never-armed unit ever turns up**, the replay is in git history before this commit and
    regenerable via `fcp_decode.py --emit-deblob`; that is the bridge to cross then.
    **HARDWARE-CONFIRMED on the 2Pre (Aug 20 2026):** loads with no arm, model auto-detected, one info
    line, fcp-server adopts the hwdep (so the flash-persisted session really is enough for the control
    plane), 60 s duplex at cadence 4 clocks 44997/45000 periods with `late=0`, 10 duplex start/stop cycles
    clean, `rmmod` clean. **8Pre AND 8PreX ALSO CONFIRMED (Aug 21 2026, EliteBook 640 G11 behind the
    Dock G4):** `Clarett 8Pre: ... PCM 20/20ch, MIDI` / card `1 [C8Pre]`, and
    `Clarett 8PreX: ... PCM 28/28ch, MIDI` / card `1 [C8PreX]`, one info line each. **The 8PreX result is
    the important one — its `{28,28}` pair was XML-derived and had never touched hardware, and it is
    correct.** Detection-only is now validated on 2Pre, 8Pre and 8PreX.
    **The unknown-geometry `-ENODEV` path is now HARDWARE-CONFIRMED (Sep 2 2026, Red 8Line):** it
    refused to register and logged `playback=64 capture=60` to key a new entry on, exactly as designed.
    See the Red range section.
    **Still untested:** a genuinely never-armed unit, and the 4Pre — lowest risk of the four, since its
    pair came from a real capture rather than the XML.
  - **★ SERIAL AND FIRMWARE-VERSION WORDS ARE CONSTANTS, NOT PER-UNIT DATA (Aug 21 2026).** An 8Pre and an
    8PreX print byte-identical identity: `serial 000012345678abcd fw app 0x04061973 fpga 0x18101966` on
    both (the fw words read as dates — 04/06/1973, 18/10/1966). So the "dummy serial" in the hardware
    facts extends to the version words: **none of these fields can identify a unit or distinguish a
    model**, and nothing may key off them. Independently justifies geometry detection being the only path,
    and rules out fw-version-gated feature detection if that is ever tempting.
  - History: probe used to ALWAYS arm (July 23), after an "is it already armed?" detection proved
    unworkable — every host-visible surface (`CAP_READ`, a `GET_DATA` echo, the pre-mailbox block) reads
    *identically* fresh-vs-armed, so probe skipped the bring-up on exactly the devices that needed it
    (quiet casualty then: input meter slots read flat 0). Aug 7 showed the arm is a no-op on used devices;
    Aug 12 confirmed it covers writes too, and that "unarmed"-looking devices are the cold-readiness-race
    collapse (which arming does **not** rescue — only waiting does). So the unconditional arm was inverted
    to opt-in. The old `skip_arm` lever is **removed** — the default now *is* "don't arm".
- **OPEN BUG — the session can COLLAPSE (July 23 2026, 2Pre).** Symptom: fcp-server refuses the device
  with **"Device does not support required INIT category"**. The mailbox still answers and still echoes
  the opcode correctly, but **every response payload is zeros** — `CAP_READ` reports no category supported
  *including DATA*, while a DATA-category `GET_DATA` is what just answered (the self-contradiction is the
  tell). Seen after a run of PCM arm/stop churn (four `engine armed` → `stream-svc: stopped periods=0`
  cycles). **A module reload clears it with no bring-up and no power cycle** (`clarett_is_armed` correctly
  reports armed afterwards), so it is host/session state, not the device losing its arm.
  Check with `sudo ./fcp_cap_read /dev/snd/hwCxD0`; recover with `rmmod`/`insmod`.
  - **Aug 19 2026: CHURN AND CLIENT CONCURRENCY ARE RULED OUT as the trigger, and the ~48 ms MMIO blackout
    is now the prime suspect.** Two deliberate provocations on the EliteBook — a platform with **no**
    blackout — both came back healthy: (A) 20 rapid `arecord` arm/stop cycles with no other mailbox client;
    (B) the identical 20 cycles with **fcp-server active and polling meters** throughout. So neither churn
    nor a second client is sufficient. **What differs is the HOST:** every collapse ever recorded was on the
    ASRock, which blacks out MMIO for ~48 ms every ~44 s, and a mailbox command issued inside that window
    fails. Each engine arm fires ~20 commands, so churn buys more chances to collide with a blackout —
    which explains why churn *correlated* without being sufficient, why the trigger was never isolated
    (probabilistic on a ~44 s cadence), why it survives a power cycle while fcp-server/PipeWire run (they
    keep issuing commands into blackouts and re-collapse it), and why it hit both the 2Pre and the 8PreX
    (different models, one host). It fits mechanically too: a command whose response never lands leaves the
    mailbox reading responses against the wrong command — exactly "opcode echoes, payload zeros, `CAP_READ`
    denies the category that just answered". **NOT PROVEN.** The decisive test is on the ASRock: reload
    with `resp_trace=1`, churn until it collapses, and check whether the first `rseq != seq` line lands in
    the same second as a `badreads` bump. If so, collapse is not its own bug but another symptom of the
    platform fault. Find the onset with
    `journalctl -k --since "$start" | grep FCPr | perl -ne 'print if /seq=(\d+).*rseq=(\d+).*err=(\d+)/ && ($1 != $2 || $3)'`.
- **Control plane WORKS (July 16 2026 — wall crossed, `spec/provenance/clarett-manifestation-wall.md` §8).**
  The response-landed-gated trailing ack + pre-submit header zero are the **unconditional default
  mailbox cycle** (attribution matrix closed 3/3: gated arms, ungated walls; `gated_ack` lever
  retired, `resp_trace` telemetry kept). The full bring-up answers `err=0` with real data and
  alsamixer toggles physically move the 2Pre (LEDs + relays). TODO: re-audit everything written for
  a walled device (shadow-refresh paths, the `err=3`/notification-storm handling, meter-poll
  hypothesis in `meter_poll_ms` desc). The "re-arming an armed device wedges `GET_DATA`" rule was
  **DISPROVEN July 23** — re-armed twice with no power cycle, `GET_DATA` stayed correct; probe now always
  arms (see the bring-up entry below).
- **★ MIDI TX FLOW CONTROL + ONE-PASS RX DRAIN — PORTED Oct 10 2026 from Bennett's `0af0398`
  (interrupt-driven-mailbox branch).** `0x500` bit16 = TX FIFO ready; a word written while it is clear is
  discarded. Every TX write now waits on it (`read_poll_timeout`, 200 us poll); `midi_tx_pace_us` removed.
  RX drain bound 64 -> 256 (FIFO measured 139 B by him; a partial drain strands bytes, since a non-empty
  FIFO raises no new interrupt). **4Pre, single-cable self-loop:** old driver at full speed delivered
  **75 of 3003** bytes; with the fix 3003/3003, 1500/1500 notes, 4 KB SysEx PASS; TX ~7% over the wire
  floor (poll granularity).
  **★ OPEN — THE 8PRE LOSES 2 MIDI BYTES EVERY ~49.7 ms, AND IT IS NOT THE DRIVER'S DOING SO FAR.**
  Self-loop, paced at half the wire rate (FIFO never full): losses sit on a strict ~49.7 ms grid
  (phase-coherent over 2 s, skipping beats only when no byte is in flight), 2 bytes each, occasionally a
  short burst of 2-of-every-3. Unchanged by: the port (the OLD driver loses identically), closing
  alsa-scarlett-gui, `meter_poll_ms` 40 -> 100, stopping fcp-server, no PCM open. The 4Pre on the same
  host, cable and driver is byte-exact. So it is 8Pre-specific and most likely device-side (a ~20 Hz
  internal task?). Unresolved: TX or RX side (needs a second MIDI device cross-connected), and whether
  Focusrite Control's own driver loses the same (Windows VM + the same loop). Scripts: `midi_gaps.py`,
  `midi_paced.py` (session scratchpad); Bennett's `tools/midi_loopback.py` is on the branch.
- **Surprise removal panicked the host (July 23 2026) — FIXED, hardware-confirmed.** Powering
  the unit off mid-stream: `snd_card_free()` frees the PCM devices (and `runtime->dma_area`) *before*
  `card->private_free`, where the stream servicer was stopped, so the servicer ticked into a freed
  capture buffer. `clarett_remove()` now stops the servicer + meter poll before `snd_card_disconnect()`;
  the servicer also exits on an all-ones `0x300` from a disconnected device (bit31 is set in `~0`, so
  every read looked like a period event), and the mailbox fails fast on `pci_dev_is_disconnected()`
  instead of waiting out the response timeout per command. Confirmed by powering the unit off during a
  14ch `arecord`: the host survives and `arecord` exits `-EBADFD` (the correct ALSA disconnect error).
- Packed bitfield controls: monitor mute/dim enables (bytes 72/73) set at probe; others not implemented.
- **SW/HW output gain — verified, and the knob now follows into it (July 24 2026, 2Pre).** First
  hardware confirmation of `hwGainEnable` (offset 52, bit per output; 56 for outputs 3/4), previously
  XML-only: byte 52 read `0x03` (outputs 1-2 under HW control) while their stored SW gains at 32/33 sat
  at `0x7f` (the −127 dB floor) and the output was plainly audible at the knob's level — so **HW mode
  bypasses the stored SW gain entirely**. Confirmed not self-inflicted: the driver writes 72/73 at probe,
  never 52. **The device never mirrors the knob back** — turning it moves byte 112 only, 32-39 never
  budge. Consequences the USB siblings avoid (in-kernel `scarlett2` synthesises the link): the GUI fader
  won't follow, and a HW→SW toggle JUMPS to the stale software value. **FIXED in the driver, not
  fcp-server** (`clarett_hw_gain_follow`, lever `hw_gain_follow`): on every monitor-region change — and
  on the first poll, so a fresh load is already in sync — write byte 112 into the SW gain of each output
  whose HW-enable bit is set. Fixing the *device state* rather than the presentation makes both symptoms
  fall out with **zero userspace change**: fcp-server re-reads the byte so the fader tracks, and the
  toggle is silent because the stored value already matches. Writes are change-gated and use
  `clarett_write_u8_nosave()` — a mirror is not user intent, and persisting would commit the NVRAM on
  every movement of the knob. Note it DOES overwrite any stored SW gain on a HW output (inherent;
  `scarlett2` behaves the same). Both behaviours user-confirmed on hardware.

## Clean-room discipline

Build only from interface facts: the XML descriptors (Focusrite's own functional
description of the hardware), black-box MMIO captures, and public `scarlett2`/FCP
docs. **No vendor driver code is disassembled or copied.** Keep the original
vendor XML out of any distributed driver source; carry facts into the authored
spec instead. Cross-confirm XML-derived facts against the live trace where
possible — independent observation is the strongest provenance.

**NO CALENDAR DATES ANYWHERE UNDER `snd-clarett/`** — not in code comments, not in
`snd-clarett/README.md` or `snd-clarett/DEVELOPMENT.md`. That directory IS the
**public-facing git submodule** (formerly `driver/`), and dated comments timestamp the RE
work against the observation sessions, inviting a reader to correlate driver source with a discovery timeline.
State the finding, the model, the method and the numbers; drop the date tag — write
"Established on hardware (2Pre) by a dyn_period cadence sweep", not "…(2Pre, Aug 19 2026)".
Dates stay where they earn their keep: this file and `spec/provenance/*`, which exist to BE
the dated evidence trail. Audit with:
```sh
grep -rniE "\b(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)[a-z]* [0-9]{1,2},? 20[0-9]{2}|\b20[0-9]{2}-[0-9]{2}-[0-9]{2}\b" \
  --include='*.c' --include='*.h' --include='*.md' snd-clarett/
```
The submodule's commit timestamps are public too, so keep RE narrative out of its commit
messages.
