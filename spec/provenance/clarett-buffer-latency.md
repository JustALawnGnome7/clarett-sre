# ALSA buffer ceiling, `tx_guard`, and the digital-loopback ramp

Moved verbatim out of `CLAUDE.md` (Driver limitations / TODO) on Oct 8 2026 to keep that file under
the session size limit; `CLAUDE.md` keeps a summary and points here. Note that the `max_buffer`
default described below was later SUPERSEDED by the per-stream `CLARETT_MAX_PERIODS` rule (Sep 10
2026; see `CLAUDE.md`, "THE 128-FRAME PIN BROKE A REAL JUCE APP").

- **★★ THE ALSA BUFFER WAS PINNED TO THE 4096-FRAME RING — THE REAL LATENCY CEILING, AND THE PERIOD WAS
  NEVER THE POINT (Aug 27 2026, 8Pre; fixed and measured on hardware).** A DAW at a 16-frame
  period reported 1.75 ms round trip and sounded far worse; `/proc/asound/card4/pcm*/sub*/status` settled
  it in one look: `period_size: 16` (so the period request WAS honoured — PipeWire coercion and the
  cadence-1 overruns are both exonerated), `buffer_size: 4096`, and **playback `delay: 4000` frames = 83 ms**
  against capture's 112. `clarett_pcm_open()` was calling
  `snd_pcm_hw_constraint_minmax(..., BUFFER_BYTES, buf, buf)` — the same value twice, pinning the ALSA
  buffer to the hardware ring — and the DAW, which had asked for 3 periods of 16 = 48 frames, filled the
  4096 it was handed instead. **Diagnostic lesson: `delay` in `status` is the measured latency; every
  number the DAW displays is what it REQUESTED.**
  - **Fix, part 1:** buffer is now any power-of-two frame count from `CLARETT_MIN_BUFFER_FRAMES` (128) up
    to the ring. Pow2 is load-bearing — it makes the buffer divide the 4096-frame ring, so ALSA frame k and
    ring frame k wrap coherently and `clarett_rx_drain`/`clarett_tx_fill` (which already clipped at both
    boundaries) just need the buffer passed in alongside the ring.
  - **★★ PART 1 ALONE WAS INERT ON HARDWARE, AND THE REASON IS AN ALSA-LIB ASYMMETRY WORTH REMEMBERING:
    `snd_pcm_hw_params_choose()` resolves every parameter with `set_first` (the MINIMUM) except
    `BUFFER_SIZE`, which it resolves with `set_last` (the MAXIMUM).** So an app that pins only the period
    — most of them, including a DAW that displays a period count it never actually requests — is handed
    whatever ceiling the driver advertises, and making a small buffer *legal* changes nothing for it. The
    retest proved it: with the new module confirmed loaded (`/sys/module/snd_clarett/parameters/tx_guard`
    readable), `buffer_size` came back 4096 and `delay` 4048, unchanged.
    **Fix, part 2:** the `max_buffer` param (frames, rounded down to pow2) lowers
    `runtime->hw.buffer_bytes_max`. **`max_buffer=256` fixed it: on the same DAW session playback `delay`
    went 4048 -> 256 frames, 83.3 ms -> 5.33 ms.** **Default is now `CLARETT_MIN_BUFFER_FRAMES` = 128
    (Sep 2 2026, operator's call over a recommendation of 256).** **[SUPERSEDED Sep 10 2026: the default
    is 0 again and latency is bounded per stream by `CLARETT_MAX_PERIODS` — see the ★★★ bullet above.]**
    The old default of 0 (the ring) was gone;
    its stated rationale — that PipeWire needs 2048 at a 1024-frame quantum and would regress — was
    **disproven twice**, on the 2Pre and again on the 8Pre.
    **KNOW WHAT 128 IMPLIES: it is the FLOOR as well, so ceiling == floor and the buffer is PINNED.**
    Measured on the 8Pre: with `max_buffer=128`, requests for 128/256/1024 are all granted **128**,
    silently and with no error — the same shape as the original bug (an app handed a buffer it never
    asked for), relocated from 4096 to 128. Deliberate here, but it means a host whose scheduling stalls
    exceed 2.7 ms has no way up except changing the parameter. Any value above the floor restores a real
    range: at `max_buffer=256` a request for 128 gets 128 and 256 gets 256.
  - **Why the whole-runway TX fill SURVIVES a small buffer** (the part that looked like a redesign and
    wasn't): filling `ring - guard` frames from a buffer that divides the ring simply TILES it, and ring
    frame f still receives the buffer frame due to play when the engine reaches f, because both advance
    by the same delta. Underrun now repeats one small buffer instead of a whole 85 ms ring pass.
  - **★★ THE OLD `tx_guard` TEXT WAS WRONG, BUT SO WAS MY FIRST REPLACEMENT — READ THIS BEFORE TOUCHING
    THE GUARD (Sep 2 2026, 8Pre, digital loopback).** The parameter's default and clamp floor are
    UNCHANGED; only the documentation moved. What is established:
    - **It is not a latency term.** Reported `delay` was identical at guard 64/128/256 (512 frames,
      10.66 ms, buffer 512). It sets a write DEADLINE, not a queue.
    - **The old advice — "if skipping appears, turn `tx_guard` down first" — has no support** and is
      removed. So is the hazard it was premised on: a client pinning its lead at 48 frames against a
      64-frame guard was indistinguishable from one leading by 128, break counts tracking the client's
      OWN underruns (ur 0/1/1/2/2/6 -> breaks 0/1/2/8/10/27) and not the lead.
    - **With a NORMAL full-buffer client every guard from 16 to 96 is equally clean** — single-digit
      breaks at a 32- AND a 64-frame period alike. There is no cliff and no measured basis for changing
      the value in either direction.
    **THE UNRESOLVED PART, AND THE METHOD LESSON.** A synthetic client pinning a SHORT lead showed guard
    16 and 32 tearing catastrophically (38k-71k whole-buffer skip/repeat pairs per 25 s) against single
    digits at 64, reproducibly 3/3 — and I raised the clamp floor to 64 on the strength of it. That was
    **confounded**: those runs changed the client AND the period together, and the effect vanishes with
    an ordinary client at either period. The floor change was reverted. The synthetic client is also
    only viable at period 32 (at period 64 a 48-frame lead underruns 24999 times in 22 s), so its one
    working configuration is precisely the one that misbehaved — most likely an artifact of that client.
    **Lesson, the second time today: when a result comes from a rig you built for the occasion, vary ONE
    thing against a stock client before believing it.** (The first time was reading a single 10-vs-2 run
    as the lead hazard; repetitions killed it.)
  - **Also fixed in passing:** the capture drain clamped a servicer lag to the RING and copied the OLDEST
    frames of the burst; with a small buffer that is reachable in normal operation (the ~42 ms platform
    freeze advances the engine ~2000 frames), so it now skips forward and hands over the NEWEST.
  - **Known cost, not addressed:** `clarett_tx_fill` copies `ring - guard` ≈ 4032 frames EVERY tick
    regardless of period — ~322 KB per 333 µs at cadence 1 on a 20-channel 8Pre, near 1 GB/s of memcpy to
    deliver 16 frames. Pre-existing, now conspicuous. The runway only has to cover worst-case servicer
    lag, not the whole ring.
  - **★ PIPEWIRE ADAPTS TO THE LOWERED CEILING BY SHRINKING ITS PERIOD, NOT ITS QUANTUM — MEASURED
    PROPERLY AT LAST (Aug 31 2026, 2Pre, tone audible in BOTH legs).** The A/B that matters is run with
    **no DAW open**, restarting PipeWire between legs (the ceiling is read at `open()`, so a sink that is
    already open keeps what it negotiated and the param looks inert):
    | `max_buffer` | period | buffer | `delay` | audible |
    |---|---|---|---|---|
    | 0 | 1024 | 4096 | 3072 = 64 ms | yes |
    | 256 | 64 | 256 | 128 = **2.7 ms** | yes |
    `clock.quantum` stayed **1024 in both** — so PipeWire decouples the ALSA node's period from the graph
    quantum and simply runs the node faster. Ordinary desktop playback therefore does NOT break at a
    lowered ceiling; it gets 24x less latency, at the cost of 16x the node wakeups (unmeasured CPU).
    **Two of this project's own claims died here, both from measuring under a DAW that had already pulled
    the graph to a small quantum:** "PipeWire pins BUFFER_SIZE and is indifferent to the ceiling" (it
    pins nothing at the default quantum — it took the full 4096) and "PipeWire at a 1024-frame quantum
    needs 2048 for its two periods, so lowering the default would regress the desktop" (it needs no such
    thing). A third, predicted this session and also wrong: that `buffer < period` would make `hw_params`
    unsatisfiable and fail the open outright — PipeWire never asks for that intersection, it re-picks the
    period first. **The stated rationale for `max_buffer` defaulting to 0 is thus disproven**; the default
    stays 0 for now on sample size (one host, one PipeWire version, one model), not on evidence of harm.
    **Method note:** `pactl suspend-sink <sink> 0` does NOT force the ALSA open — PipeWire opens on
    demand. Playing a WAV with `pw-play` and reading `hw_params` mid-stream does. And read `hw_params`
    with a *listening* check beside it: `state: RUNNING` with `hw_ptr` advancing was equally true during
    the 8Pre silence, so the telemetry alone cannot tell playing from silent.
  - **★ THE DIGITAL-LOOPBACK RAMP — the method that made all of the above measurable, REUSE IT (Sep 2 2026,
    8Pre).** Playback glitches had only ever been assessed by listening. The router turns that into a
    frame-exact count with no cables and no ears: **`PCM 01 Capture Enum` accepts a PLAYBACK channel as its
    source**, so a playback channel loops straight back into capture inside the device, bit-exactly.
    - Put the signal on **PCM 7** (on the 8Pre, PCM 7-10 feed no physical output and no mixer input, so a
      full-scale test tone is completely SILENT — check this per model before trusting it).
    - Signal is a **sample counter scaled by 256** (low 8 bits zero, so the device's 24-bit truncation is
      lossless). Consecutive recovered samples must differ by exactly 256.
    - The delta classifies the fault by itself: `skip B`/`repeat B` where B == the ALSA buffer is an
      **xrun** (client starvation); a delta that is not a multiple of the step is **sample-level
      corruption**; and `skip 2016` is literally the ~42 ms platform freeze read off the wire.
    - Drive it with `aplay`/`arecord` on `hw:N,0` (one playback + one capture substream, so the two
      processes coexist). **Do NOT try to use PipeWire as the playback leg** — activating its sink also
      opens the capture substream, and `arecord` then gets EBUSY.
    Harness (ramp generator, checker, runners) is disposable but the recipe above is not.
  - **★ THE CEILING IS FULLY EFFECTIVE ON A 20-CHANNEL MODEL (Sep 2 2026, 8Pre, `max_buffer` swept with
    PipeWire as the only client).** `buffer == max_buffer` at every value, period always buffer/4, and
    **`clock.quantum` pinned at 1024 throughout** — so the 2Pre finding (PipeWire shrinks the ALSA node's
    period, not its graph quantum) generalizes:
    | `max_buffer` | 0 | 2048 | 1024 | 512 | 256 | 128 |
    |---|---|---|---|---|---|---|
    | period | 512 | 512 | 256 | 128 | 64 | 32 |
    | buffer | 2048 | 2048 | 1024 | 512 | 256 | 128 |
    | latency | 42.7 ms | 42.7 ms | 21.3 ms | 10.7 ms | 5.3 ms | 2.7 ms |
    Note `max_buffer=0` gave **2048 here but 1024 earlier in the same session** — PipeWire's unconstrained
    pick is NOT deterministic, which is an independent argument for setting the ceiling explicitly.
  - **Servicer CPU is FLAT across the whole buffer range** (8Pre, 20 s samples of the `clarett-svc` kthread):
    11.70% at buffer 4096 / 11.25% at 512 / 11.70% at 256 / **12.25% at 128**, while the fill's memcpy load
    rises 14 -> 461 MB/s. So the known `clarett_tx_fill` inefficiency (copying `ring - guard` every tick
    regardless of period) is real in bytes and **irrelevant in practice** — the servicer's cost is dominated
    by MMIO polling over the Thunderbolt link. Do not argue against a small period on CPU grounds.
  - **★★ RETEST LIST — RUN ALL OF THIS ON THE ELITEBOOK 640 G11 BEFORE TRUSTING ANY NUMBER ABOVE
    (queued Sep 2 2026).** Every measurement in this section was taken on the ASRock X570 Creator, i.e.
    THROUGH the ~42 ms periodic firmware stall ([[clarett-playback-skipping]]), with a NON-REALTIME test
    client (`/tmp` is `nosuid`, so a `setcap cap_sys_nice` wrapper was inert — put the binary on a
    filesystem without `nosuid` next time). Both make every small-buffer number pessimistic, and the
    stall is why only a 4096-frame buffer was ever break-free here.
    1. ~~**The `max_buffer` sweep, repeated on the clean host**~~ **DONE Sep 4 2026 — 128 IS RIGHT, and
       item 3's realtime client was covered in the same run.** Red 8Line, 60 channels, 48 kHz, client at
       SCHED_FIFO 50, six 60 s legs on the EliteBook 640 G11:
       | max_buffer | 128 | 256 | 512 | 1024 | 2048 | 4096 |
       |---|---|---|---|---|---|---|
       | period | 32 | 64 | 128 | 256 | 512 | 1024 |
       | `gapmax` | 874 us | 1551 us | 2886 us | 5566 us | 10872 us | 21553 us |
       | **excess over nominal** | **207** | **218** | **219** | **233** | **205** | **220 us** |
       | periods delivered | 90000 | 45000 | 22500 | 11250 | 5625 | 2813 |
       **Every leg delivered its EXACT expected period count, with `client_ov`/`late`/`overrun`/`badreads`
       all zero throughout, `readmax` 43-97 us.** So 128 frames is clean at the widest geometry in the
       range, on the widest device, with nothing dropped.
       **★ THE NUMBER THAT MATTERS: worst-case servicer jitter is 205-233 us and is INDEPENDENT of the
       period** — it does not scale, so it is pure scheduling overhead, not a stall. At buffer 128
       (2667 us) that is 9 % of the buffer, an **11x margin**. Nothing here is marginal.
       **★★ `gapmax` IS NOT A STALL METRIC — it tracks the NOMINAL PERIOD, and misreading it cost two
       wrong conclusions in one session.** `gapmax` ~= period/48000 + ~220 us. A 21.5 ms `gapmax` at
       period 1024 is *health*; the same number at period 32 would be a catastrophe. **Always divide by
       the nominal period before interpreting it**, and judge a run by `client_ov`/`late`/`overrun`/
       `badreads` instead. (Compare the ASRock, where `readmax` reaches 42047 us and `gapmax` runs
       45-60 ms against a 10.7 ms nominal — that is what a real stall looks like.)
       **Method trap that voided the first attempt:** running `sudo arecord ... /dev/null` **clobbers
       `/dev/null`** (root recreates it as a regular file), after which every `>/dev/null` in the script
       silently fails — so the `max_buffer` writes never happened and all six legs ran at 4096 while
       *appearing* to sweep. Have `arecord` write to stdout (`-`) and let the unprivileged shell do the
       redirect; read `buffer_size`/`period_size` back from `hw_params` MID-run and print them, so
       identical legs are visible rather than inferred.
       **★★ DUPLEX SWEEP FOLLOWED, AND IT CLOSES THE QUESTION — `max_buffer=128` IS CORRECT.** The
       capture sweep tested the wrong direction: the floor is `2 * CLARETT_TX_GUARD_FRAMES`, so PLAYBACK
       defines it. Red 8Line, **64ch playback + 60ch capture simultaneously**, both clients SCHED_FIFO,
       45 s legs:
       | max_buffer | 128 | 256 | 512 | 1024 |
       |---|---|---|---|---|
       | period | 32 | 64 | 128 | 256 |
       | excess over nominal | 304 us | 218 us | 210 us | 215 us |
       | margin (buffer/excess) | **8.8x** | 24.5x | 50.7x | 99.4x |
       | periods delivered | 67500 | 33751 | 16875 | 8438 |
       **Exact expected period counts at every size, with capture xruns, playback xruns, `late` and
       `overrun` ALL ZERO throughout.** Excess is ~210-215 us as in the capture sweep, rising to 304 us
       only at the tightest setting (where `readmax` also rose, 253 us against 42-75 us elsewhere) — two
       realtime clients plus `clarett_tx_fill` at the smallest period, and still an 8.8x margin.
       **Three further firsts in the same run:** the Red's **PLAYBACK works** (first playback ever on a
       non-Clarett device, and the first non-Clarett duplex); its TX fragment at 64 channels is
       `64*4*16` = `0x1000`, page-exact, so the 8PreX's fragment-fold hazard does not arise; and the
       **duplex arm/close races did not recur** — simultaneous start and stop at four buffer sizes on a
       new model left no orphaned `clarett-svc` kthread and no `WARN_ON` splat.
       Still not covered: rates above 48 kHz, and a NON-realtime client.
    2. **The `tx_guard` question** — does the synthetic short-lead client reproduce the 16/32 catastrophe
       there? If it does NOT, it is this host or that client and the floor stays at one fragment; if it
       DOES, the floor genuinely needs raising. Do not change the clamp on this box's evidence.
    3. **A realtime client.** Everything here ran at normal priority; a DAW runs SCHED_FIFO. Re-run the
       lead A/B and the buffer sweep with the client at RT before believing any break count.
    4. ~~**Does the 128 pin survive real use?**~~ **ANSWERED NO (Sep 10 2026):** a JUCE app garbled at
       every buffer above 64 samples. Replaced by the `CLARETT_MAX_PERIODS` rule — see the ★★★ bullet at
       the top of this section.
    5. **Round-trip latency, measured not derived** — analogue loopback (output patched to input,
       impulse, count frames). NOTHING today measured plucked-string-to-speaker latency; the digital
       loopback cannot see the converters. This is the number the amp-modeling target is about.
    6. **The 60 s cadence-4 duplex regression** (never re-run) and **rates other than 48 kHz** (all of
       today was 48k).
  - **★ THE 8PreX/8Pre "PIPEWIRE PLAYS NOTHING" EPISODE WAS DEVICE EXCLUSIVITY, NOT A DRIVER BUG
    (Aug 27 2026, resolved Aug 31 by the operator).** `snd_pcm_new(..., 0, 1, 1, ...)` gives **one
    playback and one capture substream** — no dmix, no sharing — so a DAW opened on `hw:N,0` **as an ALSA
    device** locks PipeWire out of the card completely. PipeWire had the 8Pre, the DAW was then granted
    it directly, and everything PipeWire subsequently "played" went nowhere. Corroborated by three
    observations from that session that were each misread at the time: `aplay -D hw:N,0` returning
    `Device or resource busy` (blamed on PipeWire, equally the DAW), `pw-record` yielding a 44-byte
    header-only WAV with `frames=0` (the capture substream was held too), and every `Level Meter` slot
    reading zero for a `pw-play` tone that never reached the device. **None of routing, output level,
    channel mapping or `max_buffer` was involved**, and a long hunt through all four found nothing
    because there was nothing there.
    **THE ONE-LINE DISCRIMINATOR, and use it before trusting any `/proc/asound` reading:**
    `/proc/asound/card<N>/pcm0p/sub0/status` prints **`owner_pid`** — check it against `pidof pipewire`.
    `state: RUNNING` with an advancing `hw_ptr` only ever means *some* client is streaming, never which,
    and reading it as "PipeWire is playing" is what cost that session. Run a DAW through PipeWire's JACK
    layer (`pw-jack`) rather than on the raw ALSA device if both are wanted at once.
