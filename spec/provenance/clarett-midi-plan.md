# Clarett MIDI — transport reverse-engineering plan (SOLVED)

Status: **DONE Aug 4 2026 — transport SOLVED (register PIO at BAR0 `0x58c`) and `clarett_midi.c` IMPLEMENTED
and HARDWARE-CONFIRMED on the 2Pre.** Decoded from three 2Pre captures (`captures/2pre_midi_{idle,rx,tx}.log`)
via the CH345-on-host rig; driver built and tested on hardware — RX and TX both work, including a full 10-byte
SysEx each way (note `90 3C 7F` and `F0 7D 43 4C 41 52 45 54 54 F7` round-tripped through the CH345↔2Pre DIN
cables). RX interrupt is already enabled by the bring-up (no explicit enable needed); the all-vector ISR drain
caught it regardless of MSI vector. Remaining (non-blocking): large-SysEx flow control (midi_tx_pace_us lever,
untested), count=2 framing (PC/channel-pressure), concurrent-with-audio-streaming, 8PreX confirmation, maps.

## RESULT — the MIDI transport (Aug 4 2026)

**Mechanism: a memory-mapped MIDI register at BAR0 `0x58c`** (+ status `0x500`, ack `0x504`). NOT the FCP
mailbox, NOT the audio DMA ring, NOT multiplexed into the sample stream — pure PIO through one data
register (the FPGA exposing the MIDI UART). Independent of the FCP mailbox, so MIDI should work regardless
of control-session state.

**TX — write packed word to `0x58c`:** `(count<<24) | (byte2<<16) | (byte1<<8) | byte0`; `count` = number
of MIDI bytes 1–3, bytes packed low→high in transmit order. `count` is a plain BYTE COUNT, not a USB-MIDI
CIN (the SysEx `F7` terminator is `count=1` = `0x010000f7`). Examples (from tx.log + rx.log thru-echo):
note-on `90 31 64`=`0x03643190`; SysEx `F0 7D 43 4C 41 52 45 54 54 F7` → `0x03437df0`,`0x0352414c`,
`0x03545445`,`0x010000f7` (3+3+3+1). Each TX write is preceded by a `0x500` read (TX-ready gate? or
advisory — TBD). 2-byte messages (PC/channel-pressure) → `count=2` inferred, not captured.
**CONFIRMED from a clean SysEx-only TX capture (10× SysEx → exactly those 4 writes ×10, 40 reads/40 writes,
no other regs).** Chunking is by **raw 3-byte grouping of the outgoing byte stream, NOT by MIDI message
boundaries** (the 10-byte SysEx splits 3+3+3+1 purely by count) — so `0x58c` is a byte FIFO with a valid-count,
and the TX path just packs whatever bytes ALSA gives it 3-at-a-time (final word `count`=remainder). No TX-side
message parsing needed.

**RX — read `0x58c`, ONE byte per read:** returns `(valid<<24) | byte`, `valid`=bit24 (`0x01000000`);
returns `0x0` when the FIFO is empty. Injected `90 3C 7F` read back as `0x01000090`,`0x0100003c`,
`0x0100007f` then `0x0`.

**RX interrupt flow (RX is interrupt-driven — `idle.log` is EMPTY, no polling):** MSI → cause sweep →
`0x500` reads `0xff000a` (low nibble `0xa` = MIDI RX pending; `0xff0000` otherwise) → drain `0x58c` to
`0x0` → write `0x8` to `0x504` to ACK. One byte per interrupt in this capture.

**Registers:** `0x58c` = MIDI data (RW), `0x500` = IRQ status (low byte carries MIDI-RX bit `0x0a`; already
read by the stream servicer), `0x504` = IRQ ack (write `0x8` = MIDI RX). All in the `0x5xx` IRQ block.

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
