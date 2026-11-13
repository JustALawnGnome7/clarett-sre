# Clarett — FCP Mailbox Framing (trace targets)

> **Scope:** the FCP mailbox transport is shared across the Clarett Thunderbolt line; the register map
> was confirmed on the 8PreX (the reference model).

**Purpose:** predict the transport-layer framing of FCP messages in the 64 KB MMIO BAR, so the
`x-no-mmap` trace (see memory `clarett-mmio-trace-setup`) has concrete byte patterns to hunt for.

**Confidence tags:** `[S2]` = taken from the `scarlett2` USB driver (verified reference, but *USB*
transport — expect the 8PreX to reuse the packet/opcodes, confirm the values);
`[HYP]` = our hypothesis about the PCIe transport; `[TRACE]` = resolved only by capture.

> **The central bet:** the device speaks the *same FCP packet* `scarlett2` sends over USB control
> transfers; only the carrier changes — USB control msg → **BAR mailbox + doorbell + MSI**. The
> packet structure and command opcodes should port over; the mailbox plumbing is what we RE.

> **⚠ Correction (June 28 2026).** The transport framing in §8 is solid (mailbox, doorbell, header
> layout, cause regs all confirmed). But three §8 conclusions are now **disproven** and corrected
> inline: (a) "control changes don't manifest *without the data plane*" — the data-plane requirement is
> FALSE (FC moves the LEDs at idle, no stream); (b) "GET-response returns `config[off+i]`, CONFIRMED" —
> our driver's `GET_DATA` returns an **empty** payload (`size=0`) in every tested state; (c) the
> notification mask is `0x3`, not `0x200000`/`0x400000`. See `spec/provenance/clarett-manifestation-wall.md`.

---

## 1. The FCP packet (request/response envelope) `[S2]`

`scarlett2` wraps every command in a 16-byte header + payload (little-endian; x86 PCIe is LE too):

| off | field | bytes | meaning |
|---|---|---|---|
| 0x00 | `cmd`   | 4 | "big" command opcode (see §2) |
| 0x04 | `size`  | 2 | length of `data[]` payload |
| 0x06 | `seq`   | 2 | sequence #, incremented per request, **echoed in the response** |
| 0x08 | `error` | 4 | 0 = success (meaningful in the response) |
| 0x0c | `pad`   | 4 | unused / zero |
| 0x10 | `data`  | n | payload (command-specific) |

`seq` incrementing across transactions and `error`/`seq` echoed back are strong fingerprints for
locating and validating the header in the trace.

## 2. "Big" command opcodes (`cmd` field) `[S2 — confirm for 8PreX]`

These are the `scarlett2` USB values. Expect the 8PreX to match or be close; **verify**.

| opcode | name | use |
|---|---|---|
| `0x00000000` | INIT_1 | startup handshake (step 1) |
| `0x00000002` | INIT_2 | startup handshake (step 2) |
| `0x00000003` | REBOOT | reboot device |
| `0x00800000` | GET_DATA | read config space: payload `{u32 offset, u32 size}` → response returns bytes |
| `0x00800001` | SET_DATA | write config space: payload `{u32 offset, u32 size, u8 data[size]}` |
| `0x00800002` | DATA_CMD | **commit/activate** a config write: payload `{u32 activate}` |
| `0x00001001` | GET_METER | read level meters (GUI polls this continuously) |
| `0x00002001` / `0x00002002` | GET_MIX / **SET_MIX** | mixer-gain coefficients (§6 of control-plane spec). **SET_MIX confirmed** — see §8 |
| `0x00003001` / `0x00003002` | **MUX_READ** / **SET_MUX** | routing matrix (§8 of control-plane spec). **Both confirmed on hardware** — see §8; MUX_READ's windowed reply is §8's last subsection |
| `0x00006004` | GET_SYNC | clock-sync / lock status |

**This resolves the open questions in the control-plane spec:**
- mixer-gain command = **SET_MIX**; routing command = **SET_MUX**; meters = **GET_METER**;
  clock status = **GET_SYNC**. `[HYP]`
- The XML per-control `command` (1–8) = the **`activate` argument to DATA_CMD**, sent *after* the
  SET_DATA that writes the value. `[HYP — the linchpin to confirm first]`

## 3. A config write decomposes into TWO round-trips `[S2/HYP]`

Setting a config control = `SET_DATA` (write the bytes) then `DATA_CMD` (tell firmware to apply).

### Worked example — **master mute ON** (control-plane spec §9: offset 24, 1 bit, command 2)

**Round-trip A — SET_DATA**, request buffer contents:
```
+0x00  01 00 80 00      cmd   = 0x00800001 (SET_DATA)
+0x04  09 00  NN NN      size  = 0x0009, seq = N
+0x08  00 00 00 00      error = 0
+0x0c  00 00 00 00      pad
+0x10  18 00 00 00      offset = 24
+0x14  01 00 00 00      size   = 1
+0x18  01               data   = 0x01  (mute on)
```
**Round-trip B — DATA_CMD (commit)**, request buffer contents:
```
+0x00  02 00 80 00      cmd   = 0x00800002 (DATA_CMD)
+0x04  04 00  MM MM      size  = 0x0004, seq = N+1
+0x08  00 00 00 00      error = 0
+0x0c  00 00 00 00      pad
+0x10  02 00 00 00      activate = 2   ← the XML <mute ... command="2">
```

So in the trace, one mute toggle ≈ **two** mailbox transactions, the second carrying the literal
`02` from the XML. Air-on-input-1 would instead be `SET_DATA{offset=174,size=1,data=1}` +
`DATA_CMD{activate=7}`.

## 4. Expected mailbox transaction shape (per round-trip) `[HYP/TRACE]`

A typical BAR mailbox cycle — what one transaction should look like in `vfio_region_*` events:

1. **Fill request window** — a run of ascending `vfio_region_write`s into one offset window
   (the header then payload, as in §3). Ascending contiguous offsets = "filling a buffer."
2. **Doorbell write** — a single write to a fixed, separate small register (e.g. value 1) that
   kicks the device to process. Watch for a lone write to a low/fixed offset right after the burst.
3. **Completion** — one of:
   - **poll**: repeated `vfio_region_read`s of a status register until a bit flips, or
   - **MSI**: a gap, then the host reads the response (MSI itself won't show in `vfio_region_*`;
     it arrives via a different path — see §6).
4. **Read response window** — a read burst (`error`/`seq` echo, plus data for GET_* commands).

The **unknowns to map** (`[TRACE]`): request vs response window offsets (may be the same buffer),
the doorbell register offset, the status register offset/bit, and whether completion is poll or MSI.

## 5. Trace fingerprints — how to find the mailbox fast

- **Best anchor — boot init.** The first mailbox traffic at driver load should be `INIT_1`
  (`cmd=0x00000000`) then `INIT_2` (`cmd=0x00000002`). Two early transactions where the request
  window's `+0x00` gets `0x0` then `0x2`, each followed by the same lone doorbell write, pins down
  the **request window + doorbell offset** immediately. `[S2/HYP]`
- **Distinctive constants.** The 32-bit `cmd` values (esp. `0x00800001`/`0x00800002`/`0x00800000`)
  are rare bit patterns — grep the trace for them to spot headers.
- **Incrementing `seq`.** Find a 16-bit field at header `+0x06` that climbs by 1 each request.
- **Meter metronome.** The GUI polls `GET_METER` (`0x00001001`) continuously (~tens of Hz). This is
  the bulk of the "noise" but also a free, repeating transaction that reveals the **response/readback
  region** and meter data layout. Identify it early so you can filter it out.

## 6. MSI vectors (separate trace concern) `[TRACE]`

The 4 MSI vectors won't appear in `vfio_region_*` events (MSI is delivered via KVM irqfd, not BAR
MMIO). To map them, trace QEMU's `vfio` MSI events / the guest ISR separately. Expected roles to
distinguish: **mailbox completion**, **async notification** (the `0x00200000`/`0x00400000` masks
from control-plane spec §11 — likely written to a notification register the ISR then reads),
**playback period IRQ**, **capture period IRQ**.

## 7. Probe plan (escalating)

1. **Boot init** — capture driver load; locate request window + doorbell via INIT_1/INIT_2 (§5).
2. **Master mute** — single-bit, two-round-trip; confirm the `SET_DATA`+`DATA_CMD{activate=2}` model
   and the full header layout (§3).
3. **Output gain** — multi-byte payload; confirm the 8-bit gain encoding and dB mapping
   (control-plane spec §5).
4. ~~**A routing change** — reveals `SET_MUX` payload format (the 91-entry table).~~ **DONE.** Two ops:
   mixer *coefficients* via `SET_MIX` (`{u16 mix_num, u16 coeff[]}`); *source assignment* (incl.
   mixer-input slots) via `SET_MUX` (`{u32 band<<16, u32 (src<<12|dst)[]}`, one cmd per band). See §8.
5. ~~**A mixer fader**~~ **DONE.** `SET_MIX` coeff = `floor(8192·10^(dB/20))`, `0x0000`…`0x2000`(0 dB)
   …`0x3fd9`(+6 dB) (control-plane §6).
6. ~~**Idle capture**~~ **DONE.** Idle is **silent** (no polling; `GET_METER` is GUI-meter-only).
   Notifications arrive on **MSI vector 0** (bare metal: only vec0 ever fires) with the §11 mask in
   **cause reg `0x400`**; host re-syncs with `GET_DATA{0x18,0x5c}` + per-band `GET_MUX`. See §8.

## 8. CONFIRMED from boot-init trace (`8prex_init_short.log`, ControlServer NOT running)

The boot capture located the mailbox and validated the framing. `[TRACE-CONFIRMED]`

### BAR0 register map (so far)
| offset | role | evidence |
|---|---|---|
| `0x00` | caps/version = `0x032003fd` | read at init |
| `0x04`,`0x08` | `0x80`, `0x2000` | read at init |
| `0x10`,`0x14` | **device serial** = `0x5678abcd`, `0x1234` | matches lspci DSN `…12-34-56-78-ab-cd` |
| `0x100`/`0x200`/`0x300`/`0x400` | **interrupt cause registers** (functional blocks, stride `0x100`), read-to-clear. `0x100` = **mailbox-done** (`0x20000000`); `0x400` = **async notifications** — a Mute/Dim press raises `0x3` (+ intermittent `0x200000`), NOT the XML `0x200000`/`0x400000` mask `[CORRECTED June 28 2026]`. NB **not** one-per-MSI-vector: bare metal fires only vec0 (below) | `0x100`=`0x20000000` then `0x0`; `0x400`=`0x3` on front-panel Mute/Dim |
| `0x104` | IRQ enable mask = `0xf000003f` (written once) | |
| `0x410`/`0x414` | GET-response DMA buffer bus address: low32 / **high32** | init wrote `0x521ff000`/`0x2`; `0x414`=addr-high confirmed (hardcoding `0x2` → IOMMU fault at `0x2_xxxxxxxx`) |
| `0x408` | **doorbell**: write `0x1` = submit, `0x2` = ack/clear prior completion | 58×`0x1`, 57×`0x2`, strictly alternating |
| `0x500`/`0x510`/`0x514` | IRQ summary/mask block (`0x500`=`0xff0000`) | |
| `0x8000`–`0x801f` | **read-only info header**: `0x8000`=`0x04061973`, `0x8004`=`0x18101966` (fw version/build), then `1,1,0x400,0x410,0x20` | read once before any command |
| `0x8020`+ | **FCP request mailbox** (see below) | |

### FCP request mailbox @ `0x8020` — header confirmed, matches scarlett2
| offset | field | observed |
|---|---|---|
| `0x8020` (+0) | `cmd` | bit31 (`0x80000000`) = **execute flag**; low bits = opcode |
| `0x8024` (+4) | `size` (low 16) \| `seq` (high 16) | size & seq both confirmed; **seq increments 0,1,2,3,…** |
| `0x8028` (+8) | `error`/status | host writes 0 |
| `0x802c` (+12) | pad | 0 |
| `0x8030` (+16) | `data[]` | size bytes |

### Submit/complete cycle (per command)
1. `write 0x408 = 0x2` (ack/clear previous)
2. fill `0x8020`(cmd) → `0x8024`(size|seq) → `0x8028`,`0x802c`(0) → `0x8030+`(data)
3. `write 0x408 = 0x1` (doorbell/submit)
4. poll cause regs `0x100`/`0x200`/`0x300`/`0x400` (and `0x500`) until the mailbox-done bit
   (`0x20000000` @ `0x100`) clears → completion. **Superseded as a driver recipe (Oct 10 2026,
   and the polled path removed Nov 13 2026): the driver completes a command on the `0x400`
   phase bits in the vec0 ISR — bit0 accepted, bit1 response landed. See the addendum below.**

### Opcodes observed at init (device-specific numbering, as predicted)
`0x5000` (×46, 2-byte payload — bulk config/routing push), `0x6000`/`0x6001`/`0x6002`,
`0x7000`–`0x7003` (1-byte index payload), `0x0002`, `0x800005` (the `0x008000xx` "data" class —
note XML flash-command=5). The init-only opcodes (`0x5000`/`0x6000`/`0x7000`) are device-specific,
but the **data/meter-class opcodes match scarlett2 exactly** — see confirmed map.

### Firmware — the device self-boots from onboard flash `[HW/TRACE]`
The 8PreX carries its own firmware — the **App** segment plus the **Thunderbolt front-end FPGA
bitstream** — in onboard non-volatile flash. **The host uploads nothing at boot**; init is a
version/identity *check*, not code loading. Two independent proofs:

- **Structural.** For the device to enumerate as PCIe `1cb5:0002` at all, its Thunderbolt controller
  and front-end FPGA must *already be configured* — that link is the precondition for the PCIe tunnel
  the host driver rides on. The host cannot upload the bitstream over a link that needs that same
  bitstream to exist. So the FPGA loads from local flash at power-on, before any host involvement.
- **Empirical (`8prex_init_short.log`).** The boot histogram is **zero `SET_DATA` and zero bulk
  transfer** — only `0x5000` CONFIG_PUSH (2-byte payloads ×46), small `0x6000`/`0x7000` queries, and
  `0x800005` **READ_SEG ×2** (the host *reading* flash segments). A bitstream upload would be hundreds
  of KB–MB of mailbox writes; nothing of that size exists. And the driver reads back valid
  `fw app`/`fpga` version words on probe **before issuing any write**, which is only possible if the
  firmware is already resident.

So **firmware code** (FPGA bitstream + App) is resident from flash and never uploaded — the
`0x6000`/`0x7000`/`INIT_2`/`READ_SEG` queries are a check-then-maybe-update identity read, not a code
load. **But this does NOT mean the host can skip init**: a fresh device still requires a host **session
bring-up** to arm config access (see "Session bring-up IS required" below). An earlier version of this
note wrongly concluded "the control plane works without replaying init" — that was an artifact of only
ever testing a device the vendor app had already initialized before hand-over; the fresh-power-cycle
experiments (`8prex_full_init_mute.log`) disproved it. `[corrected]`

**Vendor firmware files** (in the VM at `…/Focusrite Control/Server/Resources/Firmware/`) are the
update/recovery payloads, flashed **only on a version mismatch** — never read or pushed at boot:
- `ClarettThunderbolt.tca` — Focusrite firmware container (App package + metadata).
- `fp001005_tb_top.bit`, `fp001054_tb_top.bit` — Xilinx FPGA bitstreams for the Thunderbolt front-end
  (`tb_top`), one per board revision; an updater selects the matching one. Update writes go through
  the `0x008000xx` flash-command class (`0x800005` = XML flash-command 5), same family as `READ_SEG`.

Firmware update is therefore an **optional, user-initiated** capability, never part of bring-up.
**Clean-room:** these are vendor binary blobs — keep them out of the driver tree (like the XML
descriptors); any future update support should flash only user-supplied images, not bundled ones.

### Session bring-up IS required — host must arm the device `[HW/TRACE — 8prex_full_init_mute.log]`
Distinct from firmware loading: a freshly power-cycled, self-booted 8PreX does **not** accept config
operations until the host replays the vendor session init. Confirmed on bare metal:
- On a **fresh** device, `GET_DATA` returns an FCP error — the config space is not "armed".
- After replaying the captured bring-up, `GET_DATA` succeeds and config writes ACK cleanly (`done=1`).
- The bring-up must run against a **fresh** device; re-running it on an already-armed device **wedges**
  `GET_DATA` (double-init). A robust driver should probe with a `GET` and skip the replay if armed.

**Bring-up sequence** (232 mailbox commands before the first control write; regenerate as a C replay
table with `tools/fcp_decode.py --emit-init`):
1. Identity/version (2–3 rounds): `READ_SEG`, `GET_7.1`×3, `INIT_2`, `GET_6.x`, `GET_7.x`.
2. **`CONFIG_PUSH` (`0x5000`) ×122** — register/enable config items by 2-byte id.
3. **Subsystem enables `0x000001`** (id 1..8), bracketed by `INIT_2`.
4. **Count queries** `0x003000`/`0x002000`/`0x001000`/`0x004000` (MUX/MIX/meter/sub-4); subsystem-4
   setup `0x004001` (idx 0..5) + `0x004005`; `0x006004`/`0x006005`.
5. **Full config read+writeback**: `GET_DATA` 0..0x1fc0 (8 KB, 1016-B chunks) → `SET_DATA` of the 8 KB
   persistent store → `DATA_CMD{5}` persist.
6. **`SET_MIX`×16** (all buses unity `0x2000`) + **`SET_MUX`×3** (full routing matrix).

New opcodes (not in the original control-plane map; names provisional `[INF]`): `0x000001` subsystem
enable, `0x001000`/`0x002000`/`0x003000`/`0x004000` count queries, `0x004001`/`0x004005` subsystem-4
setup, `0x006004`/`0x006005` queries.

### Control-plane changes ACK but don't physically manifest `[HW]` `[data-plane theory DISPROVEN June 28 2026]`
Even after a correct bring-up, a monitor `Mute`/`Dim` write (`SET_DATA{24|28}` + `DATA_CMD{2}`)
**completes genuinely** (`done=1, fcperr=0`) yet produces **no physical change** — the front-panel
Monitor Mute/Dim LED does not move, where the vendor app's *identical* write does. We matched the
vendor's entire control-plane behaviour byte-for-byte (full init, config read/writeback, per-write
`DATA_CMD{5}` commit).

**The earlier conclusion here — "effects stay inert until the audio engine is streaming; the remaining
work is the data plane" — is WRONG and has been disproven.** FC moves the same LEDs **at idle with no
stream and no engine armed** (`8prex_monitor_mutedim.log` touches only the mailbox/cause regs). So
manifestation does not require the data plane. This session added a second, cleaner symptom of the same
root: `GET_DATA` returns an **empty** payload too (below). Both the control plane and the data plane fail
the same way — our observable traffic (BAR0 + FCP + PCI config) is byte-identical to FC's, yet neither
functions — so the differentiator is **off-wire / below the BAR surface**, not a missing control-plane
command and not the data plane. Full elimination chain: `spec/provenance/clarett-manifestation-wall.md`.

### Opcode map — CONFIRMED by stimulus (master-mute capture, `8prex_master_mute.log`; decode via `tools/fcp_decode.py`)
| opcode | name | payload | proof |
|---|---|---|---|
| `0x001001` | GET_METER | `{u32 offset, u32 len}` | GUI polls continuously (`offset=0x560000`) |
| `0x800000` | GET_DATA | `{u32 offset, u32 len}` | reads monitor region `{0x18, 0x5c}` = spec §9 (offset 24, len 92) |
| `0x800001` | SET_DATA | `{u32 offset, u32 len, u8 data[len]}` | mute → `offset=0x18(24), len=1, data=01` = XML mute @ offset 24 |
| `0x800002` | DATA_CMD | `{u32 activate}` | mute commit → `activate=2` = XML `<mute command="2">` |

These equal scarlett2's USB opcode values. **A config write is therefore: `SET_DATA{offset, len,
value}` then `DATA_CMD{activate = the XML control's `command`}`** — the control-plane *encoding* is
directly executable and byte-matches FC. (The bytes are correct; whether the write physically
manifests is the separate, unsolved manifestation problem — see the disproven-data-plane note above and
`manifestation-wall.md`.) E.g. master-volume-up (stereo) = two pairs `SET_DATA{offset=0x20/0x21, len=1,
val=0x22}` + `DATA_CMD{activate=1}`, confirming Monitor Out 1/2 gain @ offsets 32/33, command 1,
8-bit (spec §5).

**GET responses arrive via DMA, not the BAR.** Neither `GET_DATA` nor the continuous `GET_METER`
polls ever produce response reads in the MMIO trace — the device DMAs results into the host buffer
whose bus address was programmed at BAR `0x410` (=`0x521ff000`) during init. The BAR trace therefore
cannot see GET payloads; to capture them, dump that DMA buffer in guest RAM. Opcode `0x003001`
(`GET_MUX`) is the routing read-back: request `{u16 count, u8 elem=0x02, u8 band}`, one per
sample-rate band; the routing data comes back via DMA. (This is the `{u16, 0x02, index}` triple first
seen at init after the monitor `GET_DATA`.)

#### GET-response buffer layout — header CONFIRMED, payload EMPTY for our driver `[CORRECTED June 28 2026]`
Decoded by dumping the driver's own coherent response buffer after a `GET_DATA`. The 16-byte **header**
is confirmed (echo/status/size/pad land via DMA every time); the **data region does NOT** — see the
correction below the table:

| resp offset | field | notes |
|---|---|---|
| `+0` | echoed `cmd` (`CMD_EXEC_FLAG | opcode`) | e.g. `0x80800000` for a `GET_DATA` — guard on this |
| `+4` | `size | seq` | echoed |
| `+8` | **FCP error word** | **`0x00` = OK** (scarlett2 convention); **`0x3` = refusal — the walled-session code** `[CORRECTED July 9 2026, see below]` |
| `+12` | pad | |
| `+16 + i` | (`config[offset + i]` for a *working* device) | **for our driver this region stays empty** — see below |

**⚠⚠ `+8` CORRECTION `[TRACE — July 9 2026]`: `0x3` is NOT "success"; it is an error/refusal code.**
The original "`0x3` on success" reading was calibrated **entirely on walled responses** (our driver's —
the only response headers ever seen before). Live pmemsave sampling of the **working** Windows session's
response buffer (`spec/provenance/clarett-respbuf-plan.md`; S0 + 668-snapshot S1 burst, 460 distinct states) shows
the working device writes **`+8 = 0x00` on every response, with real payload sizes** (`GET_METER`
size=192, `GET_6.5` size=4 payload=48000, `SET_DATA` size=0 write-ack) — matching the scarlett2/FCP
convention (error field, 0 = OK). Our device answers **every** command with `+8 = 0x3, size=0`: a named
**refusal**, not "success with empty data". The manifestation wall restated: the device rejects our
entire session with FCP error 3 from query #1 (wall spec §7). Bonus fact from the same capture:
**`0x6005` = sample-rate query** (returns u32 rate; 48000 observed).

#### Working-session response payloads — RECOVERED from the cold gdb ladder `[TRACE — July 10 2026]`
The `doorbell_ladder.gdb` capture of a full cold vendor bring-up (`/tmp/ladder.bin`, 85 records) gives
the device's real response to **every** init command (all `error=0`). Confirmed opcode semantics:
- **`CONFIG_PUSH` (`0x5000`) is a per-id NAME QUERY**, not a config write. Each returns the port/source
  name string: id list → `"Internal"`, `"S/PDIF"`, `"ADAT"` (clock sources); `"Output 1".."Output 4"`,
  `"Input 1"/"Input 2"`, `"S/PDIF 1"/"2"`, `"Loop 1"/"2"`, `"ADAT 1".."ADAT 8"` (2Pre port names).
- **`0x4001` = firmware SEGMENT query** — returns the segment names `App_Gold`, `App_Upgrade`,
  `FPGA_Gold`, `FPGA_Upgrade`, `App_Env`, `App_Settings` (matches the macOS §5d capture), each with a
  preceding `{u16 ver, u32 size}`-ish header.
- **`GET_6.2` = sample-rate + width**: `80 bb 00 00 18 00 18 00` → rate `0xbb80 = 48000`, `0x18=24`.
- **`GET_7.2`/`GET_7.3` = per-direction channel-count blocks** (`0x2b=43`, `0x0d=13`), aligning with
  the data-plane geometry. `INIT_2` (`0x2`) returns an 88-byte device-info block (fw build/caps).
These are the working answers to the *same* commands our driver issues; ours all return `size=0`.

**⚠ For our driver, `GET_DATA` returns an EMPTY payload `[CORRECTED June 28 2026]`.** The header DMAs in
fine (`resp[+0]=0x80800000` echo, `resp[+8]=0x03` **error — see the July 9 correction above**) but `size@+4 = 0` and the data region (`+16`
onward) is never written — exhaustively confirmed on **both** 8PreX and 2Pre, at every offset, every
length (4 / 0x3f8 / 92 / 32), with a 3 s settle, in FC's exact read window, and even in a genuine Mute
notification context. So `resp[16+i] == config[off+i]` does **not** hold for us; the device's config
backend is dormant and returns zero bytes (`spec/provenance/clarett-manifestation-wall.md` §5a).

The single non-empty read this section originally cited — `GET_DATA{24,92}` → `resp[16]=0x01`, "on a Mute
notification" — **could not be reproduced** and must have been a different historical state. A reader
**must guard on `resp[+0]` (echo) AND `resp[+4]` (size) before consuming the buffer** — echo-present with
`size=0` is the empty case the old code mistook for valid (it only checked the echo word), which let
`clarett_notify_work`/`seed_shadow` copy stale `0xAA`/zero into the monitor shadow.

### Async notification path — CONFIRMED (physical Mute button) `[mask CORRECTED June 28 2026]`
The notification **bitmask** lives in **cause register `0x400`** (read-to-clear). The MSI itself is
invisible to `vfio_region_*` (KVM irqfd), but the ISR's register read is not — that's how we mapped it.
**Which MSI vector delivers it was corrected on bare metal** (see below). Notes:
- **Live 8PreX:** a Mute/Dim press raises `0x400 = 0x00000003` (bits 0|1) on every press, with
  `0x00200003` intermittently. The earlier reading "`0x400 = 0x00200000` on Mute, `0x3` is just
  mailbox-status noise" was **wrong**; the real notification bits are `0x3` (+ aux `0x200000`). The
  driver masks `NOTIFY_MONITOR_MASK = 0x3 | 0x00200000`. `0x500` is the IRQ summary/mask reg (constant
  `0xff0000`) — *not* a notification source.
- The device **re-asserts** an unsatisfied notification (it retries `0x3` periodically). FC clears it by
  reading the config the event points at (its GET returns data); our GET returns empty, so the device
  keeps re-firing — a downstream symptom of the empty-GET, not a separate signal.
- **Delivery vector — corrected `[HW]`:** bare-metal `/proc/interrupts` shows the device fires **only
  MSI vector 0** for *all* control-plane events (mailbox-done and notifications alike); vectors 1–3
  never increment. So the cause-register index is **not** the MSI vector index — they are independent.
  A driver hooks **vec0**, and there reads the *notification* cause `0x400` (leaving the mailbox cause
  `0x100` to the poll, else the read-to-clear races it). **The parenthesis is superseded — see the
  addendum below:** `0x400` carries the mailbox phase bits as well as the notification bits, so the
  ISR owns it and takes the completion too. The race is avoided by ORDER (read `0x100` first while a
  command is in flight), not by leaving `0x100` to a poll.
- After the notification the host **re-syncs**: `GET_DATA{0x18,0x5c}` (monitor region) then `GET_MUX`
  per band. A driver should mirror this: vec0 IRQ → read `0x400` → if notify bits set, re-read config.
- **Idle is silent** — no periodic polling; the earlier `GET_METER` stream was purely the GUI meter
  view. `fcp_decode.py --async` surfaces these cause-register reads (filtering the meter noise).

### Appspace persist write-back — CONFIRMED (the "1 KB bulk SET_DATA")
After a control change, Focusrite Control writes the **entire 8192-byte persistent config store**
back to the device as a run of `SET_DATA` (`0x800001`) commands: chunks of **`len=0x3f8` (1016 bytes)**
— so each FCP payload is `8 ({offset,len}) + 1016 = 1024` bytes (1 KB) — at offsets striding by 1016
from **`0xc8` (200)** through `0x2088`, a final 64-byte chunk closing the region at offset 8392.
`200 + 8192 = 8392` = exactly control-plane §12's `appspace` (app region @ 200, persistent store 8192).
No opcode-5 flash follows, so this is a **RAM write-back, not a flash-to-nonvolatile**. This resolves
the previously-unidentified "1 KB bulk `SET_DATA`" and is **trace-side noise to filter** when hunting
a single control (it fires on every change): drop `SET_DATA` with `len=0x3f8` and `off>=0xc8`.

### Routing/mixer — CONFIRMED by stimulus (Focusrite Control: Analogue 1-2 → Monitor 1-2)
Routing an input pair to the monitor outputs produced a run of **`SET_MIX` (`0x002002`)** commands,
seq 296–304, mix-bus index incrementing 0,1,2,…,8 — and **no `SET_MUX`/`0x3002` traffic at all.**

| opcode | name | payload | proof |
|---|---|---|---|
| `0x002002` | SET_MIX | `{u16 mix_num, u16 coeff[]}` | mix_num runs 0,1,2,…; coefficient `0x2000` = unity (== scarlett2 mixer 0 dB), `0x0000` = off |

So on the 8PreX, **feeding a hardware input to an output goes through the 30×16 mixer matrix**
(control-plane §6), *not* a separate routing-matrix write. One row = one `SET_MIX{mix_num, coeffs}`;
each `u16` coefficient is an input slot's gain into that bus. The decoder originally mis-split this
with the `{offset,len,data}` template (the tell-tale `off=0x20000000 len=0x20000000` garbage = the
`00 20` unity bytes smeared across field boundaries); `fcp_decode.py` now parses the mix payload
natively (`mix=N coeffs=…`).

> **Two distinct operations:** changing a mixer *coefficient* (gain of an already-routed input) =
> `SET_MIX`. Assigning a *source to a destination* (incl. a mixer-input slot) = `SET_MUX` (below).
> The first routing capture only moved coefficients, so it showed `SET_MIX` only.

### `SET_MUX` (`0x003002`) — CONFIRMED (assigning a source to a mixer-input slot)
Adding Analogue 2 as a second source to a mixer input produced a full 16-bus `SET_MIX` rewrite **then
three `SET_MUX` commands** — one per **sample-rate band**. Payload = `{u32 header, u32 entry[]}`:

- **header** = `band << 16`: band **0** (low, 44.1/48), **1** (med, 88.2/96), **2** (high, 176.4/192).
  Entry counts shrink with band (high rates drop ADAT S/MUX channels): real wire sizes **86/70/62**
  (each +16 trailing zero-pad words), i.e. the enumerated-destination counts, not the XML `num`
  (91/75/67); see control-plane §8.
- **each entry** = u32 packing two **12-bit pins**: `entry = (src_pin << 12) | dst_pin`. `src=0x000`
  = unrouted/silent. Decoded against §3 pins, e.g. `0x400600` = Record 1 (`0x600`) ← Mic 1 (`0x400`);
  `0x300408` = Monitor 1 (`0x408`) ← Mix 1 (`0x300`); the stimulus entry `0x401301` = **Mixer input
  slot 2 (`0x301`) ← Analogue in 2 (`0x401`)**. `[TRACE-CONFIRMED]`

So the 8PreX **does** have a routing matrix; outputs/captures/mixer-input-slots all get their source
via `SET_MUX`, while per-input mix *levels* come from `SET_MIX`. `fcp_decode.py` decodes mux entries
(`dst<-src`) natively. NOTE: `SET_MUX`/appspace payloads run to ~1 KB; the decoder's mailbox capture
window was widened to `MBOX_BASE+0x410` to see them in full (earlier it truncated at 256 B → `??`).

### `MUX_READ` (`0x003001`) — CONFIRMED on hardware, reply is WINDOWED `[HW — Clarett 4Pre, July 20 2026]`
Reading the routing table back. This closes the control-plane spec's "`0x3001` query triple" open item;
it was decoded not from a capture but by asking a live armed device directly
(`tools/fcp_mux_probe.c`, plus a write-then-read-back test).

- **Request** = `{u8 offset, u8 pad, u8 count, u8 mux_num}` — `mux_num` is the sample-rate band
  (0/1/2, as `SET_MUX`), `count` the number of entries wanted, `offset` where to start.
- **Response** = `u32 entry[]` in the same `(src_pin << 12) | dst_pin` packing `SET_MUX` writes.
- **THE REPLY IS CAPPED AT 28 ENTRIES (112 bytes)**, however large a `count` is requested. Asking for
  74 returns 28 real entries and nothing more; the caller must walk `offset` in ≤28-entry windows to
  read a whole band. Confirmed by reading one table both ways: a single `count=74` request and three
  `count=28` windows agree on words 0–27 and diverge from word 28 on, where the windowed reads return
  real entries (`400 300`, `401 301`, …) and the big read returns filler.
- **`offset` is a FLAT entry index that crosses band boundaries** — it is not relative to the band.
  On the 4Pre, whose bands declare 74/70/68 words, reading at flat offset 56 returns the tail of band 0,
  then two words of inter-band content (`000 000`, `000 121`), then **band 1's table restarting at flat
  index 76**. So band 0 occupies flat 0–75 (74 entries + 2), and `mux_num` selects a base that `offset`
  is added to rather than an independent address space.

**Method note — why this took several passes, and the trap to remember.** A short reply is not
self-announcing: our own transport copied the full *requested* length out of `resp_buf` regardless of
how much the device wrote, so the tail of every short reply was **the remains of the previous command's
response**. That fabricated a coherent and very persuasive "the routing table is truncated at entry 28"
story — one that survived cross-checks, because the stale bytes were real device data from an earlier
read and therefore matched other observations. It was broken only by a **write-then-read-back**
experiment: writing a known-good 74-entry table from userspace and reading it back in windows showed
the table was complete all along and the *read* was lying. Same shape as the manifestation wall
(`clarett-manifestation-wall.md` §8): the instrument, not the device, was generating the anomaly.
Fixed in `clarett_mailbox.c` — the copy now honours the response's own size field and zero-fills the
tail, so a short reply is unmistakably short (it was also a kernel-memory disclosure via the hwdep).

**Consumer note.** `fcp-server`'s `fcp_mux_read()` requests a whole band in one command and believes
the answer, so it needs a chunking patch to read any table longer than 28 entries.

## 9. Caveats

- Opcode *values* (§2) are `scarlett2` USB constants — the 8PreX may renumber them; the *structure*
  is the durable bet. Confirm against the boot capture before trusting any specific hex.
- The two-round-trip model (§3) and the mailbox plumbing (§4) are hypotheses; PCIe framing could add
  a length-prefix, use a descriptor ring, or split request/response windows — the trace decides.
- `[S2]` items should also be cross-checked against the *current* `scarlett2`/`fcp` source, since
  field names/values evolve (see memory `focusrite-fcp-vs-scarlett2`).

## Addendum (Nov 13 2026): completion is interrupt-driven, and the polled cycle is gone

The per-command cycle recorded in §4/§6 above — poll `0x100` for DONE, sweep the cause blocks, then
wait on the response buffer — was the driver's only cycle until Oct 10 2026, when the port of
Geoffrey D. Bennett's interrupt-driven mailbox made `0x400`'s two low bits the completion signal
(bit0 = request accepted, bit1 = response DMA landed), delivered on vec0 and recorded per command by
the ISR. Both cycles then coexisted behind `phase_cycle`; the lever went on Oct 16 2026 and the
polled code on **Nov 13 2026**, with MSI now mandatory (probe fails without it).

What the §6 observations got right and wrong, for the record:

- **Right, and still load-bearing:** only vec0 ever fires for control-plane events, so the
  cause-register index is independent of the MSI vector index; the device re-asserts an unsatisfied
  notification; idle is silent.
- **Wrong:** reading `0x400` in the ISR was taken to be *instead of* taking the completion there,
  because `0x400 = 0x3` on every Mute/Dim press looked like notification bits. Bits 0 and 1 are the
  command phase; `0x3` appeared on a front-panel press only because a command was in flight at the
  time. The real monitor/dim-mute bits are 21 and 22 (the vendor descriptors' masks), which is why
  the old `NOTIFY_MONITOR_MASK = 0x3 | 0x00200000` both missed bit22 and relayed the phase bits as
  if they were events.
- **The ordering trap**, found on the first interrupt-driven build: `0x100`'s DONE bit reflects
  "`0x400` has bits pending", so an ISR that reads `0x400` before `0x100` clears what DONE reports
  and every command times out. Read `0x100` first while a command is in flight.

## Addendum (Nov 13 2026): `0x100` is a SUMMARY register, bit per cause block — and bits 4/5 are not overrun errors

§8's table calls `0x100` an interrupt cause register like the others. It is not: it is a **summary**
of the other four, and the §8 entry's "`0x100` = mailbox-done" is one bit of it. Derived by
histogramming every `0x100` read in every capture here — **~150k reads over all five models**, boot,
streaming, MIDI, preamp toggles and mute/dim — in which only **five distinct values** ever appear:
`0x00000000`, `0x20000000`, `0x80000000`, `0xa0000000`, `0x0000000f`, plus `0x10000000` in the one
MIDI capture.

| bit | block | evidence |
|---|---|---|
| 31 | `0x300`, stream block 1 (capture) | Agrees with `0x300`'s own bit31 in **45905 of 45910** vendor sweeps (`red_8line.log`). All 5 exceptions are bit31 CLEAR here with `0x300` set — the event landing between the two reads, since `0x100` is read first. Zero in the impossible direction. |
| 30 | `0x200`, stream block 0 (playback) | **CONFIRMED, see below.** Never observed in any capture, for the same reason `0x200` never carries a nonzero value there: nothing marks a TX descriptor. |
| 29 | `0x400`, mailbox phase + device events | The "DONE" bit. It reflects "`0x400` has bits pending", which is why the vec0 ISR must read `0x100` FIRST while a command is in flight (see the addendum above). |
| 28 | `0x500`, MIDI RX | Seen in exactly one capture, `2pre_midi_rx.log`: **70 reads, 70 MIDI acks** (`0x504` writes) in that trace, one for one, and in no other capture. |

`REG_IRQ0_ENABLE` (`0x104`) is written once with `0xf000003f`, which enables exactly those four
block bits plus bits 0-5 — a tidy fit for "four block summaries and six of something else".

**The low bits are a different class, and bits 0-3 are now pinned down.** They appear only ever
together, as `0xf`, **exactly once per stream stop** — the write of 0 to `REG_STREAM_IRQ_ARM`
(`0x110`) — in all ten captures that stream: 7/7, 5/5, 9/9, 9/9, 12/12, 6/6, 1/1, 5/5, 14/14 stops,
68 in all, each read followed by the vendor writing `0xf` back to clear it. That is where our
driver's `0x100 = 0xf` write at arm comes from: it clears what the PREVIOUS stop left standing. Our
servicer never sees them, correctly — it is stopped before `0x110 = 0` is written.

**Bits 4 and 5: still never observed, and NOT the overrun error they were suspected of being.** The
driver now accumulates every `0x100` value the servicer reads (`cause=` in the `stream-svc` summary)
and reports any bit outside the four known ones at `info`. The vendor captures could not settle this
because **not one of them contains a single period overrun**, so `tools/bench/cause_probe.sh`
provokes them instead, by pushing the event rate up rather than waiting for a slow ack (Red 8Line,
16-frame period, 20 s legs):

| leg | device overruns | `cause=` |
|---|---|---|
| 48 kHz capture | 1 | `0xa0000000` |
| 96 kHz capture | 3 | `0xa0000000` |
| 192 kHz capture | 6 | `0xa0000000` |
| 192 kHz duplex | **16** | `0x80000000` |

**26 device-flagged overruns and `0x100` showed nothing but bits 31 and 29.** So the summary does
not carry a per-stream error bit for this class; the overrun is reported where it always was, in the
block's own cause word (`0x300` bit30).

**A second class is ruled out too: they do not report the HOST failing to refresh a stream ring.**
Starving `clarett_play_fill()` for 2 s mid-stream, with the engine demonstrably replaying the TX ring
~47 times (one capture break per 2048-frame lap, `gaps=0`), left `0x100` and `0x200` at zero — in the
shipped configuration AND with the TX ring IRQ-marked so the playback block was live and reporting
(`blk0=0x80000070`, bit30 of `0x100` alongside it). The device has no ownership or valid bit in its
descriptors, so staleness is not a condition it can detect at all; full record in
`clarett-data-plane.md`, final subsection.

What remains open is narrower still: whether bits 4/5 are two more of the stop class for engines
these models do not have, or a FETCH-level error — the engine's own FIFO running dry because DMA was
too slow, which is detectable, unlike stale bytes. The latter would fit "enabled but never seen" on
a PCIe Gen1 x1 link whose worst case (8PreX, 28 in + 28 out, S32 at 192 kHz) is ~344 Mb/s against
~2 Gb/s, i.e. a starvation error on a bus that never starves. Provoking that needs the link throttled,
which this rig cannot do.

**Side finding, and a real one: at the cadence floor the device flags FAR fewer overruns than it used
to, because the period is now acked from the interrupt.** The record for the poll loop was ~0.6
overruns/s at a 16-frame period (2Pre); the first probe run here saw **zero** in 20 s at 48 kHz, and
a repeat saw one. They only become frequent as the event rate climbs (1 -> 3 -> 6 -> 16 above, at
one event every 333/167/83 µs). So the interrupt-driven servicer did not merely save CPU, it removed
most of a device-visible fault class.

### bit30 is `0x200`, and its silence is the TX ring's doing (Nov 13 2026) `[HW — Red 8Line]`

Two facts had a single cause. `clarett_build_rings()` ORs `CLARETT_DESC_IRQ` (bit1) into every
`clarett_irq_descs()`-th **RX** descriptor only; the TX ring gets `CLARETT_DESC_WRAP_TX` (bit0,
wrap) on its last entry and no IRQ marker at all. So the playback engine never raises a period
event, `0x200` never gets a cause, and bit30 -- its summary -- can never set. The vendor's rings
must be built the same way, since its `0x200` reads zero in every capture too.

Tested by marking the TX descriptors like the RX ones in a scratch build (never committed; it also
recorded the `0x200` value the servicer normally discards, as `blk0=`). Red 8Line, 15 s legs:

| leg | periods | `cause=` | `blk0=` |
|---|---|---|---|
| capture only (TX plays silence, so its engine still runs) | 1407, `late=0` | `0xe0000000` | `0x80000070` |
| duplex | 1408, `late=0` | `0xc0000000` | `0x80000070` |

`0x200` reports an event bit and an advancing counter exactly as `0x300` does, bit30 of `0x100`
appears alongside it, and the driver's unknown-bit report fired twice reading **`0x40000000` alone**
-- the TX block pending with nothing else. The top nibble is now fully mapped and confirmed. Both
legs streamed cleanly, so the extra interrupt source is not in itself harmful; it is simply not
needed, since the capture side already paces the shared clock.

**Side note, worth remembering for the engine-wedge hunt:** a PCM opened and closed without ever
being triggered logs `stream-svc: stopped (periods=0 ... ptr0=0x0 ptr1=0x0)` -- the same line the
wedge produces. Four appeared in this run, from the script's `--dump-hw-params` probes, with a
perfect stream immediately after. So that summary line alone is not the wedge signature; the
zero-period WARNING is, because it requires 100 ms of a stream actually running, and it correctly
stayed silent here.
