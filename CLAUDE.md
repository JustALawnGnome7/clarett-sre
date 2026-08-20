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
  clarett-rate-aware-plan.md        OPEN — the remaining rate-aware control-plane work, with steps and
                                    acceptance criteria: (1) hardware-test the fcp-server per-rate meter
                                    path (landed but compile-only), (2) per-rate router pins for the
                                    8PreX's re-pinning second ADAT port, (3) decode the 0x006004 upper
                                    bit. START HERE when picking this thread back up.
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
sudo make install                 # (top-level) maps -> $PREFIX/share/fcp-server,
                                  # WirePlumber drop-in -> conf.d. `make help`.
```
- **Userspace install**: the top-level `Makefile` places the per-model FCP maps and the
  WirePlumber naming drop-in where fcp-server/WirePlumber read them (replacing the old manual
  copies). It does NOT build the module — that's `driver/`. fcp-server auto-launch (udev rule +
  systemd template) still installs from fcp-support (`sudo make install` there).
- **PREFIX is `/usr/local` everywhere — don't qualify it.** fcp-support and alsa-scarlett-gui both
  default there, and this repo's Makefile now matches, so a bare `sudo make install` in each of the
  three is correct and consistent. The prefix must agree because fcp-server compiles its DATADIR in
  (`-DDATADIR=$(PREFIX)/share/fcp-server`), so maps installed under the other prefix are invisible
  to it. **Never leave both prefixes populated:** systemd (`/usr/local/lib/systemd/system` before
  `/usr/lib/systemd/system`) and udev (`/usr/local/lib/udev/rules.d` first) prefer `/usr/local`, so a
  stale `/usr/local` install silently shadows a freshly built `/usr` one — the unit keeps launching
  the old binary and nothing reports an error. `sudo make uninstall PREFIX=<old>` in fcp-support
  before switching, and check with `systemctl cat 'fcp-server@*'` that `ExecStart` is the binary you
  just built. The WirePlumber drop-in reaches `/usr/local/share` via `XDG_DATA_DIRS` (WirePlumber's
  config lookup honours it, and the systemd user manager has it), not via WirePlumber's own prefix.
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
- **A TB4 HOST NEEDS AN INTERMEDIATE TB3 DOCK for the Apple TB3→TB2 adapter (Aug 19 2026).** An **HP
  EliteBook 640 G11** (integrated Thunderbolt 4, Core Ultra) will **not** enumerate a Clarett through the
  Apple adapter plugged in directly; putting an **HP TB3 dock** between host and adapter makes it work. The
  legacy TB1/TB2 link negotiation the active adapter needs lives in **discrete** TB3 controller silicon, and
  Intel's integrated TB4 host ports dropped it — the dock terminates the TB4 link at its own discrete
  controller and re-originates a downstream port that still speaks legacy, so the handshake happens
  dock↔adapter, never host↔adapter. (The 840 G5 above is discrete TB3, which is why it takes the adapter
  directly.) Secure Boot off, stock `pci=` parameters, no security-level change needed.
  **`boltctl` saying `authorized` is NOT enough — `lspci -nn -d 1cb5:` is the check that the PCIe tunnel
  actually came up**, and behind a dock plus a legacy adapter that is where a chain falls down.
  Bridge chain also identifies the device's own Thunderbolt controller: host → **DSL6540 Alpine Ridge 4C**
  (the dock) → **DSL2210 "Port Ridge 1C"** (upstream + 2 downstream ports = the unit's in/thru jacks) →
  the `1cb5:0002` endpoint. So the **10 Gb/s single-lane link is the Clarett's own TB1-class controller, not
  a dock penalty**, and it refines "FPGA-based Thunderbolt front-end" above: the TB layer is an Intel
  DSL2210 and the FPGA is a PCIe endpoint *behind* it. Irrelevant for bandwidth (worst case in the line,
  8PreX 28in+28out S32 @192 kHz, is ~344 Mb/s both directions).
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
  becomes ready. Everything else already sat behind an opt-in param (`force_arm`, `stream_probe`,
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
    rates). The stream width genuinely does not shrink — but the "SMUX'd-away channels go silent" half of
    that claim was **WRONG, disproven on hardware Aug 14 2026**; see the S/MUX bullet below. Still untested
    (not blockers, none affect the audio path): HS *playback* re-verified only
    on the 2Pre (clean 96k tone; 8Pre TX untested at any rate). The rate-dependent LEVEL METERS that used
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
  - **OPEN BUG — the GET_METER slot array COMPACTS at high speed, so fcp-server's meter map is wrong above
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
    coax, 2Pre over TOSLINK). **8PreX ADAT 2=1 is UNVERIFIABLE by this method and stays OPEN:** on the 8PreX
    `Sync Status` does NOT reliably track the selected source — feeding one ADAT port from an 8Pre, the
    invalid control value 7 read `Locked` in 2 of 3 trials, while value 1 locked with EITHER port fed and
    value 0 locked ONLY with port 2 fed. Those are mutually inconsistent, so no port mapping can be claimed
    (a tempting "the XML labels are inverted" reading fitted 3 of 4 cells and was dropped when the control
    failed). Likely the 8PreX reports a lock if EITHER ADAT receiver has locked, independent of the
    SET_CLOCK selection — which would also make Sync useless as a probe on any two-ADAT-port model. Note the
    2Pre/8Pre results above are NOT affected: their negative control held in every run. Wordclock=2 needs a
    BNC source and is untested. **Anchor every such test on a negative control and re-check it per run** —
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
  the id_table's 2Pre now exists *solely* as the stand-in for `force_arm`'s (model-agnostic) bring-up,
  which must run before `GET_7.1` can be asked. **Both former fallbacks now fail the probe with
  `-ENODEV`:** a device that answers with a geometry matching no `clarett_model` (previously
  "unrecognized stream geometry; override with model=. Assuming 2Pre") and the same case after a
  `force_arm` bring-up. The collapse/not-ready `-ENODEV` was already there and is unchanged.
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

**NO CALENDAR DATES ANYWHERE UNDER `driver/`** — not in code comments, not in
`driver/README.md` or `driver/DEVELOPMENT.md`. That directory is destined for a separate
**public-facing git submodule**, and dated comments timestamp the RE work against the
observation sessions, inviting a reader to correlate driver source with a discovery timeline.
State the finding, the model, the method and the numbers; drop the date tag — write
"Established on hardware (2Pre) by a dyn_period cadence sweep", not "…(2Pre, Aug 19 2026)".
Dates stay where they earn their keep: this file and `spec/provenance/*`, which exist to BE
the dated evidence trail. Audit with:
```sh
grep -rniE "\b(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)[a-z]* [0-9]{1,2},? 20[0-9]{2}|\b20[0-9]{2}-[0-9]{2}-[0-9]{2}\b" \
  --include='*.c' --include='*.h' --include='*.md' driver/
```
