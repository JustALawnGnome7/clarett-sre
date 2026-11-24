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

> **Superseded (Nov 17 2026, §12):** these are channel-list reads `{u32 band}`, one 16-byte record per
> stream channel; the reply was never visible in the traces.

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
| 0x09-0x0c | `Analog`, `S/PDIF`, `ADAT`, `LOOP` | 0x47+ | ~~clamps to `Output 28` (4Pre) / `Output 5Pre` (2Pre, a read past the table)~~ **refused** (§12: the clamp was a stale response buffer) |

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

## 10. Addendum (Oct 12 2026): STREAM_INFO re-read on the 2Pre, and the port to main

Read with `tools/fcp_cmd.c` through main's driver (fcp-server stopped), 20-byte replies:

| band | playback / capture | trailing words |
|---|---|---|
| 0 | 4 / **14** | 11, 14 |
| 1 | 4 / **10** | 9, 16 |
| 2 | 4 / **8** | 9, 14 |
| 3 | empty reply (size 0) | — |

- **The capture widths repeat §9 exactly** (14/10/8), and equal main's `rx_live_mid`/`rx_live_high`
  table for the 2Pre.
- **Band 3 is an empty reply, not a repeat of band 2.** Main now fills the response payload with a
  poison byte (`0xa5`) before every command, and band 3 came back as poison, untouched. §9's
  "repeats band 2" was most likely band 2's reply still sitting in the response buffer, read before
  that fill existed. The 4Pre's "all zero" may be the same thing. Nothing asks past band 2.
- **The trailing words differ from §9** in bands 0 and 1 (§9: 14/17 and 9/17), so §9's "identical on
  both models" does not hold across sessions either. Still undecoded; they may track device state.

**Ported to main (snd-clarett), with one change of design.** Probe reads bands 1 and 2 after the
model is final (`clarett_read_rx_live()`; after the Red twin decision, since twins share band 0) and
the driver uses the device's answer. Unlike the branch, the per-model table is KEPT: it is the
fallback for a band the device does not answer (the branch fell back to full width, which would pass
the S/MUX residue through to the capture), and a cross-check that warns on any disagreement, so the
8Pre/8PreX/Red tables, never read live at bands 1-2, are checked the first time each attaches. On the
2Pre: no warning at probe (agreement), and the digital-loopback ramp passed 9/9 at 48/96/192 kHz with
the removed channels (9-12, 7-12) all zero.

## 11. Addendum (Oct 16 2026): `0x800005` on every model, the serial, and the firmware words

Read on all five Thunderbolt models with `tools/bench/unit_ident.sh` (fcp-server stopped; appspace
bytes 0-127, `0x800005 {0, 8}`, the first 32 bytes of flash segments 3 and 4), through main's driver.

**`0x800005` is a {family, model} pair, not the S/PDIF clock value.** §9 read `{1, 3}` on the 4Pre and
`{1, 4}` on the 2Pre and suggested the second word might be the per-model S/PDIF clock enum, since the
2Pre's XML gives S/PDIF as 4. With the other three units:

| model | `0x800005 {0,8}` |
|---|---|
| Clarett 8Pre | `{1, 1}` |
| Clarett 8PreX | `{1, 2}` |
| Clarett 4Pre | `{1, 3}` (as §9) |
| Clarett 2Pre | `{1, 4}` (as §9) |
| Red 8Line | `{2, 6}` |

The S/PDIF clock value is 3 on every Clarett, the 2Pre included (measured with `Sync Status` against a
live S/PDIF source; the 2Pre's XML is wrong there), so the 8Pre's 1 and the 8PreX's 2 rule that
reading out. Five distinct values over five models, the first word 1 on every Clarett and 2 on the
Red, fit a model identifier, and would explain why the vendor driver sends it first. Not yet shown:
that a second unit of a model gives the same value (one unit per model so far), and whether the Red
twins (8Line/4Pre, 16Line/8Pre, which share stream geometry) differ; no Red Pre or 16Line has been
read. Replies were stable across reads. A Clarett 8Pre **USB**, switched to the kernel `fcp` driver for
the test (`snd_usb_audio vid=0x1235 pid=0x8208 device_setup=8`), answers `0x800005` at lengths 8, 16, 32
and 64 with FCP error 7, as it does `DEVMAP_INFO`, while `GET_DATA` works on the same path: the query
is Thunderbolt-firmware only, as fits USB models already being told apart by their USB product ID.
§9 also noted `0x006000`'s second word tracking the same 3/4; it has not been read on the other three.

**The serial is in flash `App_Env`, as the branch's note said.** The segment (index 4 on both the Red
and the Claretts; 64 KB) begins with one ASCII line, `SERIAL_NO = <decimal>`, and is otherwise erased
(`0xff`). Six or seven digits, different on every unit; the Clarett lines end in a newline, the Red's
does not. The values are deliberately not recorded in this repo. Main's driver now reads it at probe
(segment found by name) and exposes it as `/sys/bus/pci/devices/<bdf>/serial`, never logged.

**The firmware versions are appspace words.** The descriptors' `<firmware>` `<version offset-bytes>`
are appspace offsets: App at 8, FPGA at 12 (low 16 bits), and on a Red the front panel at 20. Every
Clarett reads App 1016 / FPGA 1021; the Red 8Line App 1077 / FPGA 1020 / Front 242. All match the
versions the descriptors name. Appspace word 12 equals BAR0 `0x000`, so that register (main's
Red-vs-Clarett "family register") is the FPGA version word: the build in the low 16 bits, a family code
above (`0x0320` Clarett, `0x0420` Red). Flash agrees for the App (`App_Upgrade`+8); the FPGA segments
instead open with a Xilinx bitstream header naming the design (`fp001005_tb_top` on every Clarett,
`fp001100_tb_top` on the Red). Appspace word 0 (5 on every Clarett, 2 on the Red) and the FPGA `type`
word at 16 (1 / 4, named in the Red XMLs, which pick between two Red 4Pre FPGA images with it) are not
decoded further.

**`FLASH_INFO` replies `{u32 total size, u32 segment count, u8[4]}`**, as §9's table already reads it
(`0x3f0000`, 6 segments, then `02 00 e8 03`). `tools/fcp_flash_dump.c`'s header comment had the count
first; corrected.

**Appspace @80 on the Claretts (@202 on a Red) is the descriptor's `<nickname>`**: a saved, renamable
name, not identity. Its default is `<model>-<serial as 8 hex digits>` on every unit read but the 8Pre,
whose suffix is `00000001`.

### Nov 10 2026: how the vendor stack identifies a unit (`captures/red_8line.log`)

Question: how could the driver tell the Red twins apart (4Pre/8Line, 8Pre/16Line: same stream
geometry) without the unit's Thunderbolt controller, which a VM with the endpoint passed through
cannot see? The driver uses that controller today (Alpine Ridge = a Line model). The capture is
itself that case: the Red was passed through to the Windows guest, so the vendor stack had no
controller to look at either. The trace shows every request and every BAR read, but not the
replies, which the device DMAs into host memory.

**The registers cannot do it.** Every BAR register the vendor driver reads before its first command
(`0x004 = 0x80`, `0x008 = 0x2000`, `0x8000`-`0x801c`, the dummy serial) reads the same on all four
Claretts and the Red, in every capture that has them. `0x000` differs, but only by family.

**The vendor kernel driver runs one generic sequence on every model, and `0x800005` comes first.**
Opening commands, compared across the 2Pre, 4Pre, 8Pre, 8PreX and Red captures:

1. `0x800005 {0, 8}`, then `STREAM_INFO` bands 0-2, `INIT_2` (twice on the Red), `0x006002`
   (the Red adds `0x006004` and `0x006005` here), then `0x006000`.
2. One `0x005000` per clock source: ids 3/5/6 on the 2Pre, 4Pre and 8Pre, 3/5/7/8/4 on the 8PreX,
   3/5/7/8/4/0x0a/9 on the Red. The count matches each model's clock-source list, so it is read from
   the device (`0x006000` is the clock-source table: {name id, clock value} pairs, the 2Pre's S/PDIF
   value 4 where the 4Pre's is 3, as §9 read).
3. `0x006001`, then `0x800005 {0, 12}` again (third word 0 on the 4Pre, §9; undecoded), then
   `0x007000`.
4. `0x007002` carrying the playback width from `STREAM_INFO` (`0x04`/`0x08`/`0x14`/`0x1c`/`0x40`),
   one `0x005000` per channel; the same for capture with `0x007003`.

Everything from step 2 on is driven by the device's own replies, and the `0x005000` strings are a
fixed table per family (§9: ids 1-2 read `Clarett8Pre` on a 4Pre and a 2Pre alike). So the only
per-model reply the kernel driver asks for is `0x800005`.

**RedNet Control's service asks again once it starts** (from seq 288, interleaved with the kernel
driver): `INIT_1` for categories 1-8, the capability reads, the flash segment list
(`0x004000`/`0x004001`), then `0x800005 {0, 8}` twice, then `GET_DATA` at appspace 16 (the FPGA
`type` word: 1 on every Clarett, 4 on the Red 8Line), 8 (App version), 12 (FPGA version) and 20
(front-panel version), and only then the bulk appspace read.

**No vendor file maps a number to a model.** None of the device descriptors carries an id, and
`Classes.xml` only groups descriptor names into classes (Clarett, Clarett USB, Red). The mapping lives
in the vendor software.

**What the Red descriptors do show: each twin pair splits on Pre vs Line.** The twins have identical
`<appspace>` (8508 bytes, app at 316) and `<routing>` sizes. Their `<firmware>` blocks differ:

| | App image | FPGA image(s), selected by `<type offset-bytes="16">` | Front panel |
|---|---|---|---|
| Red 4Pre, 8Pre | `RedApp.tca` (1049) | `fp001054_tb_top.bit` (657) or `RedPreFPGA.bit` (1020); "one of the FPGA segments will be removed at runtime" | `Red4Pre…`/`Red8PreFrontPanel.bin` |
| Red 8Line, 16Line | `RedLineApp.tca` (1077) | `RedLineFPGA.bit` (1020) | `Red8Line…`/`Red16LineFrontPanel.bin` |

So a Pre and a Line run different App and FPGA images, and the `type` word chooses a Pre's FPGA image.
It is therefore a hardware-type value that the Pre models have at least two of, and it may well
differ from the Line models' (4 on the 8Line). The App version is no use as an identity (it moves with
updates); the `type` word could be.

**Conclusion.** `0x800005` is the vendor stack's identity source: it is the first thing both the
kernel driver and RedNet Control ask, it is the only per-model query in the kernel driver's
bring-up, and its second word already differs on all five units read. The appspace `type` word at 16
is the second candidate, at least for Pre vs Line, which is exactly what separates the twins. Neither
needs the Thunderbolt controller. **Still needed:** a `0x800005 {0,8}` reply and appspace @16 from a
Red 4Pre, 8Pre or 16Line (`tools/fcp_cmd`, `tools/bench/unit_ident.sh`). If they separate the
twins, the driver keys on `0x800005` and keeps the controller check as a logged cross-check.

**The `type` word at appspace 16, examined (Nov 10 2026).** What the record holds: 1 on all four
Claretts, 4 on the Red 8Line (one unit each; the raw `unit_ident.sh` logs were not kept). No capture
writes offsets 16-19, so it is a firmware-supplied value. `<type offset-bytes="16">` appears only in
the four Red descriptors (both XML sets), always inside an `FPGA_Upgrade` segment. The Red 4Pre and
8Pre list two FPGA images ("one of the FPGA segments will be removed at runtime"): `fp001054_tb_top.bit`,
frozen at version 657 in both sets, and a second line (`Red4PreFPGA.bit` 951 in the 2023 set,
`RedPreFPGA.bit` 1020 in RedNet Control 2). The Line models list one. So `type` tells the updater which
FPGA build a unit's hardware takes: in effect a board revision, with at least two among the Red Pres.

**The vendor app reads it only after it has chosen the descriptor.** Focusrite Control reads appspace
8 and 12 on every Clarett and nothing at 16 or 20; RedNet Control reads 16, 8, 12 and 20 on the Red.
Those are exactly the `<version>`/`<type>` fields each descriptor's `<firmware>` block declares, and
in both the reads follow `0x800005`. So the app knows the model before it reads `type`, and uses
`type` only to pick an FPGA image within it. That makes `0x800005` the identity source, and leaves
`type` as an indirect hint for the twins at best: it separates a Pre from a Line only if no Red Pre
revision shares the Line models' value, which the descriptors do not say. Reading it on a Red Pre
(`unit_ident.sh`) answers that; the firmware bundle's `.bit` headers might too (design name and FPGA
part per image), but the bundle is not on the laptop host.

## 12. Addendum (Nov 16-17 2026): the device describes itself — MODEL_ID, clocks, channels; MSI under Windows

All on the 2Pre, laptop host. Device reads with `tools/fcp_cmd.c` through main's driver (fcp-server
stopped); vendor behaviour from two routine Windows-guest traces, MMIO and (the second) MSI,
taken Nov 17 (kept in `/tmp`, not committed).

### Correction to §9: nothing lies past the string table

`0x005000` read for every id 0x0000-0x00ff, plus 256, 512, 1024, 4096, 32768 and 65535, twice each, all
reads agreeing: id 0 answers an empty string, 0x01-0x46 exactly §9's table (ids 1-2 `Clarett8Pre`,
0x46 `Output 28`), and **every id from 0x47 on is refused**. §9's "clamps to `Output 28` (4Pre) /
`Output 5Pre` (2Pre)" was the response buffer read after a refusal, from before the driver failed
refusals (`-EIO`) and poisoned the buffer: the same artifact as §10's "band 3 repeats band 2".
`Output 5Pre` was never a firmware string, so there is no hidden name list after the table, and the
table holds no per-model name: ids 1-2 read `Clarett8Pre` on every Clarett. The table is the line's
superset (word clock, two ADAT ports, `Output 1`-`28`, i.e. sized for the 8PreX), and each model
uses a subset; the vendor requests carry only the 2-byte id, so there is no table selector.

### `0x800005` (MODEL_ID) is a 12-byte record `{u32 family, u32 model, u32}`

`{u32 offset, u32 len}` addresses it like GET_DATA: `{0,4}` = 1, `{4,4}` = 4, `{8,4}` = 0, `{0,12}` =
`1 4 0` (three reads, stable), and any range past byte 12 (`{0,16}`, `{0,32}`, `{8,8}`) is refused. So
the third word is **0 on the 2Pre**, as on the 4Pre (§9); two zeros do not decode it. It is neither
the appspace FPGA `type` word (@16 = 1) nor the 2Pre's word @20 (5). Next reading worth having: the
Red 8Line's.

The driver now identifies by it (snd-clarett, uncommitted at the time of writing): MODEL_ID names
the entry, STREAM_INFO band 0 cross-checks it, and geometry matching remains for the models never
read. On the 2Pre: `Clarett 2Pre: identified by MODEL_ID {1, 4}`.

**Hypothesis (operator's): the model word indexes one list of model names shared by both families.**
8Pre 1, 8PreX 2, 4Pre 3, 2Pre 4 (Clarett); the first two Reds reused Clarett names (4Pre, 8Pre), and
the first new names were appended in release order: Red 16Line (released 2017) = 5, Red 8Line (2020)
= 6, measured. It fits all five readings with no invented products, explains why the 8Line is 6, and
predicts **Red 8Pre `{2, 1}`, Red 4Pre `{2, 3}`, Red 16Line `{2, 5}`**: one reading of any of those
units confirms or breaks it. The family word is then what tells a Clarett 4Pre from a Red 4Pre. Kept
out of the driver's table until read: a wrong guess would turn a working geometry match into a
refusal. Two coincidences fit it too, neither evidence: the shared Clarett firmware names itself
after model 1 (`Clarett8Pre`, above), and the Red 8Line's Thunderbolt DROM names it `Red16Line`
after model 5.

### `0x006000` is the clock-source list; the vendor sends its values, so S/PDIF is 4 on the 2Pre

`0x006000` (any payload, identical replies) -> `{u16 clock value, u16 name id}` pairs: `24/3`,
`4/5`, `0/6` = **Internal 24, S/PDIF 4, ADAT 0**, the names being `0x005000` ids 3/5/6. `0x006001`
lists the supported rates as u32s (44100, 48000, 88200, 96000, 176400, 192000); `0x006002` answers
`{u32 current rate, u16 source, u16 source}`.

The trace settles which value the vendor uses: at attach it reads `0x006000`, then `0x005000` ids 3,
5, 6 (the vendor driver's "one `0x005000` per clock source", §11 step 2), and Clock Source -> S/PDIF
in Focusrite Control sends **`0x006003 {192000, 4}`**, back to Internal `{192000, 24}`. Nothing
between attach and the change reads the S/PDIF Source setting or re-reads `0x006000`: the value is the
table's, not derived. It matches the descriptors, which give S/PDIF as a fixed per-model option: 4
on the 2Pre (one S/PDIF source, Optical), 3 on the 4Pre, 8Pre and 8PreX (RCA or Optical). With the
Aug 14 measurement (on the 2Pre, 3 locks only to S/PDIF, 4 to S/PDIF or ADAT, both over TOSLINK),
4 reads as "clock from the optical input", 3 as "from S/PDIF": a reading, not proven.

Each clock change also raises the `0x400` sync event (bit 3) and is followed by `0x006002` and a
fresh `0x007002`/`0x007003` + per-channel `0x005000` burst.

The driver now builds its Clock Source items from `0x006000` + `0x005000` (model table as the
fallback). On the 2Pre: Internal 24, S/PDIF 4, ADAT 0; `tools/bench/clock_live.sh` passed
(Unlocked/Locked both ways, 960,000 frames 0 breaks, raw and with PipeWire holding the card).
**This corrects the driver's long-standing S/PDIF = 3 on the 2Pre** (and the "S/PDIF = 3 on every
model INCLUDING the 2Pre" reading in CLAUDE.md: 3 does select S/PDIF there, but it is not what the
vendor sends).

### `0x007002`/`0x007003` are channel lists, not width declarations (corrects §2)

`{u32 speed band, u16 count}` -> one **16-byte record per stream channel** at that band, in stream
order: `{u8 0, u8 index, u16 port id, u16 port id, u16 port class, u32 0, u32 type}`. The count is
ignored (4 and 32 both return the full list); a band past 2 is refused. Port id and class are
`0x005000` ids. The 2Pre's capture list:

| index | 0 | 1 | 2 | 3 | 4 | 5 | 6-13 |
|---|---|---|---|---|---|---|---|
| port | Input 1 (0x0d) | Input 2 | S/PDIF 1 (0x15) | S/PDIF 2 | Loop 1 (0x27) | Loop 2 (0x28) | ADAT 1-8 (0x17-0x1e) |
| class | Analog (0x09) | Analog | S/PDIF (0x0a) | S/PDIF | **LOOP (0x0c)** | **LOOP** | ADAT (0x0b) |
| type | 1 | 1 | 2 | 2 | 7 | 7 | 4 |

Band 1 returns the first 10 (ADAT 1-4), band 2 the first 8 (ADAT 1-2): the S/MUX widths, as
STREAM_INFO's. Playback (`0x007002`) lists Output 1-4 (0x2b-0x2e, class Analog). So the device
reports where its loopback pair sits (index 4 on the 2Pre: main's measured `rx_loopback_at`), and
the identity of every stream channel at every speed. The vendor driver reads both at every stream
setup and then names each record's port with `0x005000`, which is the per-channel burst §3 traced.
The driver now takes the loopback position from it (model table as the fallback and cross-check).
**Hardware-tested Nov 17 2026 on the 2Pre** (snd-clarett `06c8383`, srcversion `3B1CC8E7505DB019F53A673`):
probe logged `loopback pair at capture channel 4` with no cross-check warning, and the
digital-loopback ramp (`tools/bench/loopback_order.sh`) passed 9/9 sample-exact at 48/96/192 kHz,
"PCM 13"/"PCM 14" on ch 13/14, ADAT 1 on ch 5, the S/MUX-dead channels zero. That commit's message
says the device-read loopback position was not yet tested on hardware; it was tested after the
commit, and passes.

**The other four models, the same build, the same day** (laptop, one unit at a time, module loaded
with `dyndbg` on `clarett_models.c`; `loopback_order.sh` then `clock_live.sh` against the device's
own ADAT item):

| | MODEL_ID | bands (capture live) | clock list from `0x006000` | loopback | ramp | clock_live |
|---|---|---|---|---|---|---|
| 4Pre | `{1,3}` | 3 (20/16/14) | Internal 24, S/PDIF 3, ADAT 0 | ch 10 | 9/9 | PASS |
| 8Pre | `{1,1}` | 3 (20/16/14) | Internal 24, S/PDIF 3, ADAT 0 | ch 10 | 9/9 | PASS |
| 8PreX | `{1,2}` | 3 (28/20/16) | Internal 24, S/PDIF 3, ADAT1 0, ADAT2 1, Word Clock 2 | ch 10 | 9/9 | PASS |
| Red 8Line | `{2,6}` | 3 (60/52/32) | Internal 24, S/PDIF 3, ADAT1 0, ADAT2 1, Word Clock 2, Dante 4, Loop Sync 5 | (not reordered) | 8/8 | PASS |

No cross-check warning on any unit: every device-read value agrees with the model table it replaces.
Ramps sample-exact at 48/96/192 kHz with the S/MUX-dead channels zero; clock_live 960,000 frames,
0 breaks, Sync Unlocked/Locked both ways raw and with PipeWire holding the card. So S/PDIF = 4 is the
2Pre alone. The 8PreX's and Red's item names differ from the old table (`ADAT 1`/`ADAT 2`/`Wordclock`),
so a saved alsactl state on one of those three sources fails to restore once; the bench presets now
use `ADAT1`.

### Under Windows the vendor driver is interrupt-driven, idle as well (corrects `clarett-data-plane.md` §17)

Two 2Pre traces with `vfio_msi_interrupt` (needs `x-no-kvm-msi=true`), Nov 16 (~5900 interrupts) and
Nov 17 (17,007 over two minutes: attach, idle, two clock changes, a knob, three streams):

- **Every interrupt arrives on vector 0**, although 4 are enabled.
- **After each one the vendor reads `0x100, 0x300, 0x200, 0x400, 0x500`**, p50 29 us later (Nov 17),
  whether streaming or idle; that sweep is its interrupt handler. While streaming the period event
  comes every 4 ms (250/s), and a separate 10 Hz sweep reads the ring pointers and the
  `0x800`/`0x880` blocks. The "4 ms timer, never used the interrupt" reading of `captures/red_8line.log`
  (§17) was wrong: that capture had no MSI trace, and the 4 ms cadence is the period event.
- **Idle with Focusrite Control open:** ~24 `GET_METER`/s and ~1.5 interrupts per command (the
  `0x400` phase bits 1 and 2, sometimes one interrupt for both). Attached but with Focusrite Control
  not yet connected: **no interrupts and no traffic at all** for 30 s.
- **What each sweep found** (Nov 17): 9,009 RX periods, 5,772 mailbox phases, 93 both, one stop
  class (`0xf`), and 246 nothing. 238 of those 246 came ~115 us after a mailbox interrupt whose sweep
  had already read `0x400 = 0x3`: both phases latched at once, and the second phase's interrupt then
  arrives after its cause has been collected. Interrupt delivery lags the register; not spurious.
  (Likely the Nov 16 trace's 353 sweeps that read `0x300 = 0`.)
- **Knob (`0x400` bit 22), Focusrite Control's response per event:** one `GET_DATA {24, 92}` (the
  monitor region), three `MUX_READ`s (two entries at index 14/10/8, one per speed band: by position,
  what feeds Monitor Outputs 1-2), then, if the level moved, `SET_DATA` 32 and 33 with `DATA_CMD 1`
  each, and an 8 KB app-storage persist. So Focusrite Control mirrors the knob into the SW gains
  itself, as main's driver does in `hw_gain_follow`, and it reads the region once per notification,
  not once per control.

Still open: whether `0x108`/`0x10c` route causes to vectors 1-3 (needs writes, so a scratch build).

## 13. Addendum (Nov 24 2026): category 8 reports the MIDI ports; the opcodes the vendor sends that we do not `[HW]` `[TRACE]`

**Question:** can a driver tell whether a unit has DIN MIDI from the device, not a per-model list?

**What does not tell.** The MIDI UART block is present on the Red 8Line, which has no MIDI jacks:
`0x514` = `0x847` and `0x500` idle `0xff0000` on every unit, so register probing cannot. The `0x005000`
string table names `MIDI In`/`MIDI Out` (ids `0x29`/`0x2a`) but is a fixed table (§9). The
`0x007002`/`0x007003` channel lists cover stream channels only. No vendor XML has a MIDI element.

**What the vendor driver does** (every boot capture: 2Pre, 4Pre, 8Pre, 8PreX, Red 8Line). Its attach is
access-for-access the same on both families up to the first command, the MIDI setup included (read `0x514`,
`0x510` = 8, two `0x58c` reads, `0x500` = 8); `0x500` is read in every interrupt sweep on the Red too. The
values that differ are register `0x000` (`0x032003fd` / `0x042003fc`) and the reply to the first command,
`0x800005` (MODEL_ID, family 1 / 2). Every unit reports PCI subsystem `1cb5:0002` (config reads at `0x2c`
in every capture), so Windows cannot bind by ID. Operator, Nov 24: Windows lists no MIDI ports for the Red,
and both the Red and the Claretts run `FocusritePCIe.sys`. So the vendor decides, most likely from MODEL_ID
or `0x000`, in a table of its own; the MMIO cannot show which.

**CAP_READ asks about a category nothing uses.** The vendor sends `CAP_READ` (`0x000001 {u16 category}`,
one-byte reply) for categories 1-8 at attach, then uses 1-7 and DATA and never sends a category-8 opcode.
`tools/fcp_cap_read` now asks 0-8 as well as 9 and DATA (it asked 0-4, 6, 9, DATA before):

| category | 2Pre | 4Pre | 8Pre | 8PreX | Red 8Line |
|---|---|---|---|---|---|
| 0-7, DATA | supported | supported | supported | supported | supported |
| **8** | **supported** | **supported** | **supported** | **supported** | **supported** |
| 9 ESP_DFU | not supported | not supported | not supported | not supported | not supported |

ESP_DFU is the negative control: the firmware does answer "no". Categories 5 and 7 are supported, matching
the commands the driver sends in them.

**Category 8's info query, `0x008000`** (the `N000` pattern; `tools/fcp_cmd`, 64-byte reply asked, the
hwdep zero-fills past the reply):

| unit | MIDI jacks | `0x008000` |
|---|---|---|
| Clarett 2Pre | 1 in, 1 out | `01 00 01 00` |
| Clarett 4Pre | 1 in, 1 out | `01 00 01 00` |
| Clarett 8Pre | 1 in, 1 out | `01 00 01 00` |
| Clarett 8PreX | 1 in, 1 out | `01 00 01 00` (the same with a `u32 0` argument) |
| Red 8Line | none | all zero (4 zero bytes or an empty reply: indistinguishable here) |

**Reading: `{u16, u16}` = the MIDI port counts**, one each on a Clarett, none on the Red, which keeps the
category (and the UART) but reports no ports. A correlation over all five units, not a decoded field: no unit
has a count other than 0 or 1, so "count" against "flag" is untested, and which half is In and which Out is
unknown. The vendor driver never sends `0x008000`, so this is a device-reported answer the vendor does not
use. Not tried: `0x008001` and up (in other categories the later commands include writes: MIX/MUX write,
flash erase, SET_CLOCK).

**In the driver (Nov 24 2026, snd-clarett `eebb365`, released in 0.5.2, srcversion `D30285195BA3BF87052082F`):**
`clarett_read_midi_ports()` sends `0x008000` (`FCP_MIDI_INFO`) at probe, after the clock list and the
loopback position, and the rawmidi is created when either count is non-zero. The per-model `has_midi`
decides if the unit does not answer (a refusal, an error status or a reply under 4 bytes, with a warning)
and is a cross-check otherwise (a disagreement warns and the device is believed). Laptop, one attach each:

| unit | probe | MIDI device |
|---|---|---|
| Clarett 2Pre, 4Pre, 8Pre, 8PreX | `MIDI_INFO: 1/1 MIDI ports` | created |
| Red 8Line | `MIDI_INFO: 0/0 MIDI ports` (a 4-byte reply of zeros, not an empty one) | none |

No unit warned. A MIDI loop on one Clarett (`midi_loop.py`, 6002 B unpaced) was byte-exact at 3113 B/s.

### The opcodes the vendor sends that this project does not

Tallied over every vendor capture (`captures/*.log`, the `0x8020` command writes), against the driver and
fcp-server. Everything else the vendor sends, one of the two sends too; `0x002001` (MIX read) is ours alone.

| opcode | where | known |
|---|---|---|
| `0x006001` | every unit, once at attach | the supported-rate list, u32s 44100 ... 192000 (§12); the driver offers rates from STREAM_INFO's bands instead |
| `0x007000` | every unit, once at attach, after `0x006001` | all zero on the 2Pre and 4Pre (§9); undecoded |
| `0x800007` | Red only, once | `{u32 0, u32 0x28}`: a 40-byte read, reply by DMA; undecoded |
| `0x80000b` | Red only, ~300 | `{0, 4}`, `{4, 2}`, `{6, 1}` reads, repeated: a 7-byte record; undecoded (Dante status is the guess; only the Red has Dante) |

None of the four is needed for anything the stack does. The vendor sends no INIT_1 (`0x000000`), reboot or
flash write/erase in any capture.

