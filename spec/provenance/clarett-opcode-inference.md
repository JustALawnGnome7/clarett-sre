# Clarett — opcode inference from the boot traces (Aug 15 2026)

> **Scope:** what the snd-clarett opcodes outside the `scarlett2` / `fcp-server` vocabulary actually
> do, inferred from the existing `captures/*boot*.log` traces. No hardware was involved — the 2Pre
> had not yet arrived. Everything here is trace-derived and re-checkable by re-running the commands
> quoted below.

**Confidence tags:** `[TRACE]` = read off the captures and cross-checked across models;
`[POS]` = positional evidence only (where it sits in the sequence); `[OPEN]` = unresolved.

---

## 1. The comparison that started this

Opcodes in `driver/clarett.h` + the arm tables, diffed against `mixer_scarlett2.c` and
`fcp-support/server/fcp.c`. fcp-server has the fullest vocabulary and absorbs most of what
looked novel:

| opcode | fcp-server name | had been called |
|---|---|---|
| `0x000001` | `CAP_READ` | "subsystem enable" (`clarett.h` already aliases `FCP_CAP_READ`) |
| `0x001000` | `METER_INFO` | unknown info query |
| `0x002000` | `MIX_INFO` | unknown info query |
| `0x003000` | `MUX_INFO` | unknown info query |
| `0x004000` | `FLASH_INFO` | — |
| `0x004001` | `FLASH_SEGMENT_INFO` | "indexed subsystem set" |
| `0x004005` | `FLASH_READ` | — |

The `0xN000` = category-N INFO pattern holds throughout, and the traces show the textbook
discovery flow: `CAP_READ` per category 1-8, then `MUX_INFO`/`MIX_INFO`/`METER_INFO`/`FLASH_INFO`,
then `FLASH_SEGMENT_INFO` per segment index.

**Genuinely absent from both references:** `0x005000`; `0x006000/1/2/3/5`; `0x007000-3`; `0x800005`.
That is the real novel surface — category 5, most of category 6, category 7, one DATA op.

## 2. `0x007002` / `0x007003` — stream width declaration, per speed class `[TRACE]`

Payload `{u32 speed_class, u16 channels}`. The host **supplies** the count, so these declare
geometry rather than query it — the `GET_7.2`/`GET_7.3` names in `clarett.h` are misnomers.
`speed_class` 0/1/2 = single/double/quad, fixed by the `0x006003` immediately preceding.

`0x007002` = TX/playback, `0x007003` = RX/capture. From `8pre_boot_to_stream.log`, which sweeps
all six rates:

| speed | rates | TX | RX |
|---|---|---|---|
| 0 | 44.1/48 | 20 | 20 |
| 1 | 88.2/96 | 16 | 16 |
| 2 | 176.4/192 | 14 | 14 |

20 -> 16 -> 14 is ADAT S/MUX 8->4->2 in both directions: **the vendor XML's `pin`/`pin-m`/`pin-h`
model confirmed from the wire**, not inferred from the descriptor.

The driver declares nothing and keeps the single-speed width at every rate. That works (SMUX'd
channels go silent instead of shrinking the stream, hardware-confirmed Aug 12) but it is not what
the vendor does.

**Fix available now:** `clarett_main.c:2697` tags 8Pre `playback_channels = 20` as
`[XML] Playback 1-20 (untraced)`. The trace has it — `0x007002` speed 0 = `0x14`. Re-tag `[TRACE]`.

## 3. `0x005000` — one per stream channel, not a "push" `[TRACE]`

Exactly one `0x005000` follows each width declaration, matching the declared count. Verified
14/14 across four models and three speed classes:

```
2Pre   TX speed0:  4 ->  4      8Pre   TX speed0: 20 -> 20
2Pre   RX speed0: 14 -> 14      8Pre   TX speed1: 16 -> 16
4Pre   TX speed0:  8 ->  8      8Pre   TX speed2: 14 -> 14
4Pre   RX speed0: 20 -> 20      8PreX  TX speed0: 28 -> 28
                                (+ RX identically in each case)
```

The payload is a bare `{u16 id}` with no data, so it cannot push anything. Combined with the
existing observation that its responses carry port names, it is a **per-port descriptor read**.
`CONFIG_PUSH` is the wrong name; `PORT_INFO` fits.

The 2Pre's 42 occurrences are 21 ids x the doubled session block. Ids `3/5/6` (plus `4/7/8` on the
8PreX) sit outside the stream-port runs — non-stream entities, unidentified.

## 4. `0x006003` — SET_CLOCK `{u32 rate, u32 source}` `[TRACE]`

Rates seen: `0xac44` 44100, `0xbb80` 48000, `0x15888` 88200, `0x17700` 96000, `0x2b110` 176400,
`0x2ee00` 192000. Source `24` = internal (matches `CLARETT_CLOCK_INTERNAL`); one instance uses
source `3`, an external source. `fcp_decode.py` renders this as `off=`/`len=`, which is
misleading — it is a rate/source pair, not a range.

## 5. Unresolved

- `0x006000` / `0x006001` / `0x006002` / `0x006005` — all zero-payload, so only positional
  evidence. `0x006002` immediately follows every `SET_CLOCK` and precedes the width declarations,
  which would fit "read back the resulting speed class" — the host does need that value to pick
  the index. `[POS]`, not a finding.
- `0x800005` — reads `{off 0, len 8}` as the session's first command and `{off 0, len 12}` later,
  identically on every model. A small fixed header read; nothing in the traces says of what.
  `[OPEN]`
- `0x005000` ids `3/5/6` (`4/7/8` on the 8PreX). `[OPEN]`

## 6. What this says about the bring-up and `force_arm`

The 2Pre's open block repeats verbatim (`#5-#33` == `#34-#62`), and the whole port-descriptor block
**re-runs on every rate change**. So the sequence is a per-configuration ritual the vendor performs
whenever stream geometry changes, not a one-time initialisation.

That matters because `force_arm`'s premise is a one-time bring-up for a never-configured device,
and nothing supports it:

- No never-configured unit has ever been observed; every device in the project arrived configured.
- Every capture the sequence was derived from was taken on a *configured* device, so it is evidence
  about vendor session-open behaviour, not about virgin hardware.
- A fresh device cannot be distinguished from an armed one by any host-visible means.
- The observation that once justified always-arm (input meters reading flat 0) was reattributed on
  Aug 12 to the cold-attach readiness race, which arming does not fix.
- On USB, the analogous init (`scarlett2_usb_init`, commented in-tree as *"Cargo cult proprietary
  initialisation sequence"*) is **three steps and runs at every driver load** — per-session, not
  per-lifetime, with no notion of a virgin unit needing more.

Earlier claims that parts of the sequence are "known-skippable" do not survive scrutiny: those
subsets were skipped on devices where the *entire* sequence is already a no-op, so they carry no
information. Nothing is established about what a virgin device needs, including whether it needs
anything at all.

Note also that most of the step count is interrogation, not configuration. Of the 2Pre's 152 steps:
16 `CAP_READ`, 12 `FLASH_SEGMENT_INFO`, 13 `GET_DATA`, 4 `0x800005`, ~28 zero-payload info queries,
and 42 port-descriptor reads are all reads. The state-setting remainder is 16 `SET_MIX`,
3 `SET_MUX`, 9 `SET_DATA` (whose payloads the driver replaces with the device's own bytes) and one
`DATA_CMD`.

## 7. When the 2Pre arrives

Ordered by value. None of these need a virgin device.

1. **Confirm the port-descriptor reading.** `0x005000 {u16 id}` via the hwdep for an id in the 2Pre's
   capture run (`0x0d`, `0x0e`, `0x15`..`0x1e`, `0x27`, `0x28`) and for a TX id (`0x2b`..`0x2e`).
   Expect a port name in the response. `tools/fcp_cfg_read.c` is the closest existing bench tool to
   adapt. This promotes §3 from inference to confirmed and justifies the rename.
2. **Resolve `0x800005`.** Send `{0, 8}` and `{0, 12}` and read the DMAed response. One command,
   currently pure `[OPEN]`.
3. **Probe category 6.** `0x006000`/`0x006001`/`0x006002`/`0x006005`, zero payload, at idle and
   again right after a rate change. If `0x006002` returns 0/1/2 tracking the rate, §5's hypothesis
   is settled.
4. **Test the width declarations at high speed.** Declare `0x007002`/`0x007003` with the vendor's
   speed-1 counts before streaming at 96 kHz and see whether anything changes versus our
   declare-nothing approach — in particular whether the meter table follows (the open
   metering-display caveat at 2x/4x).
5. **Do not** try to bisect the arm sequence on this device. On a configured device the whole
   sequence is a no-op, so sending 3 steps and sending 152 both look identical to sending 0. There
   is no signal to measure.

## 8. Reproducing the analysis

```sh
python3 tools/fcp_decode.py --brief captures/2pre_boot.log | head -80
python3 tools/fcp_decode.py --brief captures/8pre_boot_to_stream.log |
  grep -E '0x006003|0x00700[23]'
```

The width-vs-descriptor-count check in §3 was a throwaway script: decode each of the four boot
captures, and for every `0x007002`/`0x007003` line count the `0x005000` lines that follow before
any other opcode appears.

## 9. On hardware — 4Pre and 2Pre (Sep 9 2026)

Everything below was read live through the hwdep with `tools/fcp_cmd.c` (the generic bench tool:
opcode, response length and request bytes on the command line). fcp-server stopped for the duration.
Items 1-3 of §7 are settled; item 4 became moot.

### `0x007001` is STREAM_INFO `{u8 speed_band}` and bands 1-2 are the S/MUX widths `[HW]`

Reply `{u16 playback, u16 capture, u32, u32}`. The vendor asks bands 0, 1 and 2 on every model.

| model | band 0 | band 1 | band 2 | band 3 |
|---|---|---|---|---|
| 4Pre | 8 / **20**, 14, 17 | 8 / **16**, 9, 17 | 8 / **14**, 9, 14 | all zero |
| 2Pre | 4 / **14**, 14, 17 | 4 / **10**, 9, 17 | 4 / **8**, 9, 14 | repeats band 2 |

The capture words at bands 1 and 2 equal the `rx_live_mid`/`rx_live_high` values the driver carried
per model (4Pre 16/14, 2Pre 10/8; the 8Pre trace's `0x007003` declarations give 20/16/14 too). The
driver now reads them at probe (`clarett_read_rx_live()`) and the per-model fields are gone. The
trailing two words (14/9/9 and 17/17/14, identical on both models) are undecoded. Out-of-range bands
are not an error but behave differently per model, so stop at band 2.

### `0x005000` is a FIXED string table, identical on both models `[HW]`

| id | name | id | name |
|---|---|---|---|
| 0x01-0x02 | `Clarett8Pre` (on a 4Pre and a 2Pre alike) | 0x0d-0x14 | `Input 1`..`Input 8` |
| 0x03 | `Internal` | 0x15-0x16 | `S/PDIF 1`..`2` |
| 0x04 | `Word Clock` | 0x17-0x26 | `ADAT 1`..`ADAT 16` |
| 0x05 | `S/PDIF` | 0x27-0x28 | `Loop 1`..`2` |
| 0x06 | `ADAT` | 0x29-0x2a | `MIDI In`, `MIDI Out` |
| 0x07-0x08 | `ADAT1`, `ADAT2` | 0x2b-0x46 | `Output 1`..`Output 28` |
| 0x09-0x0c | `Analog`, `S/PDIF`, `ADAT`, `LOOP` | 0x47+ | clamps to `Output 28` (4Pre) / `Output 5Pre` (2Pre, a read past the table) |

So it confirms the model-independent id enumeration §3 derived, and it cannot enumerate a model's
ports (a 4Pre names ADAT 9-16 and Output 28). Ids 3-8 are the clock-source names, which resolves the
§5 `[OPEN]` on ids 3/5/6 (and 4/7/8).

### The per-channel `0x005000` burst is NOT needed to stream `[HW]`

Streaming with the whole stage removed (the `0x007002`/`0x007003` declarations and every `0x005000`),
capture and playback:

| model | rates | result |
|---|---|---|
| 4Pre | 48 / 96 / 192 kHz | exact byte counts, same six live channels as with the burst, `late=0 badreads=0` |
| 2Pre | 48 / 96 kHz | exact byte counts, channels 0-1 live, `late=0 badreads=0 overrun=0` at 48k |
| Red 8Line | 48 kHz | streamed with the stage skipped from the start (§ Red range in CLAUDE.md) |

The stage is removed from the driver. The 8PreX symptom it was kept for (28 ch folding to 12) matches
the TX page-straddle bug fixed later, not the burst. Untested: the 8PreX itself.

### Category 6 and the rest `[HW]`

| query | 4Pre | 2Pre |
|---|---|---|
| `0x006000` (any payload) | `0x00030018 0x00050003` | `0x00030018 0x00050004` |
| `0x006001` | 44100, 48000 | same |
| `0x006002` | 48000, `0x18`, `0x18` | same |
| `0x006005` | 48000 | same |
| `0x800005 {0,8}` | `{1, 3}` | `{1, 4}` |
| `0x800005 {0,12}` | `{1, 3, 0}` | — |
| `METER_INFO` | `00 02 0c` | same |
| `MIX_INFO` | `10 1e 10 0d 07` | same |
| `MUX_INFO` | 74 / 70 / 68 | 64 / 60 / 58 |
| `FLASH_INFO` | `0x3f0000`, 6 segments, 2, 1000 | same |

`0x006000` and `0x800005` both carry a 3 on the 4Pre and a 4 on the 2Pre. The 2Pre's XML gives its
S/PDIF clock value as 4 where every other model uses 3, so one of those words is plausibly the
per-model S/PDIF clock enum. Two data points; a hypothesis, not a finding. `0x006002`'s `0x18`
words are the current source (Internal = 24). `0x007000` answers all zeros with or without a payload.
