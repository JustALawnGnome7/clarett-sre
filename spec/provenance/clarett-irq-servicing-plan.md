# Servicing the stream from the interrupt — plan

Status: PLANNED (Nov 20 2026). Nothing built. Prerequisite: the capture-at-START anchor (round trip =
lead + 32 frames) is tested and committed first.

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

## Open questions carried

- Whether the 42 ms block on the ASRock changes once the ack is in the ISR (operator's expectation: it
  goes). Measure with `start_stall_ab.sh` against 0.4.2 after step 1 and again after step 5.
- MIDI TX still polls `0x500` bit16 (`read_poll_timeout`, 200 us). Whether a TX-ready interrupt exists is
  unknown; out of scope unless E-series work finds one.
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

