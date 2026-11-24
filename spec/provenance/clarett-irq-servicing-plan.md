# Servicing the stream from the interrupt — plan

Status: DONE (Nov 23 2026). Steps 0-6 complete; the full matrix passed on all five models with the step 5
build (srcversion `452D1083A5C5B0C80475B11`), released as 0.5.0 (snd-clarett `0c25efa`, tag `v0.5.0`, same srcversion). Results under Step 6 below. Originally
PLANNED Nov 20 2026, with the capture-at-START anchor (round trip = lead + 32 frames) as prerequisite.

## Goal

Service the data plane from the device's interrupt instead of a thread with timers:

- one interrupt handler owns `0x100` and every cause block, on whichever vector fires;
- the period ack (`0x300` read) happens in that handler;
- the mailbox, MIDI and the stream all complete from what that handler records;
- playback is refilled when the app writes (`.ack`), not on a 150 us timer;
- no polling cadence anywhere: the dedicated `clarett-svc` kthread and both its timers
  (`CLARETT_SVC_REFILL_US`, `CLARETT_SVC_FALLBACK_US`) go.

**Design rule (operator, Nov 20 2026): the ASRock's 42 ms MMIO block is not a constraint on any design
choice here.** Design for a healthy host; the ASRock is a host to re-test on, and the expectation is that
the block goes once the driver is right.

## Where it stands (0.4.2 + the START anchor)

Every interrupt arrives on vec0 (vec1-3 never fire, all five models, `irq_rate.sh`), one per period
event (1.001-1.011 per event). The vendor driver services exactly that way: after every vec0 MSI it reads
`0x100`, `0x300`, `0x200`, `0x400`, `0x500` in that order, p50 29 us after the interrupt (2Pre VM trace
with `vfio_msi_interrupt`, `clarett-opcode-inference.md` §12).

Ownership of the read-to-clear registers today:

| register | read by | where |
|---|---|---|
| `0x400` | the ISR, always | `clarett_irq_event()` (clarett_main.c) |
| `0x100` | the ISR while `cmd_inflight`, else the servicer | `clarett_irq_event()` / `clarett_svc_read_causes()` |
| `0x300` (the period ack), `0x200` | the servicer, every pass | `clarett_svc_read_causes()` (clarett_engine.c) |
| `0x500` | the servicer when idle | `clarett_svc_read_causes()` |
| MIDI FIFO (`0x58c`) + ack | the ISR, every vector | `clarett_midi_irq()` |
| `0x200`/`0x300` | the ISR's vec1/vec2 branch (never fires; would steal the ack if it did) | `clarett_irq()` |

So: the mailbox already completes from ISR flags (`mbox_accepted`/`mbox_landed`, step 4b, Oct 10 2026)
and reads no cause register itself. The stream does not: its ack waits for the kthread to be scheduled,
and `0x100`/`0x500` change owner on a flag (`cmd_inflight`) that can change between the servicer testing
it and doing the read.

The kthread (`clarett_stream_service()`) does five jobs:

1. reads `0x300` on an ISR wake, or after 1 ms without one (lost-interrupt backstop);
2. per event: advances `pcm_frames`, copies capture, fills playback, `snd_pcm_period_elapsed()`
   (`clarett_pcm_tick()`, under `pcm_lock`, a mutex);
3. at least every 1 ms: samples the live ring positions (`+0x20`) and copies capture up to the live
   position (`clarett_pcm_sample_live()`, 0.4.2's sub-period `.pointer`);
4. every 150 us: copies what the app wrote since (`clarett_pcm_tx_refill()`; `.ack` only sets
   `tx_dirty`, because it is atomic and the copy needs the mutex);
5. telemetry: the 2 s `stream-svc:` window, the run summary, the zero-period warning.

## Target design

```
any vector -> clarett_irq()            hard IRQ, the ONLY reader of 0x100/0x300/0x200/0x400/0x500
                 sweep (vendor order) under sweep_lock (raw, irqsave)
                 0x300: advance = modular counter delta -> pending frames (atomic), latch ctr,
                        timestamp; overrun/badread counts
                 0x400: phase bits -> mailbox completions; event bits -> relay      (as today)
                 0x500 / MIDI: drain                                                (as today)
                 0x100: stop class, bit 4 underrun, unknown-bit report
              -> IRQ_WAKE_THREAD if a period advanced

threaded handler (request_threaded_irq, SCHED_FIFO by the core)
                 consume pending frames -> pcm_frames, capture copy, playback fill,
                 period_elapsed                       (today's clarett_pcm_tick(), unchanged)

.ack (app wrote playback)  -> incremental fill of the newly written frames only
backstop hrtimer           -> if no event in N periods, run the same sweep (same lock)
```

Positions: `.pointer` reads the live ring position itself (`+0x20`), computed against the counter the
sweep latched (a seqcount snapshot of `{stream_ctr, pcm_frames}`). That replaces job 3's 1 ms sampling.
Capture's sub-period position also needs the frames COPIED by then; see the decision in step 4.

## Steps

Each step is a separate, revertable commit that passes the regression set before the next starts.
Regression set (all models available, the Red and one Clarett minimum): `irq_rate.sh`, `phase_check.sh`,
`loopback_order.sh`, `live_cap.sh`, `SHORT=1 duplex_stress.sh`, `tx_guard_lead.sh`,
`DIGITAL=1 rt_latency.sh`, `start_loss`, `lead_ab.sh` against the previous step's build, MIDI self-loop
on a Clarett, `sleep_test.sh` s2idle once. Plus checkpatch `--strict`.

### Step 0 — experiments (scratch builds, never committed)

Settle the register behaviour the design leans on. Each is a small change to a copy of the tree.

- **E1. Does reading `0x100` re-arm the MSI?** The step 4a trap (reading `0x400` before `0x100` made every
  command time out) suggests the device raises the next MSI only once `0x100` is read. If so, while idle
  today the next period's interrupt waits for the servicer's `0x100` read. Build: the ISR sweeps all five
  registers unconditionally; servicer reads none. Measure `irq_rate.sh` (MSIs per event), `phase_check.sh`
  (every command `0x3`, none lost), and a variant that skips the `0x100` read to see what stops.
- **E2. Read every block, or only those whose summary bit is set?** 5 of 45910 vendor sweeps had `0x300`
  set with `0x100` bit31 clear (an event landing between the reads). Count, in E1's build, events found in
  `0x300` with the summary bit clear, and check whether a following MSI always comes. Expected answer:
  read all blocks unconditionally, like the vendor.
- **E3. Arm: is there an MSI before the first ack?** An armed engine waits for the first `0x300` read.
  Log whether an MSI arrives between `0x110 = 7` and that read. If not, the arm calls the sweep once.
- **E4. MIDI bit28.** Seen in one capture (70/70). In E1's build, count MIDI words received on sweeps with
  bit28 clear during a self-loop. Zero over a 25 KB loop -> the drain can be conditional; otherwise keep
  draining every interrupt (cheap: one read of an empty FIFO).
- **E5. Ack latency.** With the ack in the ISR, re-run `cause_probe.sh` (16-frame period, 48/96/192 kHz):
  device-flagged overruns (`0x300` bit30) against today's 0-1 / 1 / 3 / 6 / 16. A drop is the first
  measurable gain of step 1.

- **E6. Position from interrupt timestamps** (operator's proposal, Nov 20 2026): the sweep stores
  `{frames at this event, ktime}`; `.pointer` = frames + elapsed x rate, clamped below the next event, with no
  `+0x20` read. Measure, in a scratch build, the estimate against the live `+0x20` position at every
  servicer sample (distribution of the difference, per rate and period), including the first events after a
  rate change (the device keeps the old rate ~20-30 ms). Decides whether step 4 needs the ring-position
  register at all.

Exit: a written answer to E1-E6 appended here, with the numbers.

### Results

**E1-E3, Nov 20 2026, Red 8Line, laptop** (scratch build srcversion `733285811205505BE6F9542`, a runtime
`e1_mode`; 10 s duplex 60/64 ch at 48 kHz, periods 512 and 64, fcp-server stopped so no command was in
flight):

| mode | ISR reads | period 512: vec0 / periods | period 64: vec0 / periods | clients |
|---|---|---|---|---|
| 0 (shipped) | `0x400`; servicer `0x100`/`0x300`/`0x200`/`0x500` | 950 / 939 | 7515 / 7503 | rc 0 |
| 1 | `0x100`, `0x300`, `0x200`, `0x400`, `0x500` every MSI; the ack in the ISR | 950 / 939 | 7513 / 7502 | rc 0 |
| 2 | as 1 but **`0x100` never read** | 950 / 939 | 7513 / 7502 | rc 0 |

(The ~11 extra vec0 interrupts per leg are the Red's idle events, e.g. the 4.99 s temperature event.)

- **E1: NO. Reading `0x100` does not re-arm the MSI.** With nothing reading it for 10 s, the device still
  raised one interrupt per period event (942 MSIs for 939 events; 7504 for 7502) and the stream ran clean.
  So the step 4a trap (reading `0x400` before `0x100` timed commands out) is not an interrupt re-arm: it is
  the old polled DONE cycle's own dependence on bit29, which the phase-bit mailbox no longer has.
- **E2: no event found with the summary bit clear** in 8441 events with `0x300` read straight after
  `0x100` in the ISR (`nosum=0`). `0x100` read `0xa0000000` at most (bit31 period, bit29 `0x400`); bit30
  (`0x200`) and bit28 (`0x500`) never set.
- **E3: the engine raises an MSI for its first event without any ack first.** At period 64 (1.33 ms) the
  first event came by interrupt 1.26-1.30 ms after the servicer started. At period 512 the 5 ms wait was
  shorter than one period (10.7 ms), so the sweep was kicked: inconclusive there, not contrary. So the arm
  needs no kick (re-check at a long period with a wait of two periods).
- With the ack in the ISR, `gapmax` and `overrun` were the same as shipped (0 overruns everywhere at these
  periods); E5 (the 16-frame floor) is still to run.


### Step 1 — one sweep, one owner

- New `clarett_irq_sweep()` (clarett_main.c or a new clarett_irq.c): the vendor-order sweep, the only
  reader of the five cause registers, under a raw spinlock (`sweep_lock`, irqsave) so the backstop
  (step 5) can call it too.
- The `0x300` handling moves from `clarett_svc_event()` into the sweep: badread rejection, overrun count,
  modular delta (`CLARETT_CTR_MOD`), `stream_ctr`/`stream_ctr_step`, a timestamp per event. The sweep
  publishes the advance as `atomic_t svc_pending_units` and wakes the servicer.
- The servicer reads NO register: it takes `svc_pending_units` and runs `clarett_pcm_tick()` as today.
  Jobs 3-5 unchanged for now (the 1 ms live sampling keeps reading `+0x20`, which is not a cause register).
- Delete: the `cmd_inflight` ownership switch in both readers, the vec1/vec2 branch, the `0x100`-first
  special case (the sweep always reads it first).
- `stream_run` still gates acking: the sweep reads `0x300` only while it is set, else the armed engine
  would run before `stream_run` (today's guarantee). Per E3, the arm runs one sweep.
- The `readmax` telemetry becomes the sweep's own duration; `gapmax` the event-to-event time from the
  sweep's timestamps.
- Tests: the regression set; `cause_probe.sh` for E5's number; mailbox A/B (`mbox_ab.sh`) for no
  latency cost on commands; MIDI loop during a duplex stream.

### Step 2 — decide the context the PCM callbacks run in

The refill (job 4) needs `.ack` to copy, and the copy is under the `pcm_lock` mutex. Two ways:

- **A. `pcm->nonatomic = true`.** ALSA then runs `trigger`, `pointer` and `ack` in process context under a
  mutex stream lock, so `.ack` can take `pcm_lock` and copy directly, `trigger` can take `pcm_lock` (the
  `cap_rebase` hand-off from the START anchor becomes a plain locked update), and `.pointer` may read the
  device. Constraint: `snd_pcm_period_elapsed()` must then be called from sleepable context — the
  threaded handler is. Used in-tree (ASoC SOF and others).
- **B. Keep atomic callbacks; `pcm_lock` becomes a spinlock.** Every holder must be non-sleeping (attach,
  detach, hw_free, the TX-ring clear at close, the copies). Works, but long copies then run with the
  lock held and, from `.ack`, with interrupts off.

Recommendation: **A**. It removes a class of atomic-context workarounds rather than adding one, and the
callbacks' only costs (mutex instead of spinlock in the stream lock) are what a threaded design implies
anyway. Open check: alsa-lib / PipeWire paths that call `snd_pcm_avail()` or `delay` from contexts that
assume a cheap pointer — measure with `live_cap` (250 us polling) and PipeWire playback.

### Step 3 — incremental playback fill from `.ack`

Today `clarett_play_fill()` rewrites the whole runway (ring - guard: up to 2048 frames x 64 ch x 4 B =
512 KB on the Red) every time. Called on every app write (leadplay writes 16 frames at a time) that is
untenable, so the fill becomes incremental:

- track `tx_data_to` (engine frame up to which the ring holds app data) and `tx_silence_to` (up to which it
  holds silence written for this lap);
- on `.ack`: copy only `[max(tx_data_to, play_pos + guard), appl)`, then extend silence to a ring ahead
  only if `tx_silence_to` has fallen behind;
- on a period event: advance the silence window (stale-lap protection, the reason the fill writes
  silence past `appl_ptr` at all);
- START, STOP (`tx_silence`), the prime and xrun recovery reset the marks.
- Remove `tx_dirty` and `clarett_pcm_tx_refill()`; the 150 us wait goes.
- Tests: `tx_guard_lead.sh` and `lead_ab.sh` are the decisive ones (the 0.4.0 cadence regression was
  found only by leadplay); servicer CPU (`svc_cpu_ab.sh`) should FALL, since the full-runway copy goes.

### Step 4 — positions without sampling

- Playback: `.pointer` computes the live position itself from `+0x20` and the sweep's latched counter
  (seqcount snapshot). Drop the servicer's playback sampling.
- Capture: `.pointer` can only report frames already copied into the ALSA buffer. Decide between:
  (a) `.pointer` (nonatomic, step 2A) copies up to the live position itself before reporting;
  (b) report at event granularity again plus the copy at each event (gives up 0.4.2's sub-period capture
  position); (c) zero-copy capture is not possible (reorder, S/MUX blanking, padded slots).
  Recommendation: (a), under `pcm_lock`; `live_cap.sh` decides.
- After this, job 3 is gone.

### Step 5 — the backstop and the end of the kthread

- Replace the 1 ms fallback with an hrtimer armed per stream at a few periods' spacing; on expiry with no
  event since, it runs `clarett_irq_sweep()` (same lock) and wakes the thread. Count its firings in the
  telemetry: on a healthy host it should never find an event the interrupt missed.
- `request_threaded_irq()` for vec0 with the tick as the thread function; the dedicated kthread,
  `clarett_engine_run()`'s `kthread_run`, the `stream_svc` handle and the orphan/`kthread_stop` guards in
  `clarett_engine_stop()` go (synchronize_irq + the stream_run gate replace them). Re-read the duplex
  arm/close race fixes (`clarett-duplex-arm-race`) against the new teardown before deleting them.
- Telemetry: the window line moves to a timer or to the thread on events; the zero-period warning keys on
  the sweep's event count.
- Suspend/resume and surprise removal: `hw_gone` and the teardown order (`clarett_hw_teardown()`) re-checked
  with the threaded handler (a removal while the thread runs a copy).

### Step 6 — release

Full matrix on all five models (the 0.4.x set), the ASRock included as a test host (not a gate on the
design), then a version bump. Records: data-plane §20 for the result, CLAUDE.md entry.

**DONE Nov 23 2026: the full matrix passes on all five models** (laptop, srcversion
`452D1083A5C5B0C80475B11` = snd-clarett `1c7b060`, the loaded module checked against the tree build before
each run; `~/clarett-bench/matrix_run.sh`, logs kept off-repo). The Red 8Line and 8PreX ran together (the
8PreX chained behind the Red), then the 8Pre behind the Red, then the 4Pre and the 2Pre alone. Per card:
digital round trip (`DIGITAL=1 rt_latency.sh`), `live_cap.sh`, `loopback_order.sh`, `SHORT=1
duplex_stress.sh`, `tx_guard_lead.sh`, `irq_rate.sh`, and on the Claretts with a cable loop the MIDI
self-loop (`midi_loop.py`: 6000 B SysEx unpaced and paced at 1560 B/s, 1500 note pairs).

| test | Red 8Line | 8PreX | 8Pre | 4Pre | 2Pre |
|---|---|---|---|---|---|
| digital round trip, 6 legs (48/96/192 kHz, 2 periods each) | 32 frames, 0 xruns | 32, 0 | 32, 0 | 32, 0 | 32, 0 |
| `live_cap` (reads down to 16 frames) | 6/7 (see below) | 6/6 | 6/6 | 6/6 | 6/6 |
| `loopback_order`, dead channels zero | 8/8 | 9/9 | 9/9 | 9/9 | 9/9 |
| duplex stress (SHORT): failing parts / churn | 0 / 10 of 10 | 0 / 10 | 0 / 10 | 0 / 10 | 0 / 10 |
| TX guard lead: playback breaks | 0 of 12 | 0 of 12 | 0 of 12 | 0 of 12 | 0 of 32 (see below) |
| leadplay period 32 gaps per rep | 1/0/0 | 1/0/0 | 0/1/1 | 0/0/0 | 0/0/1, then 2/0/2/1/0 |
| vec0 interrupts per period event, 512 / 64 frames | 1.010 / 1.001 | 1.009 / 1.001 | 1.010 / 1.001 | 1.010 / 1.001 | 1.009 / 1.001 |
| vec1-3 | 0 | 0 | 0 | 0 | 0 |
| MIDI self-loop, 3 runs | (no MIDI) | not cabled | byte-exact | byte-exact | byte-exact |

Every leadplay gap matches one of the client's own underruns; at period 64 leadplay underruns every period
(its 48-frame lead is shorter than the period), as on every earlier build. **Every stream on every model:
`backstop=0 overrun=0 underrun=0 badreads=0`, `cause=0xa0000000`**, and no WARN/BUG/Oops, no `raised no
period`, no `stream-cause:` in the kernel log. Known benign signatures seen: first-window `late=1-4` in churn
streams after a change to a faster rate (the old-rate tail), and one arecord overrun in the duplex stress's
64-frame leg (non-RT clients, informational) on each Clarett.

The three results off the clean line, each traced to a client:
- **Red `live_cap` at 192 kHz period 128: one break**, a forward jump of `0x28100` (641 ramp steps, i.e. 640
  frames lost = buffer 512 + period 128, the size of an overrun recovery); the test client (`livecap`) reported 2
  xruns on that leg and the driver's telemetry for the stream was clean.
- **2Pre `tx_guard_lead`, aplay at period 32: one break** (the ramp 160 frames short, no gap, no playback
  underrun) in rep 3 of 3. `tx_guard_lead.sh` did not report arecord's own errors, so it now prints
  `capture overruns`; re-run with 5 reps, it happened once more (rep 3 again) with **`capture overruns 1`**
  on that run and on no other. 160 frames is buffer + period (128 + 32), what arecord drops recovering from an
  overrun. The kernel logged nothing for either run.
- The 2Pre's unmatched leadplay gap from 0.4.0 (one gap with no client underrun) did not recur: all 8 reps
  match.

**Not run, and not a gate (design rule above):** the ASRock as a test host, and `start_stall_ab.sh` against
0.4.2. The start-loss figure (one 16-frame loss at 192 kHz period 64 on the Red, step 5) was not re-run.
Still open from step 5: the slower period events for ~10 s after a wake on the Red.

## Open questions carried

- Whether the 42 ms block on the ASRock changes once the ack is in the ISR (operator's expectation: it
  goes). Measure with `start_stall_ab.sh` against 0.4.2 after step 1 and again after step 5.
- ~~MIDI TX still polls `0x500` bit16 (`read_poll_timeout`, 200 us). Whether a TX-ready interrupt exists is
  unknown.~~ **ANSWERED Nov 23 2026 (2Pre, scratch build): there is none.** With the cable unplugged, a full
  TX FIFO draining raised zero interrupts at every `0x500` value and at `0x510` = 2, 8 and 14, while the ready
  flag returned on its threshold (seen by polling). The poll stays. Detail: `clarett-midi-plan.md`, final
  addendum.
- Zero-copy playback (2Pre/4Pre/Red: fragment already a power of two) would remove the fill for those
  models entirely; not planned, revisit after step 3 measures what the incremental fill costs.

**E6, Nov 20 2026, Red 8Line, laptop** (scratch srcversion `A580FB9156AA832668D44E9`, mode 1; 8 s duplex
legs, rates rotated so most legs start after a rate change). At every servicer sample of the live ring
position (`+0x20`, which is quantised DOWN to 16 frames: the true position is in [live, live + 16)), the
estimate = frames at the last event + time since its ISR timestamp x rate. d = estimate - live, so 0..15 is
exact, negative = the estimate lags, 16+ = it runs ahead.

| leg | samples per direction | d, nominal rate (min / mean / max) | d, measured interval rate (min / max) | first 50 ms (min / max) |
|---|---|---|---|---|
| 48k / 512 | 7463 | -6 / 4-5 / 15 | -9 / 18 | -4 / 13 |
| 48k / 64 | 11930 | -1 / 3 / 12 | -1 / 12 | 1 / 9 |
| 96k / 128 (after 48k) | 11984 | -5 / 6-8 / 16 | -5 / 16 | -1 / **141** |
| 96k / 512 | 7463 | -19 / 2-3 / 16 | -22 / 20 | -3 / 14 |
| 192k / 256 (after 96k) | 11981 | -8 / 1-6 / 15 | -9 / 16 | -5 / **232** |
| 192k / 64 | 23855 | **-46** / 0-2 / 16 | -54 / **79** | -5 / 13 |
| 48k / 1024 (after 192k) | 7406 | -6 / 4-5 / 15 | -8 / **567** | **-647** / 14 |
| 192k / 1024 (after 48k) | 7609 | -30 / -4-0 / 14 | -49 / 29 | -5 / **1976** |
| 48k / 256 (after 192k) | 7451 | -9 / 5 / 15 | -10 / 16 | **-169** / 15 |

- **Steady state, nominal rate: never more than one frame ahead** (d = 16 at most, 1 sample in a leg, 3 in
  all), and lagging by at most ~10 frames at 48 kHz, ~20 at 96 and 30-46 at 192 (~0.24 ms: the
  interrupt-to-timestamp latency, which only ever makes the estimate late).
- **The measured interval rate is WORSE** (outliers to 79 and 567 frames ahead): one late or coalesced event
  skews the interval. Use the nominal rate.
- **After a rate change the estimate is wrong by up to ~2000 frames for the first ~50 ms**: AHEAD after a
  change to a faster rate (141, 232, 1976), behind after a slower one (-169, -647). That is the old-rate
  tail (the device keeps the previous rate ~20-30 ms after SET_CLOCK; see the duplex-stress side finding).
  Ahead is the unsafe direction for capture (frames reported that are not yet written).
- The estimate passed the next event 4 times in ~170k samples (the clamp's case).
- E2 again: 1 event in 6002 (192k / 256) found in `0x300` with the summary bit clear.

**Reading:** after the start-up tail, the timestamp estimate tracks the ring position as closely as the
register itself resolves it, and errs only late. A `.pointer` built on it needs (1) the clamp below the next
event, (2) a margin of one fragment (16 frames) for capture so it never reports past written data, and
(3) something for the first events after a start: hold at the last event (period-step position) until two
events at the nominal spacing have confirmed the rate, or read `+0x20` until then. Playback's fill counts
from the position + a 16-frame guard, against a lag of up to 46 frames at 192 kHz: either a larger guard
when the estimate is used, or keep the fill on `+0x20` (one read per fill, not per `.pointer`).

**E5, Nov 20 2026, Red 8Line, laptop** (scratch srcversion `50F6D9147B27BF3E9E423C4`): a 16-frame period, a
128-frame buffer, 10 s legs, capture-only and duplex at 48/96/192 kHz, each leg run in mode 0 then mode 1,
the whole set twice. Device-flagged period overruns (`0x300` bit30), summed:

| leg | mode 0 (servicer acks) | mode 1 (ISR acks) |
|---|---|---|
| capture 48 / 96 / 192 kHz | 2 / 0 / 15 | 0 / 0 / **1** |
| duplex 48 / 96 / 192 kHz | 0 / 0 / **276** | 0 / 0 / **0** |

No client xruns and no late ticks in either mode. **Acking in the ISR all but removes the device's overruns
at the floor** (291 to 1). Counting note: in mode 1 the servicer's `periods=` read 113-118k of 120k at
duplex 192 kHz, because it took several ISR-collected events in one pass (the counter delta carries their
frames); the ISR counted every event (120,023 / 120,004). Step 1 must count events in the ISR.

**E4, Nov 20 2026, Clarett 8PreX, laptop** (scratch srcversion `BABCE1066F070E380D1618A`, mode 1 with the FIFO
drained AFTER the sweep, the vendor's order; a 5-pin cable MIDI Out -> In):

| loop (`midi_loop.py`) | drains with data, bit28 set | with data, bit28 CLEAR | bit28 set, FIFO empty | result |
|---|---|---|---|---|
| SysEx 6000 B unpaced | 5964 | 0 | 0 | byte-exact |
| SysEx 6000 B at 1560 B/s | 5977 | 0 | 0 | byte-exact |
| 1500 note pairs | 8943 | 0 | 0 | byte-exact |
| SysEx 25000 B during a 28ch capture | 24198 | **644** | 0 | byte-exact |

- **Every MIDI interrupt had bit28 set**, and bit28 never came with an empty FIFO.
- The 644 are drains done on PERIOD interrupts (bit31) that found a byte already in the FIFO before the
  device had raised bit28 for it; drained and acked, the byte raised nothing later (no "set, empty").
- **The vendor gates the drain on bit28:** in `2pre_midi_rx.log` it reads `0x58c` only on its 70 sweeps with
  bit28 set (each a data read then an empty one), and in `2pre_stream.log` 7841 sweeps without bit28 read
  `0x58c` zero times. Gating therefore only delays a byte until its own interrupt.
- So step 1 can drain on bit28 alone, as the vendor does. Its test: the same four loops byte-exact with the
  drain gated (the 25 KB loop during a capture is the one that matters).

**Step 0 is complete.** Answers for step 1: one owner reading `0x100` first is safe (E1); read the flagged
blocks only, accepting the rare late event (E2: 2 in ~0.9 M events across E1, E5 and E6); no kick at arm (E3, at a short period); count
events and overruns in the ISR, and expect the overruns to drop (E5); drain MIDI on bit28 (E4); the position
from the event timestamp is a step 4 option with the start-up caveat (E6).

**Step 1 built and tested, Nov 20 2026** (snd-clarett working tree, srcversion `B871A2A4F8850EC7E05921B`; laptop,
Red 8Line + Clarett 8PreX). `clarett_irq_sweep()` (clarett_main.c) under `irq_lock` is the only reader of
`0x100`/`0x300`/`0x200`/`0x400`/`0x500`/the MIDI FIFO, from every vector: `0x100` first, then the flagged
blocks; `full` (the servicer's 1 ms backstop and probe's cause clear) also reads `0x300`/`0x200` unflagged.
`clarett_engine_irq_period()` does the `0x300` work in the sweep (ack, modular step, overrun, wraps, timing,
period count) and hands units to the servicer (`irq_pending`); the servicer takes units + counter together
(`clarett_svc_take()`), ticks, and copies the sweep's stats into its window (`clarett_svc_collect()`).

| test | Red 8Line | 8PreX |
|---|---|---|
| `irq_rate` (per period event, periods 512 / 64) | 1.007 / 1.001, vec1-3 zero | 1.010 / 1.001, vec1-3 zero |
| `cause_probe`, 16-frame floor, six 10 s legs | 2 overruns | 3 overruns |
| `phase_check` (GUI Levels open) | — | 60 GET_METER all `0x3`, 0 never-landed, 0 errors; knob 604 events relayed mid-capture, hands-off none |
| duplex stress (SHORT) | 0 failing, churn 10/10 | 0 failing, churn 10/10 |
| `loopback_order` | 8/8 | 9/9 |
| digital round trip | 32 frames | 32 frames, all legs |
| TX guard lead | breaks 0 | breaks 0 |
| start loss | 0/60 | — |
| MIDI self-loop (drain on bit28 only) | — | byte-exact x3 |
| short-lead A/B vs the previous build, 10 x 20 s each | gaps 17 vs 17, underruns 8 vs 8, breaks 0, lost 272 vs 240 of 9.6 M | — |

**Phase 1 ran both units' sets at once,** and `live_cap` failed 3 legs on the Red and 1 on the 8PreX, every
one with client xruns (up to 362, with aplay underrunning too): the non-RT test clients starved. In the same
phase the round-trip legs at 48 kHz / 64 showed 12-13 doubled steps (`stepmax` two periods, gap 2.7 ms, no
overrun) per window on BOTH cards. **Not reproduced in a controlled dual-unit A/B** (`dual_ab.sh`: plain
duplex 48 kHz / 64, 15 s, each unit alone and both at once, blocks A B A B): `late=0`, `stepmax` one period,
`overrun=0`, no client xruns, on both builds in all 16 legs. Attributed to the whole-set load of phase 1 (two
`rtlat` clients plus the other units' tests), not to step 1; what the device or host did there is open.
`rt_latency.sh`'s default legs included 192 kHz / 1024 (a 4096-frame buffer) which the Red's 2048-frame ring
refuses: the Red now has its own default legs.


**Steps 2 and 3 built and tested, Nov 20 2026** (srcversion `AE346231AC0C132BCDA92C5`; laptop, Red 8Line +
Clarett 8PreX chained behind it). **Step 2 took option A:** `pcm->nonatomic = true`; trigger, `.pointer` and
`.ack` run under the stream mutex, `snd_pcm_period_elapsed()` only from the servicer thread; lock order
stream mutex -> `pcm_lock`, and the servicer calls into ALSA without `pcm_lock`. **Step 3:**
`clarett_play_fill()` keeps `tx_data_to` / `tx_silence_to` under `pcm_lock`; each call copies only the new
frames `[max(tx_data_to, pos + guard, play_base), min(appl, pos + ring))` and extends silence to a ring past
the read position; a rewind (appl behind `tx_data_to`) pulls both marks back to appl. It runs from `.ack`,
START (marks reset, the prefilled buffer copied at once), STOP (ring silenced) and every period tick.
`tx_dirty`, `clarett_pcm_tx_refill()` and `CLARETT_SVC_REFILL_US` are gone: the servicer wakes on period
events or the 1 ms backstop only. Capture START unchanged (`cap_rebase`).

| test | Red 8Line | 8PreX |
|---|---|---|
| digital round trip | 32 frames, all 6 legs | — |
| `live_cap` | 6/7; 192 kHz / 128 broke once with 2 livecap xruns (client) | all legs pass (rerun) |
| `loopback_order` | 8/8 | pass |
| duplex stress (SHORT) | 0 failing | 0 failing (rerun) |
| TX guard lead, 12 runs | 2 breaks (aplay p32, leadplay p32), one with no playback underrun | breaks 0 |
| start loss | 0/60 | — |
| short-lead A/B vs step 1, 10 x 20 s each | **gaps 81 -> 15, breaks 5 -> 1, lost 1776 -> 160 of 9.6 M**; every step-3 gap matches a leadplay underrun | — |
| CPU, 64ch duplex 20 s, blocks A B A B (step 1 vs 3) | servicer at period 64 **11.5 / 13.1 % -> 4.7 / 3.9 %**, at 512 4.9 / 4.6 % -> 4.1 / 4.0 %; aplay +0.5 % | — |

The A/B is the decisive one: the fill now runs on the client's write instead of a 150 us timer, and a
short-lead client loses less than under the timer. The breaks without a playback underrun are unattributed:
`tx_guard_lead.sh` does not report the capture client's overruns, and step 1's own A/B had 5 such breaks, so
not counted against step 3. The PipeWire listening check (rewinds when a second stream joins) is owed.

**The 8PreX's stream engine wedged during the first run** (16:09:40, unattended): `live_cap`'s 192 kHz /
256 leg right after a clean 192 kHz / 1024 leg, a same-rate re-arm with only the period changed. Every arm
after it was accepted and raised no period (0 MSIs, counter 0, ring pointers 0/0) while the servicer ran
its 1 ms backstop normally; `livecap` polls `snd_pcm_avail()` and so hung instead of failing. The Red, idle
at the time and never power-cycled, streamed normally after. The 8PreX's bus reset (its only
`reset_method`) with the driver bound was inconclusive: it clears the attach-time registers (`0x104`, the
mailbox address) and the driver has no `reset_done`, so the failing capture after it proves nothing. A
power cycle cleared it, and the rerun with the engine's debug lines on armed ~49 times with identical
register writes and no repeat, the failing leg included. A screen lock during 3 min captures on both units
(monitor on the dock) broke nothing. This is the known device-side wedge (CLAUDE.md), first seen off the
ASRock and without churn; cause open. Next time: reset, unbind + bind, then a capture.

**PipeWire listening check on the steps 1-3 build: PASSED, Nov 20 2026** (Red 8Line, snd-clarett `baf9a0f`,
operator by ear: "PipeWire sounds great").

**Step 4 built and tested, Nov 20-23 2026** (snd-clarett `afc26d3`, srcversion `67276C2D48BF4371D1CEE29`;
laptop, Red 8Line + Clarett 8PreX chained behind it). **Capture took option (a):** `.pointer` copies the RX
ring up to the live position under `pcm_lock` (`clarett_cap_copy_to()`), capture START sets `pcm_base` and
`cap_drained` together under the lock (`cap_rebase` gone). Playback: `.pointer` and the fill read `+0x20`
themselves (`clarett_play_pos()`); the tick sets `pcm_frames` and `stream_ctr` together under `pcm_lock`;
`clarett_pcm_sample_live()` is deleted, so job 3 is gone.

**The first build (`00EA5CD26355B1C7A00F8EE`) regressed the short-lead client**, the only test to see it:
`lead_ab.sh` step 3 -> step 4, gaps 9 -> 167, lost 0 -> 464 of 9.6 M, breaks 0. Everything else passed.
Three scratch builds (never committed, `~/clarett-bench/step4x/`) separated it, 6 x 20 s per cell, leadplay
period 32/128 with a 48-frame lead:

| who reads `+0x20` (scratch `pos_reads`) | gaps | gaps without a client underrun | lost |
|---|---|---|---|
| `.pointer` and the fill (= the first build) | 170 | 30 | 480 |
| neither; the servicer samples every 1 ms (= step 3) | 8 | 0 | 32 |
| `.pointer` only | 124 | 0 (124 client xruns) | 0 |
| the fill only | 121 | 110 | 1776 |

So both readers lose with a position that is current, and a position up to 1 ms old had hidden it. Then,
with only the fill reading, letting it write N frames behind `pos + guard` (guard 16): N = 0 gave 123 gaps /
1888 lost, **N = 16 (= writing up to `+0x20`) 7 / 0**, 32 9 / 0, 48 8 / 16. With the fill at N = 16 and
`.pointer` reading too, reporting N frames behind `+0x20`: N = 0 115 gaps (all client xruns), **N = 16
4 / 16 lost**, 32 3, 48 4. **Reading: `+0x20` counts the 16-frame fragment the engine has FETCHED; the frames
playing start one fragment behind it.** Reported as the position it runs one fragment ahead, so a client's
lead shrinks by 16 frames, and the fill's guard measured from it skips frames the engine has not fetched.
**Fix:** `clarett_play_pos()` takes the position one fragment (`CLARETT_CTR_FRAMES`) back; the guard stays
16. No scratch value past one fragment improved anything, and 32 behind lost 16 frames once, so there is no
further slack.

| test (fixed build) | Red 8Line | 8PreX |
|---|---|---|
| digital round trip | 32 frames, all 6 legs | — |
| `live_cap` | 7/7 (single client xruns on some legs) | 6/6 |
| `loopback_order` | 8/8 | 9/9, dead channels zero |
| duplex stress (SHORT) | 0 failing, churn 10/10 (first build: 1, a 48 kHz arecord overrun) | 0 failing, churn 10/10 |
| TX guard lead, 12 runs | breaks 0; leadplay p32 gaps 5/1/1 (first build 23/26/17) | breaks 0; leadplay p32 gaps 0/0/0 |
| start loss | 0/60 | — |
| short-lead A/B vs step 3, 10 x 20 s each, run twice | **gaps 10 -> 3 and 8 -> 3, lost 16 -> 0 and 0 -> 0**, every gap a client underrun | — |
| CPU, 64ch duplex 20 s, A B A B (step 3 vs 4) | period 64: servicer ~5.1 -> 4.7 %, aplay ~3.2 -> 3.9 %, arecord ~2.6 -> 2.9 % (the reads move into the clients' `.pointer`; total ~ +1 point); period 512: servicer ~4.1 -> 3.2 %, clients ~unchanged | — |

No engine wedge on the 8PreX in these runs.

**Step 5 built and tested, Nov 23 2026** (snd-clarett working tree, uncommitted at the time of writing,
srcversion `452D1083A5C5B0C80475B11`; laptop, Red 8Line + Clarett 8PreX chained behind it). **The kthread is
gone.** vec0 is `request_threaded_irq()` (not `IRQF_ONESHOT`, since vec0 also completes mailbox commands);
the hardirq runs the sweep and returns `IRQ_WAKE_THREAD` when a period was latched (`svc_wake`), and vec1-3
or any other context kick the thread with `irq_wake_thread()` (`clarett_svc_kick()`). The thread
(`clarett_svc_thread()`) runs `clarett_pcm_tick()` and the 2 s telemetry window. **The 1 ms fallback is an
hrtimer backstop** at 3 nominal periods (at least 1 ms): it sweeps only if no event arrived since its last
expiry, counts `backstop=` in the telemetry, kicks the thread when a window is due, and stops re-arming on a
disconnected device. `clarett_engine_stop()` claims the teardown under `pcm_lock` (`stream_run` false),
then `hrtimer_cancel()` + `synchronize_irq(vec0)` outside it; `clarett_engine_run()` sets `stream_run`,
sweeps once, kicks and starts the timer. The duplex arm/close guards' job (one caller arms, one tears down)
is kept by the `pcm_lock` claims; there is no thread handle left to orphan or stop twice.

| test (`step5_run.sh`) | Red 8Line | 8PreX |
|---|---|---|
| digital round trip | 32 frames, all 6 legs | — |
| `live_cap` | 7/7 | 6/6 |
| `loopback_order` | 8/8, dead channels zero | 9/9, dead channels zero |
| duplex stress (SHORT) | 0 failing, churn 10/10 | 0 failing, churn 10/10 |
| TX guard lead, 12 runs | breaks 0; leadplay p32 gaps 0/1/0, each a client underrun | breaks 0; leadplay p32 gaps 1/1/0, each a client underrun |
| start loss | 59/60; one 16-frame (one fragment) loss at 192 kHz period 64 / buffer 128, no playback underrun (step 4: 0/60). Not re-run | — |
| short-lead A/B vs step 4, 10 x 20 s each | gaps 7 -> 4, lost 0 -> 0, breaks 0, every gap a client underrun, no stall windows | — |
| CPU, 64ch duplex 20 s, A B A B (step 4 vs 5) | servicer at period 64 4.55 / 2.95 % -> 1.55 / 1.90 %, at 512 3.10 / 3.20 % -> 0.90 / 1.10 %; aplay and arecord unchanged | — |
| `backstop=` > 0, WARN/BUG | none in the whole run | none |

**The CPU figure is partly accounting:** the step 4 kthread was charged for the sweep, the IRQ thread is
not (the sweep runs in the hardirq, which no task is charged for). So the servicer column shows the thread
got lighter, not by how much the driver's total cost fell. **The backstop never found an event the interrupt
had missed** (`backstop=0` in every window of every stream).

**Operator checks (Red 8Line, step 5 loaded):**
- **Surprise removal:** the unit switched off during a 60ch capture. The stream stopped at once (ring
  pointers read all-ones), no task in D state, no WARN/BUG/Oops; the unit re-attached when powered on.
- **System sleep with a 60ch capture held across it, s2idle and deep:** the engine stops at suspend, the
  stream re-prepares after the wake and runs on with `late=0`, `badreads=0`, `backstop=0`. On the s2idle
  run `sleep_test.sh` read RUNNING / Locked / fcp-server active before and after, meter read OK, the device
  resumed in place. (The first deep run was pointed at the wrong card number by mistake, so the script's
  card checks there are void; the same checks by hand after it passed.)

**Observation, open, not attributed to step 5: about 8 s after every wake the Red raised period events
~8 % slower for ~10 s** (174-183 per 2 s window at a 512-frame period instead of 188, the gap between
events 11.6-11.8 ms against 10.67 nominal, every step still exactly one period), then returned to normal.
Seen on 3 of 3 wakes (two s2idle, one deep). Not the screen unlock: it began 4 s before the unlock on the
third wake, and a lock/unlock with no sleep did not reproduce it. **With the mailbox trace on, no command
reached the device from the post-wake burst (fcp-server's re-read after the resume relay, ~0.6 s after the
wake) until the stream stopped**, apart from meter reads, so nothing on the host re-clocked it. The rate
fits 44.1 kHz (172 events per 2 s), but that is a fit, not a measurement. The servicing path cannot set the
device's clock, so this is read as device-side; whether the step 4 build shows the same was not checked.
