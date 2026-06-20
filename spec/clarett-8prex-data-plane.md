# Clarett 8PreX — Data-Plane Capture Plan (PCM DMA streaming)

**Status:** Phase 1 (streaming register map) **recovered** from `clarett_full_init_mute.log` — see §3b;
Phase 2 (descriptor format + sample layout) **recovered** from live guest-RAM dumps — see §3c. The
rest (IRQ cadence, implementation) is still open. This scopes how to reverse-engineer
the audio **data plane** (bus-master DMA streaming): the methodology shift it needs, the tooling, and
the phased sequence. Tags: `[PLAN]` = intended approach; `[HYP]` = hypothesis to confirm; `[TRACE]` =
confirmed from a capture; `[ANCHOR]` = a control-plane fact we build on.

The control plane (mixer/routing/gain/mute/clock/notifications) is reverse-engineered and documented
in `clarett-8prex-control-plane.md` + `clarett-8prex-fcp-transport.md`. This is the remaining work.

---

## 1. Why the existing method does not transfer

All control-plane RE relied on trapping BAR0 MMIO via QEMU `x-no-mmap` (`vfio_region_*` events). That
**only disables the BAR mmap; bus-master DMA to guest RAM is untouched and never appears in the
trace.** Audio samples move device↔RAM by DMA and never cross the BAR, so the MMIO trace can show the
streaming **setup and control** but *never a single sample*. `[ANCHOR: BAR0 = mailbox + DMA-control
only; samples move by DMA — hardware facts]`

This is the central shift: the data plane is RE'd by **triangulation across three channels**, not by
the BAR trace alone.

## 2. Three observation channels

| Channel | Tool | Sees | Blind to |
|---|---|---|---|
| BAR MMIO | `vfio_region_*` → `tools/bar_profile.py` | ring **addr/size/control** regs; pointer reads; ISR cause-reg reads | the sample DMA |
| Guest RAM | QEMU monitor `pmemsave`/`xp` at the ring base | **buffer layout**: sample format, interleave, channel order, stride, ring size | flow/timing (snapshot only) |
| MSI period IRQ | `fcp_decode.py --async` | **which vector** fires + cadence | the MSI delivery itself (irqfd) |

Registers say *where / how big*; RAM says *the format*; period IRQs say *the cadence and which vector*.

Unlike the control plane, there is **no public reference** — `scarlett2` is USB-isochronous, nothing
like this FPGA PCIe DMA engine. Expect this to be empirical and slower.

## 3. Anchors we already have `[ANCHOR]`

- **4 MSI vectors**, but bare metal fires **only vec0** for all control-plane events (mailbox-done +
  notifications); the cause regs `0x100`/`0x400` distinguish events, *not* the vector. **vec1/vec2/vec3
  are completely unused so far** — so they are the prime suspects for the playback/capture period IRQs.
  A streaming capture should reveal whether the data plane wakes them (watch their `/proc/interrupts`
  counts), or whether it too multiplexes onto vec0 with new cause registers.
- **DMA-address precedent:** GET responses DMA into a host buffer whose bus address is programmed at
  BAR `0x410` (low32) / `0x414` (high32). The audio **ring base(s) will be the same low32/high32
  pattern at *new* offsets** — exactly what `bar_profile.py` flags.
- **IRQ enable mask** `0x104 = 0xf000003f` — the low 6 bits (`0x3f`) may be per-stream period-IRQ
  enables that light up during streaming.
- **28 PCM playback / 28 PCM capture** channels; PCM pins `0x600`–`0x61b` both directions
  (control-plane §3). Loopback 1–2 at `0x60a`–`0x60b`.
- **Stream start may also use the mailbox.** The init-only opcodes `0x5000`/`0x6000`/`0x7000` are
  still unexplained and could be format/rate/stream setup — watch FCP traffic during stream start,
  not just new registers. Clock-source / sample-rate are dedicated FCP commands (control-plane §7),
  still untraced, and must be set before streaming.

## 3b. Data-plane register map — RECOVERED from a streaming capture `[TRACE]`

**Phase 1 is largely already done.** `clarett_full_init_mute.log` was captured with Focusrite Control
**live**, so its ASIO engine was streaming — the trace contains the full streaming-setup register
activity (`tools/bar_profile.py clarett_full_init_mute.log --new-only`). Two structurally identical
**ring blocks** appear, at `0x200` (→ MSI **vec1**) and `0x300` (→ MSI **vec2**), confirming the
vec1/vec2 period-IRQ hypothesis:

| Reg (block+N) | block 0x200 | block 0x300 | role `[HYP]` |
|---|---|---|---|
| `+0x04` | `0x1c` (28) | `0x1c` (28) | channel count (28 PCM ch/direction) |
| `+0x08` | `0x1c0` (448) | `0x1c0` | size / period (units TBD) |
| `+0x0c` | `0x1` | *(not written)* | enable/start bit |
| `+0x10` / `+0x14` | `0x589b9000` / `0x2` | `0x589bd000` / `0x2` | **descriptor-table base** bus addr (low32 / **high32**, not control — see §3c) |
| `+0x18` / `+0x1c` | read ×126 (`0x10`–`0x11`) | read ×126 (`0x4`–`0x5`) | **DMA pointer** (period-index units `[HYP]`) |

Other data-plane offsets: `0x108=0x10`, `0x10c=0x1e70700`, `0x110=0x7`→`0x0` (IRQ config/arm);
`0x800`–`0x8a4` read ×126 = a **meter/level readback** block (`0x814`/`0x88c` carry varying level
values, rest 0). `0x1c` reads at `0x1c`/`0x202910` = a status word.

**Stream-start write sequence `[TRACE]`** (verbatim order in the capture):
```
0x108 = 0x10                 # data-plane IRQ config
0x20c = 0x1                  # ring-0 control (written BEFORE base/size)
0x204 = 0x1c   (28 ch)       # ring-0 channels
0x208 = 0x1c0                # ring-0 size/period
0x214 = 0x2 ; 0x210 = 0x589b9000   # ring-0 base hi then lo (low write last = latch?)
0x304 = 0x1c ; 0x308 = 0x1c0 ; 0x314 = 0x2 ; 0x310 = 0x589bd000   # ring-1 setup
0x10c = 0x1e70700 ; 0x110 = 0x7 ; 0x110 = 0x0      # IRQ arm
```
The base pairs are the exact `0x410`/`0x414` GET-response DMA pattern at new offsets; both bases are
guest-physical (VM RAM), 16 KB apart (`0x589b9000` → `0x589bd000`).

**The DMA engine is DESCRIPTOR-based `[TRACE — engine-start probe]`.** The active probe (driver
`stream_probe=1`) replayed this exact sequence with a zeroed driver ring and the device immediately
faulted: **`AMD-Vi IO_PAGE_FAULT address=0x0`** — it dereferenced a *null pointer read from inside the
ring*, not our base. So `+0x10/+0x14` is a **descriptor-table** base, each entry carrying the actual
buffer address; a zeroed table = null buffer pointers. The format is now decoded from live RAM — §3c.

**Open questions this leaves (the remaining phases):**
- **The DMA pointer units** (`+0x18`, saw `0x4`–`0x11`) — descriptor index? (cadence + dump).
- Whether `0x20c` is per-ring or a global enable (only one of the two was written).
- `+0x08 = 0x1c0` (448) units still TBD — it is **not** the table size (the table is 8-byte entries,
  hundreds of them, §3c); 448 = 4 frames (4 × `0x70`), so possibly a per-fragment byte length.

## 3c. Descriptor table + sample format — RECOVERED from live guest-RAM dumps `[TRACE]`

Dumped both tables and their target buffers from the live VM via QMP `pmemsave` while FC streamed a
stereo signal (recipe in §5). **Key gotcha:** `+0x14`/`+0x14` (`0x214`/`0x314`) is the address **high32**
(`= 0x2`, i.e. the DMA arena sits at guest-physical `0x2_xxxxxxxx`), **not** a control/enable field as
`bar_profile` first guessed — dumping with `high=0` reads a blank page.

**Descriptor table** (at `0x210/0x214`, `0x310/0x314`):
- An array of **8-byte little-endian guest-physical addresses** — one bare buffer pointer per entry,
  no length/flags word. **Zero-terminated** (first all-zero entry ends the list).
- **TX/playback (`0x200`) ≈ 756 entries; RX/capture (`0x300`) ≈ 2048 entries** — capture runs a deeper
  scatter list; the count is a *buffer-depth* difference, not a channel-count one. Past the terminator
  a second structure begins (small per-entry words — likely lengths/control; not yet decoded).
- Entries point into a contiguous-ish DMA arena (TX ≈ `0x2_77–79xxxxxx`, RX ≈ `0x2_3fcaxxxx`), each
  pointer to a small fragment (consecutive entries step `0x100`–`0x700`).

**Sample / frame format** (the buffers the descriptors point to):
- **28-channel interleaved frames, frame stride `0x70` = 112 bytes = 28 × 4** — matches the `+0x04 = 28`
  channel-count register exactly.
- Each sample is **32-bit little-endian carrying a 24-bit value left-justified (MSB-aligned)** — every
  sample's low byte is `0x00`, i.e. `sample24 << 8`. ALSA equivalent: `S32_LE` (24 significant bits).

**Direction `[TRACE]`** — settled by *which* channels were live:
- **`0x200` block (vec1) = playback / TX:** only ch0/ch1 non-zero, carrying the stereo signal we played.
- **`0x300` block (vec2) = capture / RX:** the 8 analog-input channels all sat at a low noise floor
  (nothing plugged in) with two further channels carrying signal — the converters' live input, not a
  played buffer.

This is enough to build valid tables in the driver: allocate a mapped DMA buffer, lay out 28×`S32_LE`
interleaved frames, and fill the descriptor table with the fragment bus addresses (a single contiguous
buffer with entries at the fragment stride satisfies the engine — it only needs valid DMA targets).

## 4. Phased capture plan

**Phase 1 — Streaming setup (BAR registers). ✅ LARGELY DONE — see §3b.**
The register map (ring blocks `0x200`/`0x300`, base/size/control/pointer, IRQ config) and the
stream-start write sequence are already recovered from `clarett_full_init_mute.log`. Direction
(`0x200`=TX, `0x300`=RX) is now also settled — not by playback-only/record-only captures but by
reading *which channels were live* in the RAM dumps (§3c). This phase is in hand.

**Phase 2 — Buffer layout (guest RAM). ✅ DONE — see §3c.**
Live QMP `pmemsave` of both descriptor tables and their target buffers gave the descriptor format
(8-byte LE GPA, zero-terminated) and the sample layout (28-ch interleaved `S32_LE` 24-bit MSB-justified,
`0x70` frame stride), and labelled direction (`0x200`=TX, `0x300`=RX) from which channels were live.

**Phase 3 — Period IRQ correlation. `[PLAN]`**
Stream with `fcp_decode.py --async`; `vec1`/`vec2` (cause `0x200`/`0x300`) should fire at the period
rate. At a known rate+period the cadence is predictable (48000 / 256 ≈ 187 Hz), confirming the period
size and which vector is playback vs capture (run playback-only vs record-only to separate them).

**Phase 4 — Synthesize + implement. `[PLAN]`**
Combine into the streaming model, then build ALSA PCM: `snd_pcm_ops` (open/hw_params/prepare/trigger/
pointer), a DMA-coherent ring, register programming, and a period-IRQ handler calling
`snd_pcm_period_elapsed`.

## 5. Tooling

- **`tools/bar_profile.py`** (built): buckets every region0 access by offset and flags those outside
  the known control-plane map (§8 of the transport spec) — the "what lit up during streaming" diff.
  Classifies each offset's behaviour (write-once / addr-like / varying-read) to hint ring-base vs
  size vs pointer. `--new-only` shows just the data-plane candidates.
- **`fcp_decode.py --async`** (built): reused as-is for Phase 3 (period-IRQ vector + cadence).
- **Guest-RAM dump recipe** (to document once Phase 1 gives a ring base): QEMU monitor
  `pmemsave <gpa> <len> ring.bin`, then a tiny frame decoder to render it as N-channel frames.

## 6. Recommendations

- **Test signal:** play a **24-bit linear ramp** (or per-channel DC = channel index) instead of music
  — it makes sample width, stride, and channel order self-evident in the RAM dump.
- **Config:** 48 kHz, a mid buffer (256–512 frames) for a clean, predictable IRQ cadence.
- **Order:** **playback-only first**, then capture-only, then duplex — isolates the two ring/vector
  sets and keeps each capture simple.

## 7. Risks & unknowns

- **`x-no-mmap` may glitch streaming** (a trapped BAR is slow → underruns). Mitigation: we mainly need
  the *start-of-stream setup* and the *first few period IRQs*, not sustained clean audio — even a
  brief stream before underrun yields the register map.
- **Stream control may straddle mailbox + registers** — capture FCP and BAR together.
- **DMA is snapshot-only** from the host: no flow visibility, only RAM dumps at points in time.

## 8. Active bare-metal alternative `[PLAN]`

Our working control-plane driver already loads on real hardware. Once Phase 1 yields a candidate
register layout, an **active probe** is often faster than more passive tracing for a DMA engine:
program a guessed ring base/size, set the enable bit, and watch for (a) the buffer filling
(capture) or being consumed (playback) and (b) period IRQs on vec1/vec2 — reading the buffer back
**directly**, no `pmemsave` needed. Riskier (a bad address faults the IOMMU — we already saw an
AMD-Vi `IO_PAGE_FAULT` from one), so advance one register at a time.

## 9. Immediate next steps `[PLAN — current]`

Because §3b already gives the register map *and* `clarett_arm_device` already arms a fresh device, the
fastest path now is an **active probe from our own driver**, not more passive tracing. Sequenced:

1. **Engine-start probe (highest value).** After `clarett_arm_device`, in the driver: allocate a
   DMA-coherent ring buffer, program the §3b stream-start sequence with *our* ring's bus address
   (`0x204=28`, `0x208=0x1c0`, base `0x210/0x214`, then the `0x300` block, then `0x108`/`0x10c`/`0x110`
   arm, then `0x20c=1`), and observe:
   - **vec1/vec2 in `/proc/interrupts`** — do they start counting? (engine running = period IRQs)
   - **the DMA pointer regs `0x218`/`0x318`** — do they advance? (DMA is live)
   - **the front-panel Mute LED** — *does the monitor section now manifest?*
   **DONE (`stream_probe=1`):** the engine did **not** start — `IO_PAGE_FAULT address=0x0`, vec1/vec2=0,
   pointers flat. Result: the ring base is a **descriptor table** (§3b), not a flat buffer; a zeroed
   table faults on the first null descriptor. So before the engine can run we need the descriptor format.
2. **Descriptor format (Phase 2 via RAM dump). DONE — see §3c.** Live QMP `pmemsave` of both tables and
   their target buffers gave the full format: 8-byte little-endian GPA per descriptor (zero-terminated;
   ~756 TX / ~2048 RX entries), buffers = 28-channel interleaved `S32_LE` (24-bit MSB-justified) frames
   at `0x70` stride, and the direction labels (`0x200`=TX, `0x300`=RX). The earlier "16-byte × 28
   descriptors" read was wrong (that was `0x1c0` = `+0x08`, a different field).
3. **Re-run the engine probe with a valid table. DONE — the engine RUNS.** With per-ring descriptor
   tables built (8-byte LE GPA entries over a coherent buffer) and the period IRQs left **armed**, the
   DMA pointers `0x218/0x318` advance (`ptr0=0x11`, `ptr1=0x4`). Two corrections were needed:
   - **`0x110=0x0` is stream-stop, not a pulse.** The capture writes `0x110=0x7` at start and `0x110=0x0`
     **13 s later** (line 9710 vs 31657); issuing the `0x0` immediately disarmed the period IRQs and the
     engine never clocked. Arm with `0x7` only; the `0x0` belongs at teardown (`clarett_engine_stop`).
   - **Base-before-enable** (write `0x204/0x208/0x214/0x210` + the `0x300` block, *then* `0x20c=1`) —
     though on the running engine the base latched either way (readback confirms `blk0 base=…:fffc0000`,
     `tx_desc[0]=…fffc0840`).
   - **Playback (block 0) reads correctly** — `ptr0` advances with **no read faults**: the device walks
     our TX descriptor table and reads the fragments.
   - **Capture (block 1) writes to a NULL base** — the remaining bug. The faults are now device *writes*
     (`flags=0x20`) marching `address=0x0, 0x80, 0x100, 0x180 …` (0x80 stride) from base 0, and our RX
     fragments stay zero. So the device gets its capture/position write target from somewhere we leave
     zeroed — most likely the **ring control block** that followed the descriptor zero-terminator in the
     VM dumps (the `1a3c3700…` / `80006005…` words, §3c), i.e. a position/status writeback pointer. We
     don't replicate it, so the device writes to null (benign in the VM where address 0 is mapped).
4. **Recover the ring control block (next blocker).** Dump the VM region *after* each descriptor
   terminator (`pmemsave <table_base + table_len> …`) and decode the control structure — find the
   write-target / position-writeback pointer, build it in the driver, and the capture writes should land
   in our RX fragments (`capture-buf=GOT DATA`).
5. **Direction + IRQ cadence; then ALSA PCM (Phase 4).** Once both directions DMA into our buffers,
   wire `snd_pcm_ops` (open/hw_params/prepare/trigger/pointer) over the descriptor ring with the
   vec1/vec2 handler calling `snd_pcm_period_elapsed`.

Prerequisite still open: **clock-source / sample-rate** (control-plane §7) must be set before streaming
and is still untraced — but note the engine streamed in `clarett_full_init_mute.log` after only the §3b
*register* writes (no new mailbox clock command was needed beyond the bring-up we already replay), so
clocking may already be covered by `clarett_arm_device`. Confirm during step 1; if the engine won't
start, capture an FC sample-rate change to find the clock command.
