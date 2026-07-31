// SPDX-License-Identifier: GPL-2.0-only
/*
 * Focusrite Clarett (Thunderbolt) — PCM data plane (full duplex).
 *
 * The hardware is ONE full-duplex DMA engine over two ring blocks in a single contiguous coherent buffer
 * (c->stream_buf): block 0 (0x200 = TX/playback) then block 1 (0x300 = RX/capture). It raises a period on
 * the 0x300 cause register and waits for the host to ACK by reading it; clarett_stream_service()
 * (clarett_main.c) is that ACK loop and the shared frame clock for both directions. Each 0x300 event
 * carries the frames the engine advanced (ctr delta * CLARETT_CTR_FRAMES); clarett_pcm_tick() then drains
 * the RX ring into the capture ALSA buffer (behind the write pointer) and refills the TX ring from the
 * playback ALSA buffer (ahead of the read pointer).
 *
 * Two ALSA substreams share the one engine: whichever prepares first arms it full-duplex, the other
 * attaches at the current clock. The engine ONLY clocks when both rings live in one contiguous buffer
 * with r1 = r0 + ring (proven by the engine-start probe), which is why TX is always armed even for
 * capture-only (it plays silence until a playback stream fills it).
 *
 * Calibration caveat (clarett.h): frames-per-0x300-event uses CLARETT_CTR_FRAMES; verified on the 2Pre
 * (spec §14). Clocking and period flow are independent of that constant.
 */
#include <linux/dma-mapping.h>
#include <linux/math64.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include "clarett.h"

/*
 * Replay the vendor's pre-arm re-init batch in the stream handshake (see clarett_stream_handshake).
 * TESTED ON A 4Pre AND IT CHANGES NOTHING: the device accepts every command (err=0) but the engine
 * still raises one period event with ctr=0. Default OFF because it overlaps the probe-time bring-up
 * that is documented to wedge GET_DATA when re-run on an armed device, and it has no demonstrated
 * benefit to weigh against that. Kept as a lever for retesting on other models.
 */
static bool stream_batch;
module_param(stream_batch, bool, 0644);
MODULE_PARM_DESC(stream_batch,
		 "Replay the vendor's pre-arm re-init batch (INIT_2, subsystem enables 1-8, count "
		 "queries, 0x004001 x6) before arming the stream engine (default off; no effect on a 4Pre).");

/*
 * TX-ring contents (data-plane spec §12, the other half of the arm-ritual question). The vendor's four
 * failing arms and its one working arm program byte-identical registers, so what differs is elapsed time
 * or host-RAM contents. Windows was playing audio in every capture, so its TX ring held real samples by
 * the time the working arm went in; our dummy TX ring holds silence. This lever fills it with a 1 kHz
 * sine instead, to test whether the engine needs a non-silent TX ring to clock.
 *
 * -18 dBFS deliberately: this plays out of the monitor outputs the instant the engine runs, so it has to
 * be audible without being dangerous in headphones.
 */
static bool tx_tone;
module_param(tx_tone, bool, 0444);
MODULE_PARM_DESC(tx_tone,
		 "Fill the dummy TX ring with a 1 kHz -18 dBFS sine instead of silence (audible on the "
		 "outputs if the engine runs). Tests whether the engine gates on TX content.");



/* One cycle of 1 kHz at 48 kHz, 24-bit signed; shifted left 8 for S32_LE MSB-justified. */
static const s32 clarett_sine48[48] = {
	        0,    136867,    271391,    401273,    524288,    638333,
	   741455,    831891,    908093,    968758,   1012847,   1039605,
	  1048576,   1039605,   1012847,    968758,    908093,    831891,
	   741455,    638333,    524288,    401273,    271391,    136867,
	        0,   -136867,   -271391,   -401273,   -524288,   -638333,
	  -741455,   -831891,   -908093,   -968758,  -1012847,  -1039605,
	 -1048576,  -1039605,  -1012847,   -968758,   -908093,   -831891,
	  -741455,   -638333,   -524288,   -401273,   -271391,   -136867,
};

/*
 * Per-ring geometry is derived per-model at runtime (clarett.h: clarett_pcm_rx_samples() &c.). The ALSA
 * buffer is pinned to exactly the RX sample-area size so the RX ring and the ALSA ring share one geometry
 * (CLARETT_STREAM_NDESC descriptors, 1:1 byte offsets) — the per-period copy is then a straight offset copy
 * with no rescaling.
 */

/*
 * Constant capability template; the per-model geometry fields (channels, buffer/period bytes,
 * periods_max) are filled in clarett_pcm_open() from c->model.
 */
static const struct snd_pcm_hardware clarett_pcm_hw = {
	.info             = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
			    SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats          = SNDRV_PCM_FMTBIT_S32_LE,	/* interleaved, 24-bit MSB-justified */
	.rates            = SNDRV_PCM_RATE_48000,
	.rate_min         = CLARETT_PCM_RATE,
	.rate_max         = CLARETT_PCM_RATE,
	.periods_min      = 2,
};

/* Pointer to the block-1 (capture) RX sample area inside the contiguous hardware buffer. Descriptor mode:
 * [TX table][TX samples][RX table][RX samples]. Flat mode: [TX samples][RX samples]. clarett_stream_rx_off()
 * returns the right offset for the model's mode. */
static u8 *clarett_rx_area(struct clarett *c)
{
	return (u8 *)c->stream_buf + clarett_stream_rx_off(c);
}

/* RX capture sample-ring size in bytes (== the capture ALSA buffer), per mode. */
static size_t clarett_rx_ring_bytes(struct clarett *c)
{
	return clarett_stream_rx_area_bytes(c);
}

/* Pointer to the block-0 (playback) TX sample area, and its size (== the playback ALSA buffer). */
static u8 *clarett_tx_area(struct clarett *c)
{
	return (u8 *)c->stream_buf + clarett_stream_tx_off(c);
}
static size_t clarett_tx_ring_bytes(struct clarett *c)
{
	return clarett_stream_tx_area_bytes(c);
}

/* Silence the whole TX device area (all slots, including any inter-fragment padding): playback-idle,
 * capture-only must not loop stale playback audio out the DACs. */
static void clarett_zero_tx(struct clarett *c)
{
	memset(clarett_tx_area(c), 0, clarett_pcm_tx_dev_bytes(c));
}

/*
 * Drain nframes of captured audio from the RX device area (starting at hardware ring frame `pos`) into the
 * contiguous ALSA buffer (starting at that stream's own frame `apos` — the two differ by the capture
 * direction's attach base and wrap independently). The RX area is a table of NDESC fragment SLOTS of
 * c->rx_slot bytes; ring frame f lives in slot (f/FRAG_FRAMES) at byte (f%FRAG_FRAMES)*frame within that
 * slot. When rx_slot == audio-bytes/fragment (the contiguous default) this is just a linear copy; when
 * padded (scatter-gather experiment) it gathers per fragment across the gaps. FRAG_FRAMES divides the ring,
 * so a chunk clipped to the fragment boundary also handles the ring wrap.
 */
static void clarett_rx_drain(struct clarett *c, u8 *alsa, u32 apos, u32 pos, u32 nframes)
{
	u8 *ring = clarett_rx_area(c);
	u32 frame = (u32)c->model->capture_channels * 4;
	u32 slot  = c->rx_slot;
	u32 ring_frames = CLARETT_STREAM_NDESC * CLARETT_FRAG_FRAMES;

	while (nframes) {
		u32 fio   = pos % CLARETT_FRAG_FRAMES;			/* frame within its fragment */
		u32 chunk = min(nframes, CLARETT_FRAG_FRAMES - fio);	/* up to the fragment (and ring) boundary */

		chunk = min(chunk, ring_frames - apos);			/* and up to the ALSA buffer wrap */

		memcpy(alsa + (size_t)apos * frame,
		       ring + (size_t)(pos / CLARETT_FRAG_FRAMES) * slot + (size_t)fio * frame,
		       (size_t)chunk * frame);
		pos += chunk;
		if (pos == ring_frames)
			pos = 0;
		apos += chunk;
		if (apos == ring_frames)
			apos = 0;
		nframes -= chunk;
	}
}

/*
 * Fill nframes of playback audio into the TX device area (starting at hardware ring frame `pos`) from the
 * contiguous ALSA playback buffer (starting at that stream's own frame `apos`). Exact mirror of
 * clarett_rx_drain with source/destination swapped: the TX area is NDESC fragment SLOTS of c->tx_slot
 * bytes; ring frame f lives in slot (f/FRAG_FRAMES) at byte (f%FRAG_FRAMES)*frame within that slot. When
 * tx_slot == audio-bytes/fragment (tx_frag_pad=0) this degenerates to the old linear copy; when padded it
 * scatters per fragment across the gaps (matching the vendor's non-contiguous TX ring). FRAG_FRAMES divides
 * the ring, so a chunk clipped to the fragment boundary also handles the ring wrap.
 */
static void clarett_tx_fill(struct clarett *c, const u8 *alsa, u32 apos, u32 pos, u32 nframes)
{
	u8 *ring = clarett_tx_area(c);
	u32 frame = (u32)c->model->playback_channels * 4;
	u32 slot  = c->tx_slot;
	u32 ring_frames = CLARETT_STREAM_NDESC * CLARETT_FRAG_FRAMES;

	while (nframes) {
		u32 fio   = pos % CLARETT_FRAG_FRAMES;			/* frame within its fragment */
		u32 chunk = min(nframes, CLARETT_FRAG_FRAMES - fio);	/* up to the fragment (and ring) boundary */

		chunk = min(chunk, ring_frames - apos);			/* and up to the ALSA buffer wrap */

		memcpy(ring + (size_t)(pos / CLARETT_FRAG_FRAMES) * slot + (size_t)fio * frame,
		       alsa + (size_t)apos * frame,
		       (size_t)chunk * frame);
		pos += chunk;
		if (pos == ring_frames)
			pos = 0;
		apos += chunk;
		if (apos == ring_frames)
			apos = 0;
		nframes -= chunk;
	}
}

/*
 * Guard: keep the playback fill this many frames clear of the engine's current TX read position, so a
 * concurrent DMA read is never torn by our write. It only has to exceed the engine's advance during ONE
 * fill memcpy (a few frames — a ~64 KB copy is single-digit microseconds), NOT the whole inter-tick gap:
 * the fill covers the ENTIRE ring ahead of the read, so even a lagged tick reads frames a prior tick
 * already filled. It must also stay <= the app's steady-state lead (>= one ALSA period, min 256 frames),
 * or the fill's near edge reads not-yet-written data — the PipeWire skipping. 64 frames satisfies both. */
#define CLARETT_TX_GUARD_FRAMES	(4 * CLARETT_FRAG_FRAMES)	/* 64 frames */

/*
 * Called from the servicer kthread on every 0x300 period event with the number of frames the engine
 * advanced since the last event (the 0x300 counter delta * CLARETT_CTR_FRAMES — self-calibrating to the
 * real hardware period, spec §14). One engine clock drives BOTH directions of the full-duplex ring:
 *
 *   capture  — copy the add_frames the engine just WROTE from the RX ring into the capture ALSA buffer
 *              (behind the engine's write pointer).
 *   playback — refill the TX ring AHEAD of the engine's read pointer from the playback ALSA buffer, so
 *              the audio the app queued is in place before the engine reads it.
 *
 * Both rings map frame k at (k mod ring_frames), and the ALSA buffers are pinned to the same frame count,
 * so ring offset == ALSA offset in each direction. The pcm_lock serialises these copies against hw_free,
 * which clears the substream pointer and frees the ALSA buffer; period_elapsed is called after unlocking
 * (the substream object itself lives until close).
 */
void clarett_pcm_tick(struct clarett *c, u32 add_frames)
{
	struct snd_pcm_substream *cs, *ps;
	bool cap_elapsed = false, play_elapsed = false;

	if (!READ_ONCE(c->stream_run) || !add_frames)
		return;

	mutex_lock(&c->pcm_lock);

	cs = c->pcm_sub;
	ps = c->pcm_play_sub;

	/* Capture drain: the add_frames the engine just wrote, from the current ring position (slot-aware) to
	 * the matching position in the stream's own ALSA buffer (offset by where it attached). */
	if (cs && cs->runtime->dma_area) {
		u32 frame = (u32)c->model->capture_channels * 4;
		u32 ring  = clarett_rx_ring_bytes(c) / frame;
		u64 q = c->pcm_frames;
		u64 aq = c->pcm_frames - c->pcm_base;
		u32 pos = do_div(q, ring);
		u32 apos = do_div(aq, ring);

		clarett_rx_drain(c, cs->runtime->dma_area, apos, pos, min(add_frames, ring));
	}

	/* Playback fill: refresh the whole runway ahead of the engine (from GUARD past the read position to
	 * just before it), so a lagged tick can never underfill and the current read is never torn. */
	if (ps && ps->runtime->dma_area && READ_ONCE(c->play_running)) {
		u32 frame = (u32)c->model->playback_channels * 4;
		u32 ring  = clarett_tx_ring_bytes(c) / frame;

		if (ring > CLARETT_TX_GUARD_FRAMES) {
			u64 q = c->pcm_frames + CLARETT_TX_GUARD_FRAMES;
			u64 aq = c->pcm_frames + CLARETT_TX_GUARD_FRAMES - c->play_base;
			u32 start = do_div(q, ring);		/* hardware ring position */
			u32 astart = do_div(aq, ring);		/* same frame in the ALSA buffer */

			clarett_tx_fill(c, ps->runtime->dma_area, astart, start,
					ring - CLARETT_TX_GUARD_FRAMES);
		}
	}

	c->pcm_frames += add_frames;

	/* Deliver period boundaries only between trigger START and STOP (the *_running gates). Period indices
	 * are counted on each direction's own clock (pcm_frames - base), matching what .pointer reports. */
	if (cs && READ_ONCE(c->pcm_running)) {
		u64 period = div_u64(c->pcm_frames - c->pcm_base, cs->runtime->period_size);

		if (period != c->pcm_last_period) {
			c->pcm_last_period = period;
			cap_elapsed = true;
		}
	}
	if (ps && READ_ONCE(c->play_running)) {
		u64 period = div_u64(c->pcm_frames - c->play_base, ps->runtime->period_size);

		if (period != c->play_last_period) {
			c->play_last_period = period;
			play_elapsed = true;
		}
	}

	mutex_unlock(&c->pcm_lock);

	/* Outside the lock (period_elapsed takes the stream lock; the substreams live until close). */
	if (cap_elapsed)
		snd_pcm_period_elapsed(cs);
	if (play_elapsed)
		snd_pcm_period_elapsed(ps);
}

static int clarett_pcm_open(struct snd_pcm_substream *ss)
{
	struct clarett *c = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *runtime = ss->runtime;
	bool play = ss->stream == SNDRV_PCM_STREAM_PLAYBACK;
	u8 chans   = play ? c->model->playback_channels : c->model->capture_channels;
	size_t buf = play ? clarett_tx_ring_bytes(c) : clarett_rx_ring_bytes(c);
	u32 period = clarett_irq_period_frames() * chans * 4;	/* one 0x300 period in bytes */
	int err;

	runtime->hw = clarett_pcm_hw;
	runtime->hw.channels_min     = chans;
	runtime->hw.channels_max     = chans;
	runtime->hw.buffer_bytes_max = buf;
	/*
	 * Let the app choose its period between the hardware IRQ period (256 frames) and half the buffer, in
	 * whole IRQ-period steps. A fixed tiny period forced PipeWire into a rigid 5 ms cadence it services
	 * badly (audible skipping on music); a range lets it pick a comfortable larger period. The tick reports
	 * period_elapsed off runtime->period_size, so any multiple of the hardware period works. The BUFFER is
	 * still pinned to the ring size — the RX/TX copies map ALSA frame k to hardware ring frame k (mod ring).
	 */
	runtime->hw.period_bytes_min = period;
	runtime->hw.period_bytes_max = buf / 2;			/* periods_min = 2 */
	runtime->hw.periods_max      = buf / period;

	err = snd_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, buf, buf);
	if (err < 0)
		return err;
	/* Period must be a whole number of hardware IRQ periods (CLARETT_IRQ_DESCS descriptors). */
	return snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, period);
}

/*
 * Detach a direction and, once BOTH are gone, stop the shared engine. Clearing the substream pointer
 * under pcm_lock (and draining any in-flight tick) guarantees clarett_pcm_tick() stops touching this
 * runtime's dma_area before the core frees it. On playback teardown re-silence the TX ring so a
 * surviving capture stream does not loop stale playback audio out the DACs.
 */
static void clarett_pcm_detach(struct clarett *c, struct snd_pcm_substream *ss)
{
	bool play = ss->stream == SNDRV_PCM_STREAM_PLAYBACK;

	mutex_lock(&c->pcm_lock);
	if (play) {
		c->pcm_play_sub = NULL;
		WRITE_ONCE(c->play_running, false);
		clarett_zero_tx(c);
	} else {
		c->pcm_sub = NULL;
		WRITE_ONCE(c->pcm_running, false);
	}
	mutex_unlock(&c->pcm_lock);

	if (!c->pcm_sub && !c->pcm_play_sub)
		clarett_engine_stop(c);		/* last user out: tear the engine down */
}

static int clarett_pcm_close(struct snd_pcm_substream *ss)
{
	clarett_pcm_detach(snd_pcm_substream_chip(ss), ss);
	return 0;
}

/* The core calls hw_free() ahead of releasing the managed DMA buffer. */
static int clarett_pcm_hw_free(struct snd_pcm_substream *ss)
{
	clarett_pcm_detach(snd_pcm_substream_chip(ss), ss);
	return 0;
}

/*
 * Stage-1 stream-config handshake (spec §9 step 5; dataplane memory). The VM re-issues SET_CLOCK + the
 * no-arg session lifecycle (0x6004 ×2 / 0x6005) in-session, immediately before every engine arm. The device
 * resets its stream config when idle, so the byte-identical handshake we already run once in clarett_arm_device()
 * at probe does not persist to PCM-arm time. Re-run the re-run-safe SUBSET here (NOT the full bring-up, which
 * wedges GET_DATA). Process context, so the mailbox is safe.
 *
 * Stage 2: the per-channel CONFIG_PUSH burst (model->stream_tx_ids / stream_rx_ids) re-declares which physical
 * inputs feed which DMA stream channels — without it the engine arms cleanly but no samples are routed in and
 * 0x300 never ticks (periods=0). Wire order (from 2pre_streamstart.log): SET_CLOCK, GET_6.2, GET_7.2, push tx
 * ids, GET_7.3, push rx ids, then the lifecycle commands. Non-fatal: log and proceed even if a command errors.
 */
static void clarett_stream_handshake(struct clarett *c, unsigned int rate)
{
	const struct clarett_model *m = c->model;
	u8 clk[8], id[2];
	int e_clk, e_en1, e_en2, e_commit, pushes = 0, push_err = 0, batch_err = 0, i;

	if (!rate)
		rate = CLARETT_DEFAULT_RATE;
	clarett_put_le32(clk,     rate);
	clarett_put_le32(clk + 4, CLARETT_CLOCK_INTERNAL);

	e_clk = clarett_fcp(c, FCP_SET_CLOCK, clk, sizeof(clk));

	/* Per-channel routing push (skipped if the model has no captured ids). */
	if (m->n_stream_tx_ids || m->n_stream_rx_ids) {
		clarett_fcp(c, FCP_GET_62, NULL, 0);
		clarett_fcp(c, FCP_GET_72, NULL, 0);
		for (i = 0; i < m->n_stream_tx_ids; i++) {
			id[0] = m->stream_tx_ids[i]; id[1] = 0;
			push_err |= clarett_fcp(c, FCP_CONFIG_PUSH, id, sizeof(id));
			pushes++;
		}
		clarett_fcp(c, FCP_GET_73, NULL, 0);
		for (i = 0; i < m->n_stream_rx_ids; i++) {
			id[0] = m->stream_rx_ids[i]; id[1] = 0;
			push_err |= clarett_fcp(c, FCP_CONFIG_PUSH, id, sizeof(id));
			pushes++;
		}
	}

	/*
	 * The vendor's pre-arm RE-INIT batch (4pre_boot_to_stream_end.log @21:58:18.5). Our engine
	 * state at arm is byte-identical to the vendor's — its failing arms read 0x218=0xe
	 * 0x21c=0xd->0xe 0x318=0x3 0x31c=0x3 and so do we — and it arms and stalls exactly as we do
	 * four times over. What it does differently is issue this batch, then re-arm once, after
	 * which the 0x300 counter advances. The commands look like a subset of probe-time bring-up
	 * (subsystem enables + count queries), re-issued per stream start; semantics are not decoded,
	 * so this is a verbatim replay. ids 1..8 and idx 0..5 are as observed on the 4Pre.
	 */
	if (stream_batch) {
		static const u32 count_queries[] = { 0x001000, 0x002000, 0x003000, 0x004000 };
		u8 arg[4];

		clarett_fcp(c, FCP_INIT_2, NULL, 0);
		for (i = 1; i <= 8; i++) {
			arg[0] = i; arg[1] = 0;
			batch_err |= clarett_fcp(c, FCP_INIT_1, arg, 2);
		}
		clarett_fcp(c, FCP_INIT_2, NULL, 0);
		for (i = 0; i < (int)ARRAY_SIZE(count_queries); i++)
			batch_err |= clarett_fcp(c, count_queries[i], NULL, 0);
		for (i = 0; i < 6; i++) {
			clarett_put_le32(arg, i);
			batch_err |= clarett_fcp(c, 0x004001, arg, 4);
		}
	}

	/*
	 * The pre-arm triple, in the vendor's order: 0x6004, 0x6002, 0x6005 — NOT 0x6004 twice.
	 * Every occurrence of these opcodes in 4pre_boot_to_stream_end.log is that triple (sometimes
	 * doubled for full duplex, which is where the old "VM issues twice" note came from), and the
	 * triple at 21:58:18.70 is what immediately precedes the one arm that streams: the vendor
	 * arms and fails exactly as we do — 0x110=7, one period event, 0x110=0 + 0x100=0xf, retry —
	 * four times over, then issues this batch, re-arms once, and the counter starts advancing.
	 */
	e_en1    = clarett_fcp(c, FCP_STREAM_ENABLE, NULL, 0);
	e_en2    = clarett_fcp(c, FCP_GET_62, NULL, 0);
	e_commit = clarett_fcp(c, FCP_STREAM_COMMIT, NULL, 0);

	dev_info(&c->pci->dev,
		 "stream-handshake: SET_CLOCK{%u,24}=%d CONFIG_PUSH=%d(err=%d) batch=%s(err=%d) "
		 "0x6004=%d 0x6002=%d 0x6005=%d\n",
		 rate, e_clk, pushes, push_err, stream_batch ? "yes" : "off", batch_err,
		 e_en1, e_en2, e_commit);
}

/*
 * Prepare a direction and, if the shared full-duplex engine is not already running (the other direction
 * armed it), arm it. The engine is ONE full-duplex stream (block 0 = TX/playback, block 1 = RX/capture)
 * with a single frame clock; whichever direction prepares first arms it and the other just attaches at
 * the current clock position. Re-preparing an already-armed direction (xrun recovery) does not re-arm —
 * the free-running engine is undisturbed. Process context, so the mailbox handshake is safe here.
 *
 * Either way the direction records its attach point in the shared clock. ALSA resets hw_ptr to 0 on every
 * prepare, so .pointer must report from there; without the base, a stream attaching to an already-running
 * engine (a second direction, or the SAME one recovering from an xrun) saw its first .pointer return the
 * engine's absolute position mod buffer_size. That reads as an enormous hw_ptr jump and the core xruns it
 * within a tick — which then re-prepares, and xruns again. Audio stayed dead until every substream closed
 * and the engine was torn down (a module reload, in practice).
 */
static int clarett_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct clarett *c = snd_pcm_substream_chip(ss);
	bool play = ss->stream == SNDRV_PCM_STREAM_PLAYBACK;
	bool arm;
	dma_addr_t r0, r1;

	mutex_lock(&c->pcm_lock);
	arm = !c->stream_on;
	if (arm)			/* first direction in: the shared clock starts here */
		c->pcm_frames = 0;
	if (play) {
		c->pcm_play_sub = ss;
		c->play_base = c->pcm_frames;
		c->play_last_period = 0;
		WRITE_ONCE(c->play_running, false);
	} else {
		c->pcm_sub = ss;
		c->pcm_base = c->pcm_frames;
		c->pcm_last_period = 0;
		WRITE_ONCE(c->pcm_running, false);
	}
	mutex_unlock(&c->pcm_lock);

	if (!arm)			/* engine already armed: just attached at the current clock */
		return 0;

	/* First direction in: arm the full-duplex engine. block 0 (TX) base, block 1 (RX) base — in
	 * descriptor mode each points at its table; r1 offset is clarett_stream_r1_off() for the mode. */
	r0 = c->stream_dma;
	r1 = c->stream_dma + clarett_stream_r1_off(c);

	/* In-session stream-config handshake immediately before arming, matching the VM (handshake ->
	 * program regs -> arm). Establishes the device's stream routing/mode for this session. */
	clarett_stream_handshake(c, ss->runtime->rate);
	clarett_engine_arm(c, r0, r1);

	/*
	 * ACK from here, not the trigger: the engine bursts immediately after arm and stalls within ms if
	 * unserviced, so the servicer must ACK 0x300 the instant it is armed. The triggers only gate whether
	 * completed periods are reported to ALSA (pcm_running / play_running).
	 */
	WRITE_ONCE(c->stream_run, true);
	clarett_engine_run(c);
	return 0;
}

/*
 * Atomic context (stream lock held): only flip this direction's servicing gate. The engine is already
 * armed and the kthread running from prepare(); START begins reporting that direction's periods from the
 * current clock, STOP halts them. Full teardown happens in close()/hw_free, which can sleep.
 */
static int clarett_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct clarett *c = snd_pcm_substream_chip(ss);
	bool play = ss->stream == SNDRV_PCM_STREAM_PLAYBACK;
	u64 base = play ? c->play_base : c->pcm_base;
	u64 period = div_u64(c->pcm_frames - base, ss->runtime->period_size);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		if (play) {
			c->play_last_period = period;
			WRITE_ONCE(c->play_running, true);
		} else {
			c->pcm_last_period = period;
			WRITE_ONCE(c->pcm_running, true);
		}
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		if (play)
			WRITE_ONCE(c->play_running, false);
		else
			WRITE_ONCE(c->pcm_running, false);
		return 0;
	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t clarett_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct clarett *c = snd_pcm_substream_chip(ss);
	bool play = ss->stream == SNDRV_PCM_STREAM_PLAYBACK;
	u64 base = play ? READ_ONCE(c->play_base) : READ_ONCE(c->pcm_base);
	u64 frames = READ_ONCE(c->pcm_frames) - base;

	/* Position on THIS stream's clock (frames since it attached), % buffer_size — 64-bit-safe
	 * (do_div takes a u32 divisor; buffer_size fits easily). */
	return do_div(frames, ss->runtime->buffer_size);
}

static const struct snd_pcm_ops clarett_pcm_ops = {
	.open     = clarett_pcm_open,
	.close    = clarett_pcm_close,
	.hw_free  = clarett_pcm_hw_free,
	.prepare  = clarett_pcm_prepare,
	.trigger  = clarett_pcm_trigger,
	.pointer  = clarett_pcm_pointer,
};

/*
 * Build both static descriptor tables in the contiguous hardware buffer ([TX ring][RX ring], each =
 * [table][samples]). Each entry is a bare 8-byte LE fragment bus address, 0x100-aligned; the per-direction
 * fragment is clarett_frag_bytes(channels) (a whole-frame, 0x100-aligned span). The LAST entry carries the
 * wrap flag in its low bits (TX 0x01, RX 0x03) — matching the live 8PreX vendor table from the RAM dump —
 * and there is NO zero terminator. dma_alloc_coherent returns zeroed memory, so the TX samples are already
 * silence and the RX sample area (the engine's write target) starts clean.
 *
 * No 0xAA pre-fill: that was a flat-mode crutch (the engine was reading the SAMPLE area as pointers). In
 * descriptor mode the engine reads the TABLE (valid, non-null entries -> it clocks) and writes audio INTO
 * the sample area, so a clean zeroed sample area is correct and fault-free.
 */
/*
 * Write the 1 kHz sine over the whole TX sample area, every playback channel. Slot-aware: logical frame f
 * lives in slot (f/FRAG_FRAMES) at byte (f%FRAG_FRAMES)*frame (== a linear fill when tx_slot is unpadded).
 */
static void clarett_fill_tx_tone(struct clarett *c)
{
	u8 chans   = c->model->playback_channels;
	u32 frame  = (u32)chans * 4;
	u8 *ring   = clarett_tx_area(c);
	u32 slot   = c->tx_slot;
	size_t frames = clarett_stream_tx_area_bytes(c) / frame;	/* logical frame count */
	size_t f;
	u8 ch;

	for (f = 0; f < frames; f++) {
		__le32 v = cpu_to_le32((u32)(clarett_sine48[f % ARRAY_SIZE(clarett_sine48)] << 8));
		__le32 *p = (__le32 *)(ring + (f / CLARETT_FRAG_FRAMES) * slot
					    + (f % CLARETT_FRAG_FRAMES) * frame);

		for (ch = 0; ch < chans; ch++)
			*p++ = v;
	}
	dev_info(&c->pci->dev, "TX ring filled with 1 kHz -18 dBFS sine (%zu frames x %uch)\n",
		 frames, chans);
}

static void clarett_build_rings(struct clarett *c)
{
	size_t tbl     = clarett_pcm_tbl_bytes();

	/*
	 * Flat mode: no descriptor table at all — the engine reads/writes the contiguous sample ring directly
	 * at 0x210/0x310. dma_alloc_coherent already zeroed the buffer (TX = silence, RX = clean write target),
	 * so there is nothing to build. NO prefill: the §9 "0xAA prefill" was a descriptor-mode artifact (the
	 * engine dereferencing sample bytes as pointers); in a true flat ring the bytes are samples, not pointers.
	 */
	if (c->flat_buffer) {
		if (tx_tone)
			clarett_fill_tx_tone(c);
		dev_info(&c->pci->dev, "flat rings: TX %zu B + RX %zu B, no descriptor table\n",
			 clarett_flat_tx_bytes(c), clarett_flat_rx_bytes(c));
		return;
	}
	size_t tx_ring = clarett_pcm_tx_ring(c);
	u32 tx_frag = clarett_frag_bytes(c->model->playback_channels);
	u32 tx_slot = c->tx_slot;		/* TX descriptor stride: audio bytes, or a padded slot */
	u32 rx_slot = c->rx_slot;		/* RX descriptor stride: audio bytes, or a padded slot (experiment) */
	__le64 *tx_tbl = (__le64 *)c->stream_buf;
	__le64 *rx_tbl = (__le64 *)((u8 *)c->stream_buf + tx_ring);
	dma_addr_t tx_smp = c->stream_dma + tbl;
	dma_addr_t rx_smp = c->stream_dma + clarett_stream_rx_off(c);	/* page-aligned RX sample area */
	unsigned int i;

	for (i = 0; i < CLARETT_STREAM_NDESC; i++) {
		tx_tbl[i] = cpu_to_le64(tx_smp + (u64)i * tx_slot);	/* slotted: non-contiguous when padded */
		rx_tbl[i] = cpu_to_le64(rx_smp + (u64)i * rx_slot);	/* slotted: fragments non-contiguous when padded */
		/* Periodic RX IRQ marker (spec §14): the engine raises a counted 0x300 period when it
		 * consumes an IRQ-flagged descriptor. Every CLARETT_IRQ_DESCS-th one, matching the vendor's
		 * ~14-descriptor cadence. TX carries no periodic marker (vendor TX flags only the last). */
		if ((i + 1) % CLARETT_IRQ_DESCS == 0)
			rx_tbl[i] |= cpu_to_le64(CLARETT_DESC_IRQ);
	}
	tx_tbl[CLARETT_STREAM_NDESC - 1] |= cpu_to_le64(CLARETT_DESC_WRAP_TX);
	rx_tbl[CLARETT_STREAM_NDESC - 1] |= cpu_to_le64(CLARETT_DESC_WRAP_RX);

	if (tx_tone)
		clarett_fill_tx_tone(c);

	dev_info(&c->pci->dev,
		 "descriptor rings: %u entries, tx audio=0x%x slot=0x%x, rx audio=0x%x slot=0x%x, RX IRQ every %u desc (%u frames/period)\n",
		 CLARETT_STREAM_NDESC, tx_frag, tx_slot,
		 clarett_frag_bytes(c->model->capture_channels), rx_slot,
		 CLARETT_IRQ_DESCS, clarett_irq_period_frames());
}

int clarett_create_pcm(struct clarett *c)
{
	struct snd_pcm *pcm;
	size_t rxbuf, txbuf, prealloc;
	int err;

	err = snd_pcm_new(c->card, c->model->name, 0, 1, 1, &pcm);	/* 1 playback, 1 capture */
	if (err < 0)
		return err;

	pcm->private_data = c;
	strscpy(pcm->name, c->model->name, sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &clarett_pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &clarett_pcm_ops);
	c->pcm = pcm;

	c->stream_size = clarett_stream_total_bytes(c);
	c->stream_buf = dmam_alloc_coherent(&c->pci->dev, c->stream_size,
					    &c->stream_dma, GFP_KERNEL);
	if (!c->stream_buf)
		return -ENOMEM;
	clarett_build_rings(c);

	/* Each direction constrains its own buffer to its ring in open(); preallocate the larger so both
	 * fit. The ALSA buffers are plain memory we memcpy to/from the hardware rings (not DMA'd directly). */
	rxbuf = clarett_rx_ring_bytes(c);
	txbuf = clarett_tx_ring_bytes(c);
	prealloc = max(rxbuf, txbuf);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV, &c->pci->dev, prealloc, prealloc);

	dev_info(&c->pci->dev,
		 "PCM registered (playback %uch / capture %uch, S32_LE @%u, %s ring, bufs tx=%zu rx=%zu B @%pad)\n",
		 c->model->playback_channels, c->model->capture_channels, CLARETT_PCM_RATE,
		 c->flat_buffer ? "flat" : "descriptor", txbuf, rxbuf, &c->stream_dma);

	return 0;
}
