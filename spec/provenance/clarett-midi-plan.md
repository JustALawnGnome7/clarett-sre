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

## Addendum: `0x510`, `0x514` and the FIFO sizes (Nov 18 2026) `[HW — Clarett 2Pre]`

Scratch module (never committed) on a 2Pre with a 5-pin cable from MIDI Out to MIDI In, fcp-server stopped
during the measurements so only MIDI's own interrupts drained receive.

**`0x510` = the transmit FIFO's ready threshold, in free words.** Only bits 0-3 store (all ones reads `0xf`,
`0x5a5a5a5a` reads `0xa`, `0x10` and up read 0); the value never changes by itself. Writing words of 3 bytes
to `0x58c` in a tight loop and reading `0x500` after each, ready (bit 16) dropped after **17 - v** words for
v = 0..11 (17 at 0, 9 at the vendor's 8, 6 at 11), and came back ~0.6 ms later, when the next word moved
out to be sent; for v = 12..15 after 16 - v words, back within 4-19 us (the first word had not yet moved
out, and its moving freed a slot). So: ready = free slots > `0x510`. The driver's old "nine words deep" was
this threshold at 8. Receive is unaffected at every value (one interrupt per byte, one byte per word,
round trips within 0.2-0.4 ms of the wire time, every loop byte-exact).

**The transmit FIFO holds 16 words.** Ignoring the flag, bursts of up to 16 words came back complete and
bursts of 17-24 returned exactly 16 (the rest discarded), at `0x510` = 0 and 8 alike; a 32-word burst
returned 17 (it took long enough to write that the first word had moved out). This is why the old
unflagged driver delivered 75 of 3003 bytes.

**The receive FIFO holds 128 one-byte words, then packs.** With the drain held off and N bytes sent round
the loop: up to N = 129 the FIFO kept N one-byte words; beyond that the device packed further bytes into
3-byte words, up to a hard limit of **139 words / 159 bytes** (Bennett's "139"), keeping the oldest bytes
and dropping the newest. Packing starts here only once ~128 words wait; the 8PreX's occasional 2-byte words
in normal flow must have another trigger (not pursued).

**No fill-level register.** All of `0x500`-`0x588` (minus `0x58c`) dumped at every fill level: only `0x500`
changed (idle `0x00ff0000`, receive pending `0x00ff000a`, no full bit; bit 16 clear while transmitting reads
`0x00fe000a`); `0x510` = 8, `0x514` = `0x847`, everything else 0.

**`0x514` = `0x847`, read-only** (writing 0 left it), on every unit including the Red 8Line, which has no MIDI
ports. Its nibbles fit a description of the block: bits 3-0 = 7 = log2 of the 128-word receive FIFO, bits 7-4
= 4 = log2 of the 16-word transmit FIFO (both measured independently), bits 11-8 = 8 = data bits
(untestable). Strong but not proven: it cannot be changed, and every unit reports the same value. A
baud-divisor reading fits poorly (31,250 x 0x847 = 66.2 MHz, not a usual FPGA clock).

The driver needs no change: one word per ready check never overruns with the vendor's 8, and the receive
drain's 256-byte bound covers the 159-byte maximum.

## Addendum: no transmit interrupt; the `0x500` attach write is not an enable (Nov 23 2026) `[HW — Clarett 2Pre]`

Question (IRQ servicing plan, open questions): can MIDI transmit run from an interrupt instead of polling
`0x500` bit 16? Scratch module on 0.5.0 (never committed; patch `~/clarett-bench/midiirq_patch.py`, script
`~/clarett-bench/midi_txirq.sh`, both off-repo). Each run writes 16 words (48 bytes of `0xf8`) to `0x58c`
ignoring the ready flag, then records every vec0 sweep (`0x100` and `0x500`) for 40 ms while the FIFO
drains; a variant polls `0x500` every ~10 us instead to timestamp the ready flag. Settings: `0x500` written
8 (the attach value), 0, 8 plus each other low bit (9, `0xa`, `0xc`, `0x18`, `0x28`, `0x48`, `0x88`) and
`0xff`; `0x510` at 2, 8 and 14; idle windows with nothing sent as negative controls.

**MIDI cable unplugged: ZERO interrupts in all 23 runs**, at every `0x500` value and every threshold. The
ready flag still returned on its threshold, seen only by polling: 7.4 ms after the burst at `0x510` = 8,
1.65 ms at 2, 13.2 ms at 14. **So the device raises no interrupt when transmit space frees up, nor per
byte sent: the transmit poll stays.**

**The same run with a loop cable attached (by accident first) is a clean receive measurement:** exactly 48
bit-28 interrupts per 48-byte burst, ~320 us apart (one byte time at 31,250 baud), from ~0.5 ms to ~15.4 ms,
`0x500` low byte `0xa` on every one, identical at every `0x500` value and threshold. Idle: 0.

**The `0x500` write of 8 at attach is not an interrupt enable:** receive interrupts arrived identically with
0, 8 and `0xff` written. **Bit 4 is the only low bit that holds a written value** (`0x18`/`0xff` read back
`0x10`; a write without it clears it); no effect seen. What the attach write does stays unknown.

Not tried: writing `0x500` bits 16-23 (the ready flag's byte), and the bits `0x104 = 0xf000003f` leaves
clear. Nothing points to a transmit source there; not pursued.
