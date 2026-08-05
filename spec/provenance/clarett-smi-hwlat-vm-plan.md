# SMI freeze — host-hwlat-while-VM-streams test plan

**Status:** PLANNED (Aug 5 2026). Not yet run.

## Purpose

Decide whether the ~40 ms whole-CPU freeze behind the streaming dropouts
([[clarett-playback-skipping]], [[clarett-periodic-mmio-stall]]) is an
**OS-independent platform SMI** (current root cause) or is somehow correlated
with *our Linux driver's* use of the device.

The trick: **measure the freeze, not the audio.** Every "listen for dropouts"
test is confounded — the freeze is inaudible at the DMA layer, and whether you
*hear* it depends entirely on how the audio consumer reacts to the frozen
position counter (blocking `aplay` rides through inaudibly; timer-scheduled
PipeWire re-prepares → click). So "Windows VM sounds clean" proves nothing on
its own.

`hwlat` sidesteps that: it runs a kernel thread with interrupts and preemption
disabled, polling the clock for gaps. A gap it reports can **only** be time the
CPU was stolen below the OS (SMM/SMI) — not the driver, not the audio stack, not
device I/O. Run it **on the Linux host while the Windows VM drives the device**,
and it tells us whether the freeze still happens when the *vendor* driver is at
the wheel.

## Hypothesis and decision matrix

**H0 (current model):** the freeze is a platform SMI, gated on the Thunderbolt
device actively streaming, independent of which OS drives it. H0 predicts the
host `hwlat` sees ~40 ms spikes on the ~40–58 s cadence while the VM streams —
**regardless** of whether Windows audio is audibly clean.

| Host hwlat (the decisive signal) | Windows audio (secondary) | Verdict |
|---|---|---|
| Spikes present, on cadence | clean | **H0 confirmed & strengthened** — vendor stack masks a real freeze. A transparent ride-through fix on Linux is viable (existence proof). |
| Spikes present, on cadence | dropping | **H0 confirmed** — vendor stack is also susceptible. |
| **No spikes**, VM streams cleanly for the full run | clean | **H0 CHALLENGED.** The freeze correlates with our driver, not the platform. Pivot the whole investigation. |
| No spikes, but streaming wasn't genuinely active | — | Inconclusive — re-run, verify streaming (see Pitfalls). |

The only outcome that overturns the current assumptions is **"the freeze itself
disappears"** — the bottom-left row. "Dropouts disappear" alone never does.

## Preconditions

- Same physical box (ASRock X570 Creator, same BIOS as the streaming-dropout
  observations — record the BIOS version).
- Device bound to `vfio-pci`, passed through to the Win10 VM (the normal RE
  passthrough config). **You do NOT want `x-no-mmap` here** — this is a
  native-speed streaming test, not an MMIO trace. Use the plain passthrough
  domain, not the trace-enabled one.
- Vendor driver / Focusrite Control installed in the guest, and a way to make
  streaming **sustained** (a long track looping to an output, or a live input) —
  the trigger is *active* streaming, not just an app being open.
- Kernel has the tracer: `grep -wo hwlat /sys/kernel/tracing/available_tracers`
  must print `hwlat`. If absent, the running kernel lacks `CONFIG_HWLAT_TRACER`
  — either boot one that has it, or fall back to `osnoise`/`timerlat` (see
  Alternatives). hwlat is not a module, so Secure Boot / lockdown don't block it.
- Reference onset from the Linux runs: first freeze hit **~6–14 s** into
  streaming and recurred every **~40–58 s**, ~42 ms each. In ~15 min expect
  ~15–22 events.

## Procedure

Run everything on the **host** as root. `$TR` is the tracing fs.

```sh
TR=/sys/kernel/tracing; [ -d "$TR/hwlat_detector" ] || TR=/sys/kernel/debug/tracing
ls "$TR/hwlat_detector"/            # enumerate knobs (names vary by kernel)

echo 0        > "$TR/tracing_on"
echo hwlat    > "$TR/current_tracer"
echo 1000000  > "$TR/hwlat_detector/window"   # 1 s period
echo 500000   > "$TR/hwlat_detector/width"    # 0.5 s sampled per period = 50% duty (see note)
echo 1000     > "$TR/tracing_thresh"          # report gaps > 1 ms; filters normal jitter, catches ~40 ms
echo 1        > "$TR/tracing_cpumask"          # pin detector to CPU0 (mask 0x1); see CPU-pinning note
```

**Duty-cycle vs. perturbation (important).** During each `width` the detector
holds its CPU with IRQs off, so a high duty cycle can itself disrupt the VM's
streaming — which would confound the *audio* read (not the spike read). Because
the SMI hits **all** cores at once, hwlat on any one core still sees it, so you
don't need high duty to detect the freeze. Start at **50%** (above). It catches
~half of the ~15–22 events over 15 min (plenty for a "present" verdict). Only
raise toward 90–95% (`width` 900000–950000) if you need a *tighter bound on the
"absent" outcome*, and only after confirming the higher duty doesn't itself
cause VM dropouts.

**CPU pinning.** Put the detector on a core **not** running a QEMU vCPU and
**not** taking the passthrough device's MSI IRQs (check `/proc/interrupts` for
the `vfio` lines; move their affinity off the chosen core if needed). CPU0 is a
reasonable default only if your libvirt `vcpupin`/`emulatorpin` keep the guest
off it. On some kernels hwlat honours `tracing_cpumask` via a round-robin
`hwlat_detector/mode`; if a `mode` file exists, leaving it at `round-robin` with
a single-bit `tracing_cpumask` keeps it on that one CPU.

### Step A — negative control (do this FIRST)

VM up, device attached, but **not streaming**. Run ~10 min:

```sh
echo > "$TR/trace"; echo 1 > "$TR/tracing_on"
timeout 600 cat "$TR/trace_pipe" | tee ~/hwlat_vm_idle_$(date -u +%Y%m%dT%H%M%SZ).log
echo 0 > "$TR/tracing_on"
```

**Expect zero spikes** (matches the Linux "unloaded+idle = 10 min zero"
baseline). This proves the detector config is sane and that mere
attachment/enumeration isn't the trigger. If spikes appear here, something else
generates them and the streaming test is confounded — resolve before proceeding.

### Step B — the streaming test

```sh
echo > "$TR/trace"; echo 1 > "$TR/tracing_on"
cat "$TR/trace_pipe" | tee ~/hwlat_vm_stream_$(date -u +%Y%m%dT%H%M%SZ).log
# → now start SUSTAINED streaming in the guest; note the wall-clock (UTC) start.
# Let it run 15 min (raise to 30 for a confident "absent" verdict). Ctrl-C to stop.
echo 0 > "$TR/tracing_on"
```

Watch `trace_pipe` live. hwlat lines look like
`#999  inner/outer(us): 0/39632  ts:...` — the `outer` value is the stolen time.

## Data to record

- **Negative control:** spike count over 10 min (expect 0).
- **Streaming run:** total spike count; inter-arrival spacing (is it ~40–58 s?);
  max `outer` (~42 ms?); **onset delay** from streaming start (Linux was
  6–14 s).
- Whether Windows audio was audibly clean or dropping (secondary — may be
  perturbed by the detector at high duty).
- Provenance: `uname -r`, BIOS version, the pinned CPU, `width`/`window`/duty,
  `tracing_thresh`, and the two log files. Timestamps are UTC — compare with
  `date -u`.

## Pitfalls

- **Not actually streaming.** The trigger is *active* DMA, not an open app.
  Confirm audio is really flowing in the guest. A "no spikes" with no streaming
  is the inconclusive row, not the falsifying one.
- **Detector too quiet / too loud.** 50% duty may miss a run of events by
  chance — a *single* streaming run with zero spikes over 15 min at 50% is
  suggestive but not conclusive of "absent"; extend to 30 min and/or raise duty
  before calling H0 challenged. Too-high duty perturbs the VM and muddies the
  audio read (never the spike read).
- **Wrong domain.** Use the plain passthrough VM, not the `x-no-mmap`
  trace-enabled one — MMIO trapping dilates every access ~20 µs and changes
  timing (it's what hid the manifestation-wall race originally).
- **isolcpus/nohz_full core.** Prefer an ordinary housekeeping CPU for the
  detector; `nohz_full` cores can make hwlat behave oddly.

## Alternatives / corroboration

- **`osnoise` tracer** gives a per-source breakdown (`HW`, `NMI`, `IRQ`,
  `THREAD`) of stolen time — the `HW` column corroborates an SMM/hardware gap
  and, unlike hwlat, doesn't hold IRQs off for long stretches (less VM
  perturbation). Good cross-check if hwlat's duty-cycle disruption worries you.
- **`timerlat`** measures wakeup latency and also attributes hardware noise;
  another independent confirmation path.

## If H0 is challenged (no spikes while the VM streams cleanly)

Then the freeze tracks *our* driver, not the platform. First re-audit what our
streaming does that the vendor's doesn't at the same cadence — the SCHED_FIFO
servicer's MMIO read pattern, MSI handling, or a specific register access — and
reconcile against the "mailbox polling disproven" and "streaming-gated" findings
in [[clarett-playback-skipping]], which were all taken under *our* driver and
would need re-interpreting.
