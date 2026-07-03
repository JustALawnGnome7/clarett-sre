# Clarett — Control-Plane Manifestation Wall (proven boundary)

> **Scope:** applies to the whole Clarett Thunderbolt line; established on the 8PreX and 2Pre.

**Status:** **Blocked at a proven boundary.** The control plane is fully reverse-engineered and our
driver reproduces Focusrite Control's (FC) wire traffic on *every traceable surface*, yet control
changes that physically manifest for FC do **not** manifest for our driver. This document records the
elimination that establishes the boundary, so the conclusion is not re-litigated. Tags: `[TRACE]` =
confirmed from a capture; `[TEST]` = confirmed by an A/B on real hardware; `[CONCLUSION]`.

Investigated on the **Clarett 2Pre** (bare metal + Linux-guest passthrough), June 2026. The 8PreX
shares the protocol and PCIe interface, so the conclusion is expected to carry across the line.

---

## 1. The problem

After a correct vendor bring-up, a control write (e.g. Air on/off, Mic↔Inst mode, monitor Mute/Dim)
completes cleanly — `done=1, fcperr=0` — but the **front-panel state does not change.** The identical
operation in FC, in the same passthrough path, *does* move the LEDs, **with no audio stream and no
engine armed** (so this is not a data-plane prerequisite — disproven separately; LEDs move in FC at
idle). `[TRACE: clarett_monitor_mutedim.log touches only mailbox/doorbell/cause regs]`

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

## 5b. What remains

The off-wire difference is something subtler than a blob upload. Note we already matched **every
DMA-address-programming register** (only `0x410/0x414`, the GET-response buffer — no extra DMA region is
set up by FC), so it is not an additional DMA buffer we failed to allocate. Remaining angles, untested:
- **★ macOS DTrace of the working driver — leading lead `[PLAN]`.** The Clarett runs on the user's
  Apple-Silicon MacBook (M1), so the working Focusrite driver can be instrumented to capture the **DMA
  payload it builds in host RAM** — the exact off-wire content the vfio trace cannot see, **without a bus
  analyzer**. Full plan: `spec/clarett-macos-dtrace-plan.md`. This **reopens actionable RE**: the earlier
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
buffer address appears in the `0x80xx` mailbox-data writes), so our replayed `clarett_init_seq.h` (opcodes
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
A libvirt/VM reboot does **not** cut Thunderbolt bus power, so unless the TB cable was physically replugged
the Clarett stayed **continuously armed** across all captures. So the §5e inventory is the vendor
re-initializing an **already-armed** device — which is state-*independent* for the DMA *allocations*
(driver-side; the conclusion above holds), but **blind to anything the vendor does only once per physical
power cycle.** Re-reading the boot captures (`clarett_full_init_mute.log`, `8prex_boot_to_stream_with_config.log`)
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

`clarett_arm_device` is generated from the warm `clarett_full_init_mute.log`, so it reproduces the warm
sequence **by construction** (INIT_1-less, upload-less). If either is the true cold bring-up, our driver is
missing it. → the cold-boot capture plan (§5f) is the next test, ahead of the sibling audit.

## 5f. Cold-boot (physically power-cycled) capture — RUN (July 3 2026), negative on two of three surfaces `[TEST]`

The warm-device caveat's lead, pursued: power-cycle the **2Pre** (physical TB replug — no function-level
reset to fake it, config space `FLReset-`; a VM reboot stays warm) and diff cold vs warm on both instruments.
**Note: the §5e WinDbg device was also the 2Pre** (warm) — so this is a same-device cold-vs-warm comparison,
not cross-model.

- **vfio mailbox trace `[TEST]` — NEGATIVE.** `captures/2pre_cold_boot.log` (cold) vs `captures/2pre_boot.log`
  (warm): **no `INIT_1` (`0x0`), no `REBOOT` (`0x3`), no firmware-sized SET_DATA burst** — and identical
  `CONFIG_PUSH` (42), `SET_DATA` (9), `GET_DATA` (13), `SET_MIX` (16), `SET_MUX` (3) counts. The missing
  `INIT_1` I flagged is absent **cold and warm** → it is *not* a cold-only step we skip; the Clarett TB flow
  simply doesn't use it. `clarett_arm_device` is **not** missing a cold-only mailbox command. (Only diff: a
  few lightweight query/enable opcodes appear ~2× cold — an extra enumerate pass, not a new sequence.)
- **WinDbg DMA-allocation surface `[TEST]` — NEGATIVE.** Cold FocusritePCIe init DMA footprint =
  **2×{16 KB common buffer + 2 MB MDL} + 1×4 KB common buffer, cached** — **identical to the §5e warm 2Pre**,
  no extra buffer, no firmware-sized allocation. **Siblings closed:** `SwRoot`/`Audio`/`Midi` were all loaded
  yet allocated **zero** DMA buffers — only `FocusritePCIe` does DMA (the unverified §5e guess, now verified).
  Capture method that finally worked: arm the MDL/common-buffer bps at the boot break where **FocusritePCIe
  is loaded but `FocusritePcieAudio`/`Midi` are not yet** (= before `PrepareHardware`, past the `BgpFw` boot-
  graphics flood that drowns a bp armed at the uptime-0 break). `sxe ld:` never halted (with or without `.sys`).
- **WinDbg DMA-*contents* surface — STILL OPEN (one sub-task).** This pass logged allocations, not contents;
  the two 2 MB MDL `MappedVA`s weren't recorded, so they weren't `db`'d late. §5e's warm `db` showed both
  zero, but near-allocation timing makes "zero" possibly just "not filled yet." **Next: one more cold pass
  that records the MDL pointers and `db`s them at the desktop (post-init).** EXACT runbook in
  `clarett-windbg-plan.md` → "Cold contents pass — EXACT runbook". If both read zero post-init on cold (with
  §5's RAM scan finding no bitstream resident and this pass's no-extra-buffer result), firmware-over-DMA is
  closed; if nonzero, first real lead.

**Net:** the cold-boot lead is closed on the mailbox and DMA-allocation surfaces (same-device cold==warm,
siblings do no DMA); only the late MDL-contents `db` remains, and prior evidence points to it also being null.

---

## 6. Reproduction (driver A/B params)

All gated, default-off-from-FC's-perspective where relevant, so the elimination is re-runnable:
`trace_regs` (in-driver wire trace), `inject_clock` (the 0x6003 hack), `monitor_enables` (probe
seeding), `drain_causes` (FC-style all-cause poll), `meter_poll_ms` (GET_METER heartbeat),
`verify_writes` (post-write GET_DATA readback), `dma_bits` (coherent mask width).

Exact-subset replay: power-cycle the device (bring-up must run on a fresh device), then
`insmod snd-clarett.ko model=2pre inject_clock=0 monitor_enables=0 meter_poll_ms=0`, toggle a control,
observe no front-panel change.
