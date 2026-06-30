# Clarett 8PreX — Data-Plane RE (PCM DMA streaming)

**Status (June 28 2026):** far past a "plan". Phase 1 (streaming register map) **recovered** from
`clarett_full_init_mute.log` (§3b); Phase 2 (descriptor format + sample layout) **recovered** from live
guest-RAM dumps (§3c); the engine is **implemented and validated** — arms cleanly, DMAs a burst,
descriptors valid (no IOMMU faults), the DMA PTR advances. **But it will not sustain past one ring pass**:
it flags "period 0 ready" and the `0x300` counter never advances, universal across both models. Every
observable host→device surface (BAR0 setup regs, FCP handshake, PCI config, `0x500`/`0x510` enables,
converter-ready status, **and** the DMA-buffer address via `dma_bits=30/31`) has been matched/eliminated.
The black-box MMIO+FCP+config method is **exhausted**; the sustain blocker is below the BAR surface — the
**same off-wire wall the control plane hits** (`spec/clarett-8prex-manifestation-wall.md`, memo
`clarett-dataplane-pcm-findings`). **Environment is ruled out:** the control plane was retested with our
driver in a Fedora guest under the *same* vfio passthrough FC uses and still failed, so it is our-driver-
vs-FC (off-wire bus-master DMA), not host/IOMMU/VM context. (Earlier data-plane notes called this
"environmental" — superseded; both planes share the off-wire/driver-vs-FC root.) Going further needs
bus-level RE (a TB/PCIe protocol analyzer on the live link), **not** host-env work and **not** driver edits. Tags: `[PLAN]` = intended approach; `[HYP]` = hypothesis to confirm; `[TRACE]` = confirmed from a
capture; `[ANCHOR]` = a control-plane fact we build on.

The control plane (mixer/routing/gain/mute/clock/notifications) is reverse-engineered and documented in
`clarett-8prex-control-plane.md` + `clarett-8prex-fcp-transport.md`. **Both planes are now blocked at the
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
and is still untraced — but note the engine streamed in `clarett_full_init_mute.log` after only the §3b
*register* writes (no new mailbox clock command was needed beyond the bring-up we already replay), so
clocking may already be covered by `clarett_arm_device`. Confirm during step 1; if the engine won't
start, capture an FC sample-rate change to find the clock command.
