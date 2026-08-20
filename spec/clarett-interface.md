# Clarett Thunderbolt Interface — Device & Protocol Specification

This document specifies the Focusrite Clarett Thunderbolt audio interfaces
(2Pre, 4Pre, 8Pre, 8PreX) at the level needed to implement a host driver: the
device's register interface, its control protocol (FCP), its audio DMA engine,
and the bring-up sequence a host performs to bring a device into service.

Every statement here is confirmed on real hardware unless tagged **(XML)**,
meaning it is derived from Focusrite's device descriptor and has not yet been
independently observed. Model-specific values are given as tables; nothing is
carried across models by assumption.

The **(XML)** tag is a claim of provenance, not of correctness. At least one
descriptor value has been measured and found wrong (the 2Pre's S/PDIF clock source,
§6.3), so where measurement contradicts the descriptor, the measured value stands
and the discrepancy is called out.

---

## 1. Device

The Clarett Thunderbolt interfaces are FPGA-based Thunderbolt audio devices that
present a single PCIe function to the host.

| Property        | Value                                             |
|-----------------|---------------------------------------------------|
| PCI ID          | `1cb5:0002`                                        |
| Class           | Multimedia audio controller                        |
| PCIe            | Gen1 x1                                             |
| MMIO            | Single 64 KB BAR0 (`BAR0`) — the entire register interface |
| Interrupts      | 4 MSI vectors, MSI-driven (`DisINTx+`)             |
| Front-end       | Intel DSL2210 Thunderbolt controller; the FPGA is a PCIe endpoint behind it |

All host interaction — the control mailbox and the audio DMA engine — is through
BAR0. Audio samples never traverse the BAR; they move by bus-master DMA to and
from host memory (§4). The FPGA boots its own firmware from onboard flash (which
carries both App and FPGA segments); the host does not upload firmware.

The Thunderbolt layer is an Intel DSL2210 "Port Ridge" — one upstream and two
downstream ports, which are the unit's Thunderbolt in and thru jacks — with the
FPGA presenting the `1cb5:0002` function behind it. That accounts for the link
rate: **the 10 Gb/s single-lane link is the device's own TB1-class controller**, not
a host or cable limitation, so it is expected rather than a fault to chase. It is
also ample — the widest case in the line, an 8PreX running 28 in and 28 out at
`S32` / 192 kHz, is about 344 Mb/s in each direction.

### 1.1 Model summary

The four models share one protocol and one register interface; they differ only
in channel counts and pin assignments. Channel counts:

| Model  | Analogue in | ADAT in | Playback (PCM out) | Capture channels | Physical outputs | Mix buses |
|--------|-------------|---------|--------------------|------------------|------------------|-----------|
| 2Pre   | 2           | 8       | 4                  | 14               | 4                | 16 (A–P)  |
| 4Pre   | 4           | 8       | 8                  | 20               | 6                | 16 (A–P)  |
| 8Pre   | 8           | 8       | 20                 | 20               | 10               | 16 (A–P)  |
| 8PreX  | 8           | 16      | 28                 | 28               | 10               | 16 (A–P)  |

Every model also has 2 S/PDIF input channels and 2 S/PDIF output channels. The
mixer is uniform across the line: 30 mixer inputs by 16 mix buses **(XML)**.

---

## 2. Transport (FCP mailbox)

The control protocol is FCP (Focusrite Control Protocol), the same family used by
the USB Scarlett/Clarett devices. Requests are written to a mailbox in BAR0; the
device signals completion through an MSI cause register; GET responses are
returned by DMA into a host-allocated buffer.

### 2.1 Register map

| Offset            | Meaning                                                        |
|-------------------|---------------------------------------------------------------|
| `0x000`           | Capabilities                                                  |
| `0x010` / `0x014` | Serial (low / high)                                           |
| `0x100`           | MSI vector 0 cause — read-to-clear; DONE = bit `0x20000000`   |
| `0x104`           | IRQ enable (`0xf000003f`)                                     |
| `0x200`/`0x300`/`0x400` | MSI vector 1/2/3 cause — read-to-clear (one block per vector) |
| `0x408`           | Doorbell — write `1` = submit, `2` = acknowledge completion   |
| `0x410` / `0x414` | Response DMA buffer bus address (low32 / high32)             |
| `0x8000`–`0x801f` | Read-only firmware-info header                                |
| `0x8020`          | FCP request mailbox (header + data)                           |

`0x400` is a 2-bit command-phase register, not an event queue.

### 2.2 Mailbox frame

The request mailbox at `0x8020` uses the FCP header layout:

| Field   | Offset | Notes                                                        |
|---------|--------|--------------------------------------------------------------|
| `cmd`   | `+0`   | bit31 = execute flag, low bits = opcode                       |
| `size`  | `+4`   | low 16 bits = payload size                                    |
| `seq`   | `+4`   | high 16 bits = sequence number, incremented per command       |
| `error` | `+8`   | device-written status; `0` = success                          |
| pad     | `+12`  | —                                                            |
| `data`  | `+16`  | request payload                                              |

The device echoes `cmd` and `seq` in its response, which callers use to match a
reply to its request.

### 2.3 Response DMA

GET responses do **not** appear in the BAR. The host allocates a response buffer,
programs its bus address into `0x410`/`0x414`, and the device DMAs each response
there. The response begins with a 16-byte echoed FCP header (echoed `cmd` at
`+0`, echoed `seq`); response data follows at `+16`. For a `GET_DATA{off, len}`
the returned bytes satisfy `resp[16 + i] == config[off + i]`.

### 2.4 Transaction cycle

A single command proceeds as:

1. Host zeroes the response buffer's header, writes the request header and payload
   to `0x8020`, and rings the doorbell (`0x408 = 1`).
2. The device processes the command, writes its `error` status, and — for a GET —
   DMAs the response into the host buffer.
3. Host polls the vector-0 cause register `0x100` for the DONE bit
   (`0x20000000`), then acknowledges the completion (`0x408 = 2`).

**Ordering requirement.** The completion acknowledgement (`0x408 = 2`) must not be
issued before the device's response DMA has landed. The host enforces this by
zeroing the response header before submit and waiting until the device has written
it before acknowledging. Acknowledging early is a protocol violation: the device
responds by refusing the session (returning a blanket error from the first
command), which cannot be recovered without a fresh bring-up. Because the response
DMA completes asynchronously with respect to the DONE signal, the DONE bit alone
is not a sufficient condition to acknowledge.

### 2.5 Completion signalling

MSI vector 0 carries mailbox completion; the host polls its cause register `0x100`
for DONE rather than taking the completion in the interrupt handler, to keep the
acknowledgement ordered after the response DMA (§2.4). MSI vector 0 also carries
asynchronous device notifications (config-change events), distinguished from
mailbox completion by the cause bits; the driver services those on a workqueue.

---

## 3. Control protocol

The control plane covers the mixer, routing, preamps, clocking, monitor section,
and metering. It uses two distinct kinds of value that must not be confused:

- **Mailbox opcodes** — the `cmd` word written to the mailbox (§2.2), which selects
  a transport operation (read config, write config, set a mix row, etc.).
- **Control commit codes** — a small integer (1–8) that identifies *which* control
  a config write commits, passed as the argument to `DATA_CMD`.

All config-space `offset` values below are byte offsets into the device's logical
config space — the payload addressed by FCP messages — **not** offsets into the
64 KB BAR.

### 3.1 Mailbox opcodes

The `cmd` word is `0x80000000` (execute) OR the opcode:

| Opcode      | Name         | Payload                                   |
|-------------|--------------|-------------------------------------------|
| `0x800000`  | `GET_DATA`   | `{u32 offset, u32 len}` → response by DMA  |
| `0x800001`  | `SET_DATA`   | `{u32 offset, u32 len, data[]}`            |
| `0x800002`  | `DATA_CMD`   | `{u32 commit_code}`                        |
| `0x001001`  | `GET_METER`  | meter read (§3.7)                          |
| `0x002002`  | `SET_MIX`    | one mixer row: `{u16 mix, u16 coeff[30]}`  |
| `0x003001`  | `MUX_READ`   | routing read-back (§3.6)                    |
| `0x003002`  | `SET_MUX`    | routing write, one per band (§3.6)          |
| `0x006003`  | `SET_CLOCK`  | `{u32 sample_rate, u32 clock_source}`       |

### 3.2 Config-space write model

A config control is written in two steps:

1. `SET_DATA{offset, len, value}` — write the control's bytes into config space.
2. `DATA_CMD{commit_code}` — commit them; the commit code selects the control class.

The `offset`, `len`, `value`, and `commit_code` come from the per-model control
map (§3.3). Commit codes on the reference model (8PreX):

| Code | Commits                                                        |
|------|---------------------------------------------------------------|
| 1    | Output gain                                                    |
| 2    | Monitor section (master mute / dim / gain)                     |
| 3    | Output hardware-control enable flags                          |
| 4    | S/PDIF source (in / out)                                       |
| 5    | Persist config to flash                                        |
| 6    | Input mode (Mic/Line/Inst)                                     |
| 7    | Input Air                                                      |
| 8    | Meter source                                                  |

Commit code 5 with no preceding `SET_DATA` is a bare flash-commit that persists the
current config to onboard flash.

### 3.3 Config-byte map (8PreX reference)

Input controls, analogue 1–8:

| Control | Bits | Commit | Offset            | Enum                     |
|---------|------|--------|-------------------|--------------------------|
| Mode    | 2    | 6      | `166 + (n−1)`     | Mic=0, Line=1, Inst=2    |
| Air     | 1    | 7      | `174 + (n−1)`     | off=0, on=1              |

Inputs 1–2 offer Mic/Line/Inst; inputs 3–8 offer Mic/Line only. Phantom power,
phase invert, and the high-pass filter are front-panel hardware switches on the
Clarett line, with no config-space representation.

Output controls, 10 analogue outputs. `gain` is an 8-bit attenuation code
(§3.4); the hardware-control enables are single bits packed into the bytes shown
(`bN.k` = byte `N`, bit `k`):

| Output       | Gain offset | HW-gain-en | HW-mute-en | HW-dim-en |
|--------------|-------------|------------|------------|-----------|
| Monitor 1    | 32          | b52.0      | b72.0      | b73.2     |
| Monitor 2    | 33          | b52.1      | b72.1      | b73.3     |
| Line 3       | 36          | b56.0      | b72.2      | b73.4     |
| Line 4       | 37          | b56.1      | b72.3      | b73.5     |
| Line 5       | 40          | b60.0      | b72.4      | b73.6     |
| Line 6       | 41          | b60.1      | b72.5      | b73.7     |
| Line 7 (HP1) | 44          | b64.0      | b72.6      | b74.0     |
| Line 8 (HP1) | 45          | b64.1      | b72.7      | b74.1     |
| Line 9 (HP2) | 48          | b68.0      | b73.0      | b74.2     |
| Line 10 (HP2)| 49          | b68.1      | b73.1      | b74.3     |

Gain bytes are stereo-paired with stride 4. When an output's HW-gain-enable bit is
set, the hardware (front-panel knob) owns that output's level and the stored 8-bit
gain is bypassed; the device does not mirror the knob position back into the gain
byte.

Monitor section, config region offset 24, length 92:

| Control        | Bits | Commit | Offset | Notes                              |
|----------------|------|--------|--------|------------------------------------|
| Master mute    | 1    | 2      | 24     | 1 = muted, 0 = released            |
| Master dim     | 1    | 2      | 28     | 1 = dimmed, 0 = released           |
| Master gain    | 8    | 2      | 112    | attenuation code; front-panel knob |

Offset 112 is the front-panel monitor knob and is the one preamp/monitor byte the
device keeps live: `GET_DATA` reads it back tracking the physical knob, and turning
the knob raises a config-change notification.

**Every other preamp and monitor byte is host-owned and write-only in effect.** They
read back as written within a session, but the device never publishes its own state
into them. The asymmetry is sharper than "reads 0 until the host writes": across a
power cycle the device *restores Mode and Air to the hardware* from flash, yet
offsets 166 and 174 still read 0 — the preamp is physically in Inst or Air mode
while config space denies it. There is no read path at all, and no front-panel
Mode/Air control on any Clarett Thunderbolt unit through which one could be
inferred, so a host cannot display the device's boot-time preamp state. It can only
assert its own and track it from there.

Settings:

| Setting            | Bits | Commit | Offset | Enum                          |
|--------------------|------|--------|--------|-------------------------------|
| S/PDIF out source  | 2    | 4      | 124    | Optical=1, RCA=2              |
| S/PDIF in source   | 2    | 4      | 132    | Optical=1, RCA=2              |
| Meter source       | 8    | 8      | 184    | model-specific (§6)           |
| Nickname           | 32 B | —      | 80     | user string                   |

### 3.4 Gain encodings

Two distinct encodings are used, and they are inverse conventions:

- **Output and monitor gain** (commit 1 and the monitor master at 112) is a 7-bit
  **attenuation code equal to |dB|**, linear at 1 dB per code: `0x00` = 0 dB
  (unity), `0x7f` = −127 dB (floor). An ALSA driver maps a `0..127` control value
  `v` to device code `127 − v` under `DECLARE_TLV_DB_SCALE(tlv, -12700, 100, 0)`.
- **Mixer coefficients** (`SET_MIX`) are 16-bit **linear amplitude**, truncated:
  `coeff = floor(8192 · 10^(dB/20))`. `0x0000` = −∞ (off), `0x2000` = unity (0 dB),
  `0x3fd9` = +6 dB (maximum).

### 3.5 Mixer

The mixer is a 30-input × 16-bus matrix. Each mix bus is written as one `SET_MIX`
command carrying the whole row: `{u16 mix, u16 coeff[30]}`, `mix` = 0..15, each
coefficient in the §3.4 linear encoding. A mixer input slot's *source* is chosen
through the routing matrix (§3.6) by assigning a source pin to the slot's
destination pin; its *level* into each bus is the `SET_MIX` coefficient.

### 3.6 Routing

Routing is a table of *destination ← source* assignments over direction-scoped
pins (§6). It is written as three `SET_MUX` commands, one per sample-rate band
(band 0 = 44.1/48 kHz, 1 = 88.2/96 kHz, 2 = 176.4/192 kHz). Each command's payload
is `{u32 header, u32 entry[]}` where `header = band << 16` and each entry packs two
12-bit pins:

```
entry = (src_pin << 12) | dst_pin
```

`src_pin = 0` means the destination is present but unrouted (silent). Each band's
entry list is followed by 16 zero (`dst = 0`) padding words. Higher bands enumerate
fewer entries as ADAT S/MUX and high-rate PCM channels drop out.

A direct output-source reassignment writes `SET_MUX` only. A mixer-input edit
(changing what feeds a mix) also rewrites the affected `SET_MIX` row.

Routing is read back with `MUX_READ{u8 offset, u8 pad, u8 count, u8 band}`. The
reply is capped at 28 entries (112 bytes) regardless of `count`, and `offset` is a
**flat** entry index that crosses band-internal boundaries, so a caller reading a
specific region must window its requests.

### 3.7 Clocking

Sample rate and clock source are set with `SET_CLOCK{u32 sample_rate, u32
clock_source}`. Supported rates are 44100, 48000, 88200, 96000, 176400, and 192000.
The `clock_source` enum is uniform across the line — `Internal = 24`, `ADAT = 0`,
`S/PDIF = 3` — and only the *set of sources a model offers* differs (§6.3).

Neither the rate nor the source is a config-space byte: they exist only in the
`SET_CLOCK` payload, so neither can be read or written through `GET_DATA`/`SET_DATA`
and neither can be expressed as a config-map control.

### 3.8 Metering

`GET_METER` returns the level meters as an array of 32-bit slots. Each slot carries
a 16-bit level replicated into the 32-bit word. Slots are packed per model in the
order analogue → S/PDIF → ADAT (§6). The meter source (which physical bank the
hardware meters read) is selected by the meter-source setting (§3.3).

**The slot array is rate-dependent.** Meters sit at *router destinations*, and a
meter's slot is its position in **that rate's** destination table — so every
destination that S/MUX removes at double or quad speed (§4.5) shifts every later
meter down. The layout is not a constant, and a host holding one table will display
the wrong channel's level for every meter above the first removed destination.

Measured on an 8Pre fed ADAT by an 8PreX, one probe below the first removal and one
above it:

| Meter                  | 48 kHz | 96 kHz | 192 kHz |
|------------------------|--------|--------|---------|
| ADAT in (→ PCM 11–18)  | 10–17  | 10–13  | 10–11   |
| Mixer Input 01         | 40     | 32     | 28      |

The ADAT *input* meters do not move, because they sit below the first removed
destination (PCM 15 = slot 14) — which is why a test that probes only them looks
reassuring. The shift appears only above that point. Live slot counts on the 8Pre
go 70 → 62 → 58 as it loses PCM 15–18 and ADAT Out 5–8 at double speed, then PCM
13–14 and ADAT Out 3–4 at quad.

A model built from the XML `pin-m`/`pin-h` removals predicts all three columns
exactly, so per-rate meter tables can be derived without further measurement.

Note the vendor XML's `<hardware-meters>` `meters-l`/`-m`/`-h` tables (config
offsets 136/146/156) are a **different mechanism** — the front-panel meter bridge,
written per band — not this array.

### 3.9 Notifications

The device raises an asynchronous notification when self-changing state moves — for
example a front-panel monitor button or the monitor knob. The notification is
delivered as an MSI on vector 0; a host reads cause register `0x400` and, when a
monitor-class cause is present, re-reads the monitor config region
(`GET_DATA{24, 92}`) and updates the affected controls.

**Vector 0 is shared with the audio period interrupt**, so while a stream runs it
fires every period and `0x400` reads its idle value each time. The notification word
itself is not exposed, so a host cannot tell a real config change from a period
interrupt by the cause alone. Treating every vector-0 interrupt as a possible
notification during streaming means re-reading the control set once per audio
period — a mailbox flood with audible consequences.

The practical resolution is to stop treating the interrupt as the change signal
while streaming and poll for change instead: re-read the monitor region on the same
timer that polls the meters (~24 Hz) and compare it against the last snapshot,
acting only on a real difference. That costs one command per tick and reports
nothing when nothing moved. Note this covers only the monitor region — any other
self-changing control still will not update mid-stream. On the Clarett Thunderbolt
units that is not currently a gap, because none of them has front-panel Mode/Air
controls.

---

## 4. Data plane (audio DMA)

Audio streams over two scatter-gather DMA rings, each described by a register block
in BAR0 and a descriptor table in host memory. Capture and playback share one
engine and one frame clock.

| Ring | Block base | MSI vector | Direction          |
|------|------------|------------|--------------------|
| 0    | `0x200`    | 1          | TX / playback       |
| 1    | `0x300`    | 2          | RX / capture        |

### 4.1 Ring register block

Offsets are relative to the block base `B` (`0x200` or `0x300`):

| Offset  | Meaning                                                          |
|---------|------------------------------------------------------------------|
| `B+0x04`| Channel count                                                    |
| `B+0x08`| Size register — 4 frames (`channels × 16` bytes)                 |
| `B+0x0c`| Ring enable — written **last**, after base and size             |
| `B+0x10`| Descriptor-table base bus address, low 32 bits                   |
| `B+0x14`| Descriptor-table base bus address, high 32 bits                  |
| `B+0x18`| DMA pointer — current descriptor index (device-written, host reads)|

Global data-plane registers: `0x108` = IRQ config, `0x10c`/`0x110` = period-IRQ
arm. The period IRQs must be left armed once set; disarming them (`0x110 = 0`) stops
the engine.

Programming order per ring is channels → size → base (high then low) → arm the
period IRQs → enable (`B+0x0c = 1`). The enable must come after the base is
programmed.

### 4.2 Descriptor table

Each descriptor is an 8-byte little-endian value: a fragment bus address with flag
bits in its low bits (fragments are aligned, leaving the low bits free):

- **bit 0** — wrap: set on the last descriptor, points the engine back to entry 0.
- **bit 1** — IRQ: consuming an IRQ-flagged descriptor raises a counted period on
  the ring's cause register.

The RX (capture) ring sets the IRQ flag periodically, and it is the consumption of
those flagged descriptors that advances the counted period. A ring flagged only on
its last entry never advances the period counter and the engine stalls after one
pass; the periodic marker is required. The TX ring carries only the wrap flag on
its last entry.

The IRQ **cadence is chosen by the host**, not fixed by the device — it is simply
how often the host sets bit 1 when it builds the table, and it sets the audio period
(§4.3). The vendor's own RX ring flags roughly every 14 descriptors. Cadences from
1 to 64 descriptors (16 to 1024 frames at 48 kHz) are confirmed working.

### 4.3 Frame and fragment geometry

- **Frame:** N-channel interleaved `S32_LE`, 24-bit MSB-justified. Frame stride =
  `channels × 4` bytes.
- **Fragment:** 16 frames of audio — `channels × 64` bytes, with no alignment
  rounding *of the audio itself*. Each descriptor points to one fragment.
- **Slot:** the stride between consecutive fragments in memory, which is **not** the
  fragment size — see below.
- **Period:** `IRQ_DESCS × 16` frames (an IRQ flag every 16 descriptors gives a
  256-frame period; every 4 gives 64 frames).
- **Counter unit:** one increment of a ring's period counter corresponds to 16
  frames exactly.

The two rings share one frame clock, so a full-duplex stream advances both together;
a host can compute its period advance from the counter delta per IRQ, which
self-calibrates to the true hardware period.

**Every fragment must be contained within one 4 KB page.** The engine's DMA is
page-granular, and a fragment that straddles a page boundary is mis-transferred in
both directions. The vendor satisfies this implicitly: its fragment buffers are
scatter-gathered — physically discontiguous — so the engine restarts per fragment
and never streams across a boundary. A host that instead allocates one contiguous
region and strides by the packed audio size violates it as soon as
`channels × 64` fails to divide the page.

The simple equivalent is to give each fragment a **slot** whose stride is the audio
size rounded up to a power of two, over a **page-aligned** sample area. Every
fragment is then page-contained, with unused padding at the end of each slot:

| Model | Channels (capture / playback) | Audio bytes/fragment | Slot stride       |
|-------|-------------------------------|----------------------|-------------------|
| 2Pre  | 14 / 4                        | `0x380` / `0x100`    | `0x400` / `0x100` |
| 4Pre  | 20 / 8                        | `0x500` / `0x200`    | `0x800` / `0x200` |
| 8Pre  | 20 / 20                       | `0x500` / `0x500`    | `0x800` / `0x800` |
| 8PreX | 28 / 28                       | `0x700` / `0x700`    | `0x800` / `0x800` |

This is easy to miss, because a size that already divides the page never fails:
2Pre and 4Pre playback (`0x100`, `0x200`) are safe as packed, so a contiguous host
buffer works on those and breaks on the wider models. The two directions fail
differently, and neither symptom points at page alignment:

- **Capture (RX)** *drifts*. On the 2Pre the signal walked −2 channels every ~73
  frames and realigned every 512 — exactly 8 bytes per 4 KB page accumulating,
  since `0x380` does not divide the page and the misalignment never resets.
- **Playback (TX)** *folds*. On the 8PreX 28 channels collapsed into 4-channel
  groups (every output carried its source *mod 4*) while everything the device reads
  was byte-for-byte identical to the vendor's — registers, descriptor table, source
  ids, handshake, arm, and the 28-channel interleaved sample layout alike. The only
  difference left was that our fragments were contiguous and the vendor's were not.

Note the padding is a *device-side* memory layout. The buffer geometry a host
presents upward stays on the logical contiguous size; only the descriptor addresses
and the per-fragment copies are slot-aware.

### 4.4 Engine arming

To start the engine:

- Both rings' descriptor tables live in **one contiguous** DMA allocation.
- The engine arms **full-duplex**: the TX ring must be armed alongside RX, playing
  silence if no playback stream is attached. Arming RX alone does not clock.
- The RX fragment buffers are pre-filled before arming.

Once armed, the RX cause register (`0x300`) reports a period on each IRQ-flagged
descriptor consumed; the host reads the counter, advances the stream position by
the counter delta, and copies between the ring fragments and its audio buffers.

### 4.5 High sample rates and S/MUX

All six rates (44.1 through 192 kHz) are confirmed for capture on all four models.
The data plane is rate-agnostic: fragment geometry, descriptor layout and the
16-frames-per-counter-unit relationship are all in *frames* and do not change with
rate. Only the `SET_CLOCK` rate differs.

**The stream width does not shrink at high rates.** The per-model channel count in
§1.1 is correct at every rate — there is no S/MUX narrowing of the DMA stream. What
does change is which channels the device *fills*: ADAT carries 8 / 4 / 2 channels
per port at single / double / quad speed, so the ADAT channels drop out of the
capture stream from the tail. Analogue and S/PDIF are present at all rates.

Leading capture channels the device actually writes, of the full width:

| Model | Full width | 88.2 / 96 kHz | 176.4 / 192 kHz |
|-------|-----------|----------------|-----------------|
| 2Pre  | 14        | 10             | 8               |
| 4Pre  | 20        | 16             | 14              |
| 8Pre  | 20        | 16             | 14              |
| 8PreX | 28        | 20             | 16              |

The dead set is a contiguous tail on every model. The 8Pre row is measured; the
others are derived from the XML `pin-m`/`pin-h` overrides, where `0x0` means the
channel is gone at that speed *and above* — a cascade confirmed by the routing
count deltas. End-to-end verification on an 8PreX → 8Pre ADAT link, tones on all
eight ADAT channels: at 48 kHz ADAT 1–8 arrive on capture channels 12–19, at 96 kHz
ADAT 1–4 on 12–15, at 192 kHz ADAT 1–2 on 12–13 — the 8 → 4 → 2 prediction exactly.

**The removed channels are not silent, and a host must blank them.** They carry a
sparse residue the engine actively writes: one non-zero sample every 32 frames, an
impulse train at roughly −25 dBFS. Only channels dropped at the *immediately
preceding* speed tier receive it — on an 8Pre, ADAT 5–8 at double speed and ADAT
3–4 at quad — while channels dropped a full tier earlier stay exactly zero.

Blanking the ring once at stream start does **not** hold, because the engine keeps
writing those slots; the dead tail has to be blanked on the frames handed upward,
per period. A host that skips this delivers an audible impulse train on channels it
believes are unused. (Before any blanking at all the symptom is different again and
easy to misread as a driver bug: a DMA ring allocated once and reused replays a
frozen loop of the previous stream's audio in those channels.)

---

## 5. Attach: session readiness and model identification

**The device arms its own control session.** The FPGA boots its firmware from
flash, and so does the config backend: a device that has been configured at least
once restores its session across a genuine power cycle, with no host bring-up at
all. Config reads, level metering and *control writes* all work on a device the
host has only opened and polled — confirmed on hardware for the 2Pre and 8Pre,
which switch preamp relays from a cold DC power-cycle with the host having sent no
initialisation whatsoever.

A host therefore performs no bring-up at attach. It waits for the session, reads
the model, and starts using it.

### 5.1 Readiness

The session is not necessarily live the instant the PCIe function appears: a cold
Thunderbolt attach can race it, and the first command's response may not land. The
host polls an identity query (§5.2) until it answers, and only then treats the
device as present. A budget of ~2 s covers the observed worst case; a device that
never answers is not usable and should be refused rather than assumed.

This readiness race is the sole cause of a device that appears "unarmed". Replaying
the vendor initialisation does **not** rescue it — only waiting does.

### 5.2 Model identification

A routing-count query at band 0 (`GET_7.1`) returns the device's
`{playback, capture}` channel-count pair, which is unique to each model (§1.1).
This is the only identification path: the PCI ID is shared across the entire line
(and beyond it — the Red range presents the same `1cb5:0002`), and every
host-visible surface examined reads identically across models until the session
answers. There is no DROM name to rely on either; a `device_name` under
`/sys/bus/thunderbolt` is present on some hosts and absent on others, so it cannot
carry a contract.

Because channel counts, DMA ring and descriptor geometry, fragment strides (§4.3),
routing and mixer tables and the meter layout are all sized from the model, a
mis-identified device is not a cosmetic mislabel — it is a card streaming the wrong
width into wrongly strided rings. A host that reads a valid geometry pair matching
no model it knows should refuse the device, not guess.

### 5.3 The vendor initialisation sequence

The vendor host application replays a ~232-command initialisation at attach. It is
described here because its side effects matter, not because a host needs it:

1. **Config push** — a sequence of `0x5000` (CONFIG_PUSH) commands carrying the port
   inventory, followed by subsystem-enable and count-query commands.
2. **Config read/write-back** — read the ~8 KB config store and write it back.
3. **Default routing and mixer** — `SET_MUX` (three bands) and `SET_MIX` (16 rows)
   carrying the device's default patch.

On a device that already holds a session — which is every device that has been
configured once — steps 1 and 2 are a no-op, and **step 3 is destructive**: it
installs the *default* patch, discarding the user's routing and mixer state. A host
that replays this sequence unconditionally silently resets routing on every attach.

Whether a factory-fresh, never-configured device requires this sequence is
**untested and open**. Every device observed had been through the vendor
application at least once. A host that needs to support one would read back the
live band-0 routing with `MUX_READ` first and apply the default patch only to a
device that reads empty.

---

## 6. Per-model reference

### 6.1 Pin address space

Pins identify routing endpoints and are **direction-scoped**: the same number means
different things as a source vs a destination. Reference (8PreX) sources:

| Pin range       | Source                        |
|-----------------|-------------------------------|
| `0x200`–`0x20f` | ADAT input (16 ch)            |
| `0x300`–`0x30f` | Mix bus outputs (Mix A–P)     |
| `0x400`–`0x407` | Analogue input (mic pre 1–8)  |
| `0x408`–`0x409` | S/PDIF input                  |
| `0x600`+        | PCM playback (from host)      |

Reference (8PreX) destinations:

| Pin range       | Destination                       |
|-----------------|-----------------------------------|
| `0x186`–`0x187` | S/PDIF output                     |
| `0x200`–`0x20f` | ADAT output (16 ch)               |
| `0x300`–`0x31d` | Mixer input slots (1–30)          |
| `0x400`–`0x407` | Analogue output (Line 3–10)       |
| `0x408`–`0x409` | Analogue output (Monitor 1–2)     |
| `0x600`+        | PCM capture (to host)             |

The smaller models are not simple subsets; they remap several pin blocks:

| | 8PreX | 8Pre | 4Pre | 2Pre |
|---|---|---|---|---|
| Analogue-in pins | `0x400`–`0x407` | `0x400`–`0x407` | `0x400`–`0x407` | `0x400`, `0x402` |
| S/PDIF-in pins | `0x408`/`0x409` | `0x408`/`0x409` | `0x408`/`0x409` | `0x186`/`0x187` |
| Loopback capture pins | `0x60a`/`0x60b` | `0x60a`/`0x60b` | `0x60a`/`0x60b` | `0x604`/`0x605` |
| ADAT | 16 ch | 8 ch | 8 ch | 8 ch |

### 6.2 Mixer geometry

All four models have a 30-input × 16-bus mixer, with mix buses A–P on source pins
`0x300`–`0x30f` and mixer input slots 1–30 on destination pins `0x300`–`0x31d`
**(XML)**.

### 6.3 Clock sources

The clock-source enum is **not** model-specific. `Internal = 24`, `ADAT = 0` and
`S/PDIF = 3` on every model; only which sources a model *offers* differs:

| Model  | Internal | ADAT 1 | S/PDIF | ADAT 2        | Wordclock     |
|--------|----------|--------|--------|---------------|---------------|
| 8PreX  | 24       | 0      | 3      | 1 **(XML)**   | 2 **(XML)**   |
| 8Pre   | 24       | 0      | 3      | —             | —             |
| 4Pre   | 24       | 0      | 3      | —             | —             |
| 2Pre   | 24       | 0      | 3      | —             | —             |

`Internal` is confirmed everywhere (every stream arms with it). `ADAT = 0` is
confirmed on the 8Pre at 48, 96 and 192 kHz. `S/PDIF = 3` is confirmed on the 8Pre
over RCA coax and the 2Pre over TOSLINK — **the 2Pre's XML claims 4, and that is
wrong.** Value 4 is accepted by the device but is not source-specific: it locks to
whatever external source is present, so it is something looser (any external, or
any optical), not a per-model S/PDIF encoding.

The 8PreX's `ADAT 2` and `Wordclock` values are XML-derived and **unverified**.
They are not verifiable by the method that measured the others, because on the
8PreX `Sync Status` does not reliably track the selected source: feeding one ADAT
port and stepping the value, the deliberately invalid value 7 read `Locked` in 2 of
3 trials, while value 1 locked with either port fed and value 0 locked only with
port 2 fed — mutually inconsistent, so no port mapping can be claimed. The likely
explanation is that the 8PreX reports a lock if *either* ADAT receiver has locked,
independently of the `SET_CLOCK` selection, which would make `Sync Status` useless
as a clock-source probe on any two-ADAT-port model. `Wordclock` additionally needs
a BNC source that was never available.

**Method trap.** The audio path is not a probe for clock source. S/PDIF and ADAT
keep arriving on their capture channels whatever the clock source says, *even while
`Sync Status` reads `Unlocked`* — the router does not care. `Sync Status` is the
only signal that distinguishes these values, and any test of them needs a negative
control (an invalid enum value) re-checked on every run: the idle-ADC noise floor
and a `Locked` reading taken directly after another `Locked` leg both look like
signal and are not.

### 6.4 Meter source

The meter-source setting (offset 184, commit 8) selects which bank the hardware
meters read. Its enum is model-specific: the 8PreX offers Analogue=1, S/PDIF=2,
ADAT 1=4, ADAT 2=8; the 8Pre and 4Pre offer Analogue=1 only; the 2Pre has no
meter-source control **(XML)**.

### 6.5 Meter slot layout

`GET_METER` slots are packed per model in physical-input order (analogue → S/PDIF →
ADAT), followed by the routed capture/output channels. The 2Pre, 4Pre and 8Pre slot
maps are measured against hardware — the 8Pre's over all 70 slots, including the
per-rate compaction of §3.8. The 8PreX follows the same packing rule but is not
measured.

Each of these layouts describes the **single-speed** array only. At double and quad
speed the slots compact around the destinations S/MUX removes (§3.8), so a complete
per-model meter map is three tables, not one.
