# Clarett — Control-Plane Manifestation Wall (proven boundary)

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

Our driver's command stream vs FC's (`driver/our_trace.log` decoded vs `2pre_preamp_toggle.log`):

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

The control-plane *protocol* RE stands and is complete (`clarett-8prex-control-plane.md`,
`clarett-8prex-fcp-transport.md`). What is unresolved is a **device-enable step that happens over DMA**,
not any control encoding.

---

## 5. Off-wire candidate #1 (firmware upload) — TESTED AND DISPROVEN

The leading off-wire hypothesis was that FC DMA-uploads an FPGA/App firmware blob the device needs and
our driver never sends. **Tested June 28 2026 and disproven for normal operation.**

Method: with FC running and the 2Pre active in the Windows VM (`Windows10-custom`, 8 GiB), took a
memory-only guest dump (`virsh dump --memory-only --format elf`) and scanned it for the firmware
signatures. Reference files (local only, **never committed** — `FCP Server Resources/Firmware/`,
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
"can we source the blob" concern is moot (no blob is needed at runtime).

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
- **Notification-trigger test** (above) — cheapest, targets the §8 positive datum directly.
- Inspect what the device actually DMAs into FC's `0x410` response buffer for a control-region GET in a
  *working* session, vs the empty our probe sees — requires re-tracing the live Windows
  session to learn the current buffer GPA (it is re-allocated per boot; the old `0x277913000` is stale),
  then dumping that region.
- Re-examine device-state/arming: bring-up is order- and freshness-sensitive
  (`[[clarett-control-manifestation]]`); FC's working device may reach an internal state our replay does
  not, despite byte-identical init.

---

## 6. Reproduction (driver A/B params)

All gated, default-off-from-FC's-perspective where relevant, so the elimination is re-runnable:
`trace_regs` (in-driver wire trace), `inject_clock` (the 0x6003 hack), `monitor_enables` (probe
seeding), `drain_causes` (FC-style all-cause poll), `meter_poll_ms` (GET_METER heartbeat),
`verify_writes` (post-write GET_DATA readback), `dma_bits` (coherent mask width).

Exact-subset replay: power-cycle the device (bring-up must run on a fresh device), then
`insmod snd-clarett.ko model=2pre inject_clock=0 monitor_enables=0 meter_poll_ms=0`, toggle a control,
observe no front-panel change.
