// SPDX-License-Identifier: GPL-2.0-only
/*
 * Focusrite Clarett 8PreX (Thunderbolt) — PCM data plane (capture).
 *
 * Capture-only first target (data-plane spec §9 step 5): ring block 1 (0x300 = RX), the proven-working
 * direction. The hardware streams a flow-controlled DMA engine — it raises a period on the 0x300 cause
 * register and waits for the host to ACK by reading it. clarett_stream_service() (clarett_main.c) is
 * that ACK loop; here it is wired to ALSA.
 *
 * Buffer layout: the engine ONLY clocks when its two rings live in ONE contiguous coherent buffer with
 * r1 = r0 + ring (proven by the engine-start probe; split allocations — separate descriptor table, ALSA
 * buffer, TX ring — never raised a period). So both hardware rings live in c->stream_buf exactly as the
 * probe lays them: block 0 (silent dummy TX, the full-duplex arming requirement — FC always arms both
 * blocks even record-only, §9) first, block 1 (capture) second. Captured samples are copied from the
 * block-1 RX area into the ALSA-managed buffer each period (clarett_pcm_tick).
 *
 * Calibration caveat (see CLARETT_DESCS_PER_TICK in clarett.h): frames-per-0x300-tick is a hypothesis
 * until measured on hardware, so pitch/rate may be off until calibrated. The clocking and period flow
 * are independent of that constant.
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
 * Per-ring geometry is derived per-model at runtime (clarett.h: clarett_pcm_rx_samples() &c.). The ALSA
 * buffer is pinned to exactly the RX sample-area size so the RX ring and the ALSA ring share one geometry
 * (CLARETT_STREAM_NDESC descriptors, 1:1 byte offsets) — the per-period copy is then a straight offset copy
 * with no rescaling.
 */

/*
 * Diagnostic: after arming in prepare(), inline-poll 0x300 for ~1 s to measure whether the engine
 * clocks at all — independent of whether the consumer (arecord) stays alive.
 */
static bool pcm_selftest;
module_param(pcm_selftest, bool, 0444);
MODULE_PARM_DESC(pcm_selftest,
		 "On PCM prepare, inline-poll 0x300 for ~1s after arming to count periods (debug).");

/* Inline 2 s poll of 0x300 to count periods, independent of any consumer. @when tags the call site.
 * Dumps the latched engine register/descriptor state first so it can be diffed against the working
 * stream_probe engine_start. */
static void clarett_selftest_poll(struct clarett *c, const char *when)
{
	void __iomem *bar = c->bar0;
	unsigned long end = jiffies + msecs_to_jiffies(2000);
	__le64 *tx_tbl = (__le64 *)c->stream_buf;
	__le64 *rx_tbl = (__le64 *)((u8 *)c->stream_buf + clarett_pcm_tx_ring(c));
	u32 periods = 0, last = 0, maxc = 0;

	dev_info(&c->pci->dev,
		 "selftest[%s] regs: blk0 base=%08x:%08x ctrl=%08x ptr=%08x | blk1 base=%08x:%08x ctrl=%08x ptr=%08x | tx0=%016llx rx0=%016llx\n",
		 when,
		 readl(bar + STREAM_BLK0 + STREAM_OFF_BASE_HI), readl(bar + STREAM_BLK0 + STREAM_OFF_BASE_LO),
		 readl(bar + STREAM_BLK0 + STREAM_OFF_CTRL), readl(bar + STREAM_BLK0 + STREAM_OFF_PTR),
		 readl(bar + STREAM_BLK1 + STREAM_OFF_BASE_HI), readl(bar + STREAM_BLK1 + STREAM_OFF_BASE_LO),
		 readl(bar + STREAM_BLK1 + STREAM_OFF_CTRL), readl(bar + STREAM_BLK1 + STREAM_OFF_PTR),
		 le64_to_cpu(tx_tbl[0]), le64_to_cpu(rx_tbl[0]));

	while (time_before(jiffies, end)) {
		u32 c2 = readl(bar + STREAM_BLK1);	/* 0x300 read-to-clear = ACK */

		readl(bar + STREAM_BLK0);
		readl(bar + 0x500);
		if (c2 & 0x80000000) {
			u32 v = c2 & 0x7fffffff;

			periods++;
			if (v > maxc)
				maxc = v;
			last = v;
		}
		usleep_range(100, 200);
	}
	dev_info(&c->pci->dev, "pcm selftest [%s]: periods=%u max_ctr=0x%x last=0x%x\n",
		 when, periods, maxc, last);
}

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

/* Pointer to the block-1 (capture) RX sample area inside the contiguous hardware buffer. Layout is
 * [TX table][TX samples][RX table][RX samples]; the RX samples follow the whole TX ring then the RX table. */
static u8 *clarett_rx_area(struct clarett *c)
{
	return (u8 *)c->stream_buf + clarett_pcm_tx_ring(c) + clarett_pcm_tbl_bytes();
}

/* RX capture sample-ring size in bytes (== the ALSA buffer). */
static size_t clarett_rx_ring_bytes(struct clarett *c)
{
	return clarett_pcm_rx_samples(c);
}

/*
 * Called from the servicer kthread on every 0x300 period tick. Copies the freshly-captured period (4
 * interleaved frames) from the RX area into the ALSA buffer (same geometry, 1:1 offsets), advances the
 * modelled frame position, and reports a period boundary to ALSA. No-op while idle/paused. Position is
 * modelled, not read back: 0x318 is a static status word (spec §9), so the period count is the only signal.
 *
 * One 0x300 tick == one IRQ period == clarett_period_bytes(capture) bytes == 4 frames, copied at the byte
 * offset of the current ring position. The descriptors map the RX sample ring contiguously (entry i ->
 * rx_samples + i*frag), so the engine fills it linearly and the copy is a straight offset copy. The
 * descriptor fragment (>= one period) is decoupled from the IRQ period; the tick tracks the period.
 */
void clarett_pcm_tick(struct clarett *c)
{
	struct snd_pcm_substream *ss = READ_ONCE(c->pcm_sub);
	struct snd_pcm_runtime *runtime;
	u32 frame  = (u32)c->model->capture_channels * 4;	/* bytes per interleaved S32_LE frame */
	u32 pbytes = clarett_period_bytes(c->model->capture_channels);	/* one period = 4 frames */
	u32 ring_frames = clarett_rx_ring_bytes(c) / frame;
	u64 period, q;
	u32 pos, off;

	if (!ss || !READ_ONCE(c->stream_run))
		return;
	runtime = ss->runtime;

	q = c->pcm_frames;
	pos = do_div(q, ring_frames);		/* frame position within the RX ring */
	off = pos * frame;
	if (runtime->dma_area)
		memcpy(runtime->dma_area + off, clarett_rx_area(c) + off, pbytes);

	c->pcm_frames += pbytes / frame;	/* == 4 frames per period */

	/* Deliver period boundaries to ALSA only between trigger START and STOP. ACKing (stream_run) runs
	 * from prepare so the engine is serviced from the instant it is armed — it stalls within ms if not
	 * ACKed during its initial burst. */
	if (!READ_ONCE(c->pcm_running))
		return;
	period = div_u64(c->pcm_frames, runtime->period_size);
	if (period != c->pcm_last_period) {
		c->pcm_last_period = period;
		snd_pcm_period_elapsed(ss);
	}
}

static int clarett_pcm_open(struct snd_pcm_substream *ss)
{
	struct clarett *c = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *runtime = ss->runtime;
	size_t buf = clarett_rx_ring_bytes(c);			/* RX ring == ALSA buffer (per mode) */
	u32 period = clarett_period_bytes(c->model->capture_channels);	/* one IRQ period = 4 frames */
	int err;

	runtime->hw = clarett_pcm_hw;
	runtime->hw.channels_min     = c->model->capture_channels;
	runtime->hw.channels_max     = c->model->capture_channels;
	runtime->hw.buffer_bytes_max = buf;
	runtime->hw.period_bytes_min = period;
	runtime->hw.period_bytes_max = buf / 2;
	runtime->hw.periods_max      = buf / period;

	/* Pin the buffer to the full RX-ring size so the RX ring and ALSA ring share one geometry. */
	err = snd_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, buf, buf);
	if (err < 0)
		return err;
	/* Period must be a whole number of hardware periods (4-frame fragments). */
	return snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, period);
}

static int clarett_pcm_close(struct snd_pcm_substream *ss)
{
	struct clarett *c = snd_pcm_substream_chip(ss);

	clarett_engine_stop(c);
	c->pcm_sub = NULL;
	return 0;
}

/*
 * Disable the engine before the managed DMA buffer is freed. The core calls hw_free() ahead of
 * releasing the buffer; this guarantees clarett_pcm_tick() stops touching runtime->dma_area first.
 */
static int clarett_pcm_hw_free(struct snd_pcm_substream *ss)
{
	struct clarett *c = snd_pcm_substream_chip(ss);

	clarett_engine_stop(c);
	c->pcm_sub = NULL;
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
	int e_clk, e_en1, e_en2, e_commit, pushes = 0, push_err = 0, i;

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

	e_en1    = clarett_fcp(c, FCP_STREAM_ENABLE, NULL, 0);
	e_en2    = clarett_fcp(c, FCP_STREAM_ENABLE, NULL, 0);
	e_commit = clarett_fcp(c, FCP_STREAM_COMMIT, NULL, 0);

	dev_info(&c->pci->dev,
		 "stream-handshake: SET_CLOCK{%u,24}=%d CONFIG_PUSH=%d(err=%d) 0x6004=%d/%d 0x6005=%d\n",
		 rate, e_clk, pushes, push_err, e_en1, e_en2, e_commit);
}

/*
 * Arm the engine over the contiguous hardware buffer (block 0 = silent TX, block 1 = capture). The
 * descriptor tables are static (built once in create_pcm), so prepare just re-arms. Re-runnable: any
 * prior arming is torn down first. Runs in process context, so the mailbox arming is safe here — the
 * atomic trigger only flips stream_run.
 */
static int clarett_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct clarett *c = snd_pcm_substream_chip(ss);
	/* block 0 (TX) base, block 1 (RX) base — each points at its descriptor table. The RX table follows the
	 * whole TX ring (table + samples) in the one contiguous buffer, so r1 = r0 + TX ring bytes. */
	dma_addr_t r0 = c->stream_dma;
	dma_addr_t r1 = c->stream_dma + clarett_pcm_tx_ring(c);

	clarett_engine_stop(c);		/* idempotent: no-op unless a prior prepare armed it */

	c->pcm_frames = 0;
	c->pcm_last_period = 0;
	WRITE_ONCE(c->pcm_running, false);	/* no period delivery until trigger START */
	c->pcm_sub = ss;

	/* In-session stream-config handshake immediately before arming, matching the VM (stop -> handshake ->
	 * program regs -> arm). Establishes the device's buffer mode for this stream session. */
	clarett_stream_handshake(c, ss->runtime->rate);

	clarett_engine_arm(c, r0, r1);	/* full-duplex; same contiguous layout as the proven probe */

	if (pcm_selftest)
		clarett_selftest_poll(c, "prepare-arm");

	/*
	 * ACK from here, not from the trigger: the engine bursts immediately after arm and stalls within
	 * ms if unserviced, so the servicer must be ACKing 0x300 the instant it is armed. The trigger only
	 * gates whether completed periods are reported to ALSA (pcm_running).
	 */
	WRITE_ONCE(c->stream_run, true);
	clarett_engine_run(c);
	return 0;
}

/*
 * Atomic context (stream lock held): only flip the servicing gate. The engine is already armed and the
 * kthread running from prepare(); START releases the 0x300 ACK loop, STOP halts it. Full teardown
 * happens in close()/hw_free, which can sleep.
 */
static int clarett_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct clarett *c = snd_pcm_substream_chip(ss);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		c->pcm_last_period = div_u64(c->pcm_frames, ss->runtime->period_size);
		WRITE_ONCE(c->pcm_running, true);	/* begin reporting periods from the current position */
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		WRITE_ONCE(c->pcm_running, false);
		return 0;
	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t clarett_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct clarett *c = snd_pcm_substream_chip(ss);
	u64 frames = READ_ONCE(c->pcm_frames);

	/* frames % buffer_size, 64-bit-safe (do_div takes a u32 divisor; buffer_size fits easily). */
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
static void clarett_build_rings(struct clarett *c)
{
	size_t tbl     = clarett_pcm_tbl_bytes();
	size_t tx_ring = clarett_pcm_tx_ring(c);
	u32 tx_frag = clarett_frag_bytes(c->model->playback_channels);
	u32 rx_frag = clarett_frag_bytes(c->model->capture_channels);
	__le64 *tx_tbl = (__le64 *)c->stream_buf;
	__le64 *rx_tbl = (__le64 *)((u8 *)c->stream_buf + tx_ring);
	dma_addr_t tx_smp = c->stream_dma + tbl;
	dma_addr_t rx_smp = c->stream_dma + tx_ring + tbl;
	unsigned int i;

	for (i = 0; i < CLARETT_STREAM_NDESC; i++) {
		tx_tbl[i] = cpu_to_le64(tx_smp + (u64)i * tx_frag);
		rx_tbl[i] = cpu_to_le64(rx_smp + (u64)i * rx_frag);
	}
	tx_tbl[CLARETT_STREAM_NDESC - 1] |= cpu_to_le64(CLARETT_DESC_WRAP_TX);
	rx_tbl[CLARETT_STREAM_NDESC - 1] |= cpu_to_le64(CLARETT_DESC_WRAP_RX);
}

int clarett_create_pcm(struct clarett *c)
{
	struct snd_pcm *pcm;
	size_t buf = clarett_rx_ring_bytes(c);				/* ALSA buffer = RX sample ring */
	dma_addr_t r1off = clarett_pcm_tx_ring(c);			/* RX descriptor-table base offset */
	int err;

	err = snd_pcm_new(c->card, c->model->name, 0, 0, 1, &pcm);	/* 0 playback, 1 capture */
	if (err < 0)
		return err;

	pcm->private_data = c;
	strscpy(pcm->name, c->model->name, sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &clarett_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV, &c->pci->dev, buf, buf);
	c->pcm = pcm;

	c->stream_size = clarett_pcm_tx_ring(c) + clarett_pcm_rx_ring(c);
	c->stream_buf = dmam_alloc_coherent(&c->pci->dev, c->stream_size,
					    &c->stream_dma, GFP_KERNEL);
	if (!c->stream_buf)
		return -ENOMEM;
	clarett_build_rings(c);

	dev_info(&c->pci->dev,
		 "capture PCM registered (%uch S32_LE @%u, descriptor ring %zu B, frag tx=0x%x rx=0x%x @%pad)\n",
		 c->model->capture_channels, CLARETT_PCM_RATE, buf,
		 clarett_frag_bytes(c->model->playback_channels),
		 clarett_frag_bytes(c->model->capture_channels), &c->stream_dma);

	if (pcm_selftest) {
		clarett_engine_arm(c, c->stream_dma, c->stream_dma + r1off);
		clarett_selftest_poll(c, "probe-arm");
		clarett_engine_stop(c);
	}
	return 0;
}
