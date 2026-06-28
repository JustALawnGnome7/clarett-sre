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

## 5. The one remaining path (not yet taken)

Capture FC's DMA directly: dump guest RAM (`pmemsave`, see `clarett-mmio-trace-setup` memo) during FC's
boot around FC's response buffer (GPA `0x277913000`, programmed at `0x410/0x414`) and scan for a
region/blob the device reads or writes beyond the mailbox response buffer.

**Reference targets for the capture (local only, never committed).** Focusrite Control's firmware bundle
is available locally at `FCP Server Resources/Firmware/` (git-ignored via `.gitignore`'s
`FCP Server Resources/` rule — it must **never** be committed). The files relevant to the Clarett
Thunderbolt are `ClarettThunderbolt.tca` (App segment) and `fp001005_tb_top.bit` / `fp001054_tb_top.bit`
(~1.48 MB Thunderbolt FPGA bitstreams, two versions); the other `*.bin`/`*.bit`/`*.tca` files are for
other products in the line. These make the **FPGA-upload hypothesis testable**: in the DMA capture, look
for a ~1.48 MB region or a recognizable `.bit` header matching one of the `tb_top` bitstreams being
DMA'd to the device. A match would explain *both* dead planes at once — the FPGA datapath stays dark
because our driver never uploads it.

**Clean-room caveat.** Reverse-engineering the *upload protocol* from observation, and using the local
bundle only to *recognize* the payload in a capture, stays within the project's clean-room discipline.
For a working driver the standard Linux pattern is `request_firmware()`: the driver carries only the
upload *logic* and the user supplies the blob from their own FCP install — the bitstream is **not**
shipped in-tree (as dozens of mainline drivers do). The remaining blocker is narrower than first stated:
not "can we get the blob" (the user has it) but whether that file's license permits redistribution,
which only affects **mainline** acceptance, not a personal/out-of-tree build.

---

## 6. Reproduction (driver A/B params)

All gated, default-off-from-FC's-perspective where relevant, so the elimination is re-runnable:
`trace_regs` (in-driver wire trace), `inject_clock` (the 0x6003 hack), `monitor_enables` (probe
seeding), `drain_causes` (FC-style all-cause poll), `meter_poll_ms` (GET_METER heartbeat),
`verify_writes` (post-write GET_DATA readback), `dma_bits` (coherent mask width).

Exact-subset replay: power-cycle the device (bring-up must run on a fresh device), then
`insmod snd-clarett.ko model=2pre inject_clock=0 monitor_enables=0 meter_poll_ms=0`, toggle a control,
observe no front-panel change.
