# Clarett 8PreX — Linux ALSA Driver (Reverse-Engineering Project)

Clean-room reverse engineering of the **Focusrite Clarett 8PreX** (Thunderbolt
audio interface) to build a native Linux ALSA driver. This file is the portable
project memory: it captures state, key facts, and conventions so any fresh
session (or contributor) can continue without the original chat history.

## Goal & status

Build an in-kernel ALSA driver for the Clarett 8PreX. Two planes:
- **Control plane** (mixer/routing/preamp/clock/notifications) — **fully
  reverse-engineered** (every encoding confirmed against live traffic); a working
  mixer-only driver exists (`driver/`). Loads and probes on real hardware.
- **Data plane** (PCM DMA streaming) — **not yet traced; capture plan written**
  (`spec/clarett-8prex-data-plane.md`). This is the main remaining work. Key point:
  the `x-no-mmap` BAR trace is **blind to sample DMA** — the data plane is RE'd by
  triangulating BAR setup registers (`tools/bar_profile.py`) + guest-RAM dumps +
  period-IRQ correlation (`fcp_decode.py --async`).

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
models.** The 8PreX's own numbers come from `FCP Server Resources/Clarett 8PreX.xml`.

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
- **Open:** `0x3001` query triple; a 1 KB bulk `SET_DATA`; the init handshake.

### The control-plane model (the key result)
A config write = `SET_DATA{offset, len, value}` then `DATA_CMD{activate}`, where
`offset`/`len`/`value` and `activate` come straight from the XML per control
(`offset-bytes`, `bits`, and `command`). Verified end-to-end on master mute
(offset 24, activate 2) and master volume (stereo, offsets 32/33, activate 1).

### Output gain encoding (confirmed)
7-bit **attenuation** code = |dB| exactly, linear 1 dB/step: `0x00`=0 dB (unity)
… `0x7f`=−127 dB (floor). ALSA: `DECLARE_TLV_DB_SCALE(tlv,-12700,100,0)`, value
`v`(0..127) → device code `127 − v`.

## Repository layout

```
spec/clarett-8prex-control-plane.md   Authored control-plane spec (offsets, opcodes,
                                      enums, pins, mixer, routing). Provenance-tagged.
spec/clarett-8prex-fcp-transport.md   Mailbox/transport framing; confirmed reg map.
spec/clarett-8prex-data-plane.md      PCM-DMA capture PLAN (not yet traced): method, phases, risks.
spec/clarett-8prex-manifestation-wall.md  Proven boundary: control writes complete but don't manifest;
                                      every traceable surface (BAR0 + PCI config) matches FC → off-wire DMA.
driver/                               Out-of-tree module `snd-clarett` (control plane + experimental capture PCM).
  clarett.h, clarett_main.c (PCI probe + data-plane engine), clarett_mailbox.c (FCP transport),
  clarett_mixer.c (kcontrols), clarett_pcm.c (capture PCM, enable_pcm=1), Makefile, README.md
tools/fcp_decode.py                   vfio_region_* trace -> FCP transaction decoder.
                                      (--brief, --mix-diff, --async, --show-appspace, --classify).
tools/bar_profile.py                  vfio_region_* -> per-register activity profile; flags offsets
                                      outside the control-plane map (data-plane reg discovery).
FCP Server Resources/*.xml            Focusrite's device descriptors (RE source material).
*.txt / *.log                         Trace captures and working notes.
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
- Mailbox is currently instrumented with a per-command `dev_info` (op/seq/cause/
  done/fcperr) — useful for confirming round-trips; remove once trusted.

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
  - **THE WALL:** engine streams exactly one ring pass then cleanly stalls (device alive). No driver-side
    revive works — rewrite `0x110`/bases, full cause-block `pollall`, re-`activate=5` all fail. Our
    descriptor table already matches the VM's, so the wrap is an unobserved runtime behaviour → next is a
    fresh VM capture of a long capture session. Debug params: `rekick`/`rekick_ms`/`pollall`/`pcm_selftest`.
  - **No playback yet** (block-0 writeback-to-0 storm). Details: `spec/clarett-8prex-data-plane.md` §9 step 5.
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
  the 232-command vendor init (`driver/clarett_init_seq.h`, generated by `fcp_decode.py --emit-init`
  from `clarett_full_init_mute.log`): `CONFIG_PUSH`×122, subsystem enables `0x000001`, count queries,
  8 KB config read/writeback, `SET_MIX`×16 + `SET_MUX`×3. `clarett_arm_device()` replays it at probe.
  Must run on a **fresh** device — re-initializing an already-armed one wedges `GET_DATA`. Transport spec §8.
- **Control plane is complete but its effects don't physically manifest — BLOCKED at a proven boundary
  (`spec/clarett-8prex-manifestation-wall.md`).** After a correct bring-up, control writes complete
  (`done=1, fcperr=0`) but the front-panel state doesn't move. The earlier guess that this needs the
  **data plane** (streaming) is **DISPROVEN**: FC moves the same LEDs at idle, no stream/engine. This
  session eliminated *every traceable surface*: our FCP command stream is a faithful **subset** of FC's
  (init, 8 KB writeback content, per-toggle bytes, control regs all byte-identical), MSI is ruled out
  (FC polls all five cause blocks in lockstep, like us), and PCI config space matches (standard
  enumeration + MSI + bus-master). Disabling our only extras (`inject_clock=0 monitor_enables=0`,
  `drain_causes` A/B) changed nothing. **The sole remaining differentiator is bus-master DMA
  payload/timing, invisible to both the `vfio_region` and `vfio_pci_config` traces** — the same off-wire
  wall the data plane hit. Only remaining path: capture FC's DMA via guest-RAM dump (`pmemsave`), with a
  clean-room caveat that an FPGA/firmware blob may not be legally sourceable. Not a control-plane
  protocol bug — the encodings are confirmed correct.
- Packed bitfield controls: monitor mute/dim enables (bytes 72/73) set at probe; others not implemented.

## Clean-room discipline

Build only from interface facts: the XML descriptors (Focusrite's own functional
description of the hardware), black-box MMIO captures, and public `scarlett2`/FCP
docs. **No vendor driver code is disassembled or copied.** Keep the original
vendor XML out of any distributed driver source; carry facts into the authored
spec instead. Cross-confirm XML-derived facts against the live trace where
possible — independent observation is the strongest provenance.
