# Clarett — WinDbg capture of the Windows driver's init DMA (untried lead)

> **Scope:** a method to capture the **off-wire/below-BAR** DMA setup the *working* Windows Focusrite
> kernel driver does at device init — the one thing the macOS DTrace attempt could never get (it lost the
> timing race and the kext was stripped). Kernel-debug the Windows guest (which already runs FC under vfio
> passthrough) and breakpoint the **symbolicated Windows DMA APIs** the driver calls, attributing to the
> Focusrite `.sys` by module range. **Status: planned, not yet run.**

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
