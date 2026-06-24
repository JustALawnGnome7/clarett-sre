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

/* Per-ring geometry. The ALSA buffer is pinned to exactly the RX sample-area size so the RX ring and
 * the ALSA ring share one geometry (256 descriptors, 1:1 byte offsets) — the per-period copy is then a
 * straight offset copy with no rescaling. */
#define CLARETT_BUF_BYTES   (CLARETT_STREAM_NDESC * CLARETT_STREAM_FRAG)        /* 256*448 = 114688 */
#define CLARETT_TBL_BYTES   ALIGN((CLARETT_STREAM_NDESC + 1) * sizeof(__le64), 64)
#define CLARETT_RING_BYTES  (CLARETT_TBL_BYTES + CLARETT_BUF_BYTES)             /* table + samples */
#define CLARETT_HWBUF_BYTES (2 * CLARETT_RING_BYTES)                            /* block 0 + block 1 */

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
	__le64 *rx_tbl = (__le64 *)((u8 *)c->stream_buf + CLARETT_RING_BYTES);
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

static const struct snd_pcm_hardware clarett_pcm_hw = {
	.info             = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
			    SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats          = SNDRV_PCM_FMTBIT_S32_LE,	/* 28ch interleaved, 24-bit MSB-justified */
	.rates            = SNDRV_PCM_RATE_48000,
	.rate_min         = CLARETT_PCM_RATE,
	.rate_max         = CLARETT_PCM_RATE,
	.channels_min     = CLARETT_PCM_CHANNELS,
	.channels_max     = CLARETT_PCM_CHANNELS,
	.buffer_bytes_max = CLARETT_BUF_BYTES,
	.period_bytes_min = CLARETT_STREAM_FRAG,	/* one descriptor fragment */
	.period_bytes_max = CLARETT_BUF_BYTES / 2,
	.periods_min      = 2,
	.periods_max      = CLARETT_STREAM_NDESC / 2,
};

/* Pointer to the block-1 (capture) sample area inside the contiguous hardware buffer. */
static u8 *clarett_rx_area(struct clarett *c)
{
	return (u8 *)c->stream_buf + CLARETT_RING_BYTES + CLARETT_TBL_BYTES;
}

/*
 * Called from the servicer kthread on every 0x300 period tick. Copies the freshly-captured descriptor
 * fragment from the RX area into the ALSA buffer (same geometry, 1:1 offsets), advances the modelled
 * frame position, and reports a period boundary to ALSA. No-op while idle/paused. Position is modelled,
 * not read back: 0x318 is a static status word (spec §9), so the period count is the only signal.
 */
void clarett_pcm_tick(struct clarett *c)
{
	struct snd_pcm_substream *ss = READ_ONCE(c->pcm_sub);
	struct snd_pcm_runtime *runtime;
	u64 period;
	unsigned int desc, off;

	if (!ss || !READ_ONCE(c->stream_run))
		return;
	runtime = ss->runtime;

	/* Descriptor that just completed (model: one descriptor per tick, see CLARETT_DESCS_PER_TICK). */
	desc = div_u64(c->pcm_frames, CLARETT_FRAMES_PER_DESC) % CLARETT_STREAM_NDESC;
	off  = desc * CLARETT_STREAM_FRAG;
	if (runtime->dma_area)
		memcpy(runtime->dma_area + off, clarett_rx_area(c) + off,
		       CLARETT_DESCS_PER_TICK * CLARETT_STREAM_FRAG);

	c->pcm_frames += CLARETT_FRAMES_PER_TICK;

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
	struct snd_pcm_runtime *runtime = ss->runtime;
	int err;

	runtime->hw = clarett_pcm_hw;

	/* Pin the buffer to the full RX-area size so RX ring and ALSA ring share one geometry. */
	err = snd_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
					   CLARETT_BUF_BYTES, CLARETT_BUF_BYTES);
	if (err < 0)
		return err;
	/* Period must be a whole number of descriptor fragments. */
	return snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
					 CLARETT_STREAM_FRAG);
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
 * Arm the engine over the contiguous hardware buffer (block 0 = silent TX, block 1 = capture). The
 * descriptor tables are static (built once in create_pcm), so prepare just re-arms. Re-runnable: any
 * prior arming is torn down first. Runs in process context, so the mailbox arming is safe here — the
 * atomic trigger only flips stream_run.
 */
static int clarett_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct clarett *c = snd_pcm_substream_chip(ss);
	dma_addr_t r0 = c->stream_dma;				/* block 0 (TX) table base */
	dma_addr_t r1 = c->stream_dma + CLARETT_RING_BYTES;	/* block 1 (RX) table base */

	clarett_engine_stop(c);		/* idempotent: no-op unless a prior prepare armed it */

	c->pcm_frames = 0;
	c->pcm_last_period = 0;
	WRITE_ONCE(c->pcm_running, false);	/* no period delivery until trigger START */
	c->pcm_sub = ss;

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

/* Build both static descriptor tables inside the contiguous hardware buffer (block 0 = TX, block 1 =
 * RX). Each entry is a bare 8-byte LE fragment bus address; the last carries the end-of-list/wrap flag
 * in bit 0; a zero terminator follows. dma_alloc_coherent returns zeroed memory, so the TX samples are
 * already silence and the terminators are already zero. */
static void clarett_build_rings(struct clarett *c)
{
	__le64 *tx_tbl = (__le64 *)c->stream_buf;
	__le64 *rx_tbl = (__le64 *)((u8 *)c->stream_buf + CLARETT_RING_BYTES);
	dma_addr_t tx_smp = c->stream_dma + CLARETT_TBL_BYTES;
	dma_addr_t rx_smp = c->stream_dma + CLARETT_RING_BYTES + CLARETT_TBL_BYTES;
	unsigned int i;

	for (i = 0; i < CLARETT_STREAM_NDESC; i++) {
		tx_tbl[i] = cpu_to_le64(tx_smp + (u64)i * CLARETT_STREAM_FRAG);
		rx_tbl[i] = cpu_to_le64(rx_smp + (u64)i * CLARETT_STREAM_FRAG);
	}
	tx_tbl[CLARETT_STREAM_NDESC - 1] |= cpu_to_le64(1);	/* end-of-list / wrap flag */
	rx_tbl[CLARETT_STREAM_NDESC - 1] |= cpu_to_le64(1);

	/*
	 * Pre-fill the RX sample area with 0xAA, exactly as the proven engine_start does. This is the lone
	 * difference between engine_start (which clocks) and the original create_pcm arm (which did not),
	 * so it appears to be functionally required for the engine to start raising periods — not just a
	 * capture-detection marker. [WHY: not yet understood; confirm empirically.]
	 */
	memset(clarett_rx_area(c), 0xAA, CLARETT_BUF_BYTES);
}

int clarett_create_pcm(struct clarett *c)
{
	struct snd_pcm *pcm;
	int err;

	err = snd_pcm_new(c->card, "Clarett 8PreX", 0, 0, 1, &pcm);	/* 0 playback, 1 capture */
	if (err < 0)
		return err;

	pcm->private_data = c;
	strscpy(pcm->name, "Clarett 8PreX", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &clarett_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV, &c->pci->dev,
				       CLARETT_BUF_BYTES, CLARETT_BUF_BYTES);
	c->pcm = pcm;

	c->stream_size = CLARETT_HWBUF_BYTES;
	c->stream_buf = dmam_alloc_coherent(&c->pci->dev, c->stream_size,
					    &c->stream_dma, GFP_KERNEL);
	if (!c->stream_buf)
		return -ENOMEM;
	clarett_build_rings(c);

	dev_info(&c->pci->dev,
		 "capture PCM registered (28ch S32_LE @%u, %u-desc ring @%pad)\n",
		 CLARETT_PCM_RATE, CLARETT_STREAM_NDESC, &c->stream_dma);

	if (pcm_selftest) {
		clarett_engine_arm(c, c->stream_dma, c->stream_dma + CLARETT_RING_BYTES);
		clarett_selftest_poll(c, "probe-arm");
		clarett_engine_stop(c);
	}

	/*
	 * Diagnostic A/B: arm the engine HERE (probe-time, before snd_card_register and before any
	 * userspace mixer activity — exactly where the working stream_probe arms) and poll. Compared with
	 * the "prepare-arm" poll, this isolates whether arm-ordering is what makes the engine clock. The
	 * engine is stopped afterwards so normal prepare-time arming still drives operation.
	 */
	if (pcm_selftest) {
		clarett_engine_arm(c, c->stream_dma, c->stream_dma + CLARETT_RING_BYTES);
		clarett_selftest_poll(c, "probe-arm");
		clarett_engine_stop(c);
	}
	return 0;
}
