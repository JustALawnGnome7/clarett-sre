# Clarett — Control-Plane Manifestation Wall (proven boundary)

> **Scope:** applies to the whole Clarett Thunderbolt line; established on the 8PreX and 2Pre.

**Status: THE WALL IS CROSSED (July 16 2026 — see §8).** The refusal was never below the driver: it
was the **trailing doorbell ack racing the device's asynchronous response DMA**, a precondition no
dilated trace could reveal. With the ack gated on the response actually landing, the same byte stream
arms the session (`err=0` + real data from command #0) and **control writes manifest physically**
(LEDs move, relays click) — user-confirmed on the 2Pre. Sections 1–7 are preserved as the historical
elimination record: every negative in them is a true fact; only the "below-driver/off-wire"
localization they pointed to was wrong. Tags: `[TRACE]` = confirmed from a capture; `[TEST]` =
confirmed by an A/B on real hardware; `[CONCLUSION]`.

Investigated on the **Clarett 2Pre** (bare metal + Linux-guest passthrough), June 2026. The 8PreX
shares the protocol and PCIe interface, so the conclusion is expected to carry across the line.

---

## 1. The problem

After a correct vendor bring-up, a control write (e.g. Air on/off, Mic↔Inst mode, monitor Mute/Dim)
completes cleanly — `done=1, fcperr=0` — but the **front-panel state does not change.** The identical
operation in FC, in the same passthrough path, *does* move the LEDs, **with no audio stream and no
engine armed** (so this is not a data-plane prerequisite — disproven separately; LEDs move in FC at
idle). `[TRACE: 8prex_monitor_mutedim.log touches only mailbox/doorbell/cause regs]`

The control encodings themselves are XML-confirmed and correct: Air @174/175 cmd7, Mode @166/167 cmd6
(Line=1/Inst=2), monitor Mute @24 / Dim @28 / master gain @112 cmd2.

---

## 2. Method: eliminate every surface the host can influence

The device is PCIe-passed-through to a VM. We can observe, and match against FC, **two** trace
surfaces, and we built an in-driver tracer so our own driver's traffic could be diffed against FC's in
the identical format:

- **BAR0 MMIO** — QEMU `x-no-mmap` + `vfio_region_*` events → `tools/fcp_decode.py`. The in-driver
  `trace_regs` param logs every `readl`/`writel` in the same `vfio_region_*` text format, so a native
  capture of our driver decodes through the same tool. (Capture must **stream** via `dmesg --follow`
  started *before* insmod; a post-hoc `dmesg` dump truncates the front when the ring buffer wraps.)
- **PCI config space** — QEMU `enable=vfio_pci_*_config` events (`8prex_boot_to_stream_with_config.log`).

What **neither** trace can see: bus-master DMA to/from guest RAM. The `vfio_region` trace only disables
the BAR mmap; DMA is untouched and never appears. `[ANCHOR: hardware facts — BAR0 = mailbox + DMA
control only; GET responses and samples move by DMA]`

---

## 3. The elimination (each row independently confirmed)

Our driver's command stream vs FC's (`captures/our_trace.log` decoded vs `2pre_preamp_toggle.log`):

| Surface | Finding | Verdict |
|---|---|---|
| FCP init preamble | READ_SEG, GET_7.x, **CONFIG_PUSH ×42**, GET_6.x, enables ×8, count queries, second pass — all present, same order | `[TRACE]` match |
| 8 KB config read + writeback | **9× SET_DATA, 0xc8..0x2088, 8192 B, content byte-identical** (head `90 19 06 30 05 00 00 00 84 19 00 00 01 02 0d 02`) — kills the "stale static writeback" suspicion | `[TRACE]` match |
| SET_MIX ×16, SET_MUX ×3, DATA_CMD{5} | identical | `[TRACE]` match |
| **Per-toggle bytes** | `SET_DATA{off,1,val}` + `DATA_CMD{activate}` — off 0xa6/0xa7 cmd6, 0xae/0xaf cmd7, vals 00/01/02 — byte-for-byte identical to FC | `[TRACE]` match |
| Control registers | `0x104=0xf000003f`, `0x500=8`, `0x510=8` identical; only `0x410/0x414` differ = each side's own DMA buffer GPA (expected) | `[TRACE]` match |
| Injected `SET_CLOCK 0x6003` (our data-plane hack; FC sends **zero** in a control session) | gated by `inject_clock`; disabling it changed nothing | `[TEST]` not the cause |
| Probe seeding writes (0x48/0x49 monitor enables, master-vol) | gated by `monitor_enables`; with `inject_clock=0 monitor_enables=0` our stream is an **exact SUBSET** of FC's — still no manifestation | `[TEST]` not the cause |
| MSI vs polling | FC reads **all five cause blocks 0x100–0x500, 1305× each, in lockstep** → FC *polls* too; "FC uses MSI, we poll" is false | `[TRACE]` ruled out |
| Continuous all-cause drain | `drain_causes` makes our meter worker drain 0x200/0x300/0x500 ~25 Hz like FC — still no manifestation | `[TEST]` not the cause |
| PCI config space | FC's writes are all **standard enumeration**: Command-reg BAR-sizing dance (@0x4, @0x10–0x24 = 0xffffffff), MSI cap (@0x44/0x4a control+data, @0x4c=`0xfee0100c` = x86 MSI msg addr), ROM sizing @0x30, status-clear @0x6. No vendor-specific datapath enable. Our driver: `pci_set_master` + `pci_alloc_irq_vectors(4,4,PCI_IRQ_MSI)` (matches the HW's 4 vectors) + per-vector `request_irq`. | `[TRACE]` match |

Supporting observation `[TEST]`: with `verify_writes`, after a control write the device's own
`GET_DATA` of that offset reads back `0x00` (echo word valid, so the DMAed response is trustworthy) —
the device reports nothing committed in the control region, even though the same `GET_DATA` returns real
data for the writeback region (0xc8+). The write "completes" but does not take.

---

## 4. Conclusion

`[CONCLUSION]` **The device applies our exact commands for Focusrite Control and ignores them for us,
while every byte we can observe — on BAR0 *and* in PCI config space — is identical (in fact our command
stream is a faithful subset of FC's).** The differentiator is therefore **not** on any host-influenced
surface a trace can see. It is **bus-master DMA payload and/or timing**, invisible to both the
`vfio_region` and `vfio_pci_config` traces.

This is a hard boundary for the black-box-trace method, and it is the **same** boundary the data plane
hit (engine bursts then stalls with all host→device surfaces matched). Both planes fail for one
off-wire reason — consistent with the device's FPGA datapath staying dark until the host does something
over DMA that our driver never does.

The control-plane *protocol* RE stands and is complete (`clarett-control-plane.md`,
`clarett-fcp-transport.md`). What is unresolved is a **device-enable step that happens over DMA**,
not any control encoding.

---

## 5. Off-wire candidate #1 (firmware upload) — TESTED AND DISPROVEN

The leading off-wire hypothesis was that FC DMA-uploads an FPGA/App firmware blob the device needs and
our driver never sends. **Tested June 28 2026 and disproven for normal operation.**

Method: with FC running and the 2Pre active in the Windows VM (`Windows10-custom`, 8 GiB), took a
memory-only guest dump (`virsh dump --memory-only --format elf`) and scanned it for the firmware
signatures. Reference files (local only, **never committed** — `vendor-reference/Firmware/`,
git-ignored): `ClarettThunderbolt.tca` (App), `fp001005_tb_top.bit` / `fp001054_tb_top.bit` (Xilinx
Spartan-6 bitstreams, magic `00 09 0f f0`, design name `fpNNNNNN_tb_top`, part `6slx45tcsg324`).

Result `[TEST]`: **neither blob's content is resident in guest RAM** — zero `.bit` magic, zero
`6slx45tcsg324`, zero `.tca` header (`10 07 12 20 6f a5`). The only reference is FC's device-descriptor
XML, which lists both as version-gated **upgrade** segments:

```xml
<segment name="App_Upgrade"  nickname="App"  version="1016" file="ClarettThunderbolt.tca">
<segment name="FPGA_Upgrade" nickname="FPGA" version="1021" file="fp001005_tb_top.bit">
```

So FC loads firmware only to *upgrade* on a version mismatch; the device self-boots App + FPGA from
flash (it reports an FPGA version at boot). No firmware crosses the bus in a normal session.
`[CONCLUSION]` **The off-wire differentiator is not a firmware/bitstream upload** — and the clean-room
"can we source the blob" concern is moot (no blob is needed at runtime). `[CAVEAT — warm]` this RAM scan
was taken on a **warm** guest (device never physically power-cycled); a firmware stage that happens **only
once per physical power cycle** could have been freed from RAM before the dump, and would move by DMA
(invisible to vfio). The cold-boot capture (§5f) closes this residual hole.

## 5a. Second-angle confirmation — GET_DATA returns empty `[TEST]` (June 28 2026)

A cleaner, machine-readable instrument than "the LED didn't move": after a full bring-up, every
`GET_DATA` our driver issues comes back **header-only** — the device DMAs the 16-byte FCP header
(`echo=0x80800000`, `status=0x03` success) into `resp_buf` but writes **`size=0` and no payload** (the
data region past offset 16 stays at our `0xAA` pre-fill). Exhaustively eliminated as the cause of the
empty payload (`log_responses=1` post-arm + in-window probes):

| Variable | Tested | Result |
|---|---|---|
| Model | 8PreX vs 2Pre | byte-identical empty |
| Read length | 4 / 0x3f8 / 92 / 32 | all empty |
| Settle time | immediate vs +3 s | empty |
| Sequence position | mid-arm in FC's exact window (right after `0x004005`, #167→#168) vs post-arm | empty |
| Prologue commands | `0x800005 / 0x6004 / 0x6002 / 0x6005 / 0x004005` all replayed byte-identical | empty |

So config-**read** is dead under every on-wire-controllable condition, with traffic byte-identical to
FC's. This **ties the two dead symptoms to one root**: writes don't manifest ↔ reads return empty ⇒ the
device's **config engine is dormant for our driver**. Two facts the probe also nails down: the
DMA-response path itself **works** (header lands every time — not a buffer-programming bug), and the
device **deliberately** returns a well-formed zero-length response (it parsed the request).

**The one positive datum** (transport spec §8): our *own* driver once read real config —
`GET_DATA{24,92}` returned `resp[16]=config[24]=0x01` — **"on a Mute notification."**

**Notification-trigger test — DISPROVEN `[TEST]` (June 28 2026).** Loaded the driver, pressed the
front-panel Mute/Dim buttons. The device **does** notify us (`notify ISR cause=0x00000003`, sometimes
`0x200003`), and `notify_work`'s `GET_DATA{24,92}` — the *exact* §8 params — fires right after. It still
returns **empty** (header lands, payload all `0xAA`). So config-read is dead even in genuine
device-notification context; §8 does not reproduce and must have been a different historical state. Two
bonus findings: (1) the notification cause bits are `0x3` / `0x200003`, which **do not match** our
`NOTIFY_DIM_MUTE | NOTIFY_MONITOR` mask (`masked=0x0`) — a real control-plane bug (our monitor controls
would never be notified in normal operation), fix independent of the wall; (2) the front-panel
Mute/Dim LEDs move on a *physical* press (hardware-direct) — only *software*-commanded changes fail,
consistent with a dormant config backend.

**CONFIG_PUSH is not DMA-backed.** The `0x5000 ×122` payloads are 2-byte item IDs (`03 00`, `2b 00`,
…), not host pointers — so config is not read through a host DMA buffer we failed to populate. The
mailbox is the entire config channel.

**Keepalive-ack hypothesis — DISPROVEN `[TEST]` (June 28 2026).** With our driver the device emitted a
periodic ~5 s `0x3` (cause `0x400`) in bursts of ~10; theory was the config backend gates on FC acking
a heartbeat we don't send. Checked against `8prex_boot_to_stream_with_config.log` (boot→idle→30 s
stream): (1) at true idle (04:55:16→04:55:53, 37 s) FC receives **zero** notifications — there is no
periodic device keepalive; (2) `0x400` is read-to-clear but the device **re-asserts** while it has an
unsatisfied event (toggles `0x3`/`0x0` on alternate poll passes); (3) FC's response to a `0x3` is a
**full config re-sync** (enables + count queries + `0x6004/0x6002/0x6005/0x004005` prologue + 13×
`GET_DATA` + 9× `SET_DATA` writeback) — **no novel opcode**; every command is already in our arm
sequence. So the periodic `0x3` is **not** a separate gate but a **downstream symptom of the empty-GET**:
the device retries the config-change notification because our read comes back blank and never satisfies
it, where FC's read returns real config and the device goes quiet. Reinforces the dormant-backend root;
adds no new on-wire lever.

> **NOTE `[ANALYSIS]` (July 6 2026) — `0x400` is a phase register; the periodic `0x3` is the DEVICE
> re-notifying (this reading confirmed on hardware, superseding a wrong intermediate "it was us" claim).**
> Prompted by the Universal-Audio *open-apollo* init docs, we re-examined `0x400`. Two parts, one solid
> and one walked back:
> - **Solid (from captures):** `0x400` is a **2-bit command-phase register, not an async event queue** —
>   across every FC boot capture (`tools/notify_correlate.py`) it only ever holds `{0,1,2,3}` (idle/ready
>   = `0x3`, dips to `0x0` while a command is accepted, blips `0x1→0x2` mid-command; lone `0x8` only in
>   4pre at stream time). FC does **no** per-bit follow-up read — it polls `0x400` as flat status and
>   branches on nothing, so the Apollo-style staged per-bit acknowledge-handshake is **refuted**. Also:
>   unlike Apollo (whose real state rides a DMA command-ring with a base register), `bar_profile` shows
>   **no host→device DMA base beyond `0x410/0x414`**, so the mailbox is the whole control channel — the
>   command-ring model has no MMIO footing here.
> - **Walked back (a wrong intermediate claim, corrected by hardware):** I first concluded the "config-
>   change notification retried indefinitely" storm was **substantially our own self-trigger** — vec0 also
>   fires on mailbox-DONE, and `0x400` reads its idle `0x3` (`== NOTIFY_MON_PRIMARY`) at completion, so the
>   ISR could schedule `notify_work` off our own GET's completion. A live `log_responses=1` run on the real
>   2Pre (July 6) **disproved that as the primary cause**: the ISR fires vec0 in **µs-scale bursts (8 in
>   234 µs)**, far faster than our ~30 ms command rate, and with `inflight=0` — i.e. the device is
>   **genuinely re-asserting** the `0x3` notification on its own, not reflecting our commands. So the
>   **original reading above is correct**: the periodic `0x3` is the device retrying an unsatisfied
>   config-change because our `GET` returns empty (`size=0`), where FC's returns real config and it goes
>   quiet. A minor self-reflection component **does** exist (the `inflight=1` events) and is caught by an
>   `atomic_t cmd_inflight` guard in `clarett_irq`, but the guard **cannot** stop the device-side storm
>   (the device fires in the idle µs-gaps between our commands, where `inflight=0`). All cosmetic — none of
>   it touches the wall; the storm is one more symptom of the dormant backend, not a driver bug.

## 5b. What remains

The off-wire difference is something subtler than a blob upload. Note we already matched **every
DMA-address-programming register** (only `0x410/0x414`, the GET-response buffer — no extra DMA region is
set up by FC), so it is not an additional DMA buffer we failed to allocate. Remaining angles, untested:
- **★ macOS DTrace of the working driver — leading lead `[PLAN]`.** The Clarett runs on the user's
  Apple-Silicon MacBook (M1), so the working Focusrite driver can be instrumented to capture the **DMA
  payload it builds in host RAM** — the exact off-wire content the vfio trace cannot see, **without a bus
  analyzer**. Full plan: `spec/provenance/clarett-macos-dtrace-plan.md`. This **reopens actionable RE**: the earlier
  "method exhausted / bus-analyzer-only" stance was scoped to the Windows/vfio surface, not this one.
- ~~**Notification-trigger test**~~ — **DONE, DISPROVEN (§5a):** config-read stays empty even in a
  genuine device-notification context (`notify_work`'s `GET_DATA{24,92}` fired right after a real
  front-panel Mute/Dim press still returned header-only, `size=0`).
- Inspect what the device actually DMAs into FC's `0x410` response buffer for a control-region GET in a
  *working* session, vs the empty our probe sees — requires re-tracing the live Windows
  session to learn the current buffer GPA (it is re-allocated per boot; the old `0x277913000` is stale),
  then dumping that region.
- Re-examine device-state/arming: bring-up is order- and freshness-sensitive
  (`[[clarett-control-manifestation]]`); FC's working device may reach an internal state our replay does
  not, despite byte-identical init.

## 5c. Init-response audit — dormancy is universal from query #1 `[TEST]` (July 1 2026)

Prompted by reviewing the 4th-gen `scarlett2`/`fcp` source (the reference drivers capture init responses
and validate every response header), we audited our own init-response handling against the **full**
bring-up response log (`captures/our_arm_resp.log`, `log_responses=1`, 152 commands).

**1 — the emptiness is universal and immediate, not config-specific.** *Every* one of the 152 bring-up
commands returned a DMA response with **`status=0x03` (SUCCESS) and `size=0`** — not just the 13
`GET_DATA` config reads but **every `GET_7.x`/`GET_6.x` device query, from the first command**
(`READ_SEG 0x800005` doesn't DMA a response at all). The device "succeeds" our entire command stream
while returning zero response bytes, starting at query #1. This is broader than "config backend dormant":
the device's whole response/query engine is **inert for our session from attach**.

**2 — our DMA response path is verified working.** The 16-byte echoed FCP header lands correctly every
time (right echo `cmd`, `status=0x03`) in the buffer we program at `0x410/0x414`. Not a buffer-addressing
bug — the device deliberately writes well-formed zero-length responses.

**3 — the BAR-mailbox-response alternative is ruled out.** Our driver reads responses **only** from the
DMA buffer and never reads the BAR mailbox data region (`0x8030+`). Hypothesis: query responses come back
in the mailbox (trace-visible) and we look in the wrong place. Checked against a real FC capture — FC's
post-submit reads of the mailbox region (`fcp_decode`'s `resp`) are **empty too**; FC does not read
responses from the BAR either. Both FC and we take responses via DMA; FC's device produces data, ours
produces `size=0`. Disproven.

**4 — no init-response token-forwarding is missing.** The reference `scarlett2` seeds `seq` from 0 (as we
do) and feeds **no** init-response value into later commands; the 4th-gen `fcp` init likewise just sizes
fixed step0/step2 buffers. So our static replay is not skipping a session-token handshake. (Two real but
wall-independent robustness gaps surfaced: `clarett_fcp` neither validates the response header — where
`scarlett2` rejects any `cmd`/`seq`/`size`/`error`/`pad` mismatch — nor ever reads `MBOX_DATA`.)

`[CONCLUSION]` The audit finds **no in-driver parse/replay bug** that unlocks the wall, and pins it one
step earlier: the device answers our whole command stream with **SUCCESS + empty from the first query,
over a verified DMA path, at FC's own buffer address** — so the differentiator sits **upstream of the
mailbox command stream** (an attach-time/off-wire condition FC's session enters and ours never does), not
in how we form or parse commands. Consistent with, and tighter than, §4/§5a.

## 5d. Third-platform confirmation — macOS DTrace of the working vendor stack `[TEST]` (July 1 2026)

The Clarett runs on an Apple-Silicon MacBook (M1) with the vendor stack: a kernel kext
(`com.focusrite.driver.FocusritePCIe`, class `com_focusrite_FocusritePCIeDevice` on the `IOPCIDevice`;
BAR0 = 64 KB @ `0x480000000`, TB-tunnelled behind the `dart-apciec0` DART) + a user-space
`FocusriteControlServer` + a Core Audio plugin. We DTrace'd the **working** driver (plan:
`clarett-macos-dtrace-plan.md`; captures in `captures/macos_*`). Two decisive results:

**1. The vendor's user-space→kext traffic is byte-identical to our RE `[TEST]`.** Tracing
`FocusriteControlServer`'s `IOConnectCallMethod` calls (pid provider) across a device attach and a
control toggle:
- Mute toggle → `SET_DATA{off=24,len=1,val=1}` + `DATA_CMD{activate=2}` + `DATA_CMD{activate=5}` — the
  exact bytes our Linux driver sends (control-plane §9/§2).
- Bring-up → query phase, the **8 KB config read** (`GET_DATA` in `0x3f8` chunks 0→`0x1fc0` + `0x108`),
  the **8 KB write-back** whose head is **`90 19 06 30 05 00 00 00 84 19 00 00 01 02 0d 02`** (byte-for-byte
  the §3 signature), `DATA_CMD{5}`, and `SET_MIX`×16 at `0x2000` unity. So a **third** independent platform
  (Windows FC, macOS FC, our driver) emits the same stream.
- The `~122 CONFIG_PUSH` low-level declarations are **not** in the server's traffic → they are issued by
  the **kext at `start()`**, i.e. the arming lives in the kext's device-init layer.

**2. The device returns RICH real data to the vendor stack for the SAME requests that return empty for us
`[TEST]`.** Dumping the `IOConnectCallMethod` *output* structs (the GET responses) during bring-up, the
macOS device answers with:
- device caps `04 00 00 00 0e 00 00 00` = **4 playback / 14 record** (2Pre, matches RE);
- firmware build **"Jan 20 2021 09:55:25"** + versions;
- firmware segment names **App_Gold / App_Upgrade / FPGA_Gold / FPGA_Upgrade / App_Env / App_Settings**;
- serial string **`SERIAL_NO = 998559`**;
- the 8 KB config: device name `Clarett2Pre-000f3c9f`, the `7f 7f 7f…` gain-attenuation floor codes, and
  at config offset **`0xc8`** (= appspace `app-ofs=200`) the same **`90 19 06 30…`** blob it writes back.

Our Linux driver's **identical** `GET_DATA` requests return `size=0`, empty (§5a/§5c). So this is the wall
stated from the working side: **same bytes on the wire, the device pours real config/firmware/serial back
to the vendor stack and returns nothing to ours.**

`[CONCLUSION]` macOS **confirms** the boundary from a third platform and **localizes** it precisely — the
device backend is demonstrably alive; the arming is the **kext's off-wire device-init/DMA setup** (not the
mailbox command stream, which is identical). It does **not cross** the wall: DTrace can't see inside the
kext (stripped release binary → no `fbt` probes; Apple redacts its load address → stack frames
unattributable), and the actual mechanism is the off-wire DMA that only a bus analyzer (ruled out) or
disassembling the vendor kext (clean-room no-go) could reveal. The macOS/DTrace avenue is **exhausted**.
Bonus RE facts banked above (firmware build date, segment names, serial format, config layout: header
0..0xc7, persistent appspace at 0xc8).

**Boot-time capture — RUN and empty; the last DTrace card `[TEST]`.** A LaunchDaemon started `dtrace` at
boot (DMA-alloc / `appendBytes` / `iovmMapMemory` probes) to catch the kext's cold `start()` DMA by timing
(`captures/focus-boot.log`). Result: **zero** Clarett/`apciec`/focusrite frames and no unsymbolicated
(bare-`0x`) frames in the 120 s window — only unrelated boot DMA (GPU/ANE/biometric/IOReport). The daemon
**lost the timing race**: a boot-attached PCIe device matches and runs `start()` during early IOKit init,
*before* user-space dtrace can attach; and cold-attach can't be beaten from user space, while the warm
cable-replug (the only fallback) already produced nothing attributable. **Both cold and warm attach are
exhausted → the DTrace avenue is definitively closed.**

## 5e. Fourth-platform capture — WinDbg of the working Windows driver's init DMA `[TEST]` (July 2 2026)

The one lead DTrace couldn't reach (`clarett-windbg-plan.md`): kernel-debug the **working** Windows driver and
watch the DMA it builds at init. **Device under debug: the 2Pre (warm).** Setup: the FC guest (`Windows10`) with Secure Boot off + serial KD
(`bcdedit /dbgsettings serial`, COM2 bridged to a second `Windows10-WinDbg` VM over a host TCP socket —
KDNET failed on QEMU's e1000e, `0xC0000182`). **Clean-room discipline held**: breakpoints only on
**symbolicated Microsoft APIs** (`nt!MmAllocatePagesForMdlEx`, `Wdf01000!imp_WdfCommonBufferCreateWithConfig`,
`nt!HalAllocateCommonBuffer`), attributed to Focusrite by **module range** and **caller frame**; the
`FocusritePCIe.sys` code was never disassembled or stepped — only the interface data it hands the OS.

**Driver identity + DMA surface.** `FocusritePCIe.sys` — the driver that **owns BAR0 and does the
bus-master DMA** (it maps `MmMapIoSpace` for BAR0 and did every DMA allocation below) — is **KMDF**. Its
three siblings `FocusritePCIeSwRoot.sys`, `FocusritePcieAudio.sys`, `FocusritePcieMidi.sys` were **not
independently audited** this pass (roles inferred from names as root-enumerator / upper audio / upper MIDI,
**unverified**); the capture was scoped to `FocusritePCIe`'s load + module range, so any allocations a
sibling makes outside that window were not observed. **Completeness caveat / follow-up:** see "Follow-up
(planned)" — the sibling audit is the one open thread. It does **not** loosen the below-driver conclusion,
because the vfio MMIO trace is **driver-agnostic** (it traps *every* BAR0 access + mailbox write by *any*
driver) and matches FC byte-for-byte, so no sibling can be doing hidden **device-facing** programming — only
the same off-wire bus-master DMA blind spot remains. Its IAT imports the DMA-relevant `nt!MmAllocatePagesForMdlEx`, `nt!MmMapLockedPagesSpecifyCache`,
`nt!HalPutDmaAdapter` (it holds a WDF DMA adapter), and `nt!MmMapIoSpace` (BAR0). Common-buffer allocs go
through the WDF function table (invisible in the IAT), caught one level down at the HAL.

**Complete init DMA inventory** (breakpointed at `EvtDevicePrepareHardware`, via
`Wdf01000!FxPnpDevicePrepareHardware::InvokeClient`, filtered to the FocusritePCIe module range):

| Call site | Allocation | Attributes | Role |
|---|---|---|---|
| `+0x8cac` | 16 KB common buffer (`WdfCommonBufferCreateWithConfig`) + 2 MB MDL (`MmAllocatePagesForMdlEx`) | cached; MDL scattered | stream block 0 (TX): SG descriptor table + sample buffer |
| `+0x8dac` | 16 KB common buffer + 2 MB MDL | cached; MDL scattered | stream block 1 (RX): descriptor table + sample buffer |
| `+0xa6ea` | 4 KB common buffer | cached | standalone → FCP response/mailbox buffer (`0x410`) |

`MmAllocatePagesForMdlEx` args: `Low=0`, **`High=0xFFFFFFFFFFFFFFFF`** (no DMA address ceiling — buffers
floated to ~10 GB), `SkipBytes=0x1000`, `Total=0x200000`, **`Cache=MmCached`**, `Flags=0x20`; the 2 MB pages
are physically **scattered** (MDL PFN array) and **zeroed at idle** (empty audio space). Common buffers come
back `CacheEnabled=1` (cached).

`[KEY]` **None of this differs from our driver.** On x86/x64 PCIe DMA is hardware cache-coherent, so a
*cached* common buffer **is** a coherent buffer — identical to what Linux `dma_alloc_coherent` returns on
x86; `High=MAX` = our 64-bit DMA mask. Attribute for attribute (address range, cache/coherency, contiguity
strategy) the vendor's init DMA equals ours. Cross-checked against the vfio trace, the device-facing init is
also identical: **only `0x410/0x414` (response buffer) is programmed at init** — the `0x210/0x310` engine
arm is **stream-start**, ~25 s later (`8prex_boot_to_stream_with_config.log`: `0x410`@`54:30.35`, arm
@`55:12.91`), *not* device init. And the vendor pushes **no DMA pointer through the mailbox** at init (no
buffer address appears in the `0x80xx` mailbox-data writes), so our replayed `clarett_init_8prex.h` (opcodes
only, **no baked addresses**) carries nothing stale.

`[CONCLUSION]` WinDbg — with the debugger timing control, Microsoft symbols, and visible module ranges the
stripped/redacted macOS kext denied DTrace — **directly disproves the "the vendor sets up DMA we don't"
hypothesis on all three axes**: no extra device-facing buffer at init, no mailbox pointer-push, and
allocation attributes equivalent to ours. The vendor driver's **driver-level DMA construction is equivalent
to `snd-clarett`'s.** The differentiator therefore sits **below the driver** — in the Thunderbolt/PCIe
transport, link layer, or a device-init handshake that **no** host-side software trace (Windows/vfio MMIO,
macOS DTrace, or now Windows kernel debugging) can observe. This is the strongest closure available by
software means; the remaining reachable surfaces are all exhausted or excluded (bus analyzer ruled out,
kext/`.sys` disassembly clean-room no-go).

`[CAVEAT — warm device]` **This run, and every vfio capture it cross-checks against, saw a *warm* device.**
A libvirt/VM reboot does **not** cut the Clarett's power — it runs off its own DC adapter, **not** Thunderbolt
bus power, so a TB-cable unplug alone would not reset it; only a **device DC power-cycle** does. Unless the
device was DC power-cycled it stayed **continuously armed** across all captures. So the §5e inventory is the vendor
re-initializing an **already-armed** device — which is state-*independent* for the DMA *allocations*
(driver-side; the conclusion above holds), but **blind to anything the vendor does only once per physical
power cycle.** Re-reading the boot captures (`8prex_full_init_mute.log`, `8prex_boot_to_stream_with_config.log`)
sharpens this into two concrete, still-untested cold-boot suspects:
- **INIT_1 (opcode `0x0`) never appears.** Both captures show **INIT_2 (`0x000002`) but no INIT_1** — the
  scarlett2 first-contact handshake's *first* step is absent. The decoder surfaces zero-execute opcodes
  (e.g. `0x000001` is listed), so this is a real gap, not a parse artifact. Consistent with first-contact
  having happened at an untraced earlier cold boot.
- **No REBOOT (`0x3`) and no firmware/FPGA upload.** SET_DATA count is ~10–11, largest = the known 8 KB
  config write-back; an FPGA bitstream (`fpNNNNNN_tb_top.bit`, hundreds of KB) would be a long chunked-write
  burst that is nowhere present. If the vendor stages the FPGA once per power cycle (via DMA, not the
  mailbox — invisible to vfio), a warm capture cannot see it. **This also softens §5's firmware-upload
  disproof**, whose RAM scan was likewise on a warm guest (a cold-boot upload's bytes may already be freed).
- **`seq=0` is not a cold marker.** Both captures start at `seq=0`, but `seq` is a *driver*-side counter
  reset per driver load / VM reboot — it cannot distinguish cold from warm.

`clarett_arm_device` is generated from the warm `8prex_full_init_mute.log`, so it reproduces the warm
sequence **by construction** (INIT_1-less, upload-less). If either is the true cold bring-up, our driver is
missing it. → the cold-boot capture plan (§5f) is the next test, ahead of the sibling audit.

## 5f. Cold-boot (DC power-cycled) capture — RUN, NEGATIVE on all three surfaces `[TEST]` (July 3 + 6 2026)

The warm-device caveat's lead, pursued: **DC power-cycle** the **2Pre** (cut its own adapter — the unit is not
bus-powered, so a TB-cable unplug alone leaves it warm; no function-level reset to fake it, config space
`FLReset-`; a VM reboot stays warm) and diff cold vs warm on both instruments.
**Note: the §5e WinDbg device was also the 2Pre** (warm) — so this is a same-device cold-vs-warm comparison,
not cross-model.

- **vfio mailbox trace `[TEST]` — NEGATIVE.** `captures/2pre_cold_boot.log` (cold) vs `captures/2pre_boot.log`
  (warm): **no `INIT_1` (`0x0`), no `REBOOT` (`0x3`), no firmware-sized SET_DATA burst** — and identical
  `CONFIG_PUSH` (42), `SET_DATA` (9), `GET_DATA` (13), `SET_MIX` (16), `SET_MUX` (3) counts. The missing
  `INIT_1` I flagged is absent **cold and warm** → it is *not* a cold-only step we skip; the Clarett TB flow
  simply doesn't use it. `clarett_arm_device` is **not** missing a cold-only mailbox command. (Only diff: a
  few lightweight query/enable opcodes appear ~2× cold — an extra enumerate pass, not a new sequence.)
  **Confirmed twice (July 6, `captures/2pre_cold_boot2.log`):** a second cold boot, this time captured
  **driver-only** (`FocusritePCIe.sys` bring-up with **no Focusrite Control app** → `GET_METER` count 0, so no
  meter-poll noise), shows the same result — no `INIT_1`, no `REBOOT`, no firmware-sized burst; same vocabulary
  (`CONFIG_PUSH` ×43, the known 8 KB config read/write-back at off `0xc8..0x2088`, subsystem enables, `SET_MIX`/
  `SET_MUX`, `INIT_2`), ending on the init `DATA_CMD`. So even the raw kernel driver's cold bring-up carries no
  cold-only command and no mailbox firmware push.
- **WinDbg DMA-allocation surface `[TEST]` — NEGATIVE.** Cold FocusritePCIe init DMA footprint =
  **2×{16 KB common buffer + 2 MB MDL} + 1×4 KB common buffer, cached** — **identical to the §5e warm 2Pre**,
  no extra buffer, no firmware-sized allocation. **Siblings closed:** `SwRoot`/`Audio`/`Midi` were all loaded
  yet allocated **zero** DMA buffers — only `FocusritePCIe` does DMA (the unverified §5e guess, now verified).
  Capture method that finally worked: arm the MDL/common-buffer bps at the boot break where **FocusritePCIe
  is loaded but `FocusritePcieAudio`/`Midi` are not yet** (= before `PrepareHardware`, past the `BgpFw` boot-
  graphics flood that drowns a bp armed at the uptime-0 break). `sxe ld:` never halted (with or without `.sys`).
- **WinDbg DMA-*contents* surface `[TEST]` (July 6 2026) — NEGATIVE.** Captured both 2 MB MDL **pointers** at
  allocation (halt on `FR_2MB_MDL_HIT`, `gu` to the return in `FocusritePCIe+0x30b8`, `r @rax` = PMDL;
  `ByteCount 0x200000` confirmed), then re-read each MDL **post-init** (at the boot break where `FocusritePcieAudio`/
  `Midi` had just loaded — the freshest moment, before any streaming) to get its now-mapped `MappedSystemVa`, and
  `db` + searched the **full 2 MB** of each for firmware: `00 09 0f f0` (Xilinx `.bit` header), `aa 99 55 66`
  (bitstream sync), `"tb_top"` (design name). **Both buffers all-zero; all six searches empty.** A **warm** baseline
  (VM reboot) run first, for method validation, showed MDL#1 zero and MDL#2 holding 24-bit-in-S32_LE **audio
  samples** (these are the TX/RX sample buffers) — and its firmware searches were empty too. So on a genuinely
  cold device, at the freshest post-init instant, **no firmware/bitstream is present in either 2 MB DMA buffer.**
  This is **comprehensive**, not a spot check: §5e already inventoried the *entire* init DMA footprint, and the
  2 MB pair are the only buffers big enough to hold an FPGA bitstream. Firmware-over-DMA is **disproven** — consistent
  with the hardware fact that the FPGA **self-boots from flash** (no host→device firmware transfer exists to capture).
  METHOD NOTE (why July 3 recorded no `MappedVA`s): a `gu` **inside** a breakpoint command is fatal — WinDbg skips
  every command after it *and* lets the target run on, so the `.printf`/`g` never fire and nothing is captured. Use a
  **halt-only** bp (`{ .echo FR_2MB_MDL_HIT }`, no execution command) and run `gu` / `r @rax` **manually** at each halt.
  Also: `MappedSystemVa` is `0x1` at allocation (unmapped) → record the **MDL pointer**, re-read the VA post-init.

**Net:** the cold-boot lead is **fully closed** — same-device cold==warm on the mailbox and DMA-allocation
surfaces, siblings do no DMA, and cold DMA *contents* are zero (no firmware over DMA). Every host-visible surface,
warm and cold, is now exhausted; the differentiator is confirmed **below the driver** (TB/PCIe transport or a
device-init handshake) and unreachable by any permitted software method. This is the terminus of clean-room
software RE for this device.

---

## 6. Reproduction (driver A/B params)

All gated, default-off-from-FC's-perspective where relevant, so the elimination is re-runnable:
`trace_regs` (in-driver wire trace), `inject_clock` (the 0x6003 hack), `monitor_enables` (probe
seeding), `drain_causes` (FC-style all-cause poll), `meter_poll_ms` (GET_METER heartbeat),
`verify_writes` (post-write GET_DATA readback), `dma_bits` (coherent mask width),
`premailbox_reads` (default true; replay the vendor's exact pre-mailbox BAR0 read set at attach — caps,
`0x4/0x8`, serial, `0x514`, `0x58c×2`, all four cause blocks, the full `0x8000–0x801c` fw header — the
sole host-visible pre-mailbox difference the §7 cold ladder left; set 0 for the old read-minimal probe to
A/B whether the reads flip `GET_DATA` to `error=0`),
`resp_prefill` (fill the `0x410` response buffer with a byte before every submit: `0` = FC's
freshly-zeroed common buffer, `170` = the §5a 0xAA emptiness marker, `-1` = untouched baseline —
probes whether the device reads this buffer, the only host address it knows at init; see §7).

Exact-subset replay: power-cycle the device (bring-up must run on a fresh device), then
`insmod snd-clarett.ko model=2pre inject_clock=0 monitor_enables=0 meter_poll_ms=0`, toggle a control,
observe no front-panel change.

---

## 7. Open lead — host-RAM contents the device reads `[PLAN]` (July 9 2026)

The §5f terminus verdict covers every surface the four methods could *see* — but none of them ever
observed **runtime host-CPU writes into device-visible DMA memory**, nor compared that memory's
**contents** between a working and a walled session. The elimination itself points here: with the
Fedora-guest A/B making the TB/PCIe path identical by construction and every MMIO/config/MSI surface
matched by measurement, the only thing a deterministic endpoint can still observe differently is
**what it finds when it DMA-reads host RAM** — and the sole host address it knows at init is the
`0x410/0x414` response buffer. Precedent that this device acts on buffer contents: the data-plane
`0xAA` RX pre-fill was the lone difference that made the engine clock, and flat-mode faults showed
the engine dereferencing buffer bytes as pointers (data-plane §9).

Planned instruments, cheap-first:
1. **`resp_prefill` A/B (driver lever, §6) — RUN, NEGATIVE `[TEST]` (July 9 2026).** Per-command fill
   of the response buffer: `0` mirrors FC's freshly-zeroed 4 KB common buffer; `170` (0xAA) restores
   the §5a emptiness marker; `-1` baseline (zeroed at alloc, previous response left in place — the
   behaviour every walled run so far used). Result on the 2Pre with `resp_prefill=0`: `clarett_seed_shadow`
   still fails (`-EIO` after 3 attempts = the echo+size guard rejecting `size=0` responses) and an Air
   toggle via alsamixer does not move the front-panel LEDs — identical wall. So the device does not gate
   on *stale-response residue vs zeroed* buffer contents. (Validity assumes the standard fresh
   DC-power-cycle before load, per §6.) Narrows the RAM-contents theory to content FC actively *writes*
   (tests 2–3), not hygiene we lack.
2. **pmemsave temporal diff of FC's response buffer — RUN `[TEST]` (July 9 2026): no host seed, no
   request mirror, but a MAJOR CORRECTION FOUND.** Runbook: `spec/provenance/clarett-respbuf-plan.md`; tools:
   `dma_bases.py` (extracts the `0x410/0x414` GPA), `resp_burst.sh`, `resp_dump.py`. S0 (driver-only,
   quiescent) + a 668-snapshot burst across FC startup + meter steady state (460 distinct buffer
   states): **every state is a well-formed device response; nothing host-written** — the seed/mirror
   hypothesis is negative on this buffer (sub-50 ms transients technically remain for `ba w`, but
   the motivating theory is now superseded). **The discovery: resp`+8` is an FCP ERROR word, not a
   status.** The working session's every response carries **`+8=0x00` with real payloads** (`GET_METER`
   size=192; `GET_6.5` size=4 payload=48000 — new fact: `0x6005` = sample-rate query; `SET_DATA`
   size=0 ack; content cross-matches the macOS §5d captures at the +16 header shift). Our sessions get
   **`+8=0x3, size=0` on every response from query #1** — i.e. the device REFUSES our whole session
   with a named error code; "SUCCESS 0x03 + empty" (§5a/§5c wording) was a mis-calibration made when
   only walled responses had ever been seen. Transport spec §8 corrected; `clarett.h` constants renamed
   (`FCP_RESP_ERR_OK`/`FCP_RESP_ERR_WALLED`). **The wall restated: what arms a session = whatever makes
   the device answer error 0 instead of error 3.**
   **→ Cold-boot error timeline, sampling pass — RUN `[TEST]` (July 9 2026), head unresolved.**
   DC power-cycled 2Pre, burst-sampled the vendor's own cold bring-up (2907 snapshots): the earliest
   observable response — **seq 61, `CONFIG_PUSH{0x1e}` answered `"ADAT 8"` — is already `error=0` on a
   genuinely cold device.** (New fact: `CONFIG_PUSH` 0x5000 is really a per-id *name query* — the
   working device answers each with the port's name string.) The bring-up is two ~30 ms phases
   (seq 0–61, then a ~48 s OS-boot idle, then seq 62–84): far too fast for ~10 Hz sampling, and the
   buffer GPA is not boot-stable, so pre-aiming is impossible. **If a 3→0 flip exists it is inside
   seq 0–60** (READ_SEG, GET_7.1×3, INIT_2, GET_6.2/6.0, CONFIG_PUSH ladder — all commands we replay
   byte-identically). Public-source cross-check: the mainline ALSA `fcp` driver's response struct
   confirms `+8` is the error word and treats ANY nonzero value as failure; no public enumeration of
   code 3 exists.
   **→ gdb doorbell ladder — RUN `[TEST]` (July 10 2026): NO 3→0 flip; the working device answers
   `error=0` from command #0 on a cold boot. THE GATE IS PRE-MAILBOX.** `tools/doorbell_ladder.gdb`
   attached to the custom (debug-info) QEMU, broke `vfio_region_write`, and appended the 4 KB response
   buffer at every doorbell submit (GPA learned live from `0x410/0x414`; GPA→HVA via the pc.ram block;
   pure gdb reads). Captured the full **85-command cold bring-up** (`/tmp/ladder.bin`, decode
   `resp_dump.py --ladder`). Result: **record 1 (response to seq 0, `READ_SEG`) is already `error=0`
   with a real 8-byte payload, and every command seq 0→83 answers `error=0` with rich real data.**
   (Record 0 = stale pre-arm buffer, expected.) There is **no error-3 phase and no arming command** —
   the vendor's device is in the "answer this host" state from the very first mailbox command. Since
   our driver gets `error=3, size=0` from *its* command #0 (§5c, re-read with the July-9 correction),
   **the accept-vs-refuse decision is made before mailbox command #0 — an attach-time condition, not a
   mailbox step.** No mailbox change can cross the wall (finally explains why none ever did).
   **Pre-mailbox surface diff (the one new lead):** in the cold trace the vendor's pre-mailbox BAR0
   *writes* are identical to ours (`0x104=0xf000003f`, `0x500=8`, `0x510=8`, `0x410/0x414`), but it
   *reads* a wider set at attach than `clarett_hw_init` does. Vendor reads `0x0`(caps), `0x4`, `0x8`,
   `0x10/0x14`(serial), **all four cause blocks `0x100/0x200/0x300/0x400`** (read-to-clear), `0x500`,
   **`0x514`, `0x58c`(×2)**, and the **full 8-word fw header `0x8000–0x801c`**. Our probe reads only
   `0x10/0x14` + `0x8000/0x8004`. So the vendor touches, at attach, several registers we never
   read — including read-to-clear cause blocks and the peculiar `0x58c` (read twice). **NEXT (cheap):**
   replay the vendor's exact pre-mailbox read set in `clarett_hw_init` (a read-only change), fresh
   power-cycle, and check whether `GET_DATA` flips to `error=0`. Weak prior (reads rarely gate
   acceptance, and 4 methods point below-driver), but it is the *only* host-visible pre-mailbox
   difference and sits exactly where the ladder localized the gate — worth one power-cycle.
   (**Method correction:** an earlier draft proposed WinDbg `ba w4` on resp+8 — wrong: x86 debug
   registers trap CPU accesses only, never device DMA writes; the gdb ladder replaced it. gdb caveats
   learned: attach the *custom* `/usr/local/bin` QEMU by path not `pgrep qemu.*Windows10` (matches the
   stock WinDbg VM); `handle SIGUSR1/2 nostop pass` or the vCPU-kick freezes gdb and a long stop
   starves libvirt's keepalive → virt-manager drops the domain; press `c` past the attach pager.)

   **→ pre-mailbox-read replay — RUN `[TEST]` (July 10 2026): NEGATIVE. GET_DATA still `error=3`.**
   Driver lever `premailbox_reads` (default on) replays the vendor's exact pre-mailbox read set + write
   reorder (0x104 before the DMA addr) + matched inter-group timing (usleep_range 0.8–8 ms/gap) at attach.
   On the 2Pre: `clarett_seed_shadow` still fails (`-EIO`, `GET_DATA` still `error=3`/`size=0`) — the
   config backend gate is **unchanged** on every variant. The whole host-visible pre-mailbox surface
   (writes, reads, their values, order, timing) is now matched/inert and GET_DATA stays `error=3` ⇒ the
   pre-mailbox lead is **exhausted**, re-confirming the below-driver localization.
   **⚠ RETRACTED over-claim (July 10 2026) — the "Analogue-2 LED flash = first physical device response"
   was a FALSE ALARM; do not resurrect it.** During these loads an Analogue-2 LED flashed red a few times
   and was (wrongly) reported here as the device physically reacting to our driver, then "explained" as a
   pending-cause read-to-clear. **Both readings are withdrawn.** The user identified the LED as the
   **Analogue-2 INPUT CLIP meter** (fires when the analog input momentarily exceeds the clip threshold) —
   an input signal-level indicator, not a device-state LED — and it does **not** reproduce with
   `premailbox_causes=1`. So the flashes were **incidental analog input transients** (a floating input
   picking up a pop / noise), and the `premailbox_causes` correlation was small-sample coincidence. There
   was **no** physical device response and **no** foothold. LESSON (again): an intermittent, uncontrolled
   hardware observation is not evidence until it reproduces under control — do not promote it to a finding.
   **What DID stay solid (independent of the LED):** the device completes our mailbox at the BAR level
   (`MBOX_ERROR`@`0x8028 = 0`, `done=1`) and DMAs a well-formed response header, yet returns FCP
   application **`error=3, size=0`** — so the refusal is an **FCP-application-layer session block** from
   command #0, not a dead/ignoring device (supported by the mailbox round-trip, NOT by the LED). The two
   error fields differ: BAR `0x8028 = 0` but DMA `resp+8 = 3` (our `clarett_fcp` checks the BAR word, sees
   0, so the seed's `-EIO` comes from the `size=0`/echo guard). Pre-mailbox register **values** banked
   (cold 2Pre): `caps@0x0=0x032003fd`, `0x4=0x00000080`, `0x8=0x00002000` (= config size 8 KB?),
   `0x514=0x00000847`, `0x58c=0`.

   **→ error-code discrimination — first two runs INVALID (racy read); PROVISIONAL finding, re-test
   pending `[TEST]` (July 10 2026).** Driver lever `error_probe=1` sends malformed/varied FCP commands and
   reads the DMA response. The first run looked clean (valid `GET_DATA`→`err=3`; bad-offset/unknown→no
   response) and was written up as "the device discriminates per-command; `err=3` is a specific
   access-denied code." **The opcode-survey run then exposed a RACE that invalidates both runs:** a
   `GET_DATA{0xc8}` line reported `echo=0x80005000` — CONFIG_PUSH's echo from the *previous* command — which
   a GET_DATA can't legitimately produce. The device DMAs its response **asynchronously, a little after the
   BAR done bit** `clarett_fcp` polls, so reading `resp_buf` immediately catches the *previous* command's
   late response (shifted by ~one command). Per-command attribution in both runs is therefore unreliable
   and the "discriminates / `err=3` is specific" conclusion is **withdrawn pending re-test** (do not rely on
   it). What is still real: the device DMAs `err=3`/`size=0` response headers for *some* commands and none
   for others — both behaviors occur — but which command maps to which was scrambled by the race.
   **FIX (in tree):** `clarett_error_probe` polls `resp_buf` until the echoed opcode matches the command
   just sent (50 ms timeout = genuine no-response), with `meter_poll_ms=0` required (else the meter worker
   steals `resp_buf` and bumps `c->seq`, corrupting attribution — that was a *second* instrument bug behind
   the racy runs).
   **→ RESOLVED `[TEST]` (July 10 2026): BLANKET refusal — the device stamps `err=3` on EVERY command.**
   Clean run (`error_probe=1 meter_poll_ms=0`, meter off, echo-matched): all eight commands returned
   `echo=<own opcode>, err=3, size=0` — valid `GET_DATA{24,4}`, out-of-range `GET_DATA`, an **unknown opcode
   `0x0000ff`** (echo `0x800000ff` — unforgeable, proves a real fresh response), `READ_SEG`, `GET_7.1`,
   `GET_6.2`, `CONFIG_PUSH{0x1e}`, appspace `GET_DATA{0xc8}`. So the device does **NOT discriminate**: it
   denies a nonsense opcode with the same `err=3` as a valid request. It echoes the opcode back (minimal
   "received") but **does not echo our request seq** (writes `seq=0` on a refusal — a tell that this is a
   stub refusal path, distinct from the vendor's seq-echoing `err=0` responses). ⇒ **`err=3` is a blanket,
   out-of-band session-level refusal applied to the whole command stream regardless of content** — the
   earlier "discriminates / `err=3` is specific" reading is **fully retracted** (it was the meter+async
   race). This is the cleanest statement of the wall: the device receives and acks our FCP commands but
   refuses the entire session with `err=3`, a decision made outside the command layer (consistent with the
   pre-mailbox/attach-time localization and the cold ladder's "vendor gets `err=0` from command #0"). No
   FCP-layer manipulation can cross it. (LESSON, and this session's running theme: THREE instrument bugs —
   the `0x03` mis-calibration, the LED false alarm, and the async+meter read race — each produced a
   plausible-looking wrong finding before hardware/clean-instrument re-test corrected it. Verify the
   instrument first.)
3. **WinDbg `ba w` watchpoints** on the offsets (2) shows changing, attributed by module range —
   a `FocusritePCIe` CPU write into this buffer is protocol traffic invisible to all four §5 methods.
4. Same instruments on the 16 KB descriptor CBs for the data-plane wall (pre-arm seeding; the
   undecoded second structure past the descriptor terminator, data-plane §3c).
5. **DMA addresses above 4 GiB — RUN, NEGATIVE `[TEST]` (July 10 2026).** The lead: every working
   capture programs every DMA base with a nonzero high word — resp buffer `0x414` = `0x2` (2Pre ×4)
   / `0x1` (8PreX, 4Pre), and the stream ring bases `0x214`/`0x314` = `0x2` — while our driver
   (`dma_bits=32` default, **including the Fedora-guest control**) always programs `0x414 = 0`.
   Addresses are the one value class a "byte-identical" trace comparison necessarily normalizes as
   don't-care, and no prior test covered it (even `dma_bits=64` lands <4G: iommu-dma tries 32-bit
   IOVA space first). Test on the AMD box (AMD-Vi, translated DMA-FQ domains): boot
   `iommu.forcedac=1` (skips the 32-bit-first IOVA preference) + `insmod model=2pre dma_bits=34`
   (34-bit mask so the top-down alloc lands just above 4G, inside the vendor-demonstrated range,
   not at the ~48-bit aperture top). Instrument verified in-band: probe logged
   `resp buffer dma addr 0x00000003fffff000 (0x414 high word 0x3)` (new unconditional `dev_info`),
   and the pre-mailbox state matched the cold vendor baseline exactly (causes `0x100–0x400` = 0,
   `0x500` = `0xff0000` — same value all three vendor cold captures read there; regs
   `caps/0x4/0x8/0x514/0x58c` all match banked values). **Result: `config shadow seed failed (-5)`
   — `GET_DATA` still refused; the wall does not gate on the DMA address being above 4 GiB.**
   (Tested high word `0x3` vs vendor's `0x1`/`0x2` — any plausible "high address" predicate passes
   at `0x3_fffff000`.) **Data-plane variant RUN, NEGATIVE (same day):** with rings
   at `0x3_ffe00000` (>4G) the 2Pre capture engine reported `periods=2 ctr=0x0 wraps=0`; the <4G
   control (`dma_bits=32`, rings at `0x0_ffe00000`, same boot, fresh power-cycle) reported the
   **identical** `periods=2 ctr=0x0` — no address-range dependence in either plane, and no AMD-Vi
   faults in either run. (An apparent ">4G made the engine stop clocking" delta died on the control
   run — the 2Pre's PCM baseline is `periods=2 ctr=0x0` at ANY address. Side finding, separate from
   the wall: the 8PreX-era one-ring-pass burst characterization — 248 periods, `ctr=0x1b3`, needing
   contiguous buffer + full-duplex arm + 0xAA pre-fill — does NOT reproduce on the 2Pre with the
   current per-model port; its `0x300` counter never advances at all. The 2Pre engine params likely
   need re-deriving from the 2Pre stream captures, data-plane spec TODO.) With this negative, the remaining §7-style leads are:
   (a) a **zero-normalization full-stream comparison audit** of cold-vendor vs cold-ours from
   power-on to command #0 — access widths, read-to-clear read *counts*, config-vs-BAR interleave —
   auditing the "byte-identical" claim itself as an instrument; and (b) a **vIOMMU on the Windows
   guest** to observe device DMA-*read* addresses, the last never-observed input (weakened a priori
   by macOS-works-under-strict-DART, which argues the device only reads driver-given addresses).
6. **MSI-enable ordering — the config-space interleave hole `[PLAN → lever in tree]` (July 10 2026).**
   Found by the first pass of the zero-normalization audit (access widths all confirmed 4-byte on both
   sides — that sub-class is dead). The cold vendor trace interleaves config-space and BAR streams:
   Windows programs the MSI capability and **sets the MSI enable bit before its first BAR access**
   (`@0x4c=0xfee0100c` addr, `@0x54=0x49b4` data, `@0x4a=0xa5` = enable + MME=4, at `.068`; first
   pre-mailbox BAR write `.082`; COMMAND=`0x406` mem+BM+DisINTx throughout). Our probe called
   `pci_alloc_irq_vectors` at the END of probe — so the **entire arm (232 commands) and the seed
   `GET_DATA` ran with MSI disabled in the device's own config space** (COMMAND=`0x006`, MSI off), in
   every walled run **including the Fedora-guest control**. This is a genuine device-visible
   pre-command-#0 difference the earlier eliminations missed: "config space matches" compared the SET
   of config writes, not their POSITION relative to command #0, and the §7-item-2 pre-mailbox replay
   only covered BAR traffic. Mechanistically plausible: the device signals response-DMA completion and
   notifications by MSI; firmware may refuse a session for a host with no interrupt path. Driver
   change (in tree): `clarett_enable_msi()` (vector alloc = config-space enable) now runs before
   `clarett_hw_init`; handler hookup stays late; lever `early_msi` (default 1, `0` = old order for
   A/B). **RUN, NEGATIVE (same day):** fresh power-cycle, `MSI: got 4/4 vectors` now precedes all BAR
   traffic, cold baseline verified (causes 0, `0x500=0xff0000`), seed still `-5` — `GET_DATA` still
   refused. MSI-enable ordering does not arm the session. Lever stays default-on (vendor-faithful
   order costs nothing). Audit continues: remaining config-space deltas (e.g. the vendor's Device
   Control write `@0x60=0x2910` — RO/no-snoop/MRRS attributes), read-to-clear counts, full interleave.
7. **Config-space surface CLOSED byte-for-byte — RUN, NEGATIVE `[TEST]` (July 10 2026).** The audit
   reconstructed the vendor's complete pre-doorbell config activity from the cold trace (cap chain
   PM@0x40/MSI@0x48/PCIe@0x58; every write inventoried) and diffed it against our live post-probe
   `lspci -xxx`: COMMAND ends `0x406` both sides, `DevCtl@0x60=0x2910` identical (RO+ExtTag+NoSnoop,
   MPS128, MRRS512), LnkCtl=0 both, PMCSR final `0x8` both, MSI ctrl `0xa5` both (addr/data per-boot).
   Sole persistent visible diff: the INT-line scratch byte `@0x3c` (vendor writes `0x00`, BIOS default
   `0xff` on bare metal) + two transient W1C clear events (`@0x06=0xf900`, `@0x44=0x8008`×3).
   Replicated all three via setpci on a fresh power-cycled device before insmod: seed still `-5`.
   **The entire PCI config-space surface — values, order, W1C events — is now matched/inert.**
   Remaining audit surface: the BAR stream itself (read-to-clear counts, full sequence alignment).
8. **Per-command mailbox READ — the BAR-stream audit's find `[PLAN → lever in tree]` (July 10 2026).**
   Full structural alignment of the vendor cold BAR stream vs ours: header writes match (vendor writes
   the full 16-byte header `0x8020/24/28/2c` per command, like us), doorbell submit/ack pattern matches
   (one `0x408<-2` + one `0x408<-1` per command), cause-sweep polls covered by `drain_causes`. **The
   sole remaining per-command difference: we READ `MBOX_ERROR @0x8028` after every completion; the
   vendor reads NO mailbox register, ever** (its only `0x8xxx` reads are the attach-time fw header
   `0x8000–0x801c`; ours = `0x8028` × once per command, since day one). Timing makes it a live
   candidate: the device DMAs its response asynchronously AFTER the BAR done bit (§7 error_probe race),
   so our command-#0 read lands while firmware is composing response #0 — a session poisoned by a
   mailbox read on the first command is indistinguishable from the ladder's "attach-time gate"
   signature, and the read (always returning 0 for us) was never suspected because every elimination
   diffed writes and attach-reads only. Lever `mbox_err_read` (default 0 = vendor-faithful, no read;
   1 = old behavior) in `clarett_mailbox.c`; functionally free (our BAR fcperr was always 0; the real
   error channel is resp+8). **RUN, NEGATIVE (same day):** fresh power-cycle, no mailbox reads issued,
   seed still `-5`. The read was not the gate — but removing it stays default (vendor-faithful).
9. **Doorbell ACK PHASE + completion sweep — the sequence-alignment find `[PLAN → lever in tree]`
   (July 10 2026).** Exact per-command event dump of the vendor cold trace shows the cycle:
   header writes → `0x408<-1` submit → sweep ALL FIVE cause blocks in order
   `0x100,0x300,0x200,0x400,0x500` (first sweep ~40 µs after submit already sees `DONE`; `0x400`
   reads `0x3` during every command and is read-to-cleared by the sweep) → one confirming sweep →
   **`0x408<-2` ack TRAILING the command**. Our cycle acked FIRST (start of each `clarett_fcp` call)
   and polled `0x100` alone. Same per-command ack COUNT — so every count-based comparison cancelled
   the difference — but the first doorbell token our driver ever sends a fresh device is `2` (an ack
   with nothing to ack), where the vendor's is `1` (submit): **an out-of-protocol token at the exact
   pre-command-#0 boundary where the ladder localized the gate** (`VENDOR: 1,2,1,2,1…` vs
   `OURS: 2,1,2,1,2…`). Present in every walled run ever (all platforms, warm+cold, both controls).
   Also fixes: our last command was never acked, and the vendor-style sweep consumes the `0x400=0x3`
   phase value per command (we never did). Lever `legacy_mbox_cycle` (default 0 = vendor cycle:
   no leading ack, five-block sweep, confirming sweep, trailing ack on DONE only; 1 = old cycle).
   **RUN, NEGATIVE (July 10 2026):** first attempt (continuous five-block sweep during processing)
   caused arm timeouts (-110 on 2/152) — **the first device-side behavior change any lever ever
   produced; the read-to-clear phase regs are LIVE mid-command** (instrument bug, not a finding:
   hammering 0x400 at bus speed breaks the phase handshake). Paced re-run (tight 0x100 poll, then
   DONE-sweep + confirming sweep + trailing ack): arm 152/152 clean, seed still `-5`. The ack phase
   and sweep are not the gate; the vendor cycle stays default. Remaining per-command difference:
   completion DISCOVERY — vendor waits for the MSI (reads 0x100 exactly 2×/cmd), we tight-poll it.
10. **MSI-paced completion — RUN, NEGATIVE; THE HOST-VISIBLE SURFACE IS NOW EXHAUSTED AT MAXIMAL RIGOR
   `[TEST]` (July 10 2026).** ISR vec0 consumes the 0x100 cause and completes the waiting `clarett_fcp`
   (handlers hooked before any BAR access; notify path gated on `ctl_ready`). Verified in-band: every
   command of the full bring-up logged `polls=1` (`dyndbg=+p`) — 0x100 read counts now vendor-identical
   (2×/command: ISR + confirming sweep). Arm 152/152, seed still `-5`.
   **Milestone summary — one day's eliminations (all `[TEST]`, all negative on the gate):** access
   widths (all 4-byte both sides); DMA addresses >4G (both planes); MSI-enable ordering; config space
   byte-for-byte incl. `@0x3c` scratch + W1C events; per-command mailbox read (removed); doorbell ack
   phase (leading→trailing); five-block cause sweeps; MSI-paced completion. Combined with everything
   prior: **the complete host-visible surface — config values/order/W1C, MSI state, DMA address classes,
   buffer contents, every BAR write/read, their order, phase, counts, and attach timing — is matched,
   and the device still stamps `err=3` from command #0.** (Inter-command pacing still differs, but by
   causality it cannot explain the refusal of command #0 itself, which precedes any pacing signal.)
   Byproduct wins: first-ever device behavior change (phase regs are live mid-command, §7 item 9);
   mailbox now MSI-driven (a real driver improvement); vendor-faithful cycle throughout.
   **Remaining software instrument: exactly one — vIOMMU on the Windows guest** to observe the working
   session's device-initiated DMA reads (mappings + faults become guest-visible/QEMU-traceable), the
   only input channel never observed. Weakened a priori by macOS-works-under-strict-DART but now the
   sole survivor. After that: non-software paths (Focusrite/community) or accepting the wall.

---

## 8. THE WALL IS CROSSED — the trailing ack raced the response DMA `[TEST]` (July 16 2026)

**The user's reframing that broke it:** stop comparing what the sessions *say* (byte streams) and ask
what the measurement apparatus did to *time*. Every "known-good" vendor capture was taken under
x-no-mmap MMIO trapping — ~20 µs per BAR access — and the working driver **executed under that
dilation in every captured session**. A trace records byte order, not which host actions are
semantically conditioned on asynchronous device events: in the dilated environment those events had
always completed. Our replay issued the identical bytes at native speed (~100 ns/access).

**The cycle walk (exercise):** the mailbox transaction contains exactly one write whose meaning is an
acknowledgement — the trailing `0x408<-2`. What it might acknowledge: (a) the completion cause
(waited for — MSI-paced), or (b) **the response DMA consumed/buffer free (never waited for)**. The
traces cannot distinguish the two: every vendor ack sat ≥242 µs after submit (measured across
`2pre_cold_boot2.log`, all 114 commands), so the response had always landed by ack time. At native
speed ours fired ~µs after DONE — and the §7 `error_probe` race had already proven the response lands
*after* that (a resp_buf read right after DONE still holds the previous command's response). In
`our_arm_resp.log`, arm[0]'s response never arrived at all and the cycle acked it anyway. So on every
command of every walled session we acked a response that had not arrived — and the device's answer
was the blanket `err=3` session refusal from command #0, indistinguishable from an attach-time gate
(§5c's "THE GATE IS PRE-MAILBOX" was this masquerade: the localization assumed intra-command timing
was inert).

**The fix** (`clarett_mailbox.c`): pre-submit, zero the 16-byte response header (so repeated opcodes
can't match a stale echo); after the DONE sweep, `clarett_resp_wait()` polls resp+0 for THIS
command's echoed opcode (up to `CLARETT_MBOX_TIMEOUT_MS`) **before** the trailing ack; a response
that never arrives is never acked. Levers: `gated_ack` (withhold ack on no-response), `resp_trace`
(per-command onset/latency telemetry — NOTE: as first implemented it also performs the landed-wait
before the ack), `mmio_dilate_us` (wholesale dilation re-creation, `clarett_main.c`).

**Result (2Pre, bare metal):** the full 232-command arm + seed answered **`err=0` with real data from
command #0** — the device echoes our request seq (a refusal writes seq=0), `CONFIG_PUSH` returns the
port-name strings, the 8 KB config read returns 8×1016 real bytes, and the serial/fw query answers
(serial `000012345678abcd`, fw app `0x04061973`, fpga `0x18101966`). Response-landing telemetry:
64–110 µs typical, ~173 µs for the 1016-byte GETs, **631–698 µs for `DATA_CMD` activates** — the old
cycle acked those more than half a millisecond before the response landed. **PHYSICAL MANIFESTATION
CONFIRMED by the user: Mode and Air toggles in alsamixer move the front-panel LEDs and audibly switch
the relays.** `[TEST]`

**Attribution matrix — CLOSED, 3/3 deterministic `[TEST]` (July 16 2026, fresh DC power-cycle per
run, user-confirmed):** run #1 (`resp_trace=1`) armed + manifested physically; run #2
(`resp_trace=1`) armed with a matching latency profile (err=0 from seq 0, no onset variability —
the fixed input stream now has a *fixed* healthy response, closing the onset-stability question);
run #3 (levers off) **walled** (`config shadow seed failed (-5)`) exactly as predicted. The gated
cycle is the mechanism, not the boot. **Consequence: the landed-gated ack + pre-submit header zero
are now the unconditional default mailbox cycle** — `gated_ack` is retired, `resp_trace` remains as
telemetry, and a response that never lands is never acked. (The finer gated-wait-vs-header-zero
split was not run and is moot: the header zero is what makes the landing detectable; they are one
mechanism.) Side observation: the ~24 Hz meter poll interleaves cleanly with the echo-matched wait,
and `GET_METER` now returns real data (size=192) — the `meter_poll_ms` "heartbeat required to apply
writes" hypothesis was a walled-device artifact and needs re-audit.

**Open items:**
3. **Data-plane retest** on an armed session: the PCM burst-then-stall was attributed to the same
   "below-driver" differentiator — that attribution is now void. The stall may be the same class of
   violation on the stream cause blocks (`0x200/0x300` are read-to-clear and were serviced at native
   speed), or may simply resolve.
4. Propagate: CLAUDE.md status, data-plane spec preamble, INVESTIGATION.md (currently documents a
   terminus that no longer exists).

**Method lesson (the one to carry):** when a replay of a traced protocol fails, audit every host
action whose meaning asserts that something *finished* — an instrument that dilates time makes every
async precondition invisibly true. Characterize failures by their onset (first bad response), not
their endpoint.
