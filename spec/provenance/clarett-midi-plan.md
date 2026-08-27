# Clarett MIDI — transport reverse-engineering plan (SOLVED)

Status: **DONE Aug 4 2026 — transport SOLVED (register PIO at BAR0 `0x58c`) and `clarett_midi.c` IMPLEMENTED
and HARDWARE-CONFIRMED on the 2Pre.** Decoded from three 2Pre captures (`captures/2pre_midi_{idle,rx,tx}.log`)
via the CH345-on-host rig; driver built and tested on hardware — RX and TX both work, including a full 10-byte
SysEx each way (note `90 3C 7F` and `F0 7D 43 4C 41 52 45 54 54 F7` round-tripped through the CH345↔2Pre DIN
cables). RX interrupt is already enabled by the bring-up (no explicit enable needed); the all-vector ISR drain
caught it regardless of MSI vector.

**★ TX FLOW CONTROL SOLVED AND THE DRIVER WAS SILENTLY LOSING 94% OF A BURST (Aug 27 2026, 2Pre against an
ISA C8X as reference partner). `0x500` bit16 = "TX FIFO can accept a word", and the vendor's pre-write read
was a REQUIRED ready gate, not advisory** — see §"TX flow control" below. Also confirmed this session:
concurrent-with-audio-streaming (both directions, full-duplex PCM), simultaneous bidirectional MIDI, 4 KB
SysEx each way, the ALSA sequencer path, and **`count=1`/`count=2`/`count=3` framing** (a lone program
change and a lone channel pressure both round-trip, closing the "count=2 inferred, not captured" gap).

**★ A SECOND BUG, found by asking what happens on overflow: the 64-byte RX drain bound could strand a
backlog, because a non-empty FIFO does NOT re-interrupt** — the ack is what re-arms it. Fixed
(`CLARETT_MIDI_RX_GUARD` = 256, above the measured 139-byte device FIFO). See §"RX overflow and stall
recovery". Remaining (non-blocking): 8PreX/8Pre/4Pre confirmation, port naming in the maps, and
**a retracted finding worth reading as a method lesson** — `F8`/`FE` appeared to be dropped by the
hardware and were being dropped by `amidi -d`, which needs `-a -c`; nothing is filtered. See below.

## RETRACTED — nothing is filtered; `F8`/`FE` were `amidi` default behaviour (Aug 27 2026)

**`amidi -d` discards Active Sensing (`FEh`) and Clock (`F8h`) from what it prints unless given `-a` and
`-c`.** Every observation in this section's earlier version used `amidi -d` as the receiver, so the
receiver was the filter.

Retest on the 2Pre self-loop, listener as `amidi -d -a -c`:

| sent | received with `-a -c` | received without |
|---|---|---|
| `F8` | `F8` | (nothing) |
| `FE` | `FE` | (nothing) |
| `F8 FE` | `F8 FE` | (nothing) |
| `90 40 7F F8 80 40 00` | all 7 bytes | `90 40 7F 80 40 00` |
| `90 40 7F F8 F8 F8 80 40 00` | all 9 bytes | 6 bytes |

**MIDI Timing Clock and Active Sensing pass through correctly, in both directions, on both devices.
Nothing is filtered anywhere.** `tools/midi_loopback.py` now runs its listeners with `-a -c`
unconditionally, and says so in its docstring, because the default silently corrupts any result
involving those two bytes.

**The method failure, recorded because it was avoidable.** The two bytes that appeared to be dropped were
*exactly* the two bytes amidi provides command-line flags for. That coincidence was the signal and it was
missed. On top of the bad observation a self-consistent explanation was built — content-based rather than
framing (an `F8` inside a `count=3` word died while its word-mates lived), not System Real-Time as a class
(`FA`/`FB`/`FC` passed), therefore a deliberate FPGA suppression of the two *periodic* messages. Every
step of that reasoning was valid and the conclusion was worthless, because the instrument was lying. It
was even written into `driver/README.md` as a user-facing hardware limitation before being caught.

**Rule: when a device appears to filter precisely the set your tool has options about, suspect the tool.
Read the instrument's defaults, not only the flags being passed.** Directly analogous to the x-no-mmap
timing artefact that manufactured the manifestation wall — byte-identical traffic under a lying instrument
is not identical behaviour.

The genuinely unaffected results, all re-confirmed after the tool fix: the TX `MIDI_TX_READY` gate, the
139-byte RX FIFO and the drain bound, `count=1`/`2`/`3` framing, and every throughput, SysEx and
PCM-concurrency measurement — none of those involve `F8` or `FE`.
---

## TX flow control — `0x500` bit16 (Aug 27 2026, 2Pre; the pre-write read is a REQUIRED gate)

**`0x500` bit16 = the TX FIFO can accept another packed word.** It is set while there is room and clears
when the FIFO is full, and **a word written while it is clear is discarded** — silently tearing the
outgoing byte stream. This resolves the standing "TX-ready gate, or advisory?" question left by the tx.log
capture: the vendor's read before every write is load-bearing, and mirroring the read without acting on it
(what the driver did) is equivalent to not reading it at all.

**Why it was invisible until now.** The Aug 4 confirmation sent single messages and a 10-byte SysEx — well
inside the FIFO, so nothing was ever dropped. The failure needs a burst longer than nine words.

**Rig:** Clarett 2Pre (Thunderbolt, our driver, card 2) ↔ Focusrite ISA C8X (USB, `snd-usb-audio`, card 1),
DIN in/out cross-connected. The C8X is an independent known-good MIDI endpoint, so each direction is
attributable: a failure on 2Pre→C8X is our TX, a failure on C8X→2Pre is our RX. Byte streams compared
exactly (not just counted) — `first divergence at byte N` is what showed the stream was *torn*, not merely
short.

**The measurement.** 300 note-on messages (900 bytes) 2Pre→C8X, sweeping the old `midi_tx_pace_us` lever,
which added a blind `udelay` after each write:

| pace (µs/word) | bytes received of 900 | tx wall time |
|---|---|---|
| 0 | **54** | 0.105 s |
| 200 | 243 | 0.165 s |
| 500 | 525 | 0.254 s |
| 960 | **900 (exact)** | 0.393 s |
| 1200 | 900 (exact) | 0.464 s |

960 µs is not a fitted constant: it is one 3-byte word at the DIN wire rate (3125 bytes/s). So the driver
was writing at ~10× the rate the UART could drain and the excess was being dropped on the floor. The
reverse direction (C8X→2Pre, our RX) passed 900/900 untouched throughout, which localised it to TX alone.

**Direct confirmation that bit16 is the mechanism**, via temporary instrumentation logging `0x500` before
each TX write:

| run | `0x500` sequence |
|---|---|
| unpaced (fails) | `0x00ff0000` ×9, then `0x00fe0000` for every remaining word |
| paced 960 µs (passes) | `0x00ff0000` for all 40 words |

So bit16 clears after exactly **nine words accepted** — a 9-word / 27-byte FIFO — and stays clear while the
host outruns the wire, but never clears at all once writes are paced to the drain rate. (Depth was measured
with 3-byte words only; whether the FIFO counts words or bytes is unresolved and the driver does not care.)

**Fix as landed** (`clarett_midi.c`): `clarett_midi_tx_wait()` polls `0x500` for `MIDI_TX_READY` via
`read_poll_timeout` (200 µs interval, 100 ms timeout) before each write. It sleeps rather than busy-waits —
a full FIFO takes most of a millisecond to free a slot, so `udelay` at that scale would burn a CPU. The
wait is taken **before** `snd_rawmidi_transmit()`, so a device that has stopped draining leaves the bytes
queued in ALSA instead of consuming bytes it cannot send; on timeout the work item warns
(`dev_warn_ratelimited`) and pauses rather than looping. `midi_tx_pace_us` is **removed** — it was a
placeholder for exactly this problem and a real gate supersedes it.

Post-fix, TX transmits at the wire rate by construction: 3000 bytes in 1.055 s (3125 B/s = 0.96 s plus
setup).

### Saturated soak — 60 s, both directions at once, with full-duplex PCM

The heaviest case run: MIDI written straight to the rawmidi device nodes so the stream is genuinely
continuous rather than a burst (a blocking write paces itself at the wire rate), **187 500 bytes each way
= 60 s of wire time, both directions simultaneously**, while 14 ch capture and 4 ch playback ran on the
same device.

| measure | result |
|---|---|
| 2Pre TX -> C8X RX | 187500 / 187500, **exact** |
| C8X TX -> 2Pre RX | 187500 / 187500, **exact** |
| writer wall time | 60.1 s against a 60.0 s wire floor |
| PCM capture | 172032044 bytes, exactly the expected size |
| servicer, 33 windows | `late=0 overrun=0 badreads=0 stepmax=0x40` in **every** window |
| servicer run worst case | `gapmax` 21429-21548 µs (nominal 21333), `readmax` 333 µs |
| kernel log | no warnings, errors, timeouts or stalls |

The 60.1 s against a 60.0 s floor is the flow-control gate measured end to end: 0.17% overhead over a
minute of a fully saturated wire, so `MIDI_TX_READY` is releasing on essentially every drained slot rather
than costing a poll interval per word. Both MIDI directions saturated for a minute perturbed the audio
servicer not at all.

### Verification matrix (all exact byte-stream comparisons, 2Pre ↔ C8X)

| test | 2Pre TX → C8X | C8X → 2Pre RX |
|---|---|---|
| 300 / 1000 / 2000 note messages | PASS | PASS |
| SysEx 1 KB | PASS | PASS |
| SysEx 4 KB | PASS | PASS |
| simultaneous bidirectional, 3000 B each way | PASS | PASS |
| during 14 ch capture | PASS | PASS |
| during full-duplex PCM (14 ch in + 4 ch out) | PASS | PASS |
| SysEx 4 KB during full-duplex PCM | PASS | PASS |
| ALSA sequencer path (`aplaymidi` → seq client 24:0) | PASS (1200/1200 B) | — |
| every system byte `F6`/`F8`/`F9`/`FA`/`FB`/`FC`/`FD`/`FE` alone | PASS | PASS |
| `F8` embedded mid-stream; 500 sustained `F8` clocks | PASS | PASS |
| 60 s saturated soak, both ways at once, under PCM | PASS (187500 B) | PASS (187500 B) |

The full-duplex run clocked 40 s / 1876 periods with `late=0 overrun=0 badreads=0`, so the added TX sleep
does not perturb the stream servicer. `rmmod` clean afterwards, no orphaned `clarett-svc` thread, no
warnings in the kernel log across the whole session. TX now runs at the wire rate by construction, which is
itself a check: 3000 bytes took 1.055 s against a 0.96 s floor.

**★ MEASUREMENT HAZARD — a surplus means a SECOND WRITER on the port, and it happened twice.** 1500
bytes sent read back as **1560**, and 150 sent read back as **345** (the expected 150 intact, then 195
extra). **The writer was not identified.** Two candidates, not distinguished: a concurrent manual run of
the bench tool by the operator (known to have occurred at least once in the session, and it also produced
an `amidi` "Device or resource busy" collision on the single input substream), or PipeWire's ALSA
sequencer bridge echoing between ports — both surpluses did follow a PipeWire start. Correlation with the
PipeWire restart is real but weak, and was over-read at first; do not treat it as established.
With PipeWire stopped and nothing else running, every run is exact and a 6 s idle listen yields zero
bytes. **Practice: stop PipeWire, confirm `aconnect -l` shows no `Connecting To:` on the ports under
test, and run one bench instance at a time.** This is why `tools/midi_loopback.py` reports a *surplus*
distinctly — more bytes than were sent can never be a transmit fault.

**Note for the record:** the ISR was rewritten under MIDI by the interrupt-driven-mailbox work on this
branch, and MIDI RX came through it intact — including the case that had been the worry, RX with idle
mailbox traffic now zero. RX does not depend on mailbox interrupts to be drained; its own interrupt fires.

---

## RX overflow and stall recovery (Aug 27 2026, 2Pre)

Two different overflows, and they behave differently.

### Host-side (the ALSA input buffer fills)

Reader `SIGSTOP`ped, 9000 bytes sent from the C8X, then `SIGCONT`. The driver received **all 9000**
(`/proc/asound/card2/midi0` `Rx bytes` advanced by exactly 9000); the stalled reader got 7795, so ~1205
were dropped by the rawmidi core when its buffer filled. That is the core's business — it counts them in
`runtime->xruns`, readable via `SNDRV_RAWMIDI_IOCTL_STATUS` — and `snd_rawmidi_receive()`'s return value is
ignored here for that reason. **Both directions exact immediately afterwards, no warnings, no wedge.**

### Device-side (the ISR does not run and the hardware FIFO fills)

Provoked with temporary instrumentation that skips the drain for N ms while a 6000-byte stream arrives.

- **The device RX FIFO is 139 bytes**, measured three times identically by counting what came back after
  the FIFO had been overflowing for over a second. That is ~44 ms of wire time, so the FIFO alone absorbs
  a stall of that order; the ~42-48 ms platform freeze on the ASRock sits right at the edge of it.
- **There is no RX overflow status bit.** `0x500` read `0x00ff000a` before, during and after ~4370 bytes
  were dropped on the floor — nothing latched. Overflowed bytes are simply gone, and nothing tells the
  host. (For contrast, TX has `MIDI_TX_READY`; RX has no counterpart.)
- **`0x500` low byte bit1 = RX data pending.** `0x0a` while bytes are queued, `0x08` on the read that takes
  the last one, `0x00ff0000` once empty. Refines the earlier "low byte `0x0a` = MIDI RX pending".
- **★ A non-empty FIFO does NOT raise a fresh interrupt — the ack is what re-arms it.** With the drain
  skipped and therefore never acked, MIDI RX went dead permanently: further incoming bytes raised nothing,
  and the path only came back when an *unrelated* vec0 interrupt (a mailbox command from `amixer`)
  re-entered the handler and drained the 139 bytes still sitting there.

**The bug this exposed.** `clarett_midi_irq()` bounded its drain at 64 bytes per interrupt, with the
comment "anything still pending re-interrupts" — which the above disproves. A backlog larger than 64 bytes
left the remainder stranded until some unrelated interrupt happened along. It needs a >20 ms ISR stall to
reach, so it never showed in throughput testing, and in a *continuous* stream each newly arriving byte
re-triggers so it self-heals; the exposure is a backlog at the tail of a burst, on a host that stalls.

**Fix:** `CLARETT_MIDI_RX_GUARD` = 256, above the measured 139-byte FIFO, so one pass always empties it —
the bound is now a runaway guard for a stuck valid bit, not a work budget. Verified directly: the same
stall that previously drained 64 + 64 + 11 across three handler entries now drains **139 in a single pass**
(`guard_left=116`), and RX recovers exactly.

**Not fixed, and arguably not fixable:** bytes lost while the device FIFO is full are unrecoverable and
unreported, because the hardware provides no overflow indication. A host stall longer than ~44 ms silently
truncates incoming MIDI.

### TX under a stalled writer

Benign by construction: the FIFO drains at the wire rate and the gate simply waits. A stall cannot lose
bytes, only introduce a timing gap. `read_poll_timeout` re-evaluates the condition after the deadline, so a
work item preempted past the 100 ms timeout while the FIFO drained still succeeds rather than spuriously
reporting a stall.

**Implementation (`clarett_midi.c`, `snd_rawmidi`, register on ALL models):** TX = pack 1–3 bytes → write
`0x58c`; RX = on the MIDI interrupt, drain `0x58c` → push to rawmidi → write `0x8` to `0x504`. Follow-ups:
which MSI vector carries MIDI; is the pre-TX `0x500` read a required ready-gate; confirm `count=2` framing.

---

## Original plan (method + how the captures were taken) — retained for the record

## Why this is greenfield (what the survey found)

- **The vendor XML says nothing about MIDI** — no MIDI element in `Clarett 8PreX.xml` or any
  `vendor-reference/Devices/*.xml`. Unlike the control plane, there is **no descriptor shortcut**.
  Nothing in the driver, tools, or fcp-server maps touches MIDI today.
- **But the vendor ships a dedicated `FocusritePcieMidi.sys`** — a separate *upper function driver*
  that brokers through the root `FocusritePCIe.sys`, exactly parallel to `FocusritePcieAudio.sys`
  for audio (see `clarett-windbg-plan.md`, `clarett-manifestation-wall.md` §closed-siblings). So MIDI
  is a **distinct data path over the shared BAR/DMA transport**, not an accident of the control mailbox.
- **The USB reference does not port.** On USB Focusrites MIDI is a USB-MIDI *class endpoint*, wholly
  separate from FCP. The TB Clarett has no USB endpoints, so USB-MIDI gives us nothing for transport;
  the FCP/scarlett2 knowledge is about *control*, not MIDI.

## The three candidate transports (mutually distinguishable)

1. **MIDI-over-mailbox** — a new FCP opcode carrying MIDI bytes, poll/IRQ driven.
   *Tell:* a new opcode in the `fcp_decode.py` histogram that appears **only** during MIDI activity.
2. **Dedicated MIDI DMA ring** — its own descriptor/base register pair (analog of audio `0x210/0x310`)
   and possibly a new cause block / MSI vector. *Tell:* new register offsets in `bar_profile.py --new-only`.
3. **Multiplexed into the audio sample stream** — a reserved channel/slot inside the 28ch layout.
   *Tell:* the signature bytes appear inside the audio DMA ring dump, no new opcode/register.

## Method — reuse the existing MMIO-trace instrument

Same rig that cracked control + data plane: the 2Pre passed through to the Windows-10 VM under
`vfio-pci` with `x-no-mmap=true` + `-trace enable=vfio_region_*` on the custom trace-enabled QEMU;
trace lands in `/var/log/libvirt/qemu/<domain>.log`. (Timing is dilated ~20 µs/access under trapping —
irrelevant here: we are mapping *transport*, not the ack-timing that defined the manifestation wall.)

Primary RE target: **2Pre** (user-confirmed DIN MIDI In + Out on the rear panel, Aug 4 2026 — so **DIN
MIDI is line-wide across the Clarett TB range**, not an 8PreX-only feature as first assumed). The 2Pre is
the ideal target: it has MIDI, the fewest channels, and is the bare-metal default unit. The transport is
expected to be identical across models (same single-BAR/`FocusritePcieMidi` brokering), so the 2Pre result
should port directly; spot-confirm on the 8PreX afterward.

### Physical + Windows setup — external host MIDI interface (direction-DECOUPLED, preferred)
A USB-MIDI interface on the **Linux host** (user's CH345, `amidi` port `hw:5,0,0`, bidirectional) cabled to
the 2Pre's DIN ports beats a 2Pre self-loopback: the self-loopback fires BOTH tunnel directions at once
(Windows sends out OUT → returns on IN), so no register/opcode can be attributed to TX vs RX. The external
interface drives **one direction at a time**:
- **RX capture** (device→VM): host **CH345 OUT → 2Pre MIDI IN**, Windows sends nothing (but a MIDI-OX input
  monitor is OPEN on the Focusrite port so the driver actively pulls received bytes up — else it may buffer
  with no host-visible DMA). Pure device-initiated receive path.
- **TX capture** (VM→device): Windows MIDI-OX sends → **2Pre MIDI OUT → CH345 IN**; host `amidi -d` confirms
  the bytes physically emerged (ground truth). Pure driver-push send path.
- No contention: CH345 is an ordinary host USB device, the 2Pre is vfio-passed to the VM, the only link is
  the 31250-baud DIN cables. In Windows the Focusrite MIDI port appears via `FocusritePcieMidi` (MIDI-OX).

**CH345 gotchas — clear with a host-only pre-flight (CH345 OUT→IN, one DIN cable) BEFORE touching the VM:**
`amidi -p hw:5,0,0 -d` (capture) + `amidi -p hw:5,0,0 -S '90 3C 7F'` then the SysEx. (1) cheap cables
mislabel IN/OUT — if nothing echoes, swap. (2) CH345 has historically-broken SysEx (kernel quirk
`ch345_broken_sysex`); if the SysEx returns truncated/padded, DROP it and use an ascending note-on scale
`90 3C 7F 90 3E 7F 90 40 7F …` as the greppable signature instead. If SysEx survives, keep it (verbatim
opaque bytes = ideal DMA-ring grep target). NOTE: PipeWire holds the CH345 *seq* port; `amidi -p hw:` opens
the raw device directly and coexists — if it reports busy, steer PipeWire off it.

### Signatures (low-entropy, greppable — the whole reason MIDI is tractable)
- Note-on: `90 3C 7F` (repeat a few times).
- SysEx with an unmistakable ASCII body — **"CLARETT"** inside a non-standard manufacturer frame:
  `F0 7D 43 4C 41 52 45 54 54 F7`. Grep target in RAM/registers: `43 4c 41 52 45 54 54`.

### VM-crash gotcha (Aug 4 2026) — do NOT toggle the Focusrite MIDI ports
Changing which Focusrite MIDI ports MIDI-OX has open **crashes the Windows VM** (the driver's port
open/close path is unstable under passthrough+trace). MIDI-OX defaults to **both Input+Output open**, and
that state is stable (all the validated note/SysEx tests ran in it). So **leave both ports open for ALL
captures and never toggle**. Consequences, and why it's still fine:
- **`tx` stays clean** even with both open — during a pure send nothing enters the input, so MIDI-OX's
  auto-thru never fires; only the send path appears.
- **`rx` includes the thru echo** (inject → RX → MIDI-OX echoes → TX), so both directions fire. Recover RX
  by **subtraction: RX regs/opcodes = (in rx.log) − (in tx.log)**. The `tx` capture is the reference set.
- Keep `idle` with both ports open too, so its baseline matches rx/tx (cleaner diffs).
- Optional cleaner `rx`: delete the input→output line in MIDI-OX **View → Port Routings** (internal to
  MIDI-OX, should not touch the driver / crash). If it acts up, skip — subtraction fully covers it.

### Captures (name them `captures/2pre_midi_*.log`)
1. `..._idle.log` — all cabled, **nobody sends**, ~10 s. Baseline (meter-poll noise + any MIDI-init regs).
2. `..._rx.log` — **host injects** into 2Pre MIDI IN (Windows silent, input-monitor open): a note-on burst
   `for i in $(seq 20); do amidi -p hw:5,0,0 -S '90 3C 7F'; sleep 0.1; done`, then the SysEx if pre-flight
   cleared it. Pure device→VM RX path.
3. `..._tx.log` — **Windows MIDI-OX sends** note-ons/SysEx to the Focusrite MIDI OUT; host `amidi -p
   hw:5,0,0 -d` confirms they emerge from 2Pre OUT. Pure VM→device TX path.
4. (later) `..._streaming.log` — RX or TX **with** an audio stream running, to see if MIDI shares or avoids
   the audio path. Do this LAST; audio DMA noise drowns the trace, so isolate mechanism first.

### Analysis (run on the rx/tx captures, each diffed against idle)
- `tools/fcp_decode.py captures/2pre_midi_rx.log --raw` (and `_tx`) → read the **opcode histogram** at the
  end; any opcode present here but absent in `..._idle.log` is the **mailbox-transport** candidate (mech 1).
- `tools/bar_profile.py captures/2pre_midi_rx.log --new-only` (and `_tx`) → any register offset outside the
  control-plane map that lights up only during MIDI is a **dedicated-ring** register (mechanism 2): look
  for a new base pair (like `0x210/0x214`) and a new cause block (`0x600`? a 5th vector?). RX vs TX may use
  DIFFERENT registers — the decoupled captures attribute each unambiguously.
- `tools/dma_bases.py captures/2pre_midi_rx.log <domain>` → run the emitted `pmemsave`, then
  `grep -c` / `xxd | grep '43 4c 41 52 45 54 54'` (or the note-scale bytes) the dump. In the **audio ring**
  → mechanism 3; in a **new region** whose base showed up in `--new-only` → mechanism 2.

## After the transport is known — implementation sketch
- New `driver/clarett_midi.c`: register `snd_rawmidi` (1 in + 1 out substream), gated by an
  `enable_midi` module param mirroring `enable_pcm`. TX/RX plumbed to the discovered path
  (mailbox opcode | dedicated ring servicer | audio-slot demux). Port naming into the fcp-server maps.
- **Register on all models** — DIN MIDI is line-wide (2Pre confirmed), so the rawmidi is not model-gated;
  the transport is expected identical across the range (shared BAR / `FocusritePcieMidi` brokering).

## Open questions to resolve from the trace
- Is the MIDI path armed by the existing 232-cmd bring-up, or does `FocusritePcieMidi` do its own init
  (a MIDI-specific opcode/register write in `..._idle.log` before any note)? Determines whether
  `clarett_arm_device()` already sets it up.
- RX (device→host) delivery: polled, or a new MSI vector? (`bar_profile --new-only` on a cause block.)
- Running status / active sensing / SysEx framing: does the device hand us raw MIDI bytes or parsed events?
