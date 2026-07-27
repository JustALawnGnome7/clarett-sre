# Clarett — macOS DTrace off-wire capture plan

> **Scope:** a method to observe the **off-wire/below-BAR** bus-master DMA that the manifestation wall
> (`clarett-manifestation-wall.md`) and the data-plane wall (`clarett-data-plane.md`) both hide behind —
> **without a bus analyzer**. The device runs on an Apple-Silicon MacBook (M1) with Focusrite Control.
>
> **Status: RUN — outcome in `clarett-manifestation-wall.md §5d` (July 1 2026).** Summary: the vendor
> user-space→kext traffic (control + bring-up) is **byte-identical to our RE**, and the working device
> returns **rich real data** (caps/firmware/serial/config) to `GET_DATA` requests that return **empty** for
> our Linux driver — confirming the wall from a third platform and localizing it to the **kext's off-wire
> device-init**. It did **not cross** the wall: the kext is a **stripped, address-redacted release binary**,
> so DTrace can neither `fbt` its functions nor attribute its stack frames. **This DTrace avenue is
> exhausted;** the phases below are retained as the method record. Captures: `captures/macos_*`.

## Why this can cross the wall when the vfio trace can't

The Windows/QEMU `x-no-mmap` method sees **MMIO** but is blind to **DMA** — that blindness is exactly
where both walls sit (`spec/provenance/clarett-manifestation-wall.md §5c`: the device answers our whole command
stream with `SUCCESS + size=0` from query #1, over a verified-working DMA path). The off-wire
differentiator is a **bus-master DMA payload/timing FC produces that our driver never does**.

That payload does not appear from nowhere: the **working driver builds it in host RAM** before ringing
the doorbell. So instead of watching the wire (bus analyzer, ruled out), we watch the **source** of the
DMA — the driver's memory allocations and buffer writes — on the Mac that runs it. DTrace of the working
Focusrite driver is the *complement* to the BAR trace: MMIO from vfio, DMA construction from DTrace.

**Clean-room note.** Dynamic tracing of DMA-buffer contents and IOKit call arguments is **black-box
interface observation** — the same class as the MMIO capture we already do; it observes *what the device
is given*, not the driver's source. Disassembling the kext/dext binary would cross the line; **do not**.

## What a result looks like (decision criteria)

Diff the capture against our replay (`captures/our_trace.log` + `driver/clarett_init_*.h`, which
program only the single GET-response buffer at BAR `0x410/0x414`):

| Observation on the Mac | Action in our driver |
|---|---|
| FC prepares a DMA buffer we don't program | allocate + program the same region; re-test manifestation / empty-GET |
| FC writes specific bytes into a device-read buffer pre-arm | replicate those exact bytes before our doorbell |
| Device DMA-**reads** a request/token from the response buffer | pre-seed our `resp_buf` likewise (`resp_probe` groundwork already in-tree) |
| Nothing beyond our replay | driver exonerated → arming is firmware/link-level; DTrace narrows the wall, doesn't cross it |

## Phase 0 — environment + driver identification

```sh
csrutil status                                  # expect: disabled (set in recoveryOS)
# Apple Silicon: also lower boot policy to Permissive/Reduced or fbt won't attach (bputil -d / Startup Security Utility)
systemextensionsctl list                        # a Focusrite .dext? -> DriverKit (user space)
kmutil showloaded | grep -i focusrite           # a kext?            -> IOKit (kernel space)
ioreg -c IOPCIDevice -l | grep -iC 30 focusrite # confirm attach + DMA/mapper props
```
Branch on the result — the tool differs:
- **kext (kernel)** → DTrace `fbt` provider (needs SIP off + Permissive boot policy).
- **dext (user space)** → DTrace `pid` provider on the dext process (`pgrep -fi focusrite`); cleaner, no
  kernel debugging. A system-managed dext may resist `pid`-provider attach → use the fallbacks.

## Phase 1 — discover the exact probe names (don't guess symbols)

```sh
sudo dtrace -ln 'fbt::*IOBufferMemoryDescriptor*:'
sudo dtrace -ln 'fbt::*IODMACommand*:'
sudo dtrace -ln 'fbt::*IOMemoryDescriptor*prepare*:'
sudo dtrace -ln 'fbt:<focusrite-bundle-id>::'                 # kext: the driver's own functions
sudo dtrace -ln 'pid$target::*DMA*:entry' -p <dext-pid>       # dext equivalent
sudo dtrace -ln 'pid$target::*MemoryDescriptor*:entry' -p <dext-pid>
```
Pick out: the allocation call, the `prepare`/`PrepareForDMA` call (buffer is filled + DMA-ready at its
return), and any driver-named `...doorbell...`/`...writeReg...`/`...arm...`/`...start...` method.

## Phase 2 — enumerate DMA surfaces (allocations + IOVAs + sizes)

`dma-surfaces.d` (`sudo dtrace -qs dma-surfaces.d > surfaces.log`):
```d
#pragma D option quiet
fbt::*IOBufferMemoryDescriptor*inTaskWithPhysicalMask*:entry,
fbt::*IOBufferMemoryDescriptor*inTaskWithOptions*:entry
{
    printf("%Y ALLOC %s cap=%#x\n", walltimestamp, probefunc, (unsigned long)arg2);
    stack(); ustack();      /* the driver frame identifies which surface this is */
}
fbt::*IODMACommand*prepare*:return
{
    printf("%Y DMA-PREPARE done cmd=%p err=%d\n", walltimestamp, (void*)arg0, (int)arg1);
}
```
dext template (`-p <pid>`): same shape against `pid$target::*Create*:return` /
`pid$target::*PrepareForDMA*:return`, capturing the returned buffer handle + `GetAddressRange` result.

**Deliverable:** the full inventory of buffers FC prepares at attach, with sizes + call sites. Cross-check
count/sizes against our *one* GET buffer — an extra region is an immediate hit.

## Phase 3 — dump buffer *contents* at the arm moment (the key step)

Once Phase 2 gives the buffer's CPU pointer (kext: `getBytesNoCopy()` return; dext:
`GetAddressRange().address`), snapshot it when the driver hands it to the device — trigger on the
`prepare` return and/or the doorbell wrapper:
```d
/* kext: dump the just-prepared DMA buffer — the candidate off-wire payload, in host RAM */
fbt::*<driver-doorbell-or-prepare-fn>*:entry
{
    tracemem((void*)arg1 /* buffer CPU addr from Phase 2 */, 256);
    printf("%Y ARM/DOORBELL buffer above\n", walltimestamp);
}
/* dext: copyin from the user-space driver, then tracemem */
pid$target::*<doorbell-fn>*:entry { tracemem(copyin(arg1, 256), 256); }
```
Capture the **whole bring-up** this way, plus dump the response buffer **again after the completion IRQ**
— that directly answers "is the device's response truly empty, or does it fill a buffer we mis-read?"

## Phase 4 — control-toggle isolation (same method as the vfio trace)

Device live → start capture, **toggle exactly one control** in Focusrite Control (e.g. Monitor Mute),
stop. Confirm whether the write is pure-MMIO (as we believe) or *also* touches a DMA buffer. A host-buffer
change on a control write would mean control application is itself partly off-wire — a major finding.

## Fallbacks if DTrace attach is blocked (common on protected dexts)

- **`ioreg -l -w0`** on the `IOPCIDevice` / DMA-mapper nodes — static, but shows the attach-time DMA
  mapping + buffer descriptors with no tracer attached.
- **KDK + `lldb` kernel debug** from a second Mac over the network — heaviest, most reliable; breakpoint
  `IODMACommand::prepare` and dump buffers. Needs two machines.
- **`log stream --predicate 'senderImagePath CONTAINS "ocusrite"'`** — the driver may emit `os_log`.

## Capture hygiene

Mirror the vfio workflow: run DTrace to a timestamped file, perform **one** action per capture (attach /
single-control toggle), and diff against the corresponding Linux artifact. Apple Silicon sits a **DART**
(Apple's IOMMU) between device and RAM, so device-visible addresses are IOVAs — we care about buffer
**contents and existence**, not raw physical addresses.
