# Reverse-engineering the Focusrite Clarett Thunderbolt interfaces for Linux — an investigation report

**Devices:** Focusrite Clarett 2Pre / 4Pre / 8PreX (Thunderbolt), PCI ID `1cb5:0002`
**Goal:** a native in-kernel Linux ALSA driver (`snd-clarett`)
**Status:** protocol fully reverse-engineered; **control plane working on real hardware** — a Linux
driver toggles the front-panel preamp/monitor state (LEDs move, relays switch) for the first time.
Data-plane (streaming) work is re-opened and in progress.
**Method:** clean-room — built only from Focusrite's own XML device descriptors, black-box bus
traces, and public `scarlett2`/FCP documentation. **No vendor driver, kext, or `.sys` was
disassembled.**
**Date of report:** July 16 2026

---

## TL;DR

I reverse-engineered the Focusrite Clarett Thunderbolt control protocol to the byte and built a Linux
ALSA driver that reproduces Focusrite Control's (FC) wire traffic **byte-for-byte on every surface a
host can observe**. For months, despite that, the driver did not functionally control the hardware:
control writes completed but never manifested, `GET_DATA` came back empty, and the streaming engine
stalled after one ring pass. Four independent confirmation methods on three platforms (Windows
passthrough MMIO, macOS DTrace, our own driver, WinDbg kernel debug) all agreed the differentiator was
invisible to host-side software — and the investigation was declared at its terminus, "below the
driver".

**That conclusion was wrong, and the way it was wrong is the most useful thing in this report.**

The missing piece was not below the driver. It was *time*. Every "known-good" vendor trace was
captured with MMIO trapping enabled (~20 µs per BAR access, the instrument's cost), and under that
dilation the device's **asynchronous response DMA had always landed** before the driver's trailing
doorbell ack. Our replay issued the identical bytes at native speed and acked within microseconds —
**before the response arrived**. The ack evidently means *"response consumed, buffer free"*: acking a
response that hasn't landed is a protocol violation, and the device answers it by refusing the entire
session with a blanket error from command #0 — which is indistinguishable, from the host, from an
attach-time gate. Every byte-level comparison passed because the bytes were never the problem.

**The fix is one wait:** gate the trailing doorbell ack on the response DMA having actually landed.
With that change, the same command stream that was refused for a year arms the device — every command
answers `error=0` with real data, the 8 KB configuration read returns actual configuration, and
**alsamixer toggles physically move the hardware** (Mode/Air LEDs change, the input relays audibly
switch).

This report documents the working protocol, the (still valid) eliminations, the timing analysis that
broke the case, and what remains — so the next person reverse-engineering a traced protocol checks
the acknowledgement semantics *first*.

---

## 1. The hardware

- PCI ID **`1cb5:0002`**, class *Multimedia audio controller*.
- A **single 64 KB MMIO BAR0** is the entire register interface: an FCP command mailbox plus DMA
  control. **Audio samples and command responses both move by bus-master DMA, not through the BAR.**
- **4 MSI vectors**, MSI-driven. PCIe Gen1 x1, tunnelled over Thunderbolt.
- An **FPGA-based Thunderbolt front-end**; firmware has an application segment and an FPGA segment.
  The FPGA **self-boots from flash** — no host-side firmware upload exists (proven in §6).
- The Clarett Thunderbolt models **share the PCI and subsystem IDs** and the protocol; they must be
  disambiguated by a device query, not by PCI ID.

The investigation spans **three units of the line — the 8PreX (reference model), the 2Pre, and the
4Pre** — plus Focusrite's XML descriptors for the 8Pre. The **8PreX** is the primary reference; the
**2Pre** and **4Pre** independently confirm the findings on real, physically different hardware. The
4Pre in particular contributed a live Focusrite Control **boot-to-stream** capture that confirmed both
the control-plane encoding *and* the streaming ring structure (see §2 and §3), so the conclusions are
not artifacts of a single device. The working-control result (§8) is confirmed on the 2Pre.

The Clarett Thunderbolt interfaces are **self-powered** (their own DC adapter) and **cannot** be powered over
Thunderbolt bus power — a detail that matters below (§6): unplugging the Thunderbolt cable does **not**
reset the device.

---

## 2. What is fully solved: the FCP control protocol

The Clarett Thunderbolt line speaks the same protocol family as the in-kernel `scarlett2`/`fcp`
drivers (FCP = Focusrite Control Protocol). The USB Clarett is handled by `scarlett2`; the
Thunderbolt Clarett is **not** in-tree — but the protocol ports cleanly, and `scarlett2` plus
Focusrite's own USB Clarett XML descriptor are a verified interpretation reference. **Encodings are
per-model** — opcodes/offsets/enums are never copied across models; the 8PreX's numbers come from its
own XML descriptor and are cross-confirmed against the live trace.

### Transport (confirmed from the boot-init trace, completed by §8)

- **FCP request mailbox @ BAR0 `0x8020`.** Header: `cmd` @ +0 (bit 31 = execute flag, low bits =
  opcode), `size|seq` @ +4 (size in low 16, sequence in high 16, incrementing), `error` @ +8, pad
  @ +12, `data[]` @ +0x10. Matches the `scarlett2` header layout.
- **Doorbell @ `0x408`:** write `1` = submit, write `2` = ack. **The ack's semantics are the heart of
  this report: it does not mean "completion observed", it means "response consumed, buffer free" —
  it must not be written until the response DMA has landed (§8).**
- **Completion:** the DONE bit `0x20000000` in IRQ cause register `0x100` (MSI vector 0). Cause
  registers `0x100`/`0x200`/`0x300`/`0x400`/`0x500` are one block per vector, read-to-clear.
- **GET responses arrive via DMA, not the BAR.** The device DMAs the result into a host buffer whose
  bus address the host programs at `0x410` (low 32) / `0x414` (high 32) — **asynchronously, shortly
  *after* the DONE bit is raised** (typically 60–170 µs after submit; up to ~700 µs for activates).
  The response carries its own 16-byte echoed FCP header (opcode echo @ +0, size @ +4, seq echo @ +6,
  **status word @ +8**, data @ +16). This is why MMIO traces can't see GET payloads.
- Other registers: `0x000` caps, `0x010`/`0x014` serial, `0x104` IRQ enable (`0xf000003f`),
  `0x8000..0x801f` read-only firmware-info header.

### Opcodes

- **Confirmed (identical to `scarlett2` values):** `GET_DATA=0x800000 {u32 off, u32 len}`,
  `SET_DATA=0x800001 {u32 off, u32 len, data}`, `DATA_CMD=0x800002 {u32 activate}`,
  `GET_METER=0x001001` (the GUI polls this continuously — the "noise" in every trace).
- **Device-specific, init-only:** `CONFIG_PUSH=0x005000` (a per-id *name query* — a working device
  answers each with the port's name string), subsystem enable `0x000001`, `0x6000-2`, `0x7000-3`,
  `INIT_2=0x000002`, `SET_MIX=0x002002`, `SET_MUX=0x002000`.

### The control-plane model (the key result)

A configuration change is:

```
SET_DATA{offset, len, value}   // offset, len, value from the XML per control
DATA_CMD{activate}             // activate code from the XML per control
```

The encoding is confirmed **against FC's live traffic** — our bytes match FC's byte-for-byte on, e.g.,
master mute (offset 24, activate 2) and master volume (stereo, offsets 32/33, activate 1), preamp Air
(offsets 174/175, cmd 7), and Mic/Inst mode (offsets 166/167, cmd 6). Output gain is a 7-bit
**attenuation** code equal to |dB| exactly, 1 dB/step (`0x00` = 0 dB unity … `0x7f` = −127 dB).

The encoding **ports across the line**: the input-control block, previously known only from the XML,
was confirmed **byte-for-byte against a live 4Pre Focusrite Control capture** (toggling the 4Pre's
Analogue inputs). The 4Pre shares the 8PreX's input-control offsets with a smaller channel count — a
second physical device agreeing with the reference model.

The bring-up sequence is also recovered: a freshly power-cycled device rejects `GET_DATA` until the
host replays the vendor init — `CONFIG_PUSH` ×~122, subsystem enables, count queries, an 8 KB config
read-then-writeback, and `SET_MIX`/`SET_MUX`. Our driver replays this at probe (`clarett_arm_device`).

**Everything above is proven correct at the encoding level.** The problem was never the protocol —
and since §8, this model demonstrably drives the hardware.

---

## 3. The wall as it stood: correct traffic, no effect

*(Sections 3–7 are the historical record of the investigation's long middle. Every negative in them
is still a true fact — they eliminated real hypotheses — but the conclusion they pointed to was
wrong, and §8 explains why the error was systematic rather than careless. Kept intact so nobody
repeats the dead ends.)*

With a correct bring-up, the driver:

- loads and probes on real hardware, registers its ALSA mixer controls;
- round-trips the FCP mailbox (`done=1`) and fires async notifications;
- **yet did not functionally control the device.** Control writes completed but the front-panel state
  was frozen. `GET_DATA` returned `size=0`, so the mixer "get" was an in-memory shadow. Later
  instrumentation sharpened this: the device DMAed a well-formed response header for **every**
  command — valid or nonsense — carrying **status 3 and size 0**, without echoing the request
  sequence number: a blanket, session-level refusal applied from command #0.

The data plane told the same story on the streaming side: the engine arms cleanly, DMAs a burst with
correct descriptors (no IOMMU faults, the pointer advances), **but won't sustain past one ring pass** —
the period counter never advances. The streaming ring structure was reverse-engineered from FC
boot-to-stream captures across **three models** — 2Pre, 4Pre, and 8PreX — which independently
validate the same descriptor invariant at each model's own (often asymmetric) channel geometry, so the
data-plane finding, too, is not specific to one unit.

Crucially, this was **not** an environment problem. Our driver was run in a Fedora guest with the
device passed through — the *same class of path* FC uses (a Windows guest + passthrough) — and it
**still failed**. FC-in-passthrough works; our-driver-in-the-same-passthrough fails. That ruled out
host / IOMMU / VM / bare-metal as the cause and localized the difference to **our driver vs.
Focusrite Control**.

---

## 4. The elimination: every host-observable surface matches FC

We can observe and diff against FC on two trace surfaces, and we built an in-driver tracer so our own
driver's traffic decodes through the identical tooling.

| Surface | Finding |
|---|---|
| **FCP init preamble** | `READ_SEG`, `GET_7.x`, **`CONFIG_PUSH` ×42**, `GET_6.x`, enables ×8, count queries, second pass — all present, same order. **Match.** |
| **8 KB config read + writeback** | 9× `SET_DATA`, offsets `0xc8..0x2088`, 8192 bytes, **content byte-identical** — kills the "stale static writeback" theory. **Match.** |
| **`SET_MIX` ×16, `SET_MUX` ×3, `DATA_CMD{5}`** | Identical. **Match.** |
| **Per-toggle bytes** | `SET_DATA{off,1,val}` + `DATA_CMD{activate}`, byte-for-byte identical to FC on every control tested. **Match.** |
| **Control registers** | `0x104`, `0x500`, `0x510` identical; only `0x410`/`0x414` differ (each side's own DMA-buffer address — expected). **Match.** |
| **Completion discovery** | FC reads **all five cause blocks `0x100`–`0x500` in lockstep** per command, `0x100` exactly twice; our cycle was aligned to it (MSI-paced, same read counts, trailing ack). **Match.** |
| **PCI config space** | Complete pre-doorbell activity reconstructed and matched byte-for-byte — values, order, even the write-1-to-clear events and the INT-line scratch byte. **Match.** |
| **Our only "extras"** (an injected clock command, probe seeding writes, a per-command mailbox register read) | Gating them off makes our stream an **exact subset** of FC's — still no manifestation. **Not the cause.** |

What **neither** trace can see: **bus-master DMA to/from guest RAM.** The `vfio_region` trace only
disables the BAR mmap; DMA is untouched and never appears. That blind spot is where the wall was
presumed to hide — correctly, in a sense, but not in the way anyone expected (§8: the invisible thing
was not a payload, it was an *ordering constraint* between a DMA landing and a BAR write).

---

## 5. Confirming the boundary — four independent methods

Because the black-box MMIO method is blind to DMA, the boundary was confirmed from other angles.

1. **Windows-in-passthrough MMIO (vfio, `x-no-mmap`).** Our traffic matches FC byte-for-byte on every
   surface (§4). Establishes that the *observable* protocol is right.

2. **macOS DTrace of the shipping driver.** The device also runs on an Apple-Silicon Mac, so I DTrace'd
   the vendor's user-space→kext path. Result: the vendor's user-space commands are **byte-identical to
   ours**, and the device returns **rich real data** (caps/firmware/serial/config) to the **same**
   `GET_DATA` that returned empty for us. But DTrace **could not see inside the kext** (a stripped,
   address-redacted release binary — no usable function-boundary tracing). Confirmed the wall; could
   not cross it. *(In hindsight: the kext's internal ack sequencing — exactly the thing DTrace could
   not see — is where the answer lived.)*

3. **Our own Linux driver.** Replayed the FC-identical stream, and failed — the concrete symptom the
   whole project was about.

4. **WinDbg kernel-debug of the shipping Windows driver (`FocusritePCIe.sys`).** *Strictly data
   observation* — break only on symbolicated Microsoft DMA APIs, attribute the caller by **module
   range**, never disassemble or step into the vendor `.sys` (clean-room). Breakpointing
   `MmAllocatePagesForMdlEx`, `WdfCommonBufferCreateWithConfig`, and `HalAllocateCommonBuffer` showed
   the vendor's **entire init DMA footprint**:

   > **2 × {16 KB scatter-gather descriptor common buffer + 2 MB scattered sample MDL}** (the two
   > stream blocks) **+ 1 × 4 KB response common buffer** — all **cached-coherent, 64-bit, no address
   > ceiling** (`High = 0xFFFFFFFFFFFFFFFF`, `Total = 0x200000`, `Cache = MmCached`).

   This is **attribute-for-attribute equivalent to our driver's** `dma_alloc_coherent` / descriptor
   layout. And it programs **nothing extra to the device at init** — only `0x410`/`0x414` (the response
   buffer); the `0x210`/`0x310` engine arm is stream-time, and there is **no mailbox pointer-push**.

All four methods were right about what they measured: the vendor's driver-level construction *is*
equivalent to ours. The error was the inference that nothing host-side remained — see §8.

---

## 6. The cold-boot investigation (the last state-dependent lead) — negative

A remaining hypothesis: maybe a **freshly powered-on** device does something once, at first bring-up,
that a warm device (already initialized by a previous session) no longer shows — e.g. a firmware/FPGA
upload over DMA.

First, the correct definition of "cold": a **VM reboot keeps the device's own DC power on**, and
because the 2Pre/4Pre/8PreX **cannot be bus-powered**, unplugging the Thunderbolt cable *also* leaves it
warm. Config space reports no software reset (`FLReset-`). Only a **device DC power-cycle** is
genuinely cold. (Correcting this misconception was itself important — several earlier "cold" results
had actually been warm.)

On a genuinely cold, DC-power-cycled 2Pre, all three observable surfaces came back **negative**:

- **Mailbox trace matches warm** — no `INIT_1`, no `REBOOT`, no firmware-sized burst. (Confirmed
  twice, including a **driver-only** capture with no FC app running, so there is no meter-poll noise.)
- **WinDbg DMA-*allocation* footprint matches warm** — same 2×{16 KB + 2 MB} + 4 KB, siblings do zero
  DMA.
- **WinDbg DMA-*contents* are zero.** I captured both 2 MB sample-MDL pointers *at allocation*, then
  re-read their virtual addresses at the **freshest post-init instant**, and `db`-dumped + searched the
  **full 2 MB** of each buffer for firmware signatures: `00 09 0f f0` (Xilinx `.bit` header),
  `aa 99 55 66` (bitstream sync), and the `"tb_top"` design-name marker. **All six searches empty; both
  buffers all-zero.** (The warm baseline validated the method: one buffer zero, the other holding
  24-bit-in-`S32_LE` audio samples.)

**Firmware-over-DMA is disproven** — the FPGA self-boots from flash — and no once-per-power-cycle
bring-up step is missing from our init.

*(Method note for anyone reproducing the WinDbg pass: a `gu` **inside** a breakpoint command is fatal
— WinDbg silently skips the rest of the command block and runs the target on, capturing nothing. Use a
**halt-only** breakpoint and run `gu` / `r @rax` manually at each halt. And the MDL's `MappedSystemVa`
is `0x1` at allocation time — it's only populated post-init, so you must record the PMDL pointer and
re-read the VA later.)*

---

## 7. What was ruled out (all still true, and still worth not repeating)

- Environment (host / IOMMU / VM / bare-metal) — our driver failed in the same passthrough FC works in.
- The control encodings — byte-identical to FC and XML-confirmed.
- A missing/incorrect init sequence — the full vendor bring-up is replayed; the 8 KB writeback content
  matches byte-for-byte.
- "The data plane is a prerequisite for control" — FC moves the LEDs at idle with no stream and no
  engine armed.
- MSI enable ordering, completion-discovery style, per-command read counts, PCI config space —
  all matched or made vendor-identical, no change.
- DMA buffer addresses above/below 4 GiB — no dependence, either plane.
- A missing extra DMA region or a mailbox pointer-push — WinDbg shows none exists.
- Firmware/FPGA upload over DMA — buffer contents are zero on a cold device; the FPGA self-boots.
- A cold-vs-warm state difference — negative on all three surfaces.
- A staged per-notification handshake (inspired by the UA Apollo init docs) — refuted: the `0x400`
  register is a 2-bit command-phase status, not an event queue, and FC does no per-bit follow-up.

**Explicitly out of scope / declined:** a Thunderbolt/PCIe **hardware bus analyzer** (ruled out by the
project owner), and **disassembling the vendor driver/kext/`.sys`** (clean-room prohibition).

At this point the investigation was declared at its terminus: "every host-visible surface, warm and
cold, is exhausted; the differentiator sits below the driver." **That declaration is the setup for
the actual finding.**

---

## 8. The breakthrough: what the measurement apparatus did to time

The question that broke the case was not about the device. It was about the instrument:

> Every "known-good" trace was taken with MMIO trapping enabled — ~20 µs per BAR access. The working
> driver *executed under that dilation* in every captured session. Our replay reproduces the trace's
> byte sequence at native speed, ~100 ns per access. For each host action in the transaction cycle,
> ask: **is this action valid the instant the previous MMIO completes, or is it semantically
> conditioned on something the device does asynchronously?** Be especially suspicious of any host
> write whose meaning is an *acknowledgement* — an ack asserts that something finished, and the trace
> can't tell you what that something was, only that in the dilated environment it had always finished
> by then.

Walking the mailbox cycle with that question, exactly one write qualifies: the **trailing doorbell
ack, `0x408 = 2`**. Two candidate meanings: (a) "completion cause consumed" — which we waited for
(MSI-paced, vendor-identical) — or (b) **"response consumed, buffer free" — which we had never waited
for.** And the evidence that (b) was being violated was already on file:

- The device DMAs its response **asynchronously, after** the BAR DONE bit: reading the response
  buffer immediately after DONE catches the *previous* command's response (a race that had already
  invalidated an earlier instrument).
- In every dilated vendor capture, the ack sits **≥242 µs after submit** (measured across all 114
  commands of a cold bring-up) — the response had always landed. The traces are structurally
  incapable of distinguishing "ack unconditionally after the status sweep" from "ack gated on the
  response landing".
- Our native-speed cycle acked **within microseconds of DONE** — before the response landed, on every
  command of every session ever. In one archived capture, command #0's response never arrived at all
  and the cycle acked it anyway.
- A session-level refusal from command #0 is exactly what a violated command #0 produces — the
  "attach-time gate" signature was this masquerade. (Response-landing telemetry later showed
  activates land **631–698 µs** after submit; the old cycle acked those more than half a millisecond
  early.)

**The fix:** before each submit, clear the response header; after the DONE sweep, **poll the response
buffer until this command's echoed opcode appears — only then write the trailing ack.** A response
that never arrives is never acked.

**The result (2Pre, bare metal):** the identical 232-command bring-up that had been refused on every
run for a year answered **`error=0` with real data from command #0** — the device echoes our sequence
numbers (a refusal writes seq 0), `CONFIG_PUSH` returns the port-name strings, the 8 KB configuration
read returns 8 × 1016 real bytes, and the serial/firmware query answers. And the endpoint the whole
wall was named for: **toggling Mode and Air in alsamixer moves the front-panel LEDs and audibly
switches the relays.** A Linux driver controls a Clarett Thunderbolt interface.

**Why every comparison missed it:** the eliminations diffed *bytes* — values, order, counts — and the
bytes were genuinely identical. The differentiator was an **ordering constraint between an invisible
event (the response DMA landing) and a visible write (the ack)**, which the instrument satisfied as a
side effect of its own overhead in every working capture. Byte-identical traffic under a time-dilating
instrument is not identical behavior.

*(Attribution: closed, 3-for-3 deterministic across fresh device power-cycles — two gated runs arm
cleanly with matching latency profiles and no onset variability; a levers-off control run on an
equally fresh power-cycle is refused exactly as before. The landed-gated ack (with the pre-submit
header clear that makes the landing detectable) is the driver's unconditional default cycle.)*

---

## 9. Current state and remaining work

- **Control plane: working.** Bring-up, configuration reads, mixer/preamp/monitor writes with
  physical manifestation, async notifications. The landed-gated ack is the unconditional default
  mailbox cycle (attribution closed — see §8). Next: re-audit the driver paths that were written
  for a refused session (shadow refresh, notification handling, the meter-poll hypothesis).
- **Data plane: re-opened.** The burst-then-stall was attributed to the same "below-driver"
  differentiator — that attribution is now void. The streaming cause blocks are serviced at native
  speed too, so the same class of violation is the first suspect; retesting on an armed session is
  the immediate next step. The descriptor/ring structure is already validated across three models.
- **Multi-model:** the driver is per-model (`2pre`/`4pre`/`8pre`/`8prex`); the working result is
  confirmed on the 2Pre and expected to carry (same protocol, same cycle).

**To the community:** if you have a Clarett Thunderbolt unit, testers will soon be useful — especially
for the models not on this bench. If you are reverse-engineering *any* traced protocol and your
byte-perfect replay inexplicably fails, check §8's question before concluding the magic is below your
reach.

**To Focusrite:** the community is one step (streaming) from a complete native Linux driver for the
Clarett Thunderbolt line. Confirmation of the data-plane ring-advance semantics — or simply a
statement of support and a point of contact — would be warmly received.

---

## 10. Reproducibility & artifacts

The project is a self-contained clean-room record:

- **`spec/clarett-control-plane.md`** — offsets, opcodes, enums, pins, mixer/routing, provenance-tagged.
- **`spec/clarett-fcp-transport.md`** — mailbox/transport framing and the confirmed register map.
- **`spec/clarett-data-plane.md`** — the PCM-DMA RE: method, register/descriptor maps, and the
  engine (re-opened, §9).
- **`spec/clarett-manifestation-wall.md`** — the full elimination record (§§1–7 here, in detail, with
  `[TRACE]`/`[TEST]`/`[CONCLUSION]` provenance tags) and §8, the crossing.
- **`spec/clarett-macos-dtrace-plan.md`**, **`spec/clarett-windbg-plan.md`** — the two cross-platform
  confirmation passes (methods, runbooks, results).
- **`driver/`** — the out-of-tree `snd-clarett` module (control plane + experimental capture PCM).
  The landed-gated ack is the default mailbox cycle; the timing instruments from §8 are the
  `resp_trace` / `mmio_dilate_us` module parameters (`resp_trace` logs per-command DONE and
  response-landing latencies plus the FCP status word — the onset instrument that characterized
  the refusal properly for the first time).
- **`tools/`** — `fcp_decode.py` (trace → FCP transactions), `bar_profile.py` (register-activity
  profiler), `notify_correlate.py` (cause-register correlation).
- **`captures/`** — the trace and guest-RAM captures the conclusions rest on.

**Method summary (for replication):** the device is PCIe-passed-through (`vfio-pci`) to a VM; the BAR
mmap is disabled (`x-no-mmap`) so every MMIO access traps into QEMU and is logged via `vfio_region_*`
trace events, decoded by `tools/fcp_decode.py`. Cross-confirmation used macOS DTrace and Windows WinDbg
kernel debugging, both strictly as **data observation on OS-owned symbols** — no vendor binary was ever
disassembled. And the meta-method that concluded it: when the replay of a traced protocol fails,
enumerate every host action whose meaning asserts that something finished, and ask what the
measurement apparatus did to time.

---

*This report was produced as part of a clean-room reverse-engineering effort. It contains only
interface facts derived from the vendor's own published XML descriptors, black-box bus observation, and
public protocol documentation. No proprietary vendor code was disassembled or reproduced.*
