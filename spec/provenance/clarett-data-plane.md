# Clarett — Data-Plane RE (PCM DMA streaming)

> **Scope:** the Clarett Thunderbolt line — engine and per-direction geometry RE'd on the 8PreX, 2Pre,
> and 4Pre (the 8PreX is the reference model); per-model geometry is called out inline.

**Status (June 28 2026):** far past a "plan". Phase 1 (streaming register map) **recovered** from
`8prex_full_init_mute.log` (§3b); Phase 2 (descriptor format + sample layout) **recovered** from live
guest-RAM dumps (§3c); the engine is **implemented and validated** — arms cleanly, DMAs a burst,
descriptors valid (no IOMMU faults), the DMA PTR advances. **But it will not sustain past one ring pass**:
it flags "period 0 ready" and the `0x300` counter never advances, universal across both models. Every
observable host→device surface (BAR0 setup regs, FCP handshake, PCI config, `0x500`/`0x510` enables,
converter-ready status, **and** the DMA-buffer address via `dma_bits=30/31`) has been matched/eliminated.
The black-box MMIO+FCP+config method is **exhausted**; the sustain blocker is below the BAR surface — the
**same off-wire wall the control plane hits** (`spec/provenance/clarett-manifestation-wall.md`, memo
`clarett-dataplane-pcm-findings`). **Environment is ruled out:** the control plane was retested with our
driver in a Fedora guest under the *same* vfio passthrough FC uses and still failed, so it is our-driver-
vs-FC (off-wire bus-master DMA), not host/IOMMU/VM context. (Earlier data-plane notes called this
"environmental" — superseded; both planes share the off-wire/driver-vs-FC root.) Going further needs
bus-level RE (a TB/PCIe protocol analyzer on the live link), **not** host-env work and **not** driver edits. Tags: `[PLAN]` = intended approach; `[HYP]` = hypothesis to confirm; `[TRACE]` = confirmed from a
capture; `[ANCHOR]` = a control-plane fact we build on.

The control plane (mixer/routing/gain/mute/clock/notifications) is reverse-engineered and documented in
`clarett-control-plane.md` + `clarett-fcp-transport.md`. **Both planes are now blocked at the
same below-BAR wall** — neither functions despite traffic byte-identical to FC.

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

**Phase 1 is largely already done.** `8prex_full_init_mute.log` was captured with Focusrite Control
**live**, so its ASIO engine was streaming — the trace contains the full streaming-setup register
activity (`tools/bar_profile.py 8prex_full_init_mute.log --new-only`). Two structurally identical
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
`0x800`–`0x8a4` read ×126 = ~~a **meter/level readback** block (`0x814`/`0x88c` carry varying level
values, rest 0)~~ **two ten-word blocks, `0x800`-`0x824` and `0x880`-`0x8a4`, and NOT a level readback
(Nov 17 2026):** the vendor reads them only while streaming, at 10 Hz, after the ring pointers; one word
per block is live (2Pre/4Pre/8Pre `0x810`/`0x888`, 8PreX `0x814`/`0x88c`, Red 8Line `0x814`/`0x890`)
and it sits at `0x12c`-`0x139` (300-313) on every model whatever the audio, which a level would not.
A timing or fill quantity is the guess (the two blocks agree within one or two; the live slot moves up
on wider models); the 8Pre also shows small counts (1, 2, 4, 7) in `0x814`. Not decoded. `0x1c` reads at `0x1c`/`0x202910` = a status word.

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

**Third-model confirmation — Clarett 4Pre `[TRACE]`** (`4pre_boot_to_stream_end.log`, FC streaming):
the same two ring blocks appear with the 4Pre's asymmetric geometry, and they independently confirm
two things — that `+0x04` is the **per-direction channel count**, and the **universal frag invariant**
(`stream_frag = channels × 16 B`, i.e. 4 B/sample × 4 frames/descriptor):

| Reg | block 0x200 (TX/playback) | block 0x300 (RX/record) |
|---|---|---|
| `+0x04` channel count | `0x08` (8) — matches `GET_7.2=0x08` | `0x14` (20) — matches `GET_7.3=0x14` |
| `+0x08` frag | `0x80` (128 = 8×16 ✓) | `0x140` (320 = 20×16 ✓) |
| `+0x10`/`+0x14` base | `0x79998000` / `0x2` | `0x799ac000` / `0x2` (0x14000 = 80 KB apart) |
| `+0x14` mode | `0x2` | `0x2` |

`0x108=0x10`, `0x10c=0x1e70700`, `0x110=0x7`→`0x0` are byte-identical to the 8PreX arm sequence. So the
4Pre is the **third model** validating the invariant (8PreX 28→`0x1c0`, 2Pre TX 4→`0x40`/RX 14→`0xe0`,
4Pre TX 8→`0x80`/RX 20→`0x140`) and, like the 2Pre, is **asymmetric** (needs per-direction frag derived
from channel count, not a single `clarett_model.stream_frag`). The `0x300` cause was observed advancing
`0x8000000c → …18 → …24` (step `0xc`) — FC sustaining the stream, the behaviour our driver cannot hold
past one ring pass; this is FC's traffic, so it confirms the engine map but not why ours stalls (that
difference is off-wire, §the manifestation/below-BAR wall).

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
stream-start write sequence are already recovered from `8prex_full_init_mute.log`. Direction
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
   - **The `0x80`-strided write burst to base 0 is block 0's (playback), not block 1's** — see step 4's
     RESOLVED block. It is a finite, non-fatal device-default startup writeback, benign in the VM,
     faulting on bare metal. Capture (block 1) **does** write our RX buffer (marker test, step 4).
4. **Capture-to-null: hypotheses RULED OUT, root cause still open.** Tested against a freshly
   power-cycled device:
   - **Not a ring control block.** Dumping the VM region after the RX descriptor terminator showed
     unrelated Windows NonPaged **pool** memory (a NIC driver's strings + pool tags `NDst`/`Iptt`/`ExTm`/
     `VIsr`, kernel VAs `0xffffaf01…`), not a control structure. The `80006005…` words were just
     adjacent pool. There is **no writeback pointer** to recover.
   - **Not the descriptor count.** Filling all entries with a full-depth (2048) all-valid cycling ring
     (no early zero terminator) changed nothing — identical burst to base 0.
   - **Not table-base alignment.** Cleanly isolated (256-desc baseline, only the page-align added):
     block 1's base became `…fffdd000` (aligned, `rx_desc[0]=…fffdd840` valid) and capture *still* burst
     to base 0 — now a 64k-fault storm because the aligned base makes the RX engine engage harder. So
     the VM's page-aligned bases are incidental, not the fix.
   - **No 4th DMA-address register and no streaming FCP.** The entire capture has exactly three
     address-holding registers (`0x210` TX table, `0x310` RX table, `0x410` GET-response) and the only
     FCP during streaming is meter polls + a mid-stream control toggle.
   - **Not a capture-specific register step.** `bar_profile` of a **playback-only** vs a **record-only**
     stream are *register-identical* — both always program **both** ring blocks (`0x200` and `0x300`)
     with the same sequence and the same bases (`0x..a000` / `0x..7000`, also identical across the two
     runs → FC pins fixed DMA buffers). The device always arms full-duplex; there is no extra enable
     (`0x30c` is never written) or FCP arm for capture. (This also retires the Phase-1 single-direction
     captures — done.)
   **BREAKTHROUGH — capture works; the storm is block 0's, not block 1's.** The block-1-only probe
   (`blk1_only=1`: configure only `0x300`, leave block 0's base null, enable globally via `0x20c`)
   plus a **0xAA pre-fill marker** on the RX sample area settled it. Result: `ptr1` advanced to `0x4`
   and `capture-buf=WRITTEN (marker gone)` — **the device overwrote our marker with samples**. Block 1
   captures correctly into the table we program; it only *looked* broken before because, with nothing
   plugged in, it writes (near-silent) zeros over an already-zeroed buffer, which the old "any non-zero
   byte" scan read as "didn't write." Capture is effectively solved.
   The remaining `IO_PAGE_FAULT address=0x0` is **block 0's**, and it is *not* sample DMA. In the
   block-1-only run, block 0's base was deliberately left null (`blk0 base=00000000:00000000`) yet block
   0 still armed (global `0x20c`=1 sets its `ctrl`) and emitted a single write to 0 — i.e. **playback
   tries to write to a buffer we never configured**. The `0x80`-strided monotonic burst is a
   **playback-side DMA position/status writeback** (the engine reporting its TX consumer position),
   whose base is set by neither of the three known DMA registers (`0x210`/`0x310`/`0x410`) nor by any
   streaming FCP.

   **RESOLVED — the writeback base is a hardwired default of 0, not a config we skip.** The hunt is
   exhausted on every front and the conclusion is now firm:
   - **Live vendor TX descriptor table recovered** (RAM dump of the Windows ring, base `0x2_794a9000`,
     captured by siphoning the per-second-purged QEMU log through `tail -F` to beat the truncation):
     **347 entries, each a bare 8-byte LE `0x2_xxxxxxxx` fragment address**, zero-terminated. The
     **only** flagged entry is the **last** (`0x2_77df7001`, bit 0 set); all others are `0x100`-aligned.
     Bit 0 is an **end-of-list / ring-wrap marker**. There is **no header before, and no writeback
     pointer anywhere in or after** the table — past the terminator is reclaimed pool (NTFS `INDX`
     buffers, `.dll` name fragments).
   - **The 8 KB config carries no address.** It is a **write-only push** (9 `SET_DATA` = 8264 bytes;
     the largest `GET_DATA` is 8 bytes — count queries only, no 8 KB read), so nothing is hidden in a
     DMA'd read response. Reassembling all 8192 bytes (device cfg offsets `0xc8..0x20c8`) and scanning
     every 4-aligned 8-byte window for a `0x2_xxxxxxxx`/page-aligned pointer: **zero hits**.
   - **Windows' full data-plane register write set == ours, exactly** (`0x108=0x10`, `0x20c=0x1`,
     `0x204/0x208/0x214/0x210`, `0x304/0x308/0x314/0x310`, `0x10c=0x1e70700`, `0x110=0x7`). No missing
     register, no value/order difference that matters.
   - **Adding the end-of-list flag to our last descriptor did NOT stop the storm** — identical burst —
     so the writeback is not anchored by table format either.

   So the `0x80`-strided burst to base 0 is a **device-default startup writeback** the device does
   regardless: it succeeds invisibly in the VM (all guest RAM is IOMMU-identity-mapped to the device),
   and faults harmlessly on bare metal (only our coherent buffers are mapped). Confirming it's benign:
   the burst is **finite** (~10 faults, `0x0..0x480`, then it stops — a one-shot, not per-period), it is
   **non-fatal** (AMD-Vi logs and drops each write; the engine advances *through* it), and block 0 ends
   in **status `0x11` — the exact value Windows' `0x218` reads** (`R[0x11(x104)]`, constant → `0x218` is
   a status word, not a running position). Our playback engine reaches the **identical steady state as
   the vendor's.** Nothing left to configure here.

   **The real remaining blocker is the clock, not the writeback.** No period IRQs fire (`vec1=0
   vec2=0`) and neither pointer climbs at audio rate — the engine reaches the armed state but never
   streams. That points at the still-untraced **clock-source / sample-rate engagement** (control-plane
   §7), the most likely reason monitor `Mute`/`Dim` writes complete but the front-panel LED never moves.
   **Clock command FOUND and wired — but the engine still won't clock.** `SET_CLOCK = opcode 0x006003`,
   payload `{u32 sample_rate, u32 clock_source}` (e.g. `{48000, 24=Internal}`), captured by
   baseline-vs-rate-change diff (control-plane §7, TRACE-CONFIRMED). Our bring-up sends `0x6000/1/2/4/5`
   but never `0x6003`. `clarett_engine_start` now sends `SET_CLOCK` before arming and `DATA_CMD{activate=5}`
   (the rate-change capture's final mailbox command, the only `DATA_CMD` in it) as the stream-start commit.
   Both are **device-accepted** (`done=1 fcperr=0`). Result on bare metal: **still `vec1=0 vec2=0`, both
   pointers frozen at their startup-burst values** (`ptr0≈0x12`, `ptr1=0x4`). The engine takes a ~4-desc
   prefill and stops.
   - **NOT the IOMMU wedge.** `blk1_only=1` (block 0 absent → only a single benign read fault at 0, no
     write storm) with `SET_CLOCK`+commit: block 1 *still* stalls at `ptr1=0x4`, `vec2=0`. A clean engine
     with clock+commit does not stream, so the block-0 writeback fault was never the cause.
   - **NOT a missing register/mailbox trigger.** A full-write siphon of an FC stream-start shows the arm
     is exactly our 12 register writes with **nothing after `0x110=0x7`**; `activate=5` is the only
     `DATA_CMD`. Everything observable, we replay.

   **RESOLVED via a full streaming capture — the streaming signal is `0x300`, and the engine needs
   continuous host servicing, not just an arm.** A 122 k-line full read+write siphon of a *successful*
   FC playback session settled what every prior fragment couldn't:
   - **`0x218`/`0x318` are static status words, not position counters.** Throughout active VM playback
     they read a constant `0x12` / `0x4` — **the exact values our bare-metal engine reaches.** Our
     "frozen pointers" were never a stall; the VM's don't move either. We were reading the wrong signal.
   - **The streaming signal is the block cause register `0x300` (vec2).** During playback it returns
     `0x80000000 | period_counter`, the low counter **stepping by `0xc` per period** (`0x8000000c,
     0x80000018, 0x80000024 … 0x800000f0`, wrapping). `0x200` (vec1/TX) stays `0`. Windows **POLLS** the
     whole cause block `0x100/0x200/0x300/0x400/0x500` ≈400×/s (≈18 k reads each over ~45 s) — it does
     **not** use MSI for periods. (Our probe now polls `0x300` directly; see `clarett_stream_report`.)
   - **The ONLY register writes in the entire streaming capture are our exact 12 arm writes.** No
     `0x500`-block write, no extra IRQ/stream enable, nothing. Plus `SET_CLOCK` + `activate=5` (mailbox).
     We replicate every observable BAR0 write.
   - **Every remaining MMIO lever, ruled out on bare metal** (each a fresh power-cycle, with the correct
     `0x300` poll detector): `SET_CLOCK` accepted both in `engine_start` *and* injected **before
     `CONFIG_PUSH`** (clock-first ordering) → `0x300:hits=0` either way; `DATA_CMD{activate=5}` accepted →
     no change; `blk1_only` clean engine → no periods. The clock command lands (`done=0`), the engine
     reaches VM-identical armed status, **DMA works** (block 1 wrote our buffer), yet `0x300` never ticks.

   **Conclusion: this is not missing RE — it is the missing streaming RUNTIME.** The "~4 descriptors then
   stop" is a **flow-controlled DMA engine**: it fills a small buffer, raises a period on `0x300`, and
   waits for the host to acknowledge (the continuous ~400 Hz cause poll + doorbells + position-writeback
   consumption Windows does *the whole time audio plays*). A one-shot arm-and-check probe structurally
   cannot demonstrate streaming — nothing services the engine, so it stalls within milliseconds; our
   `@1s` 2000-read burst is a full second too late to recover it. We now have everything needed to build
   the runtime: descriptor format, sample layout, directions, the 12-register arm, `SET_CLOCK`, the
   commit, and **the period signal (`0x300`, polled, +`0xc`/period)**.
5. **ALSA PCM (Phase 4) — the actual remaining work.** Build a **continuous servicing path**, not a
   probe: from arm, poll `0x300` at ~period rate (mirroring FC's ~400 Hz), ack each period (read-to-clear)
   + ring the doorbell as FC does, and on each period advance the ring and call `snd_pcm_period_elapsed`.
   If the engine keeps clocking under live servicing, wire it straight into `snd_pcm_ops`
   (open/hw_params/prepare/trigger/pointer). Capture (block 1) is the proven-working first target.
   - The playback writeback-to-0 stays a **benign bare-metal artifact** (floods dmesg only); revisit at
     the IOMMU level (reserve/map a low buffer) only if it proves functionally limiting.

   **IMPLEMENTED — capture PCM (`enable_pcm=1`); clocks on hardware, stalls after one ring pass.**
   `driver/clarett_pcm.c` registers a 28-channel `S32_LE` @48 kHz **capture** device on ring block 1,
   driven by the persistent `0x300` servicer. Hardware-tested this session — what was learned:
   - The engine arm is factored into `clarett_engine_arm(c, r0, r1)` / `clarett_engine_run()` /
     `clarett_engine_stop()` (shared with `stream_probe`). The contiguous hardware buffer holds BOTH
     rings (block 0 = silent dummy TX, block 1 = capture); captured frames are `memcpy`'d into the
     ALSA-managed buffer each period. Split allocations (separate table / ALSA buffer / TX ring) do
     **NOT** clock — the rings must be one contiguous buffer with `r1 = r0 + ring`, like the probe.
   - **FULL-DUPLEX arming is required.** Block-1-only (`r0=0`) raises no periods AND hangs the
     `activate=5` commit. We arm block 0 over a silent dummy TX ring purely to satisfy the engine.
   - **The `0xAA` RX pre-fill is functionally required (KEY FINDING).** `memset`-ing the RX sample area
     before arming is the lone difference between the proven `engine_start` (clocks) and the original
     `create_pcm` arm (did not). With it, `create_pcm` clocks an identical 248-period burst (`ctr=0x1b3`).
     [WHY unknown: likely forces coherent-buffer/descriptor write visibility to the device, not the
     content — `dma_wmb()` may be the real fix; confirm later.]
   - **Context split:** mailbox arming (`SET_CLOCK` + `DATA_CMD{5}`) runs in `prepare()` and the servicer
     ACKs `0x300` from there (the engine bursts and stalls within ms if unserviced from arm); the atomic
     `trigger` only flips `pcm_running` to gate `snd_pcm_period_elapsed` delivery. `hw_free`/`close` tear
     the engine down before the managed buffer is freed.
   - **OPEN CALIBRATION:** `CLARETT_DESCS_PER_TICK` (frames per `0x300` tick) is a hypothesis (1 tick ≈
     1 descriptor). Counter math is unresolved: `periods=248` reads vs `ctr=0x1b3` (435) ⇒ bit31 likely
     stays set across ~7 reads per real period, so the read-count over-counts. Calibrate on hardware.
   - **THE WALL — one-ring-pass stall (`enable_pcm` not yet functional).** The engine streams exactly one
     ring pass (~248 periods, `ctr=0x1b3`) then **cleanly stalls** — device fully alive (`caps`/`info`
     read sane, `0x300`=`0`, not `0xffffffff`; the earlier "dead `0x7fffffff`" was a one-off). Every
     driver-side revival lever was tried and FAILED: rewrite `0x110` (rekick=1), rewrite ring bases
     (rekick=2), full cause-block poll `0x100..0x500` mimicking FC (`pollall`), re-issue `activate=5`
     commit (rekick=4). Our descriptor table already matches the VM's structure exactly, so the wrap
     mechanism is a runtime behaviour we have **not yet observed** — next is a fresh VM capture of a long
     *capture* session hunting for steady-state writes the 122k-line siphon may have missed (a per-buffer
     re-arm/doorbell). Debug levers live behind module params `rekick`/`rekick_ms`/`pollall`/`pcm_selftest`.

   - **LONG STREAMING CAPTURE DONE (2Pre, `2pre_stream.log`, ~35 s full-duplex, June 26 2026 —
     `tools/stream_profile.py`).** The "next step" above is now executed and resolves the open question:
     - **There are NO steady-state writes. Period servicing is READ-ONLY.** Over ~35 s the engine raised
       **7836 period IRQs** (the `0x300` counter wrapping ~390×) with **only 6 arm sequences total** (a
       warmup burst + occasional stream-restart re-arms). Between arms the host writes **nothing** — no
       per-buffer re-arm, no doorbell, no base rewrite. **This RETIRES the per-buffer-write hypothesis:**
       a free-running engine wraps the ring autonomously; reading the cause registers (read-to-clear) is
       the entire ack. The wall is therefore NOT a missing write.
     - **The actual per-period read set Windows performs (NEW — never replicated by our servicer):**
       `0x100` (vec0 cause, `=0x80000000`) → `0x300` (blk1 cause, `=0x80000000|ctr`) → `0x200` (blk0
       cause, `=0`) → **`0x21c`,`0x218` (blk0 status/PTR)** → **`0x31c`,`0x318` (blk1 status/PTR)**, and it
       does **NOT** read `0x400`/`0x500` per period. Our servicer reads `0x200/0x300/0x500` (and `0x100`
       only under `pollall`) but **never reads the four status/PTR words `0x218/0x21c/0x318/0x31c`.** That
       `pollall` already covered `0x100/0x400` and failed → the untested lead is the **per-period
       status-word reads**; the engine's consumer-advance handshake likely requires reading `0x318` (RX
       position) each period to release the next ring segment. ACTION: mirror this exact read set in
       `clarett_stream_service` and re-test on a fresh device.
     - **STATUS-WORD HYPOTHESIS FALSIFIED (hardware-tested, June 26 2026).** Added the four per-period
       status reads (`0x218/0x21c/0x318/0x31c`) to `clarett_stream_service`, fresh 8PreX, `enable_pcm=1`,
       `arecord -c28`: **same wall** — `ctr=0x1b3, wraps=0`; the engine reached the identical one-ring-pass
       stall (`periods` merely dropped 248→24 because the extra reads slowed the poll loop). Reverted.
       The engine halts at the SAME counter (`0x1b3`) regardless of servicing — so the stall point is
       intrinsic, not service-cadence-driven.
     - **FULL WRITE CENSUS of the working stream (decisive):** the ONLY region0 writes in all 35 s are the
       12–13 stream-engine regs we already replicate (`0x108/0x10c/0x110`, `0x20c`, `0x204/8/0x214/0x210`,
       `0x304/8/0x314/0x310`) + `0x100` ack. **No hidden register, no descriptor-count register, no
       per-period write.** This RULES THE ENTIRE BAR/MMIO DOMAIN OUT — we replicate every write and every
       meaningful read. The wall is in the **descriptor-table / RAM domain**, invisible to MMIO traces.
     - **Concrete RAM-domain discrepancy found:** the recovered vendor TX table is **`0x100`-aligned per
       entry** (step-4 RAM dump: "every entry 0x100-aligned except the last"), but our driver packs
       fragments at `base + i*stream_frag` (frag `0x1c0`/`0x40`/`0xe0`, none a multiple of `0x100`) → our
       entries are `0x40/0x80/0xc0`-aligned, NOT `0x100`-aligned. The vendor evidently allocates each
       fragment in its own `0x100`-aligned slot (frag rounded up to a `0x100` boundary, with padding),
       which our tight packing does not match. Also: 8PreX counter **freezes** at `0x1b3` (no wrap), vs the
       2Pre VM counter cleanly **wrapping** `0xf0→0` ~390× — our engine halts at end-of-ring instead of
       wrapping. **DEFINITIVE NEXT STEP** (how the format was first recovered): QMP `pmemsave` the live
       2Pre tables during streaming (GPAs TX `0x2_680f7000` / RX `0x2_680fb000`) and read off the real
       entry count, per-entry stride/alignment, and wrap-flag placement — then rebuild our table to match
       (likely `0x100`-aligned slots + correct depth) rather than guessing through hardware cycles.
     - **RAM DUMP DONE — BOMBSHELL: the 2Pre uses FLAT sample buffers, NOT a descriptor table (June 26
       2026).** `pmemsave` of both bases during a live 2Pre stream (`/tmp/{tx,rx}_tbl.bin`, 12 KB each):
       **100 % of 32-bit words are 16-bit audio** (low 16 bits all zero = MSB-justified samples; real ±50 %-FS
       values; TX≠RX content), and **ZERO 8-byte `0x2_xxxxxxxx` GPA descriptor entries.** So for the 2Pre
       the engine treats `0x210`/`0x310` as **flat contiguous PCM ring buffers** — RX *writes* captured
       audio there, TX *reads* playback audio from there — with **no descriptor-table indirection at all.**
       Our driver builds an 8-byte-GPA descriptor table and points the engine at THAT; if the device wants
       flat samples at `0x210`, our table bytes are misinterpreted and the wrap goes wrong → **the leading
       root cause of the one-ring-pass wall.** Our DMA buffer is already `dma_alloc_coherent` (physically
       contiguous), so the flat scheme is directly usable.
       - **Reconciliation with §9-step-4's "347-entry 8PreX descriptor table":** that dump genuinely looked
         like addresses (all high32==2, zero-terminated, one bit-0 flag — audio has none of those traits),
         so this is most likely a **Windows-allocation difference**, not a misread: the 28-ch 8PreX buffer
         is large/fragmented → Windows programs scatter-gather (descriptor list); the smaller 2Pre buffer
         is contiguous → Windows programs a flat base. The device evidently supports BOTH at `0x210`. For
         an in-kernel driver with a contiguous coherent buffer, **flat is the right (and simpler) path.**
       - **NEXT (disambiguate cheaply, then implement):** (1) re-`pmemsave` the **8PreX** `0x210` target
         during a live 8PreX stream — confirm table vs flat (resolves whether it's a true mode difference).
         (2) Add a flat-buffer engine path: point `0x210`/`0x310` straight at the contiguous sample area
         (no table), determine the buffer-length / wrap config (candidates: the `0x208`/`0x308` SIZE field;
         the gap to the next block's base — 2Pre TX span = `0x680fb000-0x680f7000` = `0x4000` = 16 KB; or a
         fixed internal size the host buffer must meet/exceed), and re-test the wrap on a fresh device.
     - **8PreX RAM DUMP DONE (`/tmp/{tx,rx}_8prex.bin`) — CONFIRMED per-model mode difference + a real
       descriptor bug (June 26 2026).** Same register `0x210`, same `pmemsave` method: the 8PreX base holds
       a **descriptor table** (low-16-zero only ~5 % — NOT audio), the 2Pre holds **flat audio**. So the
       mode is genuinely per-model (large 28-ch buffer → Windows scatter-gather list; small 2Pre buffer →
       flat contiguous). 8PreX table geometry, both directions: **347 entries** (TX `BASEhi=2`, RX
       `BASEhi=1`), **`0x100`-aligned** 8-byte GPAs, **bit-0 wrap flag on the LAST entry (346)**, dominant
       contiguous **fragment stride `0x700` = 16 frames** (28ch×4B×16), scatter-gathered across physical
       pages; total ring `347 × 0x700 ≈ 607 KB`. **BUG FOUND:** our driver uses `stream_frag = 0x1c0` as the
       descriptor fragment stride, but `0x1c0` is the **`0x208` SIZE/period value (4 frames)**, NOT the
       fragment size — the real descriptor fragment is **`0x700` (16 frames) = 4 × the SIZE field**. We
       conflated the two. So our 8PreX table is built with 4×-too-small fragments AND too few entries
       (256 vs 347) → the likely mechanical cause of the one-ring-pass stall. SIZE/period `= channels×4×4`
       holds for both models (8PreX `0x1c0`, 2Pre TX `0x40`/RX `0xe0`); the descriptor fragment is a
       SEPARATE, larger quantity (`channels×4×16` on the 8PreX).
     - **IMPLEMENTATION PLAN (two paths, both leverage our CONTIGUOUS coherent buffer):**
       - *2Pre — flat buffer (simplest, confirmed-correct):* point `0x210`/`0x310` at the contiguous sample
         area directly, no descriptor table; find the wrap length (counter wrapped `0xf0→0`; TX base→RX base
         gap = 16 KB is a candidate). Most promising route to first working 2Pre audio.
       - *8PreX — fix the descriptor table:* fragment stride `0x700` (16 frames), `0x100`-aligned,
         contiguously tiled over our coherent buffer, ~347 entries (or as many as the buffer holds), bit-0
         flag on the last; keep `0x208=0x1c0`. Re-test the wrap on a fresh device.
     - **8PreX `0x700`-FRAGMENT FIX IMPLEMENTED & FALSIFIED ON HARDWARE (June 26 2026) — REVERTED.** Built a
       `0x700` descriptor stride (`0x100`-aligned, sample area padded to `0x100`) with `0x208` kept at `0x1c0`
       via a new `period_bytes` model field (the period-vs-fragment split is correct: the 8PreX VM does write
       `0x208=0x1c0` AND use a `0x700` table, trace+dump confirmed). **Result: the engine arms cleanly (every
       FCP `done=1 fcperr=0`, alloc OK) but raises ZERO periods (`periods=0 ctr=0`)** — whereas the `0x1c0`
       stride clocks (~248 periods). Controlled re-test holding total buffer size constant (`NDESC=64` so
       `64×0x700 == 256×0x1c0 == 112 KB/ring`, same alloc): **still `periods=0`.** So it is the `0x700`
       descriptor stride ITSELF that stops our engine clocking, NOT buffer size/address/count. **Yet the VM
       streams fine with a `0x700` table** → there is a difference we still can't observe between the VM's
       (scatter-gathered, physically discontiguous, 347-entry) table and our (contiguous, uniform-`0x700`,
       256/64-entry) one. Hypotheses still open: the engine wants descriptors == `0x208` SIZE (`0x1c0`) and
       advances one/period (our `0x1c0` "works" only by writing into a tight ring, and the `0x700` table
       desyncs it); OR it needs the physically-discontiguous layout; OR a count near 347. **Reverted to the
       known-clocking `0x1c0`/256 baseline** (`git checkout` of clarett.h+clarett_main.c) — do NOT re-apply
       the `0x700` change without a new idea for the VM-vs-ours difference. Tools added: `tools/stream_profile.py`,
       `tools/dma_bases.py`. Dumps: `/tmp/{tx,rx}_8prex.bin` (table), `/tmp/{tx,rx}_tbl.bin` (2Pre flat audio).
     - **★ WALL BROKEN on the 2Pre via a FLAT-BUFFER path (June 26 2026) ★.** Implemented `flat_buffer`
       model mode (clarett.h/main/pcm): engine streams a contiguous sample ring directly at `0x210`/`0x310`
       with NO descriptor table; one coherent buffer = TX flat ring (`CLARETT_FLAT_FRAMES`=1024 frames=16 KB,
       == the VM gap) + RX flat ring (56 KB); per-direction period = `channels×16` to `0x208`/`0x308` (TX
       `0x40`/RX `0xe0`); engine arm + `0x300` servicer SHARED with the 8PreX; unified PCM tick (proven
       algebraically identical to the old 8PreX per-descriptor copy → 8PreX path regression-safe). Hardware
       (`model=2pre enable_pcm=1`): **`stream-svc: periods=7182 ctr=0x1c10 wraps=2`** — ~28 ring passes of
       CONTINUOUS streaming, vs the 8PreX's hard one-pass stall (`ctr=0x1b3 wraps=0`). The device wants a
       flat buffer for the 2Pre; descriptor-table mode was the wrong model for it.
       - **arecord `EIO` diagnosed → the "flat" model is INCOMPLETE; the engine reads DESCRIPTORS (June 26).**
         `AMD-Vi IO_PAGE_FAULT` storm at `address=0xaaaaaaaaaaaaaaXX` (stepping `0x80`) + `0x0`: the engine
         DEREFERENCES our buffer contents as DMA pointers. With the `0xAA` prefill it reads `0xaaaa..` as a
         fragment pointer and writes a **16-frame fragment** there (fault decode: 7×`0x80` = `0x380` =
         `14ch*4*16`) → faults, **capture never lands** (arecord gets `0xAA`/garbage, XRUNs at 66 %). The
         `0x0` faults are the same benign TX position-writeback the 8PreX does. KEY CONTROL TEST: **remove
         the prefill (zeroed buffer) → faults vanish BUT `periods=0` (engine won't clock)**. So the engine
         needs a NON-NULL pointer in the buffer to clock, and a VALID one (into our DMA buffer) to capture.
       - **The contradiction / impasse:** (a) the VM `pmemsave` of `0x310` was pure flat audio from byte 0
         (no pointers), yet the live engine reads pointers; (b) giving the **8PreX** VALID 16-frame (`0x700`)
         descriptors gave `periods=0` (didn't clock), while INVALID `0xAA` ones clock. So in OUR setup
         "valid descriptors" and "engine clocks" are mutually exclusive — for BOTH models. The engine's
         **flat-vs-descriptor (and clock-gating) mode is selected by something the VM does that we don't
         replay** — and it's NOT the arm registers (those match the VM byte-for-byte). The 16-frame fragment
         + 4-frame period (`= channels*64` / `channels*16`) structure is now confirmed identical across both
         models. The `0xAA` prefill is kept (engine clocks for debugging); arecord still `EIO` (no real data).
       - **NEXT (RE, not more guessing):** find the mode/clock-gate selector. Candidates: (1) diff our 2Pre
         init replay (`clarett_init_2pre.h`) against the VM's FULL streaming setup for a config push / FCP we
         drop or send differently; (2) a fresh long 2Pre capture correlating the buffer-content writes the VM
         makes BEFORE arming (does Windows seed a descriptor/position word we don't?); (3) check whether the
         8 KB config blob carries a DMA-mode bit. Position-rate calibration is moot until real data captures.
     - **2Pre stream geometry PINNED (the explicit milestone):** block 0 (TX) `0x204=4` chans / `0x208=0x40`
       (64 B) frag; block 1 (RX) `0x304=0xe` (14) chans / `0x308=0xe0` (224 B) frag. **Universal invariant
       across all models: `stream_frag = channels × 4 bytes × 4 frames/descriptor`** (8PreX 28→0x1c0, 2Pre
       TX 4→0x40, RX 14→0xe0). The 2Pre is **asymmetric** → needs per-direction frag (the current single
       `clarett_model.stream_frag` cannot represent it; derive from channel count instead). Arm order:
       `0x108=0x10`, `0x20c=1` (block-0 CTRL = **global** engine enable; `0x30c` block-1 CTRL is **never**
       written — confirmed by grep), block-0 CHANS/SIZE/BASEhi/BASElo, block-1 CHANS/SIZE/BASEhi/BASElo,
       `0x10c=0x1e70700`, `0x110=0x7` then `0x110=0x0` at stop, ack `0x100=0xf`. The `0xc`/period counter
       step is confirmed identical to the 8PreX (model-independent).

Prerequisite still open: **clock-source / sample-rate** (control-plane §7) must be set before streaming
and is still untraced — but note the engine streamed in `8prex_full_init_mute.log` after only the §3b
*register* writes (no new mailbox clock command was needed beyond the bring-up we already replay), so
clocking may already be covered by `clarett_arm_device`. Confirm during step 1; if the engine won't
start, capture an FC sample-rate change to find the clock command.

---

## 10. Armed-session retest + the steady-state sweep find (July 16–17 2026) `[TEST]`/`[TRACE]`

First data-plane test on an ARMED session (wall crossed, manifestation-wall §8; the old "same
below-BAR wall" attribution for the stall is void). Result: **the engine is no longer dead after one
pass, but it does not stream** — and the working captures reveal a per-period host obligation our
servicer never met.

**Our armed 2Pre (`enable_pcm=1`, gated-ack default cycle):**
- Stream handshake all `err=0` (`SET_CLOCK{48000,24}=0 CONFIG_PUSH=18(err=0) 0x6004=0/0 0x6005=0`).
- After arm, `0x300` asserts bit31 **continuously at ~14 Hz with ctr=0x0, indefinitely** (2131
  events / 151 s under a PipeWire-held session; old walled baseline: 2 events then dead forever).
  A ctr=0 heartbeat, not streaming.
- `arecord` (14ch S32_LE @48k) → EIO after 2 events (`periods=2 ctr=0x0`).

**The working stream, measured (`2pre_stream.log`, 31.4 s window, 7836 events):**
- `0x300` period events every **4.00 ms** (250/s); ctr advances **+0xc (12) per event** (7462/7462
  steady-state steps), wrapping at a small modulus (~0xf0) — **373 wraps in 31 s: "wraps" are normal
  modulo behavior, NOT ring passes**. 12 units/4 ms = 192 frames/event at 48k ⇒ **one ctr unit =
  16 frames**; the driver's one-tick-= 4-frames copy model in `clarett_pcm_tick` is wrong.
- **The host writes NOTHING during streaming** — the only writes in the whole window are the
  one-time arm (bases/sizes/ctrl + `0x110`/`0x100` ack). No produce pointer, no per-period doorbell.
  The engine free-runs on the descriptor rings.
- **The steady-state cycle is a five-block read sweep, `0x100` FIRST:** every ~4 ms (MSI-paced) FC
  reads `0x100,0x300,0x200,0x400,0x500`, and **`0x100` reads `0x80000000` — vec0's cause asserts
  bit31 on every streaming period** and is read-to-cleared by the sweep. Our servicer read only
  `0x200/0x300/0x500`, deliberately skipping `0x100` (mailbox race) and `0x400` (notify): an
  **unserviced read-to-clear ack channel on every period — the same violation class as the
  manifestation wall.** Under trapping the vendor cleared it every cycle; we never did.

**Fix (in tree):** `clarett_stream_service` now performs the exact vendor sweep
(`0x100` guarded by `cmd_inflight` so the mailbox ISR keeps its DONE bit; `0x400` swept too —
caveat: may starve the monitor-notify refresh during capture, revisit). First 8 events of each
session log raw `0x300` values at info to derive this model's ctr unit/modulus empirically.

**Next:** rerun `arecord` with the sweep. If ctr advances, rewrite `clarett_pcm_tick` to advance by
ctr-delta × 16 frames (modulo the observed wrap) instead of 4 frames/event, then validate audio
content. Note the reload path re-arms an already-armed device — the old "re-arm wedges GET_DATA"
finding predates the gated ack and gets tested implicitly (DC power-cycle if it wedges).

## 11. The 0x600x block is CLOCK/SYNC, and it is queries (July 20 2026) `[HW — Clarett 4Pre]`

**`FCP_STREAM_ENABLE` / `FCP_STREAM_COMMIT` never existed.** Those names came from watching the vendor
issue `0x6004`/`0x6002`/`0x6005` in-session immediately before arming the engine and inferring "enable,
then commit". The category number says otherwise — `0x006` is the **sync** category (fcp-server:
`FCP_OPCODE_CATEGORY_SYNC = 0x006`, `SYNC_READ = 0x006004`) — and reading them on a live armed 4Pre
returns state, not acknowledgement:

| opcode | value | meaning |
|---|---|---|
| `0x006004` | `1` | **sync lock status** (0 = unlocked, 1 = locked) |
| `0x006002` | `48000` | current rate (the rate just set) |
| `0x006005` | `48000` | rate |
| `0x006000` | `0x00030018` | caps/bitmask, undecoded |
| `0x006001` | `44100` | rate |
| `0x006003` | `44100` | rate |

So the vendor was **polling whether its clock had locked**, not enabling anything. That also explains
its 3-second pre-stream stall containing *zero* MMIO writes (§10 read it as a mystery trigger): it was
waiting to lock, polling sync at ~16 Hz. Consequence: our stream handshake has **no enabling function**
beyond `SET_CLOCK` and the `CONFIG_PUSH` burst — the triple is inert, kept only to stay byte-identical
and because the answers are worth reading.

**Our 4Pre reports LOCKED at 48000, so the stall is not a clock problem.**

### Corrections to §10 from the same session
- **The counter difference was not real.** §10 leant on the vendor reading `ctr=0xc` where we read `0x0`.
  The vendor's counter reads `0xc, 0xc, 0xc` across all three of its failing period events — **constant,
  not advancing** — and only jumps to `0x18` when streaming begins. `0xc` is stale from an earlier session
  on a device that was never power-cycled. Functionally identical to our `0`.
- **"Arms, one period event, stalls" is not a bug signature.** The vendor does exactly that **four times
  in a row** before it streams, tearing down (`0x110=0`, `0x100=0xf`) and retrying each time.
- **The engine setup is exonerated.** Our post-arm state is byte-identical to the vendor's at the same
  point: its failing arms read `0x218=0xe 0x21c=0xd→0xe 0x318=0x3 0x31c=0x3`, and so do we. `ptr0=0xe`
  is a fixed prefetch depth, not progress — proven by doubling the TX fragment and watching it not move.
- **The vendor's pre-arm re-init batch is not the trigger.** Replaying it verbatim (`INIT_2`, `INIT_1`×8
  ids 1–8, `INIT_2`, count queries, `0x004001`×6) is accepted with `err=0` and changes nothing
  (`stream_batch=1`, default off).
- **Fixed en route:** `clarett_frag_bytes()` sized descriptors by alignment, so frames-per-descriptor
  fell out of the channel width — 8 for the 4Pre's 8ch TX ring against 16 for its 20ch RX ring, i.e. the
  two rings advancing at 2:1 in **time**. Now sized by duration (16 frames) then aligned; only the 8ch
  case changes.

**Next lead: the TX ring's CONTENTS.** Everything host-visible now matches the vendor and the engine still
will not start, which points off-BAR. In the vendor capture Windows was actively playing audio, so its TX
ring held real samples; ours holds silence. This engine has shown content sensitivity before — the `0xAA`
RX pre-fill was recorded as "the lone diff that made it clock" — and DMA content is precisely what an MMIO
trace cannot see. **Refined by §12:** TX content is one of exactly TWO surviving candidates (the other is
elapsed time), and §12 gives the levers that separate them.

---

## 12. The arm ritual: four failing arms, then a silent multi-second wait (July 23 2026) `[TRACE]`

Desk re-reading of the three vendor stream captures, hunting for what distinguishes the arm that
streams from the arms that don't. **The pattern is universal and we have never replayed it.**

Every capture — `2pre_stream.log`, `4pre_boot_to_stream_end.log`, `8prex_stream.log` — opens with
**exactly four throwaway arms**, then a **multi-second window with zero BAR traffic**, then **one more
arm that streams**:

| model | four arms | held each | gap between | silent window | arm that streams |
|---|---|---|---|---|---|
| 2Pre  | 52.733–52.776 (43 ms) | ~1.4 ms | 12–14 ms | **3.105 s** | 55.881 |
| 4Pre  | 16.602–16.680 (78 ms) | 1.3–6.0 ms | 23–33 ms | 1.88 s + batch + **1.016 s** | 19.721 |
| 8PreX | 47.373–47.415 (42 ms) | ~1.3 ms | 11–15 ms | **5.914 s** | 53.330 |

Each throwaway arm is the **complete 14-write program** — `0x108=0x10`, `0x20c=1`, per-block
CHANS/SIZE/BASEhi/BASElo, `0x10c=0x1e70700`, `0x110=7` — with the **same bases and the same geometry**
as the arm that eventually streams (2Pre `0x680f7000`/`0x680fb000`, 4Pre `0x79998000`/`0x799ac000`).
It is held ~1.5 ms, raises **no period at all** (`0x300` reads `0x0` at teardown), and is torn down with
`0x110=0` → cause sweep → `0x100=0xf` → status reads `0x21c/0x218/0x31c/0x318` → fw-info block
`0x800..0x8a4`. The per-block pointers **do** advance during a failing arm (2Pre `0x21c/0x218=0xd`,
`0x31c/0x318=0x2`), i.e. the engine prefetches descriptors and then goes nowhere — our exact symptom.

**The consequence is a hard narrowing.** The register program is *provably byte-identical* between the
arms that fail and the arm that works, on three different models. So whatever changes across the silent
window is in one of the only two channels an MMIO trace cannot see:

1. **Elapsed time** — something in the device settles a few seconds after the first arm attempt.
2. **Host RAM contents** — Windows filled the TX playback ring during the window (nothing else is
   running; the bus is idle).

Everything else is eliminated by the traces themselves. In particular:

- **The re-init batch is not it.** Only the 4Pre has FCP in its window at all; the 2Pre and 8PreX
  windows contain **zero MMIO of any kind**. That independently confirms the hardware result that
  `stream_batch=1` changes nothing, and retires the §9 reading that the batch is what "flips" the engine.
- **The pre-arm `0x6004/0x6002/0x6005` triple is not it** — same reason (absent from two of three
  captures), consistent with §11 finding those opcodes are sync *queries*.
- **It is not a missing register or a missing command**, since the failing arms already write
  everything the working arm writes.

**In tree (levers, all default off = historical behaviour):** `clarett_engine_arm()` is split into
`clarett_engine_program()` (the 14 writes) + `clarett_engine_quiesce()` (the vendor's teardown: `0x110=0`,
cause sweep, `0x100=0xf`), with `arm_pre` (throwaway arm count), `arm_hold_us`, `arm_gap_ms` and
`arm_settle_ms` replaying the ritual, and `tx_tone` (clarett_pcm.c) filling the dummy TX ring with a
1 kHz −18 dBFS sine instead of silence. The two hypotheses are orthogonal and separable in three runs:

| run | levers | reads on |
|---|---|---|
| A | `arm_pre=4 arm_settle_ms=3500` | time/retry alone (TX still silent) |
| B | `arm_pre=4 arm_settle_ms=3500 tx_tone=1` | both |
| C | `tx_tone=1` | TX content alone |

Signal to watch: `stream-svc: periods=… ctr=…` — the wall is `ctr` frozen after one pass; success is
`ctr` advancing continuously (the vendor's own steady state is +0xc per 4 ms event). If B streams and
A and C don't, the two are jointly required. `tx_tone` plays out of the monitor outputs the moment the
engine runs, so a working engine should also be **audible**.

---

## 13. Arm-ritual and BASE_HI settled on hardware; the block is sample CONSUMPTION (July 23 2026) `[HW — Clarett 2Pre, armed session]`

Four hardware runs on a fresh armed 2Pre (`enable_pcm=1`), testing §12's two surviving candidates and
the long-standing `0x214`/`0x314` ambiguity. **All three candidate causes are eliminated; the real
signature is now much sharper.**

- **Time alone — NEGATIVE.** `arm_pre=4 arm_settle_ms=3500`: the ritual reproduces the vendor's failing
  arm state *exactly* (`arm ritual: 4 throwaway arms done (ptr0=0xd ptr1=0x2)` — the 2Pre vendor's own
  `0x21c/0x218=0xd`, `0x31c/0x318=0x2`), so it is a faithful replay, not a mis-arm. Still walls:
  `periods=2 ctr=0x0`, `arecord` EIO. Elapsed time is not the trigger.
- **Time + TX content — NEGATIVE.** `arm_pre=4 arm_settle_ms=3500 tx_tone=1` (TX ring filled with a
  1 kHz sine, 4096 frames × 4ch, confirmed in the log): identical wall, `periods=2 ctr=0x0`, EIO, and
  no audio. So TX-ring contents are not the trigger either — **this retires the §11/§12 "content
  sensitivity" lead** (the historical `0xAA` "content" effect was the flat-mode pointer-deref artifact,
  not a real content gate).
- **`base_hi` — SETTLED: `0x214`/`0x314` is a genuine 64-bit address high word, and we handle it
  correctly.** Forcing `base_hi=2` (what every vendor arm writes) with our low base `0xffe00000`
  produced `AMD-Vi IO_PAGE_FAULT ... address=0x2ffe00000` and `0x2ffe10800` — i.e. the engine fetched
  the table at `(2<<32)|low`, faulting, `ptr` frozen at 0. **This closes the multi-month `dma_bits`
  ambiguity:** the vendor wrote 2 because its rings genuinely lived above 8 GB (`0x2_xxxxxxxx`); our
  contiguous coherent buffer below 4 GB with high word 0 is *correct*, and the engine fetches our
  descriptor table normally (`ptr0=0xd`, no fault) when it is not forced. `0x214` is NOT a flags/tag
  field. (The per-direction entry high words in the 8PreX dump — TX 2, RX 1 — are therefore genuine
  address bits: Windows placed the two sample buffers in different 4 GB regions, unremarkable.)

**The sharpened signature (the actual finding).** Comparing the vendor's working arm to ours at the
counter level:

| | vendor working arm (`2pre_stream.log`) | ours (descriptor mode, armed) |
|---|---|---|
| first `0x300` read | `0x8000000c` (ctr already **12**) | `0x80000000` (ctr **0**) |
| steady state | `+0xc`/period (16 frames/unit × 12 = 192 frames = 4 ms) | ctr **frozen at 0**, 2 events then stall |
| `ptr` (`0x21c`/`0x31c`) | advances | prefetches to `0xd`/`0x2`, then frozen |

So the engine **fetches our descriptor table** (ptr advances, no fault — table format/address correct)
and **fires the 4 ms period IRQ on schedule** (bit31 asserts), but **consumes zero sample frames per
period** (ctr never leaves 0). The block is therefore *specifically in sample consumption / converter
data movement* — NOT in: table address (base_hi settled), table fetch (ptr advances), table format
(no fault), period timing (IRQ fires), arm-register program (byte-identical), elapsed time, or TX
content. Every host-visible and host-RAM channel we can drive is now eliminated.

**This re-converges on the d792678 tension.** That commit unified both models to descriptor mode on
the theory "descriptor is the hardware default, buffer mode is the FCP-handshake response." But the
2Pre RAM dump (§9) showed **flat audio** at the live `0x310` target (no table), and the reverted flat
path was the one config that ever got the 2Pre **counter advancing** (`ctr=0x1c10`, 28 passes) — vs
descriptor mode's dead `ctr=0`. The `ctr=0` "consumes nothing" signature is consistent with a
**mode mismatch**: we hand the 2Pre a descriptor table, its engine expects a flat sample ring, so it
walks the "table" as flat data and advances nothing recognisable. **NEXT (the surviving lead, a real
re-opening):** restore a per-model flat-buffer path for the 2Pre (0x210/0x310 → contiguous sample ring,
no table) and read the counter; the open sub-problem from §9 is why flat previously needed a non-null
prefill to clock yet then faulted dereferencing it — resolve by capturing what the VM writes into the
2Pre sample area *before* its working arm (a fresh pre-arm `pmemsave`), since RAM contents at arm time
is now the only remaining unobserved channel.

### Flat path rebuilt + the pre-arm capture (July 23 2026) `[DRIVER]`/`[PLAN]`

Acting on §13's surviving lead: the per-model flat-buffer path is back in tree (it had only ever lived
in an uncommitted working tree; the d792678 rewrite dropped it on the "descriptor is the default"
theory, which the 2Pre's `ctr=0` refutes).

**Driver.** `struct clarett_model.flat_buffer` (bool) selects the mode; set on the 2Pre only (the model
with RAM-dump evidence — 8PreX/4Pre/8Pre stay descriptor). Mode-independent accessors in clarett.h
(`clarett_stream_total_bytes`/`_rx_off`/`_rx_area_bytes`/`_r1_off`/`_tx_off`/`_tx_area_bytes`) keep the
branch out of clarett_pcm.c. Flat geometry: `CLARETT_FLAT_FRAMES` (1024) per direction ⇒ 2Pre TX ring
`1024·4·4 = 16 KB` (exactly the VM's `0x680f7000→0x680fb000` gap) + RX ring `1024·14·4 = 56 KB`;
`0x210=stream_dma` and `0x310=stream_dma+16 KB` point straight at the sample rings (no table, no wrap
flag, `r1 = r0 + flat_tx_bytes` so RX abuts TX just as the VM's two bases do). The RX sample ring IS the
ALSA capture buffer. `clarett_build_rings()` builds nothing for a flat model (coherent alloc is already
zeroed = silent TX + clean RX). **No `0xAA` prefill** — §9 exposed that as a descriptor-mode artifact
(the engine dereferencing sample bytes as pointers); in a real flat ring the bytes are samples.

**The pre-arm capture (resolves the last unobserved channel).** §13 pinned the block to sample
consumption and left RAM-at-arm-time as the only channel an MMIO trace can't see. Procedure, against the
live Windows VM streaming the 2Pre:

1. `python3 tools/dma_bases.py <live-stream-trace>.log Windows10` → emits `pmemsave` for the TX/RX bases
   (GPA = `(0x214<<32)|0x210` etc., read from the trace's most-recent base writes).
2. Dump at two moments and classify each with the new `tools/dma_classify.py` (flat-audio vs
   descriptor-table vs all-zero, automating the §9 hand analysis; for flat it reports peak/RMS dBFS so
   "pre-seeded" vs "silent" is unambiguous):
   - **DURING a running stream** — confirms the tree's assumption (2Pre TX/RX should classify `flat-audio`).
   - **BEFORE the working arm** (the 5th arm, after the multi-second settle §12) — the open question:
     does the VM's TX ring already hold playback audio at arm time (`flat-audio`, pre-seeded) or is it
     `all-zero`? If pre-seeded, that is why the old flat attempt needed a non-null TX ring to clock, and
     the driver must seed the TX ring before arming (not `0xAA` — real silence-or-signal samples).
3. Cross-check the RX **base** dump against the TX base: if RX classifies `descriptor-table` while TX is
   `flat-audio`, the two directions differ in mode (would explain a half-working engine) — not expected
   for the 2Pre, but the classifier makes it a one-command check.

**Hardware test of the flat path itself** (independent of the capture): `insmod snd-clarett.ko
model=2pre enable_pcm=1`; `arecord -D hw:N -c14 -f S32_LE -r48000 /dev/null`; watch `stream-svc:
periods=… ctr=…`. Success is `ctr` advancing continuously (the reverted flat path reached `ctr=0x1c10`,
28 passes); the descriptor-mode baseline was `ctr=0` frozen. `tx_tone=1` now fills the flat TX ring
(at offset 0, no table) if a non-silent TX ring turns out to be required.

### Flat path FALSIFIED on hardware — the engine dereferences the ring as a table (July 23 2026) `[HW — Clarett 2Pre]`

`insmod model=2pre enable_pcm=1` with the flat path armed the flat rings cleanly
(`flat rings: TX 16384 B + RX 57344 B`, bases `fffc0000`/`fffc4000`, gap `0x4000` = TX ring) and then
**IOMMU-faulted immediately, before `arecord` ran**:

```
AMD-Vi IO_PAGE_FAULT ... address=0x0    flags=0x0020
                         address=0x80
                         address=0x100
                         ... 0x180 0x200 0x280 0x300   (seven, stepping 0x80)
                         address=0x0    flags=0x0000
```

Seven `0x80` steps = `0x380` = `14ch × 4 B × 16 frames` = **one 16-frame capture fragment**, exactly the
§9 fault decode. The RX engine read entry 0 of the "flat" ring, took the zeroed value **as a pointer**
(target GPA 0), and wrote a 16-frame fragment to `0x0/0x80/.../0x300`; the trailing `0x0 flags=0x0000`
is the benign TX position writeback. `periods=0 ctr=0`.

**Conclusion: the 2Pre engine dereferences the contents of `0x210`/`0x310` as DMA pointers — it wants a
descriptor TABLE, not a flat sample ring, same as the 8PreX.** The "2Pre wants flat" hypothesis (§9/§13)
is **falsified**. The 2Pre "flat audio" RAM dump was therefore the **fragment buffers** (what a table's
entries point at), not the table at the ring base — a dump-address misattribution, not two device modes.
This also unifies the picture: descriptor mode gives no faults but `ctr=0` (valid pointers, engine reads
the table, consumes nothing); flat mode gives the deref-fault storm (zeroed "pointers" → writes to GPA 0).
Both are the *same* engine reading a table; the real defect is our descriptor table FORMAT, not the mode.

**In tree:** `flat_buffer` stays false on every model; `force_flat` (module param) can still force it for
a pmemsave-guided re-test, but the default 2Pre load is back to the non-faulting descriptor baseline.
`c->flat_buffer` is now a per-instance field (model default, `force_flat` override).

**The priority is now the descriptor-table FORMAT, and the pmemsave capture is how to get it.** Dump what
the VM's `0x210`/`0x310` point at during a live 2Pre stream and classify with `dma_classify.py`:
- If `descriptor-table` (expected, per this result): read off the real entry stride, count, and high-word
  tag from the classifier's `--hex` output and rebuild our table to match (§9's `0x1c0` clocks one pass,
  `0x700` gave `periods=0` — the true stride is between/other, and the RAM dump settles it).
- If `flat-audio`: then the base we dumped in §9 was the table's first fragment pointer, and we must dump
  the ADDRESS STORED at the base (one indirection up) to see the table itself.

---

## 14. The real 2Pre descriptor format, read out of the live VM (July 23 2026) `[HW/pmemsave — Clarett 2Pre]`

`pmemsave` of the 2Pre's live `0x210`/`0x310` targets during a running stream, classified with
`tools/dma_classify.py` (which now reports table geometry directly). **Both directions are descriptor
tables** (settling §13), and the format is fully recovered:

| block | chans | entries | fragment stride | frag frames | high tag | periodic IRQ flag | last entry |
|---|---|---|---|---|---|---|---|
| TX (0x210) | 4  | **252** | **0x100** | 16 | 2 | none | `0x01` (wrap) |
| RX (0x310) | 14 | **300** | **0x380** | 16 | 2 | **bit1 (`0x02`) ~every 14** | `0x03` (wrap\|IRQ) |

Findings, and the three bugs they expose in our old table:

1. **Fragment = channels·4·16 bytes, packed with NO alignment rounding.** TX 4ch→`0x100`, RX 14ch→`0x380`,
   8PreX 28ch→`0x700`. Our `lcm(0x100, …)` rule rounded 14ch up to `0x700` — **2× too big**. The `0x100`
   alignment was never a device rule; it was coincidence that 28ch·64 is `0x100`-aligned. The vendor RX
   stride `0x380` is only `0x80`-aligned (entry low bytes alternate `00`/`80`). **FIXED.**
2. **The RX ring carries a periodic IRQ flag (bit1) every ~14 descriptors, and THAT raises the counted
   0x300 period.** Our table set only a single wrap flag on the last entry — so the engine consumed
   descriptors but never hit an IRQ marker to advance the counter. **This is the `ctr=0` wall.** TX has
   no periodic marker (only wrap on the last), consistent with capture-only servicing the RX (0x300)
   counter. **FIXED** (`CLARETT_DESC_IRQ` every `CLARETT_IRQ_DESCS`).
3. **Entry count is per-direction (252 TX / 300 RX), not a shared 256.** We own our buffer, so we keep
   `NDESC=256` with the IRQ flag every 16 (a clean 256-frame / 5.33 ms period); the exact vendor count is
   its buffer-size choice, not a device constraint. The three quantities are now distinct and all verified:
   **SIZE reg** `0x208`/`0x308` = 4 frames (`channels·16`), **fragment** = 16 frames (`channels·64`),
   **IRQ period** = `CLARETT_IRQ_DESCS·16` frames.

The tables are physically scatter-gathered (TX in `0x1000`/16-fragment page runs, RX in 5–6-fragment
runs) because Windows' buffer spans scattered pages; our coherent buffer is contiguous, a valid special
case (the engine just follows the pointers).

**In tree:** `clarett_frag_bytes` drops the `lcm` rounding; `clarett_build_rings` sets the periodic RX IRQ
marker; the PCM period model advances `clarett_irq_period_frames()` per `0x300` event (was a wrong 4).
Hardware test: `model=2pre enable_pcm=1`, `arecord -c14`; watch `stream-svc: periods=… ctr=…` — success is
`ctr` advancing past the one-pass `0x1b3`/`0` wall. `dma_classify.py` gained `table_stats` (entries,
stride, tags, scatter runs, flag positions) and a leading-run detector so an oversized dump still classifies.

### ★ WALL BROKEN — continuous streaming on the 2Pre (July 23 2026) `[HW — Clarett 2Pre]` ★

The §14 table rebuild WORKS. `model=2pre enable_pcm=1`, `arecord -c14 -f S32_LE -r48000`:

```
descriptor rings: 256 entries, frag tx=0x100 rx=0x380, RX IRQ every 16 desc (256 frames/period)
stream-ev[0..7]: 0x300 = 0x8000000d,1a,27,34,41,4e,5b,68   (+0xd/event, ADVANCING)
stream-svc: periods=7517 ctr=0x75 wraps=471   over ~32.7 s   (vs the old one-pass 0x1b3/0 stall)
```

The `0x300` counter advances continuously and wraps (the small modulus wraps ~every 16 events — "wraps"
are modulo, not ring passes), exactly the vendor's steady-state. `arecord` ran the full 32 s to Ctrl-C
with **no EIO and no XRUN** — capture plumbing works end to end. **The periodic RX IRQ marker (bit1 every
CLARETT_IRQ_DESCS descriptors) was the fix**; a ring flagged only on the last entry gave `ctr=0` forever.
This is the first sustained data-plane streaming in the project.

**Open (refinements, not blockers):**
- **Pitch calibration — FIX IMPLEMENTED (pending verification).** The counter steps `+0xd` (13) per event
  (~208 frames/event ≈ 48 kHz ⇒ one ctr unit ≈ 16 frames, matching §10), but the fixed
  `CLARETT_IRQ_DESCS=16` model advanced 256 frames/event → ~23% fast. Now the servicer computes the ctr
  **delta** per event (reusing the last good step across the small-counter wrap; `CLARETT_CTR_STEP_MAX`
  guards a glitch) and `clarett_pcm_tick` advances by `delta × CLARETT_CTR_FRAMES` (16), self-calibrating
  to the real hardware period regardless of the per-model step (vendor `+0xc`) or our marker spacing. The
  copy now splits at the ring wrap since the per-event advance is variable. VERIFY with a real capture.
- **Real-audio-content verification.** `arecord` to `/dev/null` proves the clock/plumbing, not the samples.
  Capture a known input (tone into Analogue 1) to a WAV and inspect that the right channel carries it at
  the right level/frequency.
- **Playback (TX) and the 8PreX** still untested with the corrected table.

### ★ CAPTURE VERIFIED — real audio, correct rate (July 23 2026) `[HW — Clarett 2Pre]` ★

`arecord -c14 -f S32_LE -r48000 -d 10 /tmp/cap.wav` with a tone into Analogue 1, analysed:
- **Channel 0 = a real, recognizable instrument signal** (a guitar plugged into Analogue 1 — NOT a
  reference tone; the ~868 Hz zero-crossing figure is inflated by the guitar's harmonics and is NOT a
  pitch anchor). Temporally coherent (no drift/smear → the ctr-delta period advance is correct).
- **Channels 1–13 silent** (ch1 dither only), exactly right for one live input.
- Layout confirmed: 24-bit MSB-justified S32_LE (low byte 0), interleaved, channel 0 = Analogue 1.
- Minor artifact: rare isolated full-scale (`0x80000000`) spikes on the even channels (~0.01%, scattered,
  NOT ring-wrap-aligned) — cosmetic, chase later.

**The data plane does clean 14ch/48k capture on the 2Pre.**

**PITCH VERIFIED (July 23 2026):** a precise 1000.000 Hz reference (48k, played from a separate interface
into Analogue 1) captured as **1000.91 Hz** (Goertzel over two 5.46 s windows, +0.090%, +1.5 cents). That
is ~65× smaller than an off-by-one counter unit (±6%), so **`CLARETT_CTR_FRAMES=16` is confirmed exact**;
the residual +0.09% is the two interfaces' unlocked-crystal offset (they were not word-clock-locked), not a
2Pre rate error, and is inaudible. No driver change.

Remaining: TX playback and the 8PreX with the corrected table; then the even-channel spike cleanup.

### ★ PLAYBACK CONFIRMED — full duplex on the 2Pre (July 23 2026) `[HW — Clarett 2Pre]` ★

`aplay -D hw:5 ref_1khz_48k_4ch_s32.wav` (native 4ch S32_LE) → **audible tone out the monitor output**
once **PCM 1** is routed to **Analogue Output 1** in the router (alsa-scarlett-gui). The playback device
enumerates (`aplay -l` shows the 2Pre, 4ch), the TX engine clocks from the playback-side arm, and the
TX ring fill reaches the DACs. The DMA was never the blocker — playback is silent until the router sends
a PCM source to a physical output (no default route). So the **full-duplex PCM path is complete on the
2Pre**: 14ch capture + 4ch playback, one shared engine.

Refinements still open: the even-channel capture spikes (§ above, cosmetic); TX/playback on the 8PreX
(and 4Pre/8Pre) with the corrected table; and simultaneous duplex stress (both directions at once — the
shared-engine attach/detach was written for it but not yet stress-tested).

---

## 15. The even-channel capture drift: contiguous buffer vs page-granular DMA (July 23 2026) `[HW — Clarett 2Pre, FIXED]`

Capture had a periodic channel-alignment **drift** (misread earlier as "rare spikes"): mapping a 1 kHz
tone through the stream, the signal walked **−2 channels every ~73 frames**, cycling through all 14
channels and realigning **every 512 frames**, with the real input (ch0) dropping to zero during each
step. A raw RX-ring dump (`rx_dump`, since removed) confirmed the shift is in the bytes the **device**
DMA'd — our frame-aligned copy is faithful.

**Root cause.** The drift is exactly **8 bytes (2 samples) per 4096-byte page**: `512 frames × 56 B =
28672 B = 7 × 4096`, and `LCM(0x380 fragment, 4096 page) = 28672`. Our RX buffer was ONE **contiguous**
`dma_alloc_coherent` region, so the engine streamed straight across fragment boundaries and a
page-granular DMA quirk accumulated 8 B/page. The `0x380` (896 B) fragment does not divide the 4 KB page
(`4096/896 = 4.57`), so the misalignment never reset. The vendor's buffer is **scatter-gathered**
(physically discontiguous fragments), which restarts the engine per fragment and never accumulates the
drift — the one part of the vendor's descriptor layout we hadn't replicated. (TX never drifted: its
`0x100` fragment divides the page.)

**Fix (hardware-confirmed, now the default).** Give each RX fragment its own **page-safe slot** — the
fragment audio size rounded up to a power of two (`0x380 → 0x400`), which divides the 4 KB page — over a
**page-aligned** RX sample area. Every fragment is then page-contained and the engine cannot stream
across the gaps (per-fragment DMA, like the vendor's scatter-gather). The RX geometry now separates a
LOGICAL size (contiguous frames = the ALSA buffer + per-period math) from a DEVICE size (NDESC slots of
`c->rx_slot`, allocated and strided by the descriptors, with gaps); the capture drain
(`clarett_rx_drain`) gathers per fragment across the gaps and reduces to a linear copy when unpadded.

Result on the 2Pre (`rx_frag_pad=-1`, the default, slot `0x400`): **channels 2–13 read exactly `0`**
(were full of bursts), **ch0 is a clean 1000.00 Hz tone with zero dropouts**, and the engine clocks
normally (`periods` climbing at ~234/s). `rx_frag_pad` kept as a lever: `0` = old contiguous (drifts,
for A/B), `>0` = manual padding.

## 16. TX has the SAME page-alignment bug — the 8PreX playback fold (July 30 2026) `[HW — Clarett 8PreX, FIXED]`

The "TX unchanged (already page-safe)" note above was true only for the models tested then. **8PreX
playback was garbled and folded 28ch→4** (a tone on PCM 1 also drove PCM 5, 9, … — every output ≡ its
source mod 4), while capture was clean 28-wide. Exhaustive comparison against a fresh VM dump showed
**everything the device reads is byte-identical to the vendor** — registers, descriptor table
(`0x700` stride), source-ids, handshake, arm, and (confirmed by dumping both the vendor's TX sample
fragments *and* our live TX ring) the 28-ch interleaved sample layout. The fill clock was perfect
(`tx_trace`: `pcm_frames` tracked the 0x300 counter exactly). The one remaining difference was that our
TX fragments were **contiguous** while the vendor's (and our working RX) were not.

**Root cause = the exact TX analog of §15.** The TX fragment is `channels·4·16`; it is page-safe only
when that is a power of two. 2Pre (`0x100`) and 4Pre (`0x200`) are — which is why their playback always
worked and hid the bug. **8Pre (`0x500`) and 8PreX (`0x700`) straddle the 4 KB page**, and the device's
per-fragment TX **read** mis-frames across the page boundary, collapsing 28 interleaved channels into
4-channel groups. **Fix:** mirror the RX slotting for TX — `c->tx_slot` = fragment rounded up to a
power of two (`0x700 → 0x800`), descriptors strided by the slot, and a slot-aware fill (`clarett_tx_fill`,
the mirror of `clarett_rx_drain`); the ALSA buffer / per-period math stay on the LOGICAL contiguous size.
`tx_frag_pad` lever mirrors `rx_frag_pad` (`-1` auto pow2 default, `0` = old contiguous for A/B).
**Hardware-confirmed on the 8PreX: clean music playback, fold gone.** No change for the 2Pre/4Pre
(fragment already a power of two).

## 17. The period event DOES raise an MSI — on vec0, all along (Nov 13 2026) `[HW — Red 8Line]` + `[TRACE]`

The servicer has polled `0x300` since the engine first clocked, and the standing assumption —
written into `clarett.h` and the driver's own ISR comment — was that `0x200`/`0x300` interrupts
arrive on MSI vec1/vec2, which the ISR therefore read and discarded. **Both halves of that were
wrong: vec1/vec2 never fire at all, and the period event arrives on vec0 together with the mailbox
and the device events.**

**Measurement** (`tools/bench/irq_rate.sh`, written for this; Red 8Line, 60ch capture at 48 kHz,
10 s legs, per-vector counts from `/proc/interrupts`):

| ALSA period | expected device period events | vec0 attributable | per event | vec1 | vec2 | vec3 |
|---|---|---|---|---|---|---|
| 512 frames | 937 | 945 | **1.009** | 0 | 0 | 0 |
| 64 frames | 7500 | 7511 | **1.001** | 0 | 0 | 0 |

The two legs are the control for each other: an **eightfold** change in period cadence moved vec0's
rate by the same factor, to within 1 %, which no fixed-rate source (the ~40/s mailbox baseline, a
timer, a MIDI or notification source) can do. vec0's idle baseline was exactly 40.0/s both times
(fcp-server's ~20 Hz metering, two MSIs per command) and was subtracted. Taken on the laptop, where
`periods=` and the counts agree; the figure needs no root beyond the `periods=` line, since the
expected cadence is just rate/period.

Corroborating evidence already on record, now explained: the step-4a work found "only vec0 fires",
and the Oct 10 relay measurements found that **a stream adds no relays** — the interrupts were
arriving and being handled, but a period event raises no bit in `0x400`, so the ISR read it, found
the event mask empty, and returned. Nothing in the kernel log could show the difference.

**★ CORRECTED Nov 17 2026: the vendor IS interrupt-driven** (2Pre VM trace with `vfio_msi_interrupt`,
`clarett-opcode-inference.md` §12): its five-block sweep follows every vec0 MSI, p50 29 us, idle and
streaming, and the 4 ms cadence below is the period event at its period. The capture read here had no
MSI trace, so a sweep on every interrupt looked like a timer. The paragraph below stands as a
description of the sweep; its conclusion ("does not use this interrupt", "a 4 ms polling timer") is
withdrawn.

~~**The vendor does not use this interrupt.**~~ In `captures/red_8line.log`, through a 27 s streaming
window (the one bracketed by `0x110 <- 7` at +481.337 s and `0x110 <- 0` at +508.442 s), the vendor
driver reads all five cause blocks in one fixed sweep — `0x100`, `0x300`, `0x200`, `0x400`, `0x500`,
**8042 reads each, always in that order** (`0x300` is preceded by `0x100` and followed by `0x200` in
all 8042 cases) — at a flat **297.9/s, inter-read gap p50 3.998 ms**: a 4 ms polling timer. 1292 of
those sweeps found nothing pending and 6750 had bit 31 set, so it coalesces several periods per
sweep exactly as our servicer does. So there was never a vendor behaviour to port here, which is
why this went unnoticed for so long.

Also confirmed in passing, against a note in CLAUDE.md that read as if the driver double-wrote it:
the vendor writes `0x110 = 7` at arm and `0x110 = 0` at **stop**, 14 arm/stop pairs in the capture,
and our `clarett_engine_program()`/`clarett_engine_stop()` do exactly the same. The arm writes
(`0x108 = 0x10`, `0x20c = 1`, `0x10c = 0x1e70700`, `0x110 = 7`) match byte for byte, and `0x104`
is written once at init (`0xf000003f`) and once at teardown (`0`) by both. So the period MSI needs
no configuration we were missing — it was always being delivered.

**First driver step taken the same day:** the vec0 ISR now sets a flag and wakes the servicer, which
waits on it instead of sleeping ~150 µs between polls (`clarett_svc_wait()`,
`CLARETT_SVC_FALLBACK_US` = 1000). The servicer remains the only reader of `0x300`, because that
read is how a period is acknowledged and the engine must not be acked from interrupt context. The
fallback timeout is deliberately short: it is also the cadence at which playback is topped up
between events (`clarett_pcm_tx_refill()`), which a client writing with a short lead depends on —
the thing `tx_guard_lead.sh` measures.

**Tested the same day (Red 8Line, laptop, srcversion `1B72AB39EBE3490F747C7E4`): PASS.**
- `irq_rate.sh` as root, against the driver's own `periods=`: **1876 interrupts for 1875 period
  events (1.001) at a 512-frame period, 15004 for 15000 (1.000) at 64**, vec1/vec2/vec3 zero in
  both legs. So the interrupt arrives once per period event with the new wake path in place, and
  nothing is coalesced or lost.
- `loopback_order.sh`: **8/8 legs sample-exact** (144k/288k/576k frames at 48/96/192 kHz), ramp on
  the expected channel every time, S/MUX-removed tails all zero.
- `duplex_stress.sh`: **0 failing parts** — 48/96/192 kHz sustained sample-exact with 0 xruns, the
  64/256 leg sample-exact (one non-RT arecord overrun outside the ramp, as before), churn 10/10 with
  no servicer thread left, the 96k-into-48k open narrowed and unbroken. `overrun=0`, `badreads=0`
  throughout.
- **Servicer CPU, measured on this host at 60 channels** (`/proc/<tid>/stat` of the `clarett-svc`
  thread over 10 s): **3.00 % of a core at a 512-frame period, 4.50 % at 64**. The poll loop's
  recorded figure is ~11-12 %, but from earlier sessions on other hosts, so a controlled A/B on this
  box (reload the previous build, repeat these two) is still worth doing before quoting a ratio.
- The `late=2..6` counts in the churn windows are the already-recorded old-rate tail, not new: the
  late threshold scales with `cur_rate`, republished at the handshake, while the device keeps
  clocking the previous slower rate for tens of ms, so the first gaps of a faster leg exceed a
  threshold computed for the new rate. Every one has `gapmax` equal to the previous rate's period
  (10611 µs in a 96 kHz leg, 5337 µs in a 192 kHz one), and the range matches the Oct 16 Red run on
  the poll-loop build.

### The first cut of that step BROKE short-lead playback, and only `leadplay` saw it (Nov 13 2026) `[HW — Red 8Line, FIXED]`

The step above tied one wait to two jobs: collecting the period event, and topping playback up
between events (`clarett_pcm_tx_refill()`). At a 1 ms timeout the second is far too coarse.
`tx_guard_lead.sh` on the new build, Red 8Line, `leadplay` holding a 48-frame lead in 16-frame
writes at a 32-frame period:

| client | period | frames of ramp captured (of 480000) | breaks | gaps | client underruns |
|---|---|---|---|---|---|
| `leadplay` | 32 | **160** | 0 | 3 | ~0 |
| `leadplay` | 64 | 297056 | 0 | 6188 | 14999 (lead < period, inherent) |
| `aplay` | 32 | 720000 | 0 | 0 | 0 |
| `aplay` | 64 | 720000 | 0 | 0 | 0 |

The client wrote all 480000 frames and reported no underruns; the audio simply never reached the
device. `breaks=0` throughout, so nothing was torn — the frames were replaced by **silence**, which
is the mechanism `clarett_play_fill()` has always had (the fill writes silence past `appl_ptr`, so a
frame written after its slot has been passed is never copied). The poll loop's ~150 µs cadence hid
it; the 8Pre baseline for this cell was ~0.1 % loss. **Every other test passed on that build** — the
ramps at three rates, the duplex stress, the churn — because `aplay`-style clients keep the buffer
full and never write into a passed region. A short-lead client is the only probe for this.

**Fix: separate the two cadences, which is sound because the refill reads no register.**
`clarett_pcm_tx_refill()` checks an atomic and copies from the ALSA buffer using the software frame
clock, so waking for it costs memory traffic only, and nothing while no playback stream is attached.
The servicer therefore wakes every `CLARETT_SVC_REFILL_US` (150, the poll loop's old cadence) and
refills, but **reads the device only when the ISR has flagged a period event**, or after
`CLARETT_SVC_FALLBACK_US` (1000) without one. The four MMIO reads per iteration — the poll loop's
real cost — drop from ~20k/s to ~1000/s (the period reads plus the 1 ms fallback; "the period rate"
as first written here missed the fallback), while playback behaviour is bit-for-bit what it was.

Removing the last timer would mean refilling from the client's own write (`.ack`), which runs in
atomic context and so cannot take `pcm_lock` as `clarett_play_fill()` does. Separate change.

**Verified on hardware (Red 8Line, srcversion `4A852112C3A0973841A5262`): PASS.** The same cell,
same script, back to back against the build above: `leadplay` at a 32-frame period delivered
**719968 of 720000 frames** (was 160), `breaks=0 gaps=1`, no client underruns — better than the
8Pre's ~0.1 % baseline for that cell. The period-64 cell and both aplay cells are unchanged, as they
should be. `loopback_order.sh` 8/8 sample-exact at 48/96/192 kHz; `duplex_stress.sh` 0 failing parts
(churn 10/10, 0 xruns). `irq_rate.sh` with fcp-server stopped, so the idle baseline is **0.2/s** and
vec0's streaming rate IS the period rate: 94.9/s at a 512-frame period and 751.1/s at 64, **1.011
and 1.001 interrupts per period event**, vec1/vec2/vec3 zero.

**★ SERVICER CPU, CONTROLLED A/B ON ONE HOST AND ONE UNIT** (`tools/bench/svc_cpu_ab.sh`, written
for this: it reloads each module in turn and times the `clarett-svc` thread across a 15 s capture at
each period size; Red 8Line, 60ch):

| ALSA period | poll loop `3767D0A3B9DEA0C5709F400` | interrupt-driven `4A852112C3A0973841A5262` | saving |
|---|---|---|---|
| 512 frames | **11.27 %** of a core | **4.00 %** | −7.27 pp, 2.8x |
| 64 frames | **11.67 %** | **5.53 %** | −6.14 pp, 2.1x |

Two secondary results. (1) **The ~11-12 % on record reproduces on this host**, so that figure, taken
elsewhere, was sound. (2) **The poll loop's cost is FLAT across period sizes (11.27 vs 11.67) while
the new servicer's scales with the period rate (4.00 -> 5.53)** — which is the signature of the
change having removed the right thing: what is left is per-event work, not polling. Earlier
unprivileged runs of the same cells gave 3.50 / 5.20 %, within ~0.5 pp of the controlled figures
(15 s at a 100 Hz tick is ~0.07 pp per tick, and the thread can land on a different core).

The script hit one trap worth remembering: under zsh `sudo BENCH_LOG_DIR=~/dir` passes the tilde
through UNEXPANDED (zsh tilde-expands assignments, not arguments), so the first A/B ran correctly but
lost every figure to `tee` errors. Both new scripts now expand a leading `~` themselves and fall back
to the cwd if the directory is unwritable; `sudo` also drops an exported `BENCH_LOG_DIR` outright.

**★ MODEL MATRIX (Nov 13 2026) — so none of this rests on one model or one family.** Per unit:
`irq_rate.sh` interrupts per device period event at 512- and 64-frame periods, the ramp, and the
short-lead client at a 32-frame period.

| model | ch | per event @512 | @64 | vec1-3 | ramp | `leadplay` @32 |
|---|---|---|---|---|---|---|
| Red 8Line | 60 | 1.011 | 1.001 | 0 | 8/8 | 719968/720000, breaks=0 gaps=1 |
| Clarett 8PreX | 28 | 1.009 | 1.001 | 0 | 9/9 | **720000/720000, breaks=0 gaps=0** |
| Clarett 8Pre | 20 | 1.010 | 1.001 | 0 | 9/9 | **720000/720000, breaks=0 gaps=0** |
| Clarett 4Pre | 20 | 1.006 | 1.001 | 0 | 9/9 | **720000/720000, breaks=0 gaps=0** |

Both product lines and both Thunderbolt controllers (the Red's JHL6540, the Claretts' DSL2210) give
the same answer, with the idle baseline at 0.0/s once fcp-server is stopped. Every ramp leg was
sample-exact at 48/96/192 kHz with the S/MUX-removed tails silent. Note that all three Claretts are
CLEAN on the short-lead client while the Red leaves one gap, which fits its halved TX fragment (64
channels at 16 frames exceeds the engine's per-descriptor read cap, so its fill geometry differs);
worth remembering rather than acting on. **The 2Pre, run on the 0.4.0 release build (Nov 16 2026,
laptop, srcversion `4A852112C3A0973841A5262`), passes too:** 1.005 and 1.001 interrupts per period
event at 512- and 64-frame periods, vec1-3 zero; the ramp 9/9 sample-exact at 48/96/192 kHz with the
position-4 loopback pair on ch 13/14; `tx_guard_lead.sh` breaks=0 in all 12 runs, aplay gap-free, and
`leadplay` at a 32-frame period gaps 10/21/8 per 20 s run against the client's own reported
underruns 10/20/8 (non-RT; one gap without a matching underrun, unexplained). All five models pass.

**Lesson:** when a change alters *how often* something runs, the test is a client whose timing
depends on that rate. Everything that measures correctness of the audio path passed while playback
was being silently discarded.

### The device cannot see a stale TX ring, which is why the driver must prevent it (Nov 13 2026) `[HW — Red 8Line]`

`clarett_engine_run()`'s comment on the servicer's real-time priority — "if userspace preempts the
servicer, the TX fill falls behind and the engine plays stale audio with no XRUN" — turns out to be
literally true of the hardware, not just of ALSA's bookkeeping. **The device raises nothing at all.**

The mechanism was predictable from the vendor's own ring dump (§14): a descriptor is a bare 64-bit
address with bit0 = wrap and bit1 = IRQ, and the vendor sets NOTHING else. With no ownership or
valid bit there is no protocol by which the engine could know whether the bytes at its read position
were refreshed — "these are a lap old" is a host-side fact only.

Measured with a scratch build (never committed) whose `starve_tx_ms` lever stops
`clarett_play_fill()` mid-run, 2 s into a 12 s loopback-ramp stream:

| unit | leg | breaks | gaps | `0x100` at starve end | `0x200` | run summary |
|---|---|---|---|---|---|---|
| Red 8Line | as shipped | **48** | 0 | `0x00000000` | `0x00000000` | `cause=0x80000000 blk0=0x00000000` |
| Red 8Line | IRQ-marked | **48** | 0 | `0x00000000` | `0x00000000` | `cause=0xc0000000 blk0=0x80000070` |
| 8PreX | as shipped | 0 | **24** | `0x00000000` | `0x00000000` | `cause=0x80000000 blk0=0x00000000` |
| 8PreX | IRQ-marked | 0 | **24** | `0x00000000` | `0x80000080` | `cause=0xc0000000 blk0=0x800000c0` |

**The provocation is confirmed quantitatively on both models, and the count is PREDICTED by the ring
geometry.** The Red's TX ring is 2048 frames (256 descriptors x 8, its halved fragment) = 42.67 ms at
48 kHz, so a 2 s starve is 46.9 laps — and the capture shows 48 discontinuities, one per lap. The
8PreX's fragment stays at 16 frames (28 x 4 x 16 = 1792 B, under CLARETT_TX_FRAG_MAX_BYTES), so its
ring is 4096 frames = 85.3 ms, 23.4 laps — and it shows **24**. `ptr0` kept advancing and wrapping
throughout on both.

**Why one shows breaks and the other gaps, which is the same event:** `clarett_play_fill()` copies
only as far as the app has written and fills the rest of the runway with silence, and `aplay` ran
with a 2048-frame ALSA buffer. On the Red, ring == buffer, so the whole runway is ramp and a replayed
lap reads as backwards jumps (breaks). On the 8PreX the ring is twice the buffer, so half the runway
is silence padding and each replayed lap carries one silence run (gaps). Ring-versus-buffer size, not
a difference in the engine.

**The IRQ-marked legs are what make it conclusive.** With the TX ring marked, the playback block was
live and reporting normally (event bit plus an advancing counter — `blk0=0x80000070` on the Red,
`0x800000c0` on the 8PreX, with bit30 of `0x100` alongside) — so it demonstrably COULD have raised
something, and across every replayed lap it raised nothing. No error bit appeared in `0x200` either
(bit30 there would be the overrun analogue). Two models, two ring geometries, one answer.

Side note from the 8PreX run: it reported bit30 as an unknown bit, because `IRQ0_CAUSE_KNOWN` still
excluded it. bit30's meaning is established, so the mask now includes it — the shipped driver cannot
raise it anyway, and a build that marks TX should not be told its own doing is unknown.

Consequences worth carrying: the TX guard (`CLARETT_TX_GUARD_FRAMES`) and `sched_set_fifo_low()` on
the servicer are the whole of the protection, and the regression that `tx_guard_lead.sh` caught in
the interrupt work is the same failure arriving by a different route. Nothing in the telemetry can
flag it; a short-lead client is the only probe.

## 18. `0x108`/`0x10c` route nothing to vectors 1-3; `0x108` is the frames moved per descriptor (Nov 17 2026) `[HW — Red 8Line]`

Scratch build (never committed, srcversion `5ADEFC886D2C9C7A6AE6FC3`): `0x108`/`0x10c` taken from
module parameters at each arm and read back after it, the ISR servicing the mailbox on any vector and
no longer reading the ring cause blocks. Red 8Line, fcp-server stopped, one 4 s 60ch capture per value
pair at 48 kHz, ALSA period 512 / buffer 2048 (375 period events expected). `0x110` read back 1 in
every run.

| `0x108` | `0x10c` (reads back) | periods | vec0 / vec1 / vec2 / vec3 | servicer `gapmax` |
|---|---|---|---|---|
| `0x10` (vendor) | `0x1e70700` (`0x00070700`) | 375 | 385 / 0 / 0 / 0 | 10.7 ms |
| `0xffffffff` (reads `0x3fff`) | vendor | **0** | 9 / 0 / 0 / 0 | — |
| `0x10` | `0xffffffff` (`0x001f1f03`) | 375 | 385 / 0 / 0 / 0 | 10.8 ms |
| `0x0` | vendor | **0** | 6 / 0 / 0 / 0 | — |
| `0x10` | `0x0` | 375 | 386 / 0 / 0 / 0 | 10.7 ms |
| `0x1` | vendor | **0** | 6 / 0 / 0 / 0 | — |
| `0x2` | vendor | 375 | **12332** / 0 / 0 / 0 | **1.47 ms** |
| `0x4` | vendor | 375 | 380 / 0 / 0 / 0 | **2.76 ms** |
| `0x8` | vendor | 375 | 383 / 0 / 0 / 0 | **5.42 ms** |
| `0x20` | vendor | 375 | 382 / 0 / 0 / 0 | **21.4 ms** (every event late) |
| `0x40` | vendor | 235 (client timed out) | 245 / 0 / 0 / 0 | **42.7 ms** |
| `0x80` | vendor | 118 (client timed out) | 125 / 0 / 0 / 0 | **85.4 ms** |
| `0x10` | `0x1e707ff` / `0x1e7ff00` / `0x1180700` / `0xfee70700` | 375 each | ~382 / 0 / 0 / 0 | 10.7 ms |
| `0x10` (closing baseline) | vendor | 375 | 381 / 0 / 0 / 0 | 10.8 ms |

- **Neither register routes any interrupt to vectors 1-3**, over every value tried: vec1-3 stayed at
  exactly zero. Closes the last open item of the MSI follow-up.
- ~~**`0x108` is the period-event interval**~~ **CORRECTED the same night: `0x108` is the number of FRAMES
  the engine moves per descriptor, one per sample-clock tick.** The servicer's `gapmax` column above is real,
  but every event still advanced the counter by one 512-frame period (`stepmax=0x20`), and the arm ->
  stop times in the kernel log scale with the value: 4 s of audio (192,000 frames) took **0.50 / 1.00
  / 2.00 / 4.00 / 8.00 s** at `0x2`/`0x4`/`0x8`/`0x10`/`0x20` (`0x40`/`0x80` were cut off by the client's
  10 s timeout). So the engine completes a descriptor every `0x108` sample periods; 0 or bit 0 (alone or in all-ones) stops
  it; 14 bits are writable (`0x3fff`).
  **What it does to the audio** (digital-loopback ramp, playback 31 -> "PCM 31" -> capture 31, 3 s
  ramp, 6 s capture, 48 kHz, period 512; scratch `ramp_rate.sh`):

  | `0x108` | capture wall time | ramp values captured | within each 16-frame fragment |
  |---|---|---|---|
  | `0x10` (vendor) | 6.0 s | 144000 of 144000 | all 16 frames, sample-exact |
  | `0x8` | 3.0 s | 72000 | frames 0-7 real, **8-15 zero** |
  | `0x4` | 1.5 s | 36000 | frames 0-3 real, **4-15 zero** |
  | `0x20` | 12.0 s | 144000 of 144000 | all 16 frames, sample-exact |

  Per-position counts (scratch `frag_probe.sh`, raw captures kept) settle it: at `0x8` the captured
  ramp reads `2001..2008, 0 x8, 2017..2024, 0 x8, ...`: every fragment carries its first 8 frames,
  consecutive, and the rest stays at the blank the driver writes at prepare; the next fragment
  resumes 8 values on, so the playback side likewise read only the first 8 frames of each of its
  fragments. **`0x108` = frames transferred per descriptor**, at one frame per sample-clock tick, which
  is why the wall time scales with it while the device clock stays at 48 kHz. Above 16 the engine
  still moves only the fragment's 16 frames (no spill: the `0x20` ramp is sample-exact) and then
  waits out the remaining ticks, so both directions run at half rate in step. **Only `0x10`, the
  16-frame fragment the descriptors carry (`CLARETT_FRAG_FRAMES`), is coherent**; the vendor writes
  the same. (The first ramp run's "repeats" were these zero runs, counted as zero steps.)
  Unexplained: the `0x2` run's 12332 interrupts in its 0.5 s, about one per DESCRIPTOR (375 periods x
  32 = 12000), where every other value gave one per period.
- **`0x10c`: writable bits `0x001f1f03`** (bits 0-1, 8-12, 16-20); the vendor's `0x1e70700` stores as
  `0x00070700`, so its top byte and the upper bits of `0xe7` are ignored. No value changed the period
  events or their vector, `0x10c = 0` included. **But after a run that left bits 16-20 at `0x1f` or
  `0x18`, the next stream's handshake lost all four mailbox responses** (`accepted, response not
  landed`, recovered by the lost-response path), and only then did the arm write the vendor value
  back; after `0x07` or `0` it did not happen. The stop does not reset `0x10c`, so the setting
  outlives the stream. So bits 16-20 bear on the mailbox's response landing, not on interrupts.
- **`0x10c` field by field (follow-up the same night, scratch `c10c_probe.sh`/`c10c_probe2.sh`).** Three
  fields: **A** bits 0-1, **B** bits 8-12, **C** bits 16-20 (vendor A=0, B=7, C=7). Per value: the
  loopback ramp (playback AND capture DMA), the driver's Level Meter read every 100 ms during the stream
  and 10 times after the stop (each a `GET_METER`), and the share of non-zero samples on analogue inputs
  1-8 (idle converter noise, 96 % non-zero when healthy) against the ramp channel. A 1 s capture at the
  vendor value follows each test value, because the stop does not reset `0x10c` and a bad value
  otherwise carries into the next stream's setup commands (which is what failed the first run's C=`0x10`).

  | value | A | B | C |
  |---|---|---|---|
  | 0, 1, 3, 7 | pass (0-3) | pass | pass |
  | `0x8`, `0xf`, `0x1f` | — | **ramp lost** | **ramp broken + responses lost** |
  | `0x10` | — | pass | pass |

  - **B = the device's READS from the host (playback):** analogue inputs stay at 96 %, the ramp channel
    drops to 11-34 % non-zero, every mailbox response lands.
  - **C = the device's WRITES to the host (capture and mailbox responses):** 28 `GET_METER` responses
    "not landed" in every bad stream and 4 more after the stop; the ramp channel stays ~97 % non-zero but
    breaks tens of thousands of times (old ring contents surviving failed writes, as far as can be told).
  - Bit 3 set is what fails, in both; bit 4 is writable but changes nothing (`0x10` = 0).
  - **Guess, not established:** transfer-size limits as log2 bytes, the vendor's 7 = 128 B. The link runs
    **MaxPayload 128 B** (DevCap 512, DevCtl 128) with **MaxReadReq 512 B**, so C failing at 8 (256 B
    writes over a 128 B link) fits; B failing at 8 (256 B reads, under the 512 B read limit) does not, nor
    does bit 4 being ignored. No AER error was recorded anywhere up the chain (the endpoint's error
    reporting is off, so that proves little). Raising the link's MaxPayload to 256 and retrying C=8 would
    test it.
- **`0x110` is the engine's run control, not an interrupt arm.** Written 7 at arm (after everything else)
  and 0 at stop by both the vendor and us; DMA starts on the 7; the 0 raises the `0x100` stop class (bits
  0-3) once per stop. It reads back **1** after every 7, so only bit 0 is stored (or bits 1-2 are
  self-clearing strobes). Not varied tonight.
- **`0x220`/`0x320` are live ring-position counters, in 16-frame units** (scratch build sampling both
  every 50 ms while armed, each read twice back to back; Red 8Line, 60/64ch). Zero before the arm and
  right after it, then counting: `0x320` steps 153.0 per 51 ms sample at 48 kHz (5967 in 1.989 s =
  **3000/s = one per 16 frames**) and 306 at 96 kHz (6000/s), wrapping at 8 bits; `0x220` is the same
  count wrapping at **7 bits** (`0x220 == 0x320 & 0x7f` in every sample). 256 x 16 = 4096 frames is the
  Red's capture ring and 128 x 16 = 2048 frames its playback ring, so each is that ring's position in
  16-frame units. Same counter as the period counter `0x300` latches in its low byte
  (`CLARETT_CTR_FRAMES` 16, modulo `0x100`), here readable without clearing anything: back-to-back reads
  differ by at most 1, never reset. Unaffected by what the host does: identical with capture only,
  playback only, duplex, a direction joining mid-stream, and a capture client frozen for 1 s.
  **Clarett 8PreX, the same probe (28ch, both rings 4096 frames):** `0x220` == `0x320` in all 100
  samples, both wrapping at 8 bits (reaching `0xff`), as predicted from the ring lengths. Same rates
  (153 per 51 ms at `0x10`, doubling at `0x8`), and `0x320` stayed 0..63 ahead of the latched counter:
  less than one period again, since arecord's default period on this card is 1024 frames (64 units)
  against the Red's 512. `+0x18`/`+0x1c` read TX `0x11`-`0x17`/`0x10`, RX `4`/`4` (Red: TX
  `0x17`-`0x19`/`0x16`, RX `9`/`9`), so they differ by model as well; still undecoded.
  **`+0x18` = a current level, `+0x1c` = its MINIMUM SINCE THE LAST READ, reset to `0x1ff` by the read
  (8PreX, duplex 3 s at 48 and 96 kHz; the sampler reading `+0x1c`, `+0x1c` again at once, `+0x18`, then
  `+0x1c` a third time, every 50 ms, both rings).** In every sample the first `+0x1c` read was at or one
  below the current `+0x18` (TX 48 kHz: level `0x11`-`0x13`, minimum `0x10`-`0x11`; 96 kHz: level
  `0xc`-`0xf`, minimum `0xb`; RX `4`/`4` throughout), and the immediate second read was **`0x1ff`**; the
  third, after the `+0x18` read, was `0x1ff` or already a fresh minimum (`4`, `0xc`). So `0x1ff` idle is the
  reset value of a low-water mark with nothing measured yet, and the vendor's 10 Hz order (`0x21c` then
  `0x218`, `0x31c` then `0x318`) reads the low-water mark of the last 100 ms, then the level. What the level
  counts is still open: it falls at the higher rate, sits steady on capture, differs by model (Red
  TX `0x17`-`0x1b`, RX 9; 8PreX TX `0x11`-`0x13`, RX 4); fragments the engine holds ahead (a prefetch or FIFO
  depth) fits. If so, a playback low-water mark reaching 0 would be the engine's own FIFO running dry,
  the fetch-level error the `0x100` work could not provoke or see; the driver could report it.
  **Red 8Line, the same run: identical behaviour** (59 samples per rate, first `+0x1c` read always <= `+0x18`,
  the immediate second read `0x1ff` in 59/59 and 58/59, capture's in 59/59): TX level 23-25 with minimum 22
  at 48 kHz, 18-27 with minimum 18 at 96 kHz; RX 9/9. No unit makes the two models agree:

  | | TX min 48k / 96k | RX | TX descriptor | RX fragment (slot) |
  |---|---|---|---|---|
  | Red 8Line (64/60ch) | 22 / 18 | 9 | 8 frames, 2048 B | 3840 B (4096) |
  | 8PreX (28/28ch) | 16 / 11 | 4 | 16 frames, 1792 B (2048) | 1792 B (2048) |

  (as frames, 176 against 256; as slot bytes, 44 KB against 32 KB).
  **Starving the engine names `0x100` bit 4, and the level is not the period's or the read size's (Red
  8Line, duplex ~2 s per leg, scratch `lvl_probe.sh`):**

  | leg | vec0 interrupts | TX level / low-water mark | RX | `0x100` |
  |---|---|---|---|---|
  | `0x108` = `0x10` (control) | 251 | 23-30 / 18-22 | 9 / 9 | — |
  | `0x108` = `0x4` (engine 4x) | 780 | 23-28 / 22 | 9 / 9 | — |
  | **`0x108` = `0x2` (engine 8x)** | **50804** | **0 / 0 in 40 of 41 samples** | 9 / 9 | **bit 4** |
  | `0x108` = `0x10` again | 203 | 23-26 / 22 | 9 / 9 | — |
  | `0x10c` B = 0, 1, 3, 7 | 203 each | 23-29 / 22 | 9 / 9 | — |
  | period 64 / 256 / 1024 | 1552 / 394 / 107 | 23-31 / 22 | 9 / 9 | — |

  - **Bit 4 of `0x100` = the playback engine ran dry.** It appears exactly when the TX level reaches 0, and
    with it the interrupt storm the first sweep saw: ~25,000/s, one per descriptor at 8x (3000 x 8 = 24,000
    descriptors/s). At 4x the engine keeps its level and raises one interrupt per period as usual. This is
    the fetch-level error the `0x100` work could not provoke (the TX-starvation test starved the HOST side,
    which the device cannot see). Capture held 9/9 throughout; a capture counterpart (bit 5?) is untested.
  - **The level does not depend on the period size, on field B, or on the engine running 4x**: TX low-water
    mark 22 in every such leg, RX 9. It does drop at 96 kHz (Red 18, 8PreX 11) and differs by model, so
    it is set by something else; still undecoded.
  **It is the engine's descriptor position, aligned with the driver's (follow-up, the sampler also
  logging the counter the servicer last latched from `0x300` and the frame clock built from it; 2 s
  captures at `0x108` = `0x10`, `0x8`, `0x10`):** at `0x10`, `0x320` minus the latched counter stayed
  in **0..31 in all 120 samples**, i.e. ahead of the last period event by less than one 512-frame period
  (32 units) and never behind, so it is the live position within the ring the driver's sample-exact
  frame clock describes. At `0x8`, where the engine moves descriptors twice as fast as the sample
  clock, `0x320` stepped **306 per 51 ms (6000/s, 50 mod 256)**, twice the 48 kHz rate, and stayed
  0..31 ahead of the latched counter: it counts the engine's descriptors, not the sample clock. So it is
  the ring buffer's hardware pointer in 16-frame units, readable at any moment without clearing
  anything; the driver currently knows its position only as of the last period event.
  **Correction:** an earlier read of these only at the stop, at a few stream lengths, took the values
  (`0x41`, `0x61`, `0xc1`...) for flags; they were snapshots of this counter.
  In the same samples `+0x18`/`+0x1c` barely move while streaming (TX `0x17`-`0x1b`/`0x16`, RX
  `0x9`/`0x9` at 48 kHz; TX `0x12`/`0x11`-`0x12` at 96 kHz), so they are NOT ring positions as recorded
  earlier; a prefetch or FIFO depth fits better. Idle, `+0x1c` reads `0x1ff`. Not decoded.
- **First sighting of `0x100` bit 4** (named above: the playback engine running dry), during the `0x108 = 0x2` run (`stream-cause: 0x100=0x00000010`).
  Bits 4/5 had never been seen in ~150k vendor reads; it appears when the event interval is shorter
  than the period the descriptors mark (descriptors there completing 8x faster than they fill). Meaning unknown.

## 19. A live ALSA position from `0x220`/`0x320` (Nov 18 2026) `[HW — Clarett 2Pre, Red 8Line]` `[DRIVER — snd-clarett e0a3732]`

Until now `.pointer` reported the frame clock as of the last period event, so ALSA saw both directions move
a whole period at a time. §18 showed the ring position is readable at any moment, so the driver now reports
it between events.

**The 2Pre's counters behave as on the Red and 8PreX** (scratch build sampling `0x220`/`0x320` at the
servicer's own read point, ~1000 samples/s, 7 legs: capture, playback, duplex at 48 kHz with 64/512/1024-frame
periods, duplex at 96 and 192 kHz; ~20,000 samples):
- both counters 8 bits wide (OR of every value `0xff`; the 2Pre's rings are 4096 frames each);
- the lead over the counter the servicer last latched from `0x300` runs from 0 to exactly one period and
  never beyond: 0..4 units at a 64-frame period, 0..31 at 512, 0..64 at 1024. The value at one full period
  is a boundary passed between the `0x300` read and the position read, whose event the next pass collects;
- the driver's frame clock (in 16-frame units) equalled the latched counter in every sample, so the
  position lines up with the buffer;
- never backwards; `0x220` equal to `0x320` in practice (once one unit apart, at 192 kHz);
- the two extra reads cost ~9 us on average, 25 us worst, on the laptop.

**Design.** The read cannot go in `.pointer` (atomic context, stream lock held, and on the ASRock a single
MMIO read can stall 42 ms). It goes where the servicer already reads the device: every period event and at
least every `CLARETT_SVC_FALLBACK_US` (1 ms). The lead over the latched counter is taken modulo the ring
length in units (power of two; 7 bits on the Red's 2048-frame playback ring) and placed on the frame clock
as `pcm_frames + lead * 16`. A failed read or a lead over one period is ignored.
- **Playback:** `play_live`, monotonic, reported by `.pointer`. The fill and the START check count from it:
  counting from the last event instead would read an app legitimately writing up to a buffer past the live
  position as an underrun, and queue silence (caught in review before testing).
- **Capture:** the ALSA buffer only holds what the driver has copied, so the servicer copies from the RX ring
  up to the live position (`cap_drained`), and each period event copies only the rest. `.pointer` reports
  `cap_drained`, published with release/acquire against the copy (no stream lock between events).
  A new capture stream starts copying at its own frame 0.
- Granularity: ~48 frames at 48 kHz (the 1 ms read cadence in 16-frame units).

**Tests, 2Pre, laptop (srcversion `4F4C2C9A615AA794D7F15F4` playback only, `02B572B40C25A62FD72D3E4` both):**
- `hw_ptr` sampled from `/proc` every 0.5 ms at 48 kHz: playback, then capture, step by 48 or 64 frames at
  1024- and 256-frame periods (was one period per step).
- **Capture data between events is real.** A client polling `snd_pcm_avail()` every 250 us and reading
  whatever was there (`livecap.c`, scratchpad) kept the loopback ramp sample-exact at 48 kHz (periods 1024,
  256, 64), 96 and 192 kHz, with thousands of reads shorter than a period, down to single 16-frame units.
  So when `0x320` counts a unit, its samples are in memory; no one-unit holdback is needed. Method trap: the
  first run used `snd_pcm_avail_update()`, which does not ask the driver, and at 48 kHz saw only whole
  periods; `snd_pcm_avail()` does.
- `loopback_order.sh` 9/9, `SHORT=1 duplex_stress.sh` 0 failing parts (churn 10/10), on both builds.
- `tx_guard_lead.sh`: breaks 0 in every run on both builds. Short-lead client (leadplay, period 32, 48-frame
  lead), 10 reps x 20 s each, interleaved A5 B5 A5 B5: gaps 64 (playback only) against 22 (both), leadplay's
  own underruns 62 against 17, frames lost 96 and 128 of 9.6 M. The gaps follow the non-RT client's
  underruns, so neither build tears anything and the capture copy costs nothing measurable. (A 3-rep run
  had shown 0/0/0 for the playback-only build: a quiet stretch.)

**Committed Nov 18 2026** (snd-clarett `e0a3732`; the built tree is srcversion `ADD4F6438EEBBD2A8809875`).

### The Red 8Line passes; on the ASRock, a stall at stream start loses the first ring, with or without it (Nov 19 2026) `[HW — Red 8Line]`

The scratchpad tools are now in `tools/bench/` (`livecap.c`, `live_cap.sh`, `load_build.sh`, `lead_ab.sh`,
`start_stall_ab.sh`). A = `5788d74`, the commit before the live position (srcversion
`385CEC8B7E7B99F160966BE`); B = the committed tree (`ADD4F6438EEBBD2A8809875`). Ramp playback 31 ->
"PCM 31" -> capture 31, which survives every speed.

**Laptop (no stall), both builds back to back, B is clean:**
- `live_cap.sh`: B 7/7 sample-exact at 48/96/192 kHz (periods 512/128/64, buffer 4 periods), nearly every
  read shorter than a period, down to 16 frames. A 6/7, reads a whole period long; its one failure (192 kHz,
  period 128) was the client recovering from an xrun.
- `loopback_order.sh` 8/8 and `SHORT=1 duplex_stress.sh` 0 failing parts (96/192 kHz sustained
  sample-exact, churn 10/10) on both.
- `tx_guard_lead.sh` on B: breaks 0 in all 12 runs; leadplay at period 32 lost 0/0/2 frames-runs, matching
  its own 2 underruns (the Red's earlier baseline was gaps=1).
- 360 servicer windows, none with a gap of 40 ms or more (worst 23 ms); no driver warnings.
So the 7-bit playback position and the Red's 2048-frame ring work as on the 2Pre.

**ASRock X570 Creator, same unit.** B's first runs failed on many 96/192 kHz legs with one signature: the
first ~1000-1500 frames of playback lost, the rest of the first 2048-frame ring (the whole TX ring and the
default buffer) delivered, then 0.2-1 s of silence, then the stream correct to the end; aplay reported no
xrun. Everything that ran without a stall passed (48 kHz live capture to 16-frame reads included). A first
back-to-back run (A then B, telemetry on) looked like B provoked it: first-window stalls (gap >= 40 ms)
**A 0 of 30, B 7 of 30**, every failure on one of them. The interleaved run settled it
(`start_stall_ab.sh`, 6 blocks A B A B A B x 12 four-second duplex streams rotating 48/96/192 kHz; B was a
scratch build timing the new read, srcversion `6B0D7AB1B976F5903A72E2B`, not committed):

| build | streams | failed | failed with a stall | first-window stalls |
|---|---|---|---|---|
| A | 36 | 3 | 3 | 3 |
| B | 36 | 5 | 4 | 8 |

- **A fails the same way:** two 192 kHz streams lost the first 1488 frames, then silence, with a 42 ms stall
  in the first window. The signature is a stall at stream start, not the live position.
- **The stall blocks whichever MMIO read is in flight.** In A it sat in the `0x300` read (`readmax` 42 ms);
  in B, 6 of 8 times in the new `0x320` read (timed 42.0 ms), once in `0x300`, once in neither timed read.
  In B it struck 156-200 ms after the engine started, nearly always on a stream that followed a change to a
  faster rate (A's three: two 192 kHz, one 48 kHz after 192). The register read is a victim, not the cause.
- 8 vs 3 is not significant (p ~ 0.2); with the sequential 7 vs 0 it is a weak lean, not a result.
- One B failure had no stall (96 kHz): the ramp complete, a silent gap inside it, no frames lost; aplay's
  stderr had been discarded, so it was not shown to be a client underrun. The script now logs aplay
  underruns.
- Why a stall at start loses the first ring: at 96/192 kHz the 2048-frame TX ring is 21/11 ms, so a 42 ms
  stall lets the engine lap it before the servicer has filled past the prefill. Not investigated further.

**8PreX, 8Pre and 4Pre, laptop, B, the same day: all pass.** `live_cap.sh` 6/6 each (periods 1024/256/64,
reads down to 16 frames), `loopback_order.sh` 9/9 each (loopback 27/28 on the 8PreX, 19/20 on the 8Pre and
4Pre, ADAT 1 on ch 11, S/MUX-removed channels zero), `SHORT=1 duplex_stress.sh` 0 failing (churn 10/10),
`tx_guard_lead.sh` breaks 0 in every run. Leadplay at period 32 lost frames only around its own underruns
(8PreX gaps 3/7/6 vs underruns 3/7/6; 8Pre 45/20/43 vs 43/20/40; 4Pre 28/54/58 vs 27/53/57). The few gaps
without a matching underrun were checked on the 8Pre with an interleaved A/B (`lead_ab.sh`, 10 reps x 20 s
each, no stall windows): gaps without an underrun **A 12, B 3**, frames lost A 400, B 80 of 9.6 M, so B
misses fewer playback deadlines than the build before it. B's client underruns more (44 vs 23): with a live
position its short queue is seen to drain in real time instead of only at period events. The windows over
40 ms in these runs were 44.1 kHz churn cycles with 2048-frame periods (46.5 ms nominal), not stalls.

**Released in 0.4.2** (snd-clarett `6641cf6`, tag `v0.4.2`; srcversion `ADD4F6438EEBBD2A8809875`, the tested
build's, since only comments changed after it).

**Open:** playback START is still anchored one period ahead, which the live position may make unnecessary
(one period off the measured round trip).
