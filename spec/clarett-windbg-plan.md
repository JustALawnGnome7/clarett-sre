# Clarett — WinDbg capture of the Windows driver's init DMA (RUN warm + cold; wall below-driver; only the late MDL-contents db remains)

> **Scope:** a method to capture the **off-wire/below-BAR** DMA setup the *working* Windows Focusrite
> kernel driver does at device init — the one thing the macOS DTrace attempt could never get (it lost the
> timing race and the kext was stripped). Kernel-debug the Windows guest (which already runs FC under vfio
> passthrough) and breakpoint the **symbolicated Windows DMA APIs** the driver calls, attributing to the
> Focusrite `.sys` by module range. **Status: RUN (July 2 2026). Outcome: the vendor's driver-level DMA is
> equivalent to ours; the wall is confirmed below-driver.** Full write-up: `clarett-manifestation-wall.md` §5e.
> **Caveat found later:** that run saw a **warm** (never power-cycled) device — a VM reboot keeps the device's
> DC power on, so a once-per-power-cycle bring-up (INIT_1 `0x0`, or a DMA firmware stage) would be invisible. The **cold-boot
> capture** below (§5f) is now the top open lead, ahead of the sibling audit.

## Result (July 2 2026) — RUN, clean-room discipline held

Executed via serial KD (KDNET failed on QEMU's e1000e, `0xC0000182`; fell back to `bcdedit /dbgsettings
serial`, COM2 over a host TCP socket to a second `Windows10-WinDbg` VM). Breakpoints hit **only symbolicated
MS APIs**; `FocusritePCIe.sys` was never disassembled/stepped.

- **`FocusritePCIe.sys` is KMDF.** IAT imports `MmAllocatePagesForMdlEx`, `MmMapLockedPagesSpecifyCache`,
  `HalPutDmaAdapter`, `MmMapIoSpace`; common buffers go through the WDF table (`WdfCommonBufferCreateWithConfig`).
- **Complete init DMA footprint** (at `EvtDevicePrepareHardware`): two stream blocks, each a **16 KB common
  buffer (SG descriptor table) + 2 MB scattered MDL (sample buffer)** (`+0x8cac`/`+0x8dac`), plus one
  standalone **4 KB common buffer** (`+0xa6ea`) = the FCP response buffer. That's the entire footprint.
- **Attributes match ours.** `MmAllocatePagesForMdlEx`: `Low=0`, `High=0xFFFFFFFFFFFFFFFF` (no ceiling),
  `Cache=MmCached`; common buffers `CacheEnabled=1`. On x86 a cached common buffer *is* coherent = Linux
  `dma_alloc_coherent`. Same address range, same cache/coherency, same descriptor-table+scatter strategy.
- **Nothing extra reaches the device at init.** Cross-checked with the vfio trace: only `0x410/0x414` is
  programmed at init (`0x210/0x310` engine-arm is stream-start, ~25 s later); no DMA pointer is pushed via
  the mailbox; our `clarett_init_8prex.h` replay carries no baked addresses.

`[CONCLUSION]` The plan's **"anticlimax" branch** (Phase 3): the vendor driver does nothing extra with DMA at
the driver level. WinDbg got the timing/symbols/module-ranges the raced-out, stripped-kext macOS attempt
never could, and **definitively** closes the "vendor sets up DMA we don't" hypothesis. Differentiator is
below the driver (transport/link/device-init handshake), invisible to all host-side software tracing.

## Cold-boot capture (planned, NOT yet done) — the top open lead

**Why this jumped ahead of the sibling audit.** The July 2 run — and *every* vfio capture it cross-checks
against — saw a **warm** device: a libvirt/VM reboot does not cut the Clarett's power (it runs off its own DC
adapter, not the TB bus), so unless the device was **DC power-cycled** it stayed armed continuously, and we
captured the vendor re-initializing an **already-armed** device. Re-reading the boot captures makes the gap concrete (manifestation-wall §5e caveat):
**INIT_1 (opcode `0x0`) never appears** (we see INIT_2 `0x2` only — the scarlett2 first-contact step 1 is
missing), and there is **no REBOOT (`0x3`) and no firmware/FPGA-sized write burst**. `seq=0` is a *driver*-side
reset, not a cold marker. Our `clarett_arm_device` is generated from the warm capture, so it is INIT_1-less
and upload-less **by construction** — if either is the true cold bring-up, we don't send it.

**Goal:** capture the vendor's init from a *genuinely cold* device and diff against the warm baseline, to
either surface a cold-only command/firmware stage (→ add it to `clarett_arm_device` and re-test the wall) or
close the last state-dependent hole. This is software-only and **not** excluded.

**The only true reset is a device DC power-cycle.** Config space reports **`FLReset-`** — no function-level
reset to trigger from software, and a VM reboot keeps the device powered. The 2Pre has its **own DC adapter and
cannot be bus-powered over Thunderbolt**, so **unplugging the TB cable alone does NOT reset it** — the FPGA/
firmware stay running (warm). So: with the target VM shut down, **cut the device's own DC power (adapter off)
~10 s, then restore it** (and re-authorize if `boltctl` prompts, re-check the `vfio-pci` bind). That, and only
that, gives a cold device. (Every prior "TB replug" in this project was in practice a DC power-cycle, so those
cold results are valid — but never do a cable-only unplug and call it cold on this hardware.)

**Run both instruments at once** — they cover different blind spots:
1. **vfio MMIO trace** (device-facing FCP/mailbox) — catches a cold-only **INIT_1 (`0x0`)** or **REBOOT
   (`0x3`)**, i.e. anything pushed through the `0x8020` mailbox. This is the cheap half; it may be enough.
2. **WinDbg** (below-BAR DMA) — catches a **firmware/FPGA stage that moves by DMA, not the mailbox**, which
   the vfio trace is structurally blind to. On the warm run the two 2 MB sample MDLs read back **all zeros**;
   on a cold boot a staging buffer could contain the bitstream. So this run's new move is **dumping buffer
   *contents*, not just attributes.**

### Procedure
0. **Free the device & arm the trace.** Cold-replug per above. Bring up serial KD to `Windows10-WinDbg` exactly
   as in the §5e "Result" (KDNET fails on QEMU e1000e → `bcdedit /dbgsettings serial`, COM2 over the host TCP
   socket). Start the vfio `vfio_region_*` trace on the host in parallel (same libvirt `x-no-mmap` setup as
   `../CLAUDE.md` "Method").
1. **Break at the driver's *first* load on the cold device:**
   ```
   sxe ld:FocusritePCIe.sys
   g                       ; boot proceeds; breaks when the image loads
   lm m FocusritePCIe      ; re-read the KASLR range -> FR_LO / FR_HI (changes every boot)
   ```
2. **Arm the same three DMA breakpoints as §5e, filtered to the FocusritePCIe range** (see Phase 2 below for
   the exact `.if (poi(@rsp) >= FR_LO & poi(@rsp) < FR_HI)` conditional form). Keep `MmAllocatePagesForMdlEx`,
   `WdfCommonBufferCreateWithConfig`, `HalAllocateCommonBuffer`.
3. **For every allocation, dump attributes *and contents*** (the contents are the new part):
   ```
   gu                      ; run to return; @rax = buffer VA (MDL ptr for MmAlloc...)
   dt nt!_MDL @rax         ; for MDLs: ByteCount, MappedSystemVa
   db <MappedSystemVa> L200 ; <-- CONTENTS. On warm boot these were 0; a firmware stage would be nonzero
   dps @rax+0x30 L8        ; MDL PFN array (physical scatter), for the 2 MB sample buffers
   g
   ```
   Watch specifically for: **an allocation the §5e warm inventory did *not* have** (extra buffer = the prize),
   or **nonzero contents** in any buffer that was zero warm (candidate firmware/bitstream — check the first
   bytes against the Xilinx magic `00 09 0f f0` / design name `fpNNNNNN_tb_top`, from the local-only
   `vendor-reference/Firmware/` refs; **do not commit those refs**).
4. **From the parallel vfio trace, decode the cold init** and diff opcodes vs the warm baseline:
   ```
   python3 tools/fcp_decode.py --brief captures/cold_boot.log | sed -n '/opcode histogram/,$p'
   ```
   Compare against `8prex_full_init_mute.log`'s histogram (INIT_2 ×6, CONFIG_PUSH ×122, no INIT_1/REBOOT).
   **A cold-only `0x0` (INIT_1) or `0x3` (REBOOT), or any new chunked-SET_DATA burst, is the find.**

### Decide
| Cold capture shows | Action |
|---|---|
| INIT_1 (`0x0`) / REBOOT (`0x3`) in the vfio trace, absent warm | prepend it in `clarett_arm_device` **before** the CONFIG_PUSH block; re-test manifestation on a freshly power-cycled device |
| a DMA buffer with **nonzero** (firmware-looking) contents, or an **extra** allocation vs §5e | that's the once-per-power-cycle stage the warm runs missed → replicate the buffer + its programming; revisit §5's firmware-upload disproof (its RAM scan was warm too) |
| byte-identical to the warm baseline (same opcodes, same DMA footprint, buffers still zero) | the device-init is genuinely state-independent → the warm §5e conclusion is airtight, and the below-driver localization tightens further |

**Clean-room:** same guardrails as §5e — break only on symbolicated MS APIs, attribute by module range, dump
**data** (allocations/addresses/attributes/**bytes**). Dumping buffer contents is data observation (what a bus
analyzer would see); **do not disassemble or step `FocusritePCIe.sys`.**

## Cold contents pass — EXACT runbook — RUN July 6 2026, NEGATIVE (kept for method reference)

> **DONE (July 6 2026) — NEGATIVE.** Both 2 MB sample MDLs read **all-zero** at the freshest post-init moment on a
> cold (DC power-cycled) 2Pre; all firmware searches (`00 09 0f f0`, `aa 99 55 66`, `"tb_top"`) empty. Firmware-over-DMA
> **disproven**; cold-boot lead closed on all three surfaces (manifestation-wall §5f). This runbook is retained for
> method reference. **Two hard-won method fixes vs. July 3:** (1) a `gu` **inside** a bp command is fatal — WinDbg skips
> every command after it and runs the target on, capturing nothing (this is why July 3 recorded no `MappedVA`s). Use a
> **halt-only** bp (`{ .echo FR_2MB_MDL_HIT }`) and run `gu` / `r @rax` **manually** at each halt. (2) `MappedSystemVa`
> is `0x1` at allocation — record the **MDL pointer**, re-read the VA post-init (once `FocusritePcieAudio`/`Midi` load).

Status after July 3 2026: the cold mailbox trace and cold DMA-**allocation** capture are DONE and negative
(manifestation-wall §5f) — same-device (2Pre) cold==warm, no extra buffer, siblings do no DMA. The **one**
remaining sub-task (now also DONE, negative — see box above): `db` the two 2 MB sample MDLs **post-init** on a cold device (last pass didn't record
their addresses). This runbook is turnkey — run it verbatim.

**What worked / what didn't (so you don't relearn it):**
- `sxe ld:FocusritePCIe` **never halts** (tried with and without `.sys`). Do not rely on it.
- A live `bp nt!MmAllocatePagesForMdlEx` armed at the **uptime-0 break floods** over serial from
  `nt!BgpFwAllocateMemory` (boot graphics) and never reaches the driver. **Arm late**, past boot graphics.
- The reliable arming point is the boot break where **`FocusritePCIe` is loaded but `FocusritePcieAudio`/
  `FocusritePcieMidi` are NOT** — that's after boot graphics and before `PrepareHardware` (Audio/Midi are
  upper drivers that load only after FocusritePCIe starts its device). If Audio/Midi are already present,
  `PrepareHardware` already ran — reboot.
- `MmAllocatePagesForMdlEx` returns a **PMDL** in `rax`; its `MappedSystemVa` (MDL+0x18) may be **null at
  allocation** (mapped later). So **record the MDL pointer** and read/`db` the VA **at the desktop**.

### 0. Cold device + instruments
1. VM off. **DC power-cycle the 2Pre** — cut its own DC adapter ~10 s, restore (`boltctl authorize` on the host
   if it doesn't reattach; re-check `vfio-pci`). NOT a TB-cable unplug: the 2Pre is self-powered, so pulling the
   cable leaves it warm. Only a DC power-cycle is cold; a VM reboot is warm.
2. Host: `sudo tail -F /var/log/libvirt/qemu/Windows10-custom.log` → save to `captures/2pre_cold_boot2.log`
   (parallel re-confirm of no `INIT_1`/`REBOOT`).
3. Debugger VM (`Windows10-WinDbg`) up and listening; then start the target VM.

### 1. At the initial break (`nt!DebugService2`, uptime 0)
```
.sympath srv*C:\symbols*https://msdl.microsoft.com/download/symbols
.reload
.logopen C:\Users\Public\fr_cold_contents.log
```
Do **not** set the MDL bp yet.

### 2. Reach the arming point
```
g
```
Then Ctrl+Break (or wait for a natural break) and check — repeat until the condition is met:
```
lm m Focusrite*
```
- `FocusritePCIe` absent → `g`, check again later.
- `FocusritePCIe` present **and** `FocusritePcieAudio`/`Midi` absent → **arming point**, go to step 3.
- `FocusritePcieAudio`/`Midi` already present → too late; reboot (or use the fallback below).

### 3. Arm the 2 MB MDL breakpoint (records ptr + VA), then run
```
lm m FocusritePCIe
r @$t0 = <Start>
r @$t1 = <End>
bp nt!MmAllocatePagesForMdlEx ".if ((@r9 == 0x200000) & (poi(@rsp) >= @$t0) & (poi(@rsp) < @$t1)) { gu; .printf \"\\n>>> FR 2MB MDL  ptr=%p  MappedVA=%p\\n\", @rax, poi(@rax+0x18); g } .else { gc }"
bl
g
```
Let it run to the **login screen / desktop**. Two `>>> FR 2MB MDL ptr=… MappedVA=…` lines print. **Record both `ptr=` values.**

### 4. Dump contents LATE (post-init) — the actual test
Using each recorded MDL pointer (VA may have been null at alloc, so re-read it now):
```
dt nt!_MDL <ptr1>            ; note MappedSystemVa
db <MappedSystemVa1> L200
dt nt!_MDL <ptr2>
db <MappedSystemVa2> L200
```
Read the bytes: **all-zero → no firmware.** Nonzero → check for Xilinx magic `00 09 0f f0` / design name
`fpNNNNNN_tb_top` (refs local-only in `vendor-reference/Firmware/`, **never commit**) → firmware-over-DMA
found.

### 5. Save
```
.logclose
```
Copy `C:\Users\Public\fr_cold_contents.log` → `captures/`, and keep the host vfio trace. Update §5f with the
`db` result.

### Fallback if you keep overshooting (Audio/Midi already loaded at every break)
At the initial break, arm the low-volume PnP dispatcher (past boot graphics, not a hot path):
```
bp Wdf01000!FxPnpDevicePrepareHardware::InvokeClient
g
```
It halts at each device's `PrepareHardware`. At each halt, `lm m FocusritePCIe`; once loaded, do step 3
(`@$t0/@$t1` + the MDL bp), `bc` the InvokeClient bp, `g`.

## Follow-up (superseded July 3 2026) — sibling-driver audit

**Largely CLOSED by the July 3 cold run** (manifestation-wall §5f): with `FocusritePCIeSwRoot`,
`FocusritePcieAudio`, and `FocusritePcieMidi` all loaded, **none allocated any DMA buffer** — only
`FocusritePCIe` does DMA. So the sibling "hidden DMA surface" worry is resolved by observation. The only
thing not done is a formal IAT dump per sibling; optional now, since the behavioural result (zero DMA
allocations) is stronger than the imports list. Original per-sibling steps retained below for reference.

## Follow-up (original steps, for reference) — audit the three sibling drivers

The July 2 run was **scoped to `FocusritePCIe.sys`** (broke at *its* image load; MDL breakpoint filtered to
*its* module range). The three siblings — **`FocusritePCIeSwRoot.sys`**, **`FocusritePcieAudio.sys`**,
**`FocusritePcieMidi.sys`** — were **not** independently examined; their roles are inferred from names only.
`SwRoot` in particular, as a likely *root/parent enumerator*, may load and run **before** `FocusritePCIe`,
so anything it does would be outside the window we captured. This doesn't threaten the below-driver
conclusion (the vfio trace is driver-agnostic and bounds all device-facing MMIO/mailbox), but it's the one
open completeness thread. To exhaust it next session, for **each** sibling:

1. **Imports** — `lm m <sib>` for the range, then dump its IAT (`dps <sib>+<ImportAddressTable rva> L<n>`,
   RVA/size from `!dh <sib> -f`). Flag any of `MmMapIoSpace`, `MmAllocatePagesForMdlEx`,
   `Mm*ContiguousMemory*`, `Io/HalGetDmaAdapter`, `WdfDmaEnabler*` → does it touch hardware/DMA at all, or
   is it pure PnP/IOCTL?
2. **Topology** — `!devnode 0 1 <sib>` and `!devstack` on the Clarett devnode: parent / sibling / unrelated
   software device? (Confirms load order vs `FocusritePCIe`.)
3. **Its DMA, if any** — reboot; `sxe ld:<sib>.sys` to break at *its* load **before** `FocusritePCIe` if it
   precedes it; re-read its KASLR range; arm the same MDL / `WdfCommonBufferCreateWithConfig` /
   `HalAllocateCommonBuffer` breakpoints **filtered to its range**; enumerate. Note `SwRoot` may need
   catching very early (root enumerator) — arm at the earliest boot break and widen if it loads pre-PnP.
4. **Cross-check** any buffer it programs against the vfio `0x2xx/0x3xx/0x410` writes; if a sibling
   programs a device address our driver doesn't, that reopens the "extra surface" branch for that plane.

Expected outcome (to be confirmed, not assumed): `SwRoot` is a software-root anchor / control-app IOCTL
endpoint with no hardware DMA; `Audio`/`Midi` are upper function drivers that broker through `FocusritePCIe`
(the Audio WaveRT buffer, if any, is data-plane). If confirmed, §5e stands as written; if not, revise.

---

## Original plan (as executed below)

## Why this can get what DTrace couldn't

The macOS DTrace attempt (`clarett-macos-dtrace-plan.md`, manifestation-wall §5d) confirmed the wall but
never captured the vendor driver's DMA construction, blocked by two things WinDbg does **not** face:

1. **No timing race.** A debugger owns the timeline — break on the Focusrite driver's load / `StartDevice`
   and single-step forward. (On macOS the kext's `start()` DMA ran before any tracer could attach.)
2. **Symbolicated hooks + visible module ranges.** The Windows kernel/HAL/WDF DMA routines the driver
   *calls* ship with Microsoft symbols, so we hook **those** (not Focusrite's code). And `lm` shows every
   loaded module's base/end — **including** an unsigned third-party `.sys` — so a DMA-API call is
   attributed to Focusrite by **return-address-in-range**, cleanly, without its symbols. (On macOS the kext
   was stripped *and* Apple redacted its load address, so its frames were unattributable.)

So WinDbg can enumerate the driver's DMA allocations, their sizes/addresses, and their **contents** at
handoff — answering the open §5d question: *does the vendor set up a DMA surface or write an "arm" payload
beyond the single response buffer we program at `0x410/0x414`?*

## Clean-room rule (the whole point)

Break only on **Windows/HAL/WDF (symbolicated)** functions; attribute to Focusrite by **module range**;
capture **allocations, addresses, sizes, and buffer bytes** — the interface data (same class as the MMIO
trace, or what a bus analyzer would see). **Do NOT `u` (disassemble) or single-step the Focusrite `.sys`
to read/reimplement its logic.** Capturing the data it hands the device is clean; reading its code taints
the distributable driver exactly like disassembly would. If you find yourself reading its code to interpret
a capture — stop; that's the line.

## Do the free thing first (no WinDbg)

The vfio trace already shows the **physical addresses** the driver writes to `0x410/0x414` (and any stream
bases). Dump those in the QEMU monitor — purely software, unambiguously clean:
```
(qemu) pmemsave 0x<phys-from-0x410/0x414> 0x200 /tmp/respbuf.bin
```
That gets the contents of buffers whose address you already know. **WinDbg's only added value is finding
buffers whose address you *don't* see in a traced register, plus exact timing** — so start here.

## Setup — kernel-debug the target guest

- Target guest: `bcdedit /debug on`, then a transport.
- **KDNET (recommended):** `bcdedit /dbgsettings net hostip:<debugger-ip> port:50000` → reboot (prints a key).
- **Practical wrinkle:** WinDbg is Windows-only and the host is Fedora — so run WinDbg on a **second small
  Windows VM** on the same host, networked to the target, via KDNET. (Linux KD-protocol reimplementations
  exist but are unreliable; use real WinDbg.)
- Halting the target freezes DMA — fine for capturing *init/bring-up*, not live streaming.

## Phase 0 — recon (attribution range)
```
lm ; lm m focusr*        ; find the Focusrite driver; note Start/End  ->  FR_LO / FR_HI
!drvobj <FocusriteDriver> 2   ; (optional) driver object / dispatch routines
```

## Phase 1 — break at init
```
sxe ld:<focusrite>.sys   ; break when the driver image loads
```
Re-trigger without reboot: disable→enable the device in Device Manager (or unplug/replug the TB cable) →
its `StartDevice`/init re-runs while attached.

## Phase 2 — DMA-API breakpoints (the capture)
Coherent-buffer allocation = the analog of `dma_alloc_coherent`. Set all three (whichever fires):
```
bp hal!HalAllocateCommonBuffer
bp Wdf01000!imp_WdfCommonBufferCreate
bp nt!MmAllocateContiguousMemorySpecifyCache
```
`HalAllocateCommonBuffer(adapter, Length, PPHYSICAL_ADDRESS Logical, Cache)` — x64 fastcall: `@rcx`=adapter,
**`@rdx`=Length**, **`@r8`=out-ptr for the physical address**, `@r9`=cache. Fire only for Focusrite:
```
bp hal!HalAllocateCommonBuffer ".if (poi(@rsp) >= FR_LO & poi(@rsp) < FR_HI)
   { .echo >>> FOCUSRITE common-buffer alloc; r rdx; kb; .printf \"phys-ptr=%p\\n\", @r8 }
   .else {}; g"
```
(If WDF-wrapped, the immediate caller is `Wdf01000` and Focusrite is deeper — drop the `.if` and just `kb`;
volume is low since you broke at init.) Then, for a given allocation:
```
gu               ; run to return
r rax            ; RAX = buffer virtual address
dq @r8 L1        ; the PHYSICAL_ADDRESS -> correlate with the 0x410/0x414 MMIO writes
; run to just before the doorbell (an MMIO write you already see in the vfio trace), then:
db @rax L100     ; 256 bytes of buffer contents = candidate off-wire payload
```

## Phase 3 — correlate & decide
Match each buffer's physical address against the values written to `0x410/0x414` / stream bases in the vfio
trace. A buffer whose address is **not** in any traced register is the prize. Then:

| Observation | Action |
|---|---|
| an extra DMA buffer we don't program | allocate + program it in our driver; re-test manifestation/empty-GET |
| specific bytes written into a device-read buffer pre-arm | replicate those bytes before our doorbell |
| device DMA-reads a request/token from the response buffer | pre-seed our `resp_buf` (`resp_probe` groundwork in-tree) |
| nothing beyond our one response buffer | **anticlimax** — the differentiator is below the driver (transport/IOMMU-mapping); WinDbg then *definitively* confirms the driver does nothing extra, which the raced-out macOS capture never could |

## Guardrails (again)
Windows-API breakpoints only · attribute by module range · dump allocations/addresses/bytes · **do not
disassemble or step the Focusrite `.sys`.** Data observation, not implementation.
