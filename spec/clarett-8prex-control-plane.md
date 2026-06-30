# Clarett 8PreX — Control-Plane Spec (clean-room, derived)

**Status:** working spec, authored from interface facts. Sources are tagged per section:
`[XML]` = stated in Focusrite's `Clarett 8PreX.xml` device descriptor (interface fact);
`[INF]` = inferred/derived by us (needs confirmation);
`[TRACE]` = to be confirmed against the live MMIO/FCP capture.

> **⚠ Correction (June 28 2026).** Parts of this spec predate the manifestation-wall finding and
> overstate confirmation. Reality: control writes complete (`done=1, fcperr=0`) but **do NOT physically
> manifest** from our driver, and `GET_DATA` returns an **empty** payload (`size=0`) — the device's
> config backend is dormant for us despite an FC-identical command stream
> (`spec/clarett-8prex-manifestation-wall.md`). The encodings below are still correct (verified against
> FC's live traffic); what is wrong is any claim that our driver *effected* a change. §7 (clock as the
> "cause" of no streaming) and §11 (notification mask) are corrected inline.

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
| 5 | flash / persist app config `[TRACE-CONFIRMED]` |
| 6 | input mode (Mic/Line/Inst) |
| 7 | input "Air" |
| 8 | meter source |

> Note: opcode numbers differ from the USB 8Pre (there air=8, mode=7, flash=6). Use these. `[XML]`
> **Command 5 confirmed live** (`clarett_monitor_mutedim.log`): after a monitor mute/dim change commits
> (command 2), Focusrite Control issues a **standalone `DATA_CMD{activate=5}`** — a bare flash-commit with
> **no preceding `SET_DATA`** — on a short debounce (~52–56 transactions later). This is the persist path,
> and it is *distinct* from the bulk appspace `SET_DATA` write-back of §8/§12. `[TRACE]`
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

**`[TRACE-CONFIRMED on Clarett 4Pre]`** — this input-control block, previously `[XML]`-only, is now
confirmed byte-for-byte against a live FC capture (`4pre_boot_to_stream_end.log`). Toggling Analogue 1
in FC produced exactly the predicted FCP writes:
- **Line In 1 Level** (mode): `SET_DATA{off=166, len=1, val}` + `DATA_CMD{activate=6}`, Inst=`0x02`→Line=`0x01`.
- **Line In 1 Air**: `SET_DATA{off=174, len=1, val}` + `DATA_CMD{activate=7}`, on=`0x01`→off=`0x00`.

So mode@166/opcode 6 and air@174/opcode 7 (with the Mic/Line/Inst = 0/1/2 enum) are confirmed live, and
the encoding **ports across the Clarett TB line** (the 4Pre shares the 8PreX's input-control offsets and
opcodes). Note: like every other control, these are FC's *own* writes — they manifest because FC drives
the device; our driver replaying the identical bytes still hits the manifestation wall
(`clarett-8prex-manifestation-wall.md`).

**Model variants of the input block `[XML]`** — the offset/opcode *bases* are shared line-wide, but
*which* inputs expose mode (and whether Mic is an option) differs per model. The table above is the
**8PreX**. The other captured descriptors:

| Model | Jacks | Air inputs | Mode inputs | Mode enum |
|---|---|---|---|---|
| 8PreX | **separate XLR + ¼″** | 1–8 @ 174–181 | 1–8 @ 166–173 | 1–2: Mic/Line/Inst; 3–8: Mic/Line |
| **8Pre** | combo XLR/TRS | 1–8 @ 174–181 | **1–2 only** @ 166/167 | **Line/Inst** (Mic auto) |
| **4Pre** | combo XLR/TRS | **1–4** @ 174–177 | **1–2 only** @ 166/167 | **Line/Inst** (Mic auto) |
| 2Pre | combo XLR/TRS | 1–2 @ 174/175 | 1–2 @ 166/167 | Line/Inst (Mic auto) |

**Why the mode enum differs — it's physical, not a feature gap.** All these units have mic preamps. The
8PreX has **separate XLR and ¼″ ports** per input, so software must explicitly pick Mic/Line/Inst (and
the mode control carries Mic=0). The 8Pre/4Pre/2Pre use a **single combo XLR/TRS jack** per input that
*cannot* take an XLR and a ¼″ plug at once, so the hardware **auto-selects Mic when an XLR is inserted**;
the software mode therefore only chooses **Line vs Inst** for the ¼″ path — Mic is not a software option
(enum `{Line=1, Inst=2}`, no Mic=0). Beyond inputs 1–2, the combo-jack models' remaining preamps are
**air-only** (no mode control at all). The driver encodes this with a per-input `n_modes` (0 = air-only).
Output gains are **identical** between the 8Pre and 8PreX (§5, offsets 32/33/36/37/40/41/44/45/48/49);
the 4Pre/2Pre are subsets.

**Per-input phantom (48 V), phase invert, and high-pass filter are hardware-switched `[HW]`** — each
has a dedicated front-panel push button per XLR input. Their *live state* is not in the config space
and has no FCP opcode (absent from both the XML and the live trace); a driver cannot read or toggle
them. The one software-visible aspect is **phantom-power persistence** (see §10): whether 48 V state
is retained across a power cycle is a configurable setting, even though the on/off itself is physical.

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

**dB↔byte mapping `[TRACE, CONFIRMED encoding]`:** the byte is a 7-bit **attenuation code** equal to
|dB| exactly, linear at **1 dB/code across the whole range**: `0x00` = 0 dB (unity), `0x14` = −20,
`0x28` = −40, `0x3c` = −60, `0x7f` = −127 dB (floor). (Verified 0…−20 dB → `0x00`…`0x14` linearly; the
earlier −6→`0x08` reading was an outlier.) ALSA: a single linear TLV
`DECLARE_TLV_DB_SCALE(tlv, -12700, 100, 0)`, value `v`(0..127) → device code `127 − v`.

> **Provenance of the curve:** measured by turning the **physical monitor knob** (dB read off
> Focusrite Control's display), which is the `<hardware-controls>` master gain — **offset 112,
> command 2** (§9), an **8-bit** mono master. The clean 1 dB/code encoding fits offset 112 perfectly
> (8-bit field, 0..127 = 0..−127 dB). The per-output gains @ 32/33/… (command 1) are now
> **independently confirmed** to share this encoding: a Line 3-4 software-fader capture set −124 dB →
> `0x7c` (124) at offsets 36/37 with `activate=1` (matching −127 → `0x7f` from the knob, i.e. code =
> |dB| at 1 dB/step). So both the command-2 master gain and the command-1 per-output gains use the
> same 7-bit attenuation code. `[TRACE-CONFIRMED]`
>
> **Rejected experiment (do not revisit):** `0x8022_captures.txt` sampled bytes at mailbox
> `0x8022/0x8023` as a *16-bit* value while turning the knob. That offset is the **high half of the
> `cmd` header word**, not a gain field, and the sampling raced the continuous `GET_METER` polling →
> random `byte_lo`, a `byte_hi` that wraps repeatedly across the range, and a mid-run crash. It
> contradicts the clean curve (−20 dB → `0x14` here vs `lo=0x62 hi=0xf0` there) and is **noise, not
> data**. The master gain is 8-bit per the XML; ignore the 16-bit interpretation.

## 6. Mixer (30 × 16)

- 30 input slots → 16 mix buses; each mix bus output is a source pin (`0x300`+). `[XML]`
- Each mixer input slot's *source* is chosen via the routing matrix (§8) — **confirmed**: assigning a
  source to a mixer-input slot emits `SET_MUX` with an entry `dst=0x300+slot ← src`, e.g. adding
  Analogue 2 to slot 2 → entry `Mixer input 0x301 ← Analogue in 0x401`. The slot's *level* into each
  bus is the separate `SET_MIX` coefficient. `[TRACE-CONFIRMED]`
- 30 × 16 = **480 gain coefficients**. The `<mixer>` element carries **no** offset/opcode →
  mix-gain coefficients are set by the dedicated **`SET_MIX` (`0x002002`)** FCP command, not the
  linear config space. `[TRACE-CONFIRMED]`
- **Command CONFIRMED** by an Analogue 1-2 → Monitor 1-2 routing capture (transport spec §8): one
  `SET_MIX` per mix bus, payload `{u16 mix_num, u16 coeff[30]}` — i.e. a whole row (all 30 input
  slots) per command, mix_num = 0…15. Coefficient is a **16-bit** value (not 8-bit like the output
  gains): `0x2000` = unity/0 dB, `0x0000` = off (matches scarlett2's mixer-gain table). `[TRACE/S2]`
- **Routing input→output happens *through this matrix*, not a separate `SET_MUX`.** Patching an input
  to an output sets that input slot's coefficient to `0x2000` in the bus feeding that output.
- **Coefficient = linear amplitude (not a dB code like §5 output gains).** `0x2000` (8192) = unity/0 dB
  and the device **truncates**: `coeff = floor(8192 · 10^(dB/20))`. `[TRACE-CONFIRMED]` Verified at five
  points across a 46 dB span (Analogue 1 → Monitor 1-2 fader sweep):

  | dB | coeff | | dB | coeff |
  |---|---|---|---|---|
  | +6 (max) | `0x3fd9` (16345) | | −20 | `0x0333` (819) |
  | 0 | `0x2000` (8192) | | −40 | `0x0051` (81) |
  | −6 | `0x1009` (4105) | | | |

  Truncation (not rounding) is proven by −6 → `0x1009` (4105, not 4106) and −40 → `0x0051` (81, not 82).
  Fader fully down → **`0x0000`** (no nonzero clamp / special mute value). So the full range is
  **`0x0000` (−∞) … `0x2000` (0 dB) … `0x3fd9` (+6 dB max)**. This is the **opposite convention** to the
  §5 output gains (a nearest-|dB| attenuation code); the mixer is linear-amplitude, matching scarlett2.
  `[TRACE-CONFIRMED, full curve]`
- A stereo route (e.g. into Monitor Out 1-2) writes the *two* corresponding mix buses (0 and 1), each
  with the input's coefficient in its slot; unrouted cells default to unity (`0x2000`), not zero.

## 7. Clocking `[XML]` + `[TRACE-CONFIRMED]`

- `clock-source`: Internal=24, S/PDIF=3, ADAT 1=0, ADAT 2=1, Wordclock=2.
- `sample-rate`: 44100, 48000, 88200, 96000, 176400, 192000.
- Neither has a config offset/opcode → set via a **dedicated FCP command**.
- **`SET_CLOCK = opcode 0x006003`**, payload = two u32 words `{sample_rate, clock_source}`, size=8.
  Completes on vec0 `0x20000000` (DONE) like any mailbox command. **`[TRACE-CONFIRMED]`** — captured
  live by diffing a baseline vs. a sample-rate change (48 kHz→44.1 kHz) in Focusrite Control: the new
  command was `0x80006003` with data bytes `44 ac 00 00  18 00 00 00` = `{0xac44=44100, 0x18=24=Internal}`.
  FC issues it at stream start and on every rate/source change; on a rate change it is followed by a
  full `CONFIG_PUSH` (`0x5000`) re-init burst, the routing re-setup (`0x6004/0x6002/0x6005`), and a
  `DATA_CMD 0x800002 {activate=5}`.
- **Now replayed by the driver — and it is NOT the streaming blocker `[CORRECTED June 28 2026]`.** The
  driver issues `SET_CLOCK` (via the prepare-time stream-config handshake / `inject_clock`), byte-identical
  to FC's `{48000, 24}`. It did **not** make the engine clock: the engine arms, DMAs a brief burst, then
  stalls after one ring pass regardless. So the omitted `0x6003` was a red herring, not "the leading cause."
  The real data-plane blocker is the environmental/below-BAR sustain wall (data-plane spec §9 + memo
  `clarett-dataplane-pcm-findings`); both planes hit the same off-wire wall.

## 8. Routing matrix `[XML]`

A table of *destination ← source* assignments. Size depends on the sample-rate band:

| Band | Rates | XML `num` | **Live wire entries** `[TRACE]` | Zero pad |
|---|---|---|---|---|
| Low | 44.1 / 48 kHz | `num` = 91 | **86** | +16 words |
| Medium | 88.2 / 96 kHz | `num-m` = 75 | **70** | +16 words |
| High | 176.4 / 192 kHz | `num-h` = 67 | **62** | +16 words |

- Channels with `pin-m="0x0"` drop at the medium band; `pin-h="0x0"` drop at the high band —
  dominated by **ADAT S/MUX** (8 → 4 → 2 ch per port) plus reduced high-rate PCM channels. `[XML/INF]`
- **The device writes exactly the enumerated destinations, not the XML `num`.** Band 0's traced table
  is **86** entries = 28 hw outputs + 28 PCM capture + 30 mixer inputs — matching the enumerated count
  exactly. The XML `num`=91 over-counts by 5 reserved/phantom slots that are **never sent on the wire**
  (the `−5` holds across all three bands: 86/70/62 = `num`−5). This resolves the earlier "~5 delta". `[TRACE-CONFIRMED]`
- Each band's payload is then **zero-padded by 16 u32 words** to a fixed declared size (412/348/316 bytes
  = 102/86/78 words for bands 0/1/2). The pad words are all-zero (dst=0, not a valid pin); `fcp_decode.py`
  strips them so reported counts are the real 86/70/62. A destination present in the table but with
  `src=0` is a real, *unrouted* slot — distinct from the dst=0 padding.

**Wire format — `SET_MUX` (`0x003002`) `[TRACE-CONFIRMED]`:** the matrix is written as **three commands,
one per sample-rate band**, payload `{u32 header, u32 entry[]}`:
- `header = band << 16` (band 0 = low, 1 = medium, 2 = high). Each band sends its own (shrinking)
  entry list — the live counts (86/70/62 real entries) confirm the per-band drop, though the absolute
  sizes are the enumerated-destination counts, not the XML `num` (see table above), each +16 zero pad.
- each `entry` packs two **12-bit pins**: `entry = (src_pin << 12) | dst_pin`, top 8 bits 0.
  `src = 0x000` means the destination is **unrouted / silent**. Pins are the §3 direction-scoped values
  (e.g. `0x300+n` mixer-input slot, `0x408/0x409` monitor out, `0x600+n` PCM capture, `0x400+n` analogue).
- Worked entries: `0x400600` = Record 1 ← Mic 1; `0x300408` = Monitor 1 ← Mix 1; `0x60a186` =
  S/PDIF Out 1 ← Playback; `0x401301` = Mixer input slot 2 ← Analogue in 2.
- **Traced stimulus `[TRACE-CONFIRMED]`** (`clarett_mux_full.log`): setting *Monitor Output 1*'s input
  from Analogue 1 → Analogue 2 wrote `0x401408`/`0x401409` (Monitor Out 1 L/R ← Analogue in 2), i.e.
  `0x408/0x409 <- 0x401`; previously `<- 0x400`. Three `SET_MUX` (one per band), **no `SET_MIX`** — a
  direct output-source reassignment is MUX-only (unlike a mixer-input edit, which also rewrites `SET_MIX`).
- A control change that touches routing rewrites the **whole** matrix (all three bands). Whether a
  `SET_MIX` rewrite accompanies it depends on the action: a **direct output-source reassignment** (e.g.
  Monitor Out ← a different analogue input) is **`SET_MUX` only** (`[TRACE]`, `clarett_mux_full.log`),
  whereas a **mixer-input edit** (adding/removing a channel *into a mix*) also rewrites the affected
  `SET_MIX` bus. `--classify` distinguishes them: MUX-only → routing/patchbay; MUX+MIX → mixer-matrix edit.

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
- **Phantom-power persistence** `[HW]`: the device exposes a software toggle for whether per-input
  48 V state is retained across a power cycle (the 48 V on/off itself is a front-panel hardware button,
  §4 — only its persistence policy is configurable). Config offset/opcode **not yet identified `[TRACE]`**.
- **Buffer size** (host ASIO, not a device config write): 32–2048.

## 11. Notifications (device → host async events) `[XML]`

Bitmask values the device raises when state changes (e.g. front-panel button):

The XML lists these names/values (identical to the USB 8Pre — shared FCP framing):

| name | XML value |
|---|---|
| dim-mute | `0x00200000` |
| monitor | `0x00400000` |

**⚠ The XML mask is NOT what the device actually raises `[CORRECTED June 28 2026, live 8PreX]`.**
Pressing the front-panel **Mute** or **Dim** button raises cause `0x400 = 0x00000003` (bits 0|1) on
**every** press, with `0x00200003` (adds bit 21) appearing intermittently. The earlier claim that Mute
produced `0x00200000` "exactly" and that `0x3` was mere mailbox-status noise was **wrong** — the driver
masked `0x00200000|0x00400000`, saw `masked=0x0`, and never fired the notify handler. The driver now uses
`NOTIFY_MONITOR_MASK = 0x3 | 0x00200000` (`clarett.h`), verified `masked=0x3` on every press. Per-bit
semantics (which bit = mute vs dim vs the `0x200000` aux) are not yet pinned down; `notify_work` re-reads
the whole monitor region on any of them.

**Delivery mechanism — `[TRACE/HW]`:** a front-panel event sets the cause bits above in cause register
**`0x400`** (read-to-clear; the device re-asserts while it has an unsatisfied event). **The MSI is delivered on
vector 0** — bare-metal `/proc/interrupts` shows only vec0 ever fires (mailbox-done *and*
notifications); the cause-register index is independent of the MSI vector index. The host then
**re-syncs state**: `GET_DATA{offset=24, len=92}` to re-read this monitor region (§9), followed by
`GET_MUX` per sample-rate band. Driver implication: hook **vec0**, read `0x400` there (not `0x100` —
that is the mailbox poll's), and if a notification bit is set re-read the monitor config (offset 24).
**The control plane is event-driven — no polling occurs when idle** (the continuous `GET_METER`
traffic is only the GUI's meter view).

## 12. App storage & firmware `[XML]`

- `appspace`: app region @ offset **200**, total **8392** bytes, persistent store **8192** bytes,
  flashed via opcode **5**. **Layout confirmed by trace:** some control changes trigger a write-back of
  the full 8192-byte store via `SET_DATA` in 1016-byte (1 KB-payload) chunks from offset 200 to 8392
  (transport spec §8); that bulk write-back is RAM-only (no opcode-5 flash follows it). `[TRACE]`
- **Two distinct persist behaviours observed — they do not co-occur:** the monitor mute/dim change
  (`clarett_monitor_mutedim.log`) does the *opposite* — a standalone `DATA_CMD{activate=5}` flash-commit
  with **no** appspace `SET_DATA` write-back at all (see §2). So persist is per-control: a bulk RAM
  write-back for some controls, a bare opcode-5 flash-commit for the monitor section. The full trigger
  matrix (which controls take which path, and when the RAM store is actually flashed) is **`[TRACE]`**. `[TRACE]`
- Firmware segments:
  - `App_Upgrade` ("App") — `ClarettThunderbolt.tca`, version 1016, version field 32-bit @ offset 8.
  - `FPGA_Upgrade` ("FPGA") — `fp001005_tb_top.bit`, version 1021, version field 32-bit @ offset 12.
- The FPGA segment confirms the Thunderbolt interface is **FPGA-based** (PCIe/TB bridge + routing). `[INF]`

## 13. Open questions → resolve via MMIO/FCP trace

- **Transport framing**: where in the 64 KB BAR the FCP request/response/doorbell live; how
  (opcode, config-offset, length, data) are laid out in a mailbox message.
- ~~**Mixer-gain command** and its coefficient encoding~~ — **fully resolved: `SET_MIX` (`0x002002`),
  `{u16 mix_num, u16 coeff[30]}`; coeff = `floor(8192·10^(dB/20))`, `0x0000`…`0x2000`(0 dB)…`0x3fd9`(+6)** (§6).
- **Clock-source / sample-rate command(s)** (§7).
- **8-bit gain → dB** mapping (§5).
- **MSI vector semantics** (partial, corrected on HW): the device fires **only vec0** for all
  control-plane events — bare-metal `/proc/interrupts` shows vec1–3 never increment. The cause
  registers distinguish events (`0x100` = mailbox-done, `0x400` = notifications), independent of the
  MSI vector. **vec1/vec2/vec3** are unused so far — likely the data-plane period IRQs once streaming.
- ~~**Routing table** wire format~~ — **resolved: `SET_MUX` (`0x003002`), three per-band commands,
  entry = `(src_pin<<12)|dst_pin`** (§8). Remaining detail: the exact entry ordering / the ~5-entry
  delta vs the enumerated 86 is now directly readable from a full-matrix capture if needed.

## Provenance

Control semantics, offsets, opcodes, enum values, and channel maps are interface facts taken from
Focusrite's `Clarett 8PreX.xml` descriptor (`FCP Server Resources/`), cross-referenced for meaning
against the in-tree `scarlett2` driver via the USB Clarett descriptor. No driver code was copied.
`[TRACE]`-tagged items are to be independently confirmed by observing the live device, which also
serves as independent corroboration of the XML-derived facts.
