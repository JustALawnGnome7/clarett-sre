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
