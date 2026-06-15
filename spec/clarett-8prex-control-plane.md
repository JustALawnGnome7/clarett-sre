# Clarett 8PreX — Control-Plane Spec (clean-room, derived)

**Status:** working spec, authored from interface facts. Sources are tagged per section:
`[XML]` = stated in Focusrite's `Clarett 8PreX.xml` device descriptor (interface fact);
`[INF]` = inferred/derived by us (needs confirmation);
`[TRACE]` = to be confirmed against the live MMIO/FCP capture.

This document describes the **control plane** (mixer/routing/preamp/clock/monitor). The data
plane (PCIe DMA streaming) is separate and not covered here except for channel counts.

> **Address-space caution.** All `offset` values below are byte offsets into the device's
> **logical config/app space** — the payload addressed by FCP messages — **not** offsets into the
> 64 KB MMIO BAR. The BAR is the mailbox transport that *carries* FCP messages referencing these
> config offsets. Keep the two spaces distinct.

---

## 1. Device identity

- PCI `1cb5:0002`, Multimedia audio controller. Class **"Clarett"** (Thunderbolt) — distinct from
  the **"Clarett USB"** class (Clarett USB / Clarett+), which is the class `scarlett2` supports. `[XML: Classes.xml]`
- Protocol: **FCP** (Focusrite Control Protocol), same family as `scarlett2`. The Thunderbolt
  Clarett is *not* in `scarlett2`; the USB Clarett *is*, so the USB 8Pre descriptor + `scarlett2`
  are the verified reference for interpreting this schema. Encodings are **per-model** — never
  carry opcodes/offsets/enums across models. `[INF]`
- `nickname`: 32-byte user string @ config offset **80**. `[XML]`

## 2. Command opcodes

Each control commits with a `command` opcode. On the **8PreX** these are: `[XML]`

| opcode | meaning |
|---|---|
| 1 | output gain |
| 2 | monitor section (master mute / dim / gain) |
| 3 | output hardware-control enable flags (gain/dim/mute enables) |
| 4 | S/PDIF source (in / out) |
| 5 | flash / persist app config |
| 6 | input mode (Mic/Line/Inst) |
| 7 | input "Air" |
| 8 | meter source |

> Note: opcode numbers differ from the USB 8Pre (there air=8, mode=7, flash=6). Use these. `[XML]`
> Clock source and sample rate carry **no** offset/opcode → handled by **dedicated FCP commands**,
> not config-space writes (see §7). `[INF/TRACE]`

## 3. Channel inventory & pin address space

Pins identify endpoints in the routing matrix. **Pin numbers are direction-scoped**: the same
numeric pin means different things as a *source* vs a *destination* (e.g. `0x408` = S/PDIF-in as a
source, Monitor-Out-1 as a destination). `[INF, from overlapping pins in XML]`

### Sources (routable *from*) `[XML]`
| Pin range | Type | Channels |
|---|---|---|
| `0x200`–`0x20f` | ADAT input | ADAT 1.1–1.8 (`0x200`–`0x207`), ADAT 2.1–2.8 (`0x208`–`0x20f`) — 16 |
| `0x300`–`0x30f` | Mix-bus outputs | Mix 1–16 (the 16 mixer outputs) |
| `0x400`–`0x407` | Analogue input | Mic pre 1–8 |
| `0x408`–`0x409` | S/PDIF input | S/PDIF 1–2 |
| `0x600`–`0x61b` | PCM playback (from host) | Playback 1–28 |

### Destinations (routable *to*) `[XML]`
| Pin range | Type | Channels |
|---|---|---|
| `0x186`–`0x187` | S/PDIF output | S/PDIF Out 1–2 |
| `0x200`–`0x20f` | ADAT output | ADAT Out 1.1–2.8 — 16 |
| `0x300`–`0x31d` | Mixer input slots | Mixer input 1–30 |
| `0x400`–`0x407` | Analogue output | Line Out 3–10 |
| `0x408`–`0x409` | Analogue output | Monitor Out 1–2 |
| `0x600`–`0x61b` | PCM capture (to host) | Record 1–28 (incl. Loopback 1–2 @ `0x60a`–`0x60b`) |

So: **8 mic pres, 2× ADAT (16 ch), S/PDIF stereo, 10 analogue outs, 28 PCM play / 28 PCM capture,
30×16 mixer.** `[XML]`

## 4. Input controls (analogue 1–8) `[XML]`

`air`: 1 bit, opcode 7, @ `174 + (n−1)`. `mode`: 2 bits, opcode 6, @ `166 + (n−1)`.

| Input | pin | air @ | mode @ | modes |
|---|---|---|---|---|
| Analogue 1 | `0x400` | 174 | 166 | Mic=0, Line=1, Inst=2 |
| Analogue 2 | `0x401` | 175 | 167 | Mic=0, Line=1, Inst=2 |
| Analogue 3 | `0x402` | 176 | 168 | Mic=0, Line=1 |
| Analogue 4 | `0x403` | 177 | 169 | Mic=0, Line=1 |
| Analogue 5 | `0x404` | 178 | 170 | Mic=0, Line=1 |
| Analogue 6 | `0x405` | 179 | 171 | Mic=0, Line=1 |
| Analogue 7 | `0x406` | 180 | 172 | Mic=0, Line=1 |
| Analogue 8 | `0x407` | 181 | 173 | Mic=0, Line=1 |

(Only inputs 1–2 have the Inst mode.)

## 5. Output controls (10 analogue outs) `[XML]`

`gain`: 8-bit, opcode 1. HW enables: 1-bit, opcode 3, packed into the byte/bit shown.
(Note 8-bit gain — the USB model uses 16-bit; the 8PreX is 8-bit throughout.)

| Output | pin | gain @ | hw-gain-en | hw-mute-en | hw-dim-en |
|---|---|---|---|---|---|
| Monitor 1 | `0x408` | 32 | b52.0 | b72.0 | b73.2 |
| Monitor 2 | `0x409` | 33 | b52.1 | b72.1 | b73.3 |
| Line 3 | `0x400` | 36 | b56.0 | b72.2 | b73.4 |
| Line 4 | `0x401` | 37 | b56.1 | b72.3 | b73.5 |
| Line 5 | `0x402` | 40 | b60.0 | b72.4 | b73.6 |
| Line 6 | `0x403` | 41 | b60.1 | b72.5 | b73.7 |
| Line 7 (HP1) | `0x404` | 44 | b64.0 | b72.6 | b74.0 |
| Line 8 (HP1) | `0x405` | 45 | b64.1 | b72.7 | b74.1 |
| Line 9 (HP2) | `0x406` | 48 | b68.0 | b73.0 | b74.2 |
| Line 10 (HP2) | `0x407` | 49 | b68.1 | b73.1 | b74.3 |

`bN.k` = byte N, bit k. Gain bytes are stereo-paired with stride 4 (32/33, 36/37, …). Lines 7–10
are the two headphone pairs (HP1 = out 7-8, HP2 = out 9-10).

**dB↔byte mapping `[TRACE, CONFIRMED]`:** the byte is a 7-bit **attenuation code** equal to |dB|
exactly, linear at **1 dB/code across the whole range**: `0x00` = 0 dB (unity), `0x14` = −20,
`0x28` = −40, `0x3c` = −60, `0x7f` = −127 dB (floor). (Verified 0…−20 dB → `0x00`…`0x14` linearly; the
earlier −6→`0x08` reading was an outlier.) ALSA: a single linear TLV
`DECLARE_TLV_DB_SCALE(tlv, -12700, 100, 0)`, value `v`(0..127) → device code `127 − v`. No range table
needed. Stereo master volume = same code to both 32 and 33. Set via `SET_DATA{offset,len=1,code}`
+ `DATA_CMD{activate=1}`.

## 6. Mixer (30 × 16)

- 30 input slots → 16 mix buses; each mix bus output is a source pin (`0x300`+). `[XML]`
- Each mixer input slot's *source* is chosen via the routing matrix (§8). `[INF]`
- 30 × 16 = **480 gain coefficients**. The `<mixer>` element carries **no** offset/opcode →
  mix-gain coefficients are set by a **dedicated mixer command**, not the linear config space.
  Encoding (likely 8-bit dB-coded, matching §5) and command: **`[TRACE]`** / cross-check `scarlett2`. `[INF]`

## 7. Clocking `[XML]`

- `clock-source`: Internal=24, S/PDIF=3, ADAT 1=0, ADAT 2=1, Wordclock=2.
- `sample-rate`: 44100, 48000, 88200, 96000, 176400, 192000.
- Neither has a config offset/opcode → set via dedicated FCP command(s). **`[TRACE]`**

## 8. Routing matrix `[XML]`

A table of *destination ← source* assignments. Size depends on the sample-rate band:

| Band | Rates | Entries |
|---|---|---|
| Low | 44.1 / 48 kHz | `num` = **91** |
| Medium | 88.2 / 96 kHz | `num-m` = **75** |
| High | 176.4 / 192 kHz | `num-h` = **67** |

- Channels with `pin-m="0x0"` drop at the medium band; `pin-h="0x0"` drop at the high band —
  dominated by **ADAT S/MUX** (8 → 4 → 2 ch per port) plus reduced high-rate PCM channels. `[XML/INF]`
- Enumerated destinations total ≈ **86** (28 hw outputs + 28 PCM capture + 30 mixer inputs); header
  says 91 → ~5 additional/reserved entries. Exact entry order & the ~5 delta: **`[TRACE]`**.

### PCM-capture default source map (`record-outputs`) `[XML]`
Record pin → default hardware-input index, with per-band remap (`input-m`/`input-h`):

| pin | input | input-m | input-h | note |
|---|---|---|---|---|
| `0x600`–`0x609` | 0–9 | — | — | |
| `0x60a`,`0x60b` | — | — | — | **Loopback 1, 2** |
| `0x60c`,`0x60d` | 10,11 | — | — | |
| `0x60e`,`0x60f` | 12,13 | — | 18,19 | |
| `0x610`–`0x613` | 14–17 | 18–21 | (dropped) | `pin-h=0x0` |
| `0x614`–`0x61b` | 18–25 | (dropped) | (dropped) | `pin-m=0x0` |

## 9. Monitor / monitoring section `[XML]`

- Monitoring config region: offset **24**, length **92** bytes.
- Master `mute`: 1 bit @ 24, opcode 2. `dim`: 1 bit @ 28, opcode 2. `gain`: 8-bit @ 112, opcode 2.
  These three are an `exclusive` group.
- Monitor presets (which outputs the master section controls): `1-2`(opt 2), `1-4`(4), `1-6`(6),
  `1-8`(8), `All`(1), `None`(0), each with a 10-slot output mapping mask.

## 10. Settings `[XML]`

- **S/PDIF source**: input 2-bit @ 132, output 2-bit @ 124, opcode 4 (RCA=2, Optical=1).
- **Meter source**: 8-bit @ 184, opcode 8 (Analogue=1, S/PDIF=2, ADAT 1=4, ADAT 2=8).
- **Hardware meters**: tables `meters-l`@136 / `meters-m`@146 / `meters-h`@156 (10 bytes each),
  per-source channel-index tables (255 = unused) for the low/med/high bands.
- **Standalone**: 1 bit @ 188 (no opcode listed → persisted setting). `[INF]`
- **Buffer size** (host ASIO, not a device config write): 32–2048.

## 11. Notifications (device → host async events) `[XML]`

Bitmask values the device raises when state changes (e.g. front-panel button):

| name | value |
|---|---|
| dim-mute | `0x00200000` |
| monitor | `0x00400000` |

(Identical to the USB 8Pre — strong evidence of shared FCP framing.) Delivery mechanism (which of
the 4 MSI vectors + which BAR register): **`[TRACE]`**.

## 12. App storage & firmware `[XML]`

- `appspace`: app region @ offset **200**, total **8392** bytes, persistent store **8192** bytes,
  flashed via opcode **5**.
- Firmware segments:
  - `App_Upgrade` ("App") — `ClarettThunderbolt.tca`, version 1016, version field 32-bit @ offset 8.
  - `FPGA_Upgrade` ("FPGA") — `fp001005_tb_top.bit`, version 1021, version field 32-bit @ offset 12.
- The FPGA segment confirms the Thunderbolt interface is **FPGA-based** (PCIe/TB bridge + routing). `[INF]`

## 13. Open questions → resolve via MMIO/FCP trace

- **Transport framing**: where in the 64 KB BAR the FCP request/response/doorbell live; how
  (opcode, config-offset, length, data) are laid out in a mailbox message.
- **Mixer-gain command** and its coefficient encoding (§6).
- **Clock-source / sample-rate command(s)** (§7).
- **8-bit gain → dB** mapping (§5).
- **MSI vector semantics**: which of the 4 vectors signals mailbox completion vs notifications vs
  audio period IRQs.
- **Routing table**: exact entry ordering and the ~5-entry delta vs the enumerated 86 (§8).

## Provenance

Control semantics, offsets, opcodes, enum values, and channel maps are interface facts taken from
Focusrite's `Clarett 8PreX.xml` descriptor (`FCP Server Resources/`), cross-referenced for meaning
against the in-tree `scarlett2` driver via the USB Clarett descriptor. No driver code was copied.
`[TRACE]`-tagged items are to be independently confirmed by observing the live device, which also
serves as independent corroboration of the XML-derived facts.
