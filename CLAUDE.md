# Clarett 8PreX — Linux ALSA Driver (Reverse-Engineering Project)

Clean-room reverse engineering of the **Focusrite Clarett 8PreX** (Thunderbolt
audio interface) to build a native Linux ALSA driver. This file is the portable
project memory: it captures state, key facts, and conventions so any fresh
session (or contributor) can continue without the original chat history.

## Goal & status

Build an in-kernel ALSA driver for the Clarett line (2Pre/4Pre/8PreX).
**THE MANIFESTATION WALL IS CROSSED (July 16 2026 — `spec/clarett-manifestation-wall.md` §8).**
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
  Details: `spec/clarett-data-plane.md`.
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
spec/clarett-control-plane.md       Authored control-plane spec (offsets, opcodes,
                                    enums, pins, mixer, routing). Provenance-tagged.
spec/clarett-fcp-transport.md       Mailbox/transport framing; confirmed reg map.
spec/clarett-data-plane.md          PCM-DMA RE: method, recovered register/descriptor maps, and the
                                    validated-but-won't-sustain engine (boot→stream traced; below-BAR wall).
spec/clarett-manifestation-wall.md  The wall: full elimination record (§§1–7) + §8 THE CROSSING —
                                    trailing-ack-vs-response-DMA race; landed-gated ack arms the session.
spec/clarett-macos-dtrace-plan.md   DTrace of the working macOS driver (device runs on the M1): RUN and
                                    exhausted (§5d) — confirmed the wall, blocked inside the stripped kext.
spec/clarett-windbg-plan.md         RUN (§5e): WinDbg of the working Windows driver's init DMA — vendor's
                                    driver-level DMA is attribute-equivalent to ours; wall confirmed below-driver.
driver/                               Out-of-tree module `snd-clarett` (hwdep transport + experimental capture PCM).
  clarett.h, clarett_main.c (PCI probe + data-plane engine), clarett_mailbox.c (FCP transport),
  clarett_hwdep.c (the FCP hwdep ABI — the only control surface), clarett_pcm.c (capture PCM,
  enable_pcm=1), Makefile, README.md
fcp-server-data/*.json                Authored devmap + alsa-map pairs per model: the control set
                                      userspace (fcp-server) builds. See its README.
tools/gen_fcp_maps.py                 Generates all four map pairs (names, routing/mixer tables from
                                      the bring-up blobs, measured meter peak-index).
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
                                      MUX_READ windowing.
vendor-reference/Devices/*.xml        Focusrite's device descriptors (RE source material).
captures/*.log                        Trace captures (vfio_region_* logs, guest-RAM dumps, decoded
                                      dumps) + working notes (insmod/session notes; former .txt now .log).
```

## Build & test

```sh
cd driver && make                 # builds snd-clarett.ko
sudo insmod snd-clarett.ko        # auto-binds 1cb5:0002
```
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

- **Data plane: capture PCM clocks on hardware, stalls after one ring pass.** `clarett_pcm.c` (opt-in
  `enable_pcm=1`) registers a 28ch S32_LE @48k capture device, driven by the persistent `0x300` servicer
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
  - **No playback yet** (block-0 writeback-to-0 storm). Details: `spec/clarett-data-plane.md` §9 step 5.
- Mixer **"get" returns a shadow**: write-through on put, and the **monitor bytes
  (24/28/112) are refreshed from the DMAed GET response on a notification**, so
  those reflect live hardware. **GET-response layout decoded** (16-byte echoed FCP
  header + data at +16; `resp[16+i] == config[off+i]`; guard on the echoed cmd at
  +0). Other bytes stay write-through. See transport spec §8.
- **Async notifications implemented** (MSI **vec0** / cause `0x400`): the ISR detects
  the §11 dim-mute/monitor mask, a workqueue re-reads the monitor region and
  `snd_ctl_notify()`s the monitor controls. **Mailbox completion is still polled**
  (the ISR deliberately leaves the `0x100` cause to the poll to avoid a race).
- **Session bring-up IS required on a fresh device** (corrects an earlier note). Firmware *code*
  self-boots from flash, but a freshly power-cycled device rejects `GET_DATA` until the host replays
  the 232-command vendor init (`driver/clarett_init_8prex.h`, generated by `fcp_decode.py --emit-init`
  from `8prex_full_init_mute.log`): `CONFIG_PUSH`×122, subsystem enables `0x000001`, count queries,
  8 KB config read/writeback, `SET_MIX`×16 + `SET_MUX`×3. `clarett_arm_device()` replays it at probe.
  Must run on a **fresh** device — re-initializing an already-armed one wedges `GET_DATA`. Transport spec §8.
- **Control plane WORKS (July 16 2026 — wall crossed, `spec/clarett-manifestation-wall.md` §8).**
  The response-landed-gated trailing ack + pre-submit header zero are the **unconditional default
  mailbox cycle** (attribution matrix closed 3/3: gated arms, ungated walls; `gated_ack` lever
  retired, `resp_trace` telemetry kept). The full bring-up answers `err=0` with real data and
  alsamixer toggles physically move the 2Pre (LEDs + relays). TODO: re-audit everything written for
  a walled device (shadow-refresh paths, the `err=3`/notification-storm handling, meter-poll
  hypothesis in `meter_poll_ms` desc); untested whether re-arming an already-armed device still
  wedges `GET_DATA` (that finding predates the gated ack).
- Packed bitfield controls: monitor mute/dim enables (bytes 72/73) set at probe; others not implemented.

## Clean-room discipline

Build only from interface facts: the XML descriptors (Focusrite's own functional
description of the hardware), black-box MMIO captures, and public `scarlett2`/FCP
docs. **No vendor driver code is disassembled or copied.** Keep the original
vendor XML out of any distributed driver source; carry facts into the authored
spec instead. Cross-confirm XML-derived facts against the live trace where
possible — independent observation is the strongest provenance.
