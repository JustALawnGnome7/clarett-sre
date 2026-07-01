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
> notification mask is `0x3`, not `0x200000`/`0x400000`. See `spec/clarett-manifestation-wall.md`.

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
| `0x00003001` / `0x00003002` | GET_MUX / **SET_MUX** | routing matrix (§8 of control-plane spec). **SET_MUX confirmed** — see §8 |
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

## 8. CONFIRMED from boot-init trace (`clarett_init_short.txt`, ControlServer NOT running)

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
   (`0x20000000` @ `0x100`) clears → completion.

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
- **Empirical (`clarett_init_short.txt`).** The boot histogram is **zero `SET_DATA` and zero bulk
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
experiments (`clarett_full_init_mute.log`) disproved it. `[corrected]`

**Vendor firmware files** (in the VM at `…/Focusrite Control/Server/Resources/Firmware/`) are the
update/recovery payloads, flashed **only on a version mismatch** — never read or pushed at boot:
- `ClarettThunderbolt.tca` — Focusrite firmware container (App package + metadata).
- `fp001005_tb_top.bit`, `fp001054_tb_top.bit` — Xilinx FPGA bitstreams for the Thunderbolt front-end
  (`tb_top`), one per board revision; an updater selects the matching one. Update writes go through
  the `0x008000xx` flash-command class (`0x800005` = XML flash-command 5), same family as `READ_SEG`.

Firmware update is therefore an **optional, user-initiated** capability, never part of bring-up.
**Clean-room:** these are vendor binary blobs — keep them out of the driver tree (like the XML
descriptors); any future update support should flash only user-supplied images, not bundled ones.

### Session bring-up IS required — host must arm the device `[HW/TRACE — clarett_full_init_mute.log]`
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
stream and no engine armed** (`clarett_monitor_mutedim.log` touches only the mailbox/cause regs). So
manifestation does not require the data plane. This session added a second, cleaner symptom of the same
root: `GET_DATA` returns an **empty** payload too (below). Both the control plane and the data plane fail
the same way — our observable traffic (BAR0 + FCP + PCI config) is byte-identical to FC's, yet neither
functions — so the differentiator is **off-wire / below the BAR surface**, not a missing control-plane
command and not the data plane. Full elimination chain: `spec/clarett-manifestation-wall.md`.

### Opcode map — CONFIRMED by stimulus (master-mute capture, `clarett_master_mute_decoded_live.txt`)
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
| `+8` | status/error | observed `0x3` on success (low-nibble status, mirrors `0x400`) |
| `+12` | pad | |
| `+16 + i` | (`config[offset + i]` for a *working* device) | **for our driver this region stays empty** — see below |

**⚠ For our driver, `GET_DATA` returns an EMPTY payload `[CORRECTED June 28 2026]`.** The header DMAs in
fine (`resp[+0]=0x80800000` echo, `resp[+8]=0x03` success) but `size@+4 = 0` and the data region (`+16`
onward) is never written — exhaustively confirmed on **both** 8PreX and 2Pre, at every offset, every
length (4 / 0x3f8 / 92 / 32), with a 3 s settle, in FC's exact read window, and even in a genuine Mute
notification context. So `resp[16+i] == config[off+i]` does **not** hold for us; the device's config
backend is dormant and returns zero bytes (`spec/clarett-manifestation-wall.md` §5a).

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
  `0x100` to the poll, else the read-to-clear races it).
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

## 9. Caveats

- Opcode *values* (§2) are `scarlett2` USB constants — the 8PreX may renumber them; the *structure*
  is the durable bet. Confirm against the boot capture before trusting any specific hex.
- The two-round-trip model (§3) and the mailbox plumbing (§4) are hypotheses; PCIe framing could add
  a length-prefix, use a descriptor ring, or split request/response windows — the trace decides.
- `[S2]` items should also be cross-checked against the *current* `scarlett2`/`fcp` source, since
  field names/values evolve (see memory `focusrite-fcp-vs-scarlett2`).
