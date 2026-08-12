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
- **4 MSI vectors**; MSI-driven (`DisINTx+`). PCIe Gen1 x1. Dummy serial.
- FPGA-based Thunderbolt front-end (firmware has App + FPGA segments).

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
driver/                               Out-of-tree module `snd-clarett` (hwdep transport + experimental capture PCM).
  clarett.h, clarett_main.c (PCI probe + data-plane engine), clarett_mailbox.c (FCP transport),
  clarett_hwdep.c (the FCP hwdep ABI — the only control surface), clarett_pcm.c (capture PCM,
  enable_pcm=1), Makefile, README.md
fcp-server-data/*.json                Authored devmap + alsa-map pairs per model: the control set
                                      userspace (fcp-server) builds. See its README.
wireplumber/51-clarett-naming.conf    WirePlumber drop-in: promotes the driver's per-model card name
                                      (api.alsa.card.name) into device.description so GNOME shows
                                      "Clarett 2Pre" not the generic "Clarett Multichannel". Coupled to
                                      the driver's card->shortname (matches on it); lives here, not in
                                      fcp-support, because it depends on the driver, not on fcp-server.
tools/gen_fcp_maps.py                 Generates all four map pairs (names, routing/mixer tables from
                                      the de-blobbed bring-up tables clarett_arm_<model>.h, measured
                                      meter peak-index).
tools/gen_sim_state.py                Map -> alsactl .state file, so alsa-scarlett-gui can render our
                                      control set with no hardware attached.
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
                                      required INIT category" — unarmed device vs zero capabilities).
vendor-reference/Devices/*.xml        Focusrite's device descriptors (RE source material).
captures/*.log                        Trace captures (vfio_region_* logs, guest-RAM dumps, decoded
                                      dumps) + working notes (insmod/session notes; former .txt now .log).
```

## Build & test

```sh
cd driver && make                 # builds snd-clarett.ko
sudo insmod snd-clarett.ko        # auto-binds 1cb5:0002
sudo make install                 # (top-level) maps -> /usr/share/fcp-server,
                                  # WirePlumber drop-in -> conf.d. PREFIX=/usr must
                                  # match the fcp-server install PREFIX. `make help`.
```
- **Userspace install**: the top-level `Makefile` places the per-model FCP maps and the
  WirePlumber naming drop-in where fcp-server/WirePlumber read them (replacing the old manual
  copies). It does NOT build the module — that's `driver/`. fcp-server auto-launch (udev rule +
  systemd template) still installs from fcp-support (`make install PREFIX=/usr` there).
- **Mixer-only**: `aplay -l` shows nothing (no PCM yet). Use `amixer -c N
  contents` / `alsamixer -c N`.
- **Device must be free of `vfio-pci`** to test on the host (stop the VM, unbind).
- To unload, release the card first: `sudo systemctl stop alsa-state.service`
  (and PipeWire/WirePlumber if they hold `/dev/snd/controlC*`), then `rmmod`.
- Bare-metal test box: handle Thunderbolt auth (`boltctl authorize`) and Secure
  Boot (unsigned module needs SB off or a signed MOK).
- Mailbox has a per-command trace (op/seq/cause/done/fcperr) at **`dev_dbg`** — off by default;
  enable via dynamic debug when diagnosing the mailbox (info-level would flood at the ~24 Hz meter
  poll). The notify re-read failure log is `dev_warn_ratelimited` (a walled device retries the
  config-change notification indefinitely, so an un-limited warn would flood).

## Driver limitations / TODO

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
    advances `clarett_irq_period_frames()` per event. **Test:** `model=2pre enable_pcm=1`, `arecord -c14`,
    watch `stream-svc: ctr=` advance past the `0x1b3`/`0` one-pass wall.
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
    Not yet tested: 8Pre playback (derived, no init blob), simultaneous duplex stress.
  - **Sample rates 44.1/48/88.2/96/176.4/192 kHz — CAPTURE hardware-confirmed on ALL FOUR models (Aug 12
    2026).** A tone into Analogue 1 reads the correct, stable pitch at 96k and 192k with the full stream
    width and no glitches on 2Pre/4Pre/8Pre/8PreX (this was also the first 8Pre capture confirmation) —
    **no SMUX shrink**, so the fixed per-model channel count stays correct at every rate. Nearly free: the
    transport was already rate-agnostic (PCM prepare sends `SET_CLOCK{rate, Internal}` with the negotiated
    rate) and the whole data-plane geometry is in frames, with the servicer self-calibrating off the
    measured `0x300` counter delta — so `CLARETT_CTR_FRAMES=16` and the descriptor layout are unchanged at
    any rate; only the ALSA advertisement had pinned 48k. Per-model `clarett_model.max_rate` (all four =
    192000) gates the advertised `.rates` mask (`clarett_rate_caps` in `clarett_pcm.c`: 44.1/48 always,
    +88.2/96 double, +176.4/192 quad); the `max_rate` module param overrides it for testing an unconfirmed
    model. **ADAT S/MUX at double/quad speed is DOCUMENTED in the vendor XML** — `<adat>`
    `pin`/`pin-m`/`pin-h` = the value at single/double(mid)/quad(high) speed, `0x0` = channel gone, giving
    textbook **8→4→2 channels per ADAT port** at 1x/2x/4x (analogue/S-PDIF have no override, present at all
    rates). Because SMUX'd-away channels go silent rather than shrinking the stream, this needs **no driver
    change**. Still untested (not blockers): HS *playback* re-verified only on the 2Pre (clean 96k tone;
    8Pre TX untested at any rate), and the narrow "a source into ADAT 1 lands on its normal capture channel
    at 2x, ADAT 5 silent" spot-check (only analogue-in was fed).
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
- **Bring-up ("arm") is OPT-IN, not automatic (Aug 12 2026 — supersedes the July 23 "probe ALWAYS
  arms" design).** Firmware *code* self-boots from flash, and — the decisive finding — a
  *previously-armed* unit fully self-arms across a genuine power cycle: config reads, input metering,
  **and control writes** all work with **no host bring-up** (hardware-confirmed device-wide — 2Pre + 8Pre
  loaded with no arm: model auto-detected, meters live, Inst/Line relay switching). So the ~232-command
  replay is a **no-op on any used device**, and its `SET_MUX`/`SET_MIX` steps would only *reset the user's
  routing* to the vendor default. **Default probe now arms NOTHING:** it polls `clarett_detect_model`
  (GET_7.1, quietly) for up to `wait_ready_ms` (2000) until the flash-persisted session answers, detects
  the model from it, and leaves routing untouched. A cold Thunderbolt attach can race device readiness
  (command #0's response may not land — see [[clarett-session-collapse-recovery]]); the poll absorbs it.
  If the device never answers, probe **fails loudly (`-ENODEV`, no card registered)** instead of the old
  fake-2Pre placeholder — a used device just needs a reload; a truly *virgin/never-armed* unit must opt in
  with `force_arm=1` to run the bring-up. `wait_ready_ms` tunes the settle budget.
  - The bring-up (used only under `force_arm=1`) is the de-blobbed typed step list
    (`driver/clarett_arm_<model>.h`, built by `clarett_arm.h`'s `clarett_arm_emit()`; regenerate via
    `fcp_decode.py --emit-init | --emit-deblob`): `CONFIG_PUSH`×N, subsystem enables `0x000001`, count
    queries, 8 KB config read/writeback, `SET_MIX`×N + `SET_MUX`×N. `clarett_arm_device()` replays it and
    still preserves live routing — it reads band-0 first (`clarett_band0_routed`, `MUX_READ` `0x003001`)
    and, if any destination is already patched, skips only the `SET_MUX`/`SET_MIX` steps (so a re-arm
    can't clobber a user's routing). `tools/fcp_cap_read.c` dumps the capability bytes. Transport §8.
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
  reports armed afterwards), so it is host/session state, not the device losing its arm. Trigger not
  isolated. Check with `sudo ./fcp_cap_read /dev/snd/hwCxD0`; recover with `rmmod`/`insmod`.
- **Control plane WORKS (July 16 2026 — wall crossed, `spec/provenance/clarett-manifestation-wall.md` §8).**
  The response-landed-gated trailing ack + pre-submit header zero are the **unconditional default
  mailbox cycle** (attribution matrix closed 3/3: gated arms, ungated walls; `gated_ack` lever
  retired, `resp_trace` telemetry kept). The full bring-up answers `err=0` with real data and
  alsamixer toggles physically move the 2Pre (LEDs + relays). TODO: re-audit everything written for
  a walled device (shadow-refresh paths, the `err=3`/notification-storm handling, meter-poll
  hypothesis in `meter_poll_ms` desc). The "re-arming an armed device wedges `GET_DATA`" rule was
  **DISPROVEN July 23** — re-armed twice with no power cycle, `GET_DATA` stayed correct; probe now always
  arms (see the bring-up entry below).
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
