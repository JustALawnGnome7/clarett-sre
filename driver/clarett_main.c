// SPDX-License-Identifier: GPL-2.0-only
/*
 * Focusrite Clarett 8PreX (Thunderbolt) ALSA driver — PCI bring-up.
 *
 * Status: control plane only. Creates a mixer-only sound card. PCM/streaming
 * (the data plane) is not implemented yet — see TODO at the end.
 *
 * Reverse-engineering provenance: the spec notes under ../spec (clean-room, from
 * MMIO traces and Focusrite's own device XML; no vendor driver code was used).
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/control.h>
#include <sound/initval.h>
#include "clarett.h"

#define PCI_VENDOR_FOCUSRITE   0x1cb5
#define PCI_DEVICE_CLARETT     0x0002

static bool stream_probe;
module_param(stream_probe, bool, 0444);
MODULE_PARM_DESC(stream_probe,
		 "Data-plane experiment: after bring-up, program the §3b ring registers with a driver "
		 "buffer and watch for vec1/vec2 IRQs + DMA-pointer movement (off by default).");

static bool blk1_only;
module_param(blk1_only, bool, 0444);
MODULE_PARM_DESC(blk1_only,
		 "Engine-probe isolation: configure ONLY ring block 1 (0x300, capture) and enable it via "
		 "0x30c, leaving block 0 (0x200) untouched. Isolates whether the capture writes are block 1's.");

static void clarett_hw_init(struct clarett *c)
{
	void __iomem *bar = c->bar0;

	c->serial_lo = readl(bar + REG_SERIAL_LO);
	c->serial_hi = readl(bar + REG_SERIAL_HI);
	c->fw_app    = readl(bar + REG_INFO + 0);
	c->fw_fpga   = readl(bar + REG_INFO + 4);

	/*
	 * Program the GET-response DMA buffer address (init trace wrote
	 * REG_DMA_ADDR_LO/HI). REG_DMA_ADDR_HI is the *high 32 bits* of the bus
	 * address: confirmed when hardcoding the trace's 0x2 caused an IOMMU
	 * IO_PAGE_FAULT at 0x2_xxxxxxxx (that 0x2 was the Windows buffer's high
	 * bits, not a flag).
	 */
	writel(lower_32_bits(c->resp_dma), bar + REG_DMA_ADDR_LO);
	writel(upper_32_bits(c->resp_dma), bar + REG_DMA_ADDR_HI);

	/* Latch interrupt causes (observed init value). Mailbox completion is still
	 * polled (clarett_mailbox.c); MSI is used only for async notifications on
	 * vec0 (clarett_setup_irq). Enabling causes here only latches status — the
	 * device cannot raise MSI until pci_alloc_irq_vectors() configures it.
	 */
	writel(0xf000003f, bar + REG_IRQ0_ENABLE);

	memset(c->shadow, 0, sizeof(c->shadow));

	/*
	 * TODO: the firmware init handshake observed at boot (INIT_2 plus the
	 * 0x5000/0x6000/0x7000 command sequence) is not yet decoded. The mailbox
	 * accepts config commands without replaying it in testing, but a robust
	 * bring-up probably needs to understand/replay that sequence.
	 */
}

/* Generated bring-up replay table: clarett_init_blob[] + clarett_init_seq[]. */
#include "clarett_init_seq.h"

/*
 * Replay the vendor device bring-up captured at attach from a freshly power-cycled device
 * (clarett_full_init_mute.log), regenerated into clarett_init_seq.h by `fcp_decode.py --emit-init`:
 * every non-meter command up to the monitor-mute write, minus the bulk 8 KB config read/writeback.
 * Self-boot does NOT arm config access (GET_DATA fails); this host init arms it. Must run against a
 * device in its fresh power-on state — re-initializing an already-armed device wedges it instead.
 * Best-effort: failures are logged and the sequence continues.
 */
static int clarett_arm_device(struct clarett *c)
{
	int i, err, fails = 0;

	for (i = 0; i < ARRAY_SIZE(clarett_init_seq); i++) {
		const struct clarett_init_step *s = &clarett_init_seq[i];

		err = clarett_fcp(c, s->opcode, clarett_init_blob + s->off, s->len);
		if (err) {
			dev_warn(&c->pci->dev, "arm[%d] op 0x%06x failed: %d\n",
				 i, s->opcode, err);
			fails++;
		}
	}
	if (fails)
		dev_warn(&c->pci->dev, "arm: %d/%zu steps failed\n",
			 fails, ARRAY_SIZE(clarett_init_seq));
	return 0;
}

/*
 * Seed the config shadow from the device. clarett_hw_init() zeroes the shadow, but the
 * command-3 enable bytes (72/73) pack one bit per output, so toggling the monitor outputs'
 * bits needs the real current bytes for a safe read-modify-write. GET the monitoring region
 * (offset 24, 92 bytes — covers 24/28/52/72-74/112) and copy the DMAed response in. The first
 * GET after programming the DMA address can come back empty (echo word 0), so retry briefly.
 */
static int clarett_seed_shadow(struct clarett *c)
{
	const u8 *r = c->resp_buf;
	u32 echo;
	int err, attempt, i;

	for (attempt = 0; attempt < 3; attempt++) {
		err = clarett_get_data(c, MONITOR_CFG_OFFSET, MONITOR_CFG_LEN);
		if (err)
			return err;

		dma_rmb();	/* order the DMAed response before we read resp_buf */
		echo = r[FCP_RESP_ECHO_OFF] | r[FCP_RESP_ECHO_OFF + 1] << 8 |
		       r[FCP_RESP_ECHO_OFF + 2] << 16 | r[FCP_RESP_ECHO_OFF + 3] << 24;
		if (echo == (CMD_EXEC_FLAG | FCP_GET_DATA)) {
			for (i = 0; i < MONITOR_CFG_LEN; i++)
				c->shadow[MONITOR_CFG_OFFSET + i] = r[FCP_RESP_DATA_OFF + i];
			return 0;
		}
	}
	return -EIO;
}

/*
 * Make the two monitor outputs follow the monitor section's Mute/Dim. Without these command-3
 * enable bits, writing the global Mute (24) / Dim (28) flips the master flag but no output obeys
 * it (control-plane §5). RMW from the seeded shadow so the other outputs' enable bits are kept;
 * idempotent — clarett_write_bits() no-ops if the bits are already set.
 */
static int clarett_enable_monitor_hw_controls(struct clarett *c)
{
	int err;

	err = clarett_write_bits(c, HWEN_MUTE_OFFSET, HWEN_MONITOR_MUTE_MASK,
				 HWEN_MONITOR_MUTE_MASK, HWEN_ACTIVATE);
	if (err)
		return err;
	return clarett_write_bits(c, HWEN_DIM_OFFSET, HWEN_MONITOR_DIM_MASK,
				  HWEN_MONITOR_DIM_MASK, HWEN_ACTIVATE);
}

/*
 * Carve the coherent buffer into two identical rings (block 0 = TX, block 1 = RX). Each ring is a
 * zero-terminated descriptor table followed by its sample fragments; *ring is one ring's byte span,
 * so block 1 begins at offset *ring. Same math in start and report so they agree.
 */
static void clarett_ring_layout(size_t *tbl, size_t *smp, size_t *ring)
{
	*tbl = ALIGN((CLARETT_STREAM_NDESC + 1) * sizeof(__le64), 64);
	*smp = CLARETT_STREAM_NDESC * CLARETT_STREAM_FRAG;
	*ring = *tbl + *smp;
}

/* 1 s after engine-start, log whether DMA advanced: period IRQs, pointer regs, capture-buffer writes. */
static void clarett_stream_report(struct work_struct *work)
{
	struct clarett *c = container_of(work, struct clarett, stream_report.work);
	size_t tbl, smp, ring, i;
	const u8 *rx_smp;
	bool rx_data = false;

	clarett_ring_layout(&tbl, &smp, &ring);
	rx_smp = (const u8 *)c->stream_buf + ring + tbl;	/* block-1 (capture) sample area */
	for (i = 0; i < smp; i++) {
		if (rx_smp[i] != 0xAA) {	/* any byte the device overwrote (incl. zeros) */
			rx_data = true;
			break;
		}
	}

	dev_info(&c->pci->dev,
		 "engine probe @1s: vec1=%d vec2=%d IRQs; ptr0=0x%x ptr1=0x%x; capture-buf=%s "
		 "(0xAA marker overwritten => device wrote the RX buffer, even with silence)\n",
		 atomic_read(&c->period_irqs[1]), atomic_read(&c->period_irqs[2]),
		 readl(c->bar0 + STREAM_BLK0 + STREAM_OFF_PTR),
		 readl(c->bar0 + STREAM_BLK1 + STREAM_OFF_PTR),
		 rx_data ? "WRITTEN (marker gone)" : "untouched (marker intact)");
}

/*
 * Data-plane engine-start probe (data-plane spec §9, opt-in via stream_probe). Replays the captured
 * §3b stream-start register sequence, but now with a valid descriptor table per §3c: 0x210/0x214 point
 * at a zeroed-terminated array of 8-byte bus addresses, each naming one STREAM_SIZE_VAL fragment of our
 * coherent buffer. Then watches whether the engine runs (vec1/vec2 IRQs, advancing pointer, the device
 * writing the capture buffer). NOT a PCM implementation. The point is to test whether starting the
 * engine makes the control plane physically manifest (e.g. the Mute LED).
 */
static int clarett_engine_start(struct clarett *c)
{
	void __iomem *bar = c->bar0;
	size_t tbl, smp, ring;
	__le64 *tx_tbl, *rx_tbl;
	dma_addr_t tx_smp, rx_smp, r0, r1;
	unsigned int i;

	clarett_ring_layout(&tbl, &smp, &ring);
	c->stream_size = 2 * ring;
	c->stream_buf = dmam_alloc_coherent(&c->pci->dev, c->stream_size,
					    &c->stream_dma, GFP_KERNEL);
	if (!c->stream_buf)
		return -ENOMEM;

	/* Block 0 (vec1) = playback/TX, block 1 (vec2) = capture/RX (spec §3c). */
	tx_tbl = (__le64 *)c->stream_buf;
	rx_tbl = (__le64 *)((u8 *)c->stream_buf + ring);
	tx_smp = c->stream_dma + tbl;
	rx_smp = c->stream_dma + ring + tbl;

	/* Fill descriptors with fragment bus addresses; entry [NDESC] stays 0 = ring terminator. */
	for (i = 0; i < CLARETT_STREAM_NDESC; i++) {
		tx_tbl[i] = cpu_to_le64(tx_smp + (dma_addr_t)i * CLARETT_STREAM_FRAG);
		rx_tbl[i] = cpu_to_le64(rx_smp + (dma_addr_t)i * CLARETT_STREAM_FRAG);
	}

	/*
	 * Mark the RX (capture) sample area with 0xAA so the report can tell "device wrote silence (zeros)"
	 * from "device never wrote" — with nothing plugged in, a working capture writes near-zero samples
	 * that a plain non-zero scan would miss.
	 */
	memset((u8 *)c->stream_buf + ring + tbl, 0xAA, smp);

	r0 = c->stream_dma;		/* block-0 descriptor-table base */
	r1 = c->stream_dma + ring;	/* block-1 descriptor-table base */

	writel(0x10, bar + REG_STREAM_IRQ_CFG);				/* 0x108 */

	/*
	 * Program the descriptor-table base(s) BEFORE enabling. The Windows capture wrote the enable
	 * first, but that device kept a non-zero base latched from a prior run; on our freshly-armed
	 * device the base is 0, so enabling first made the engine fetch the table from address 0
	 * (IO_PAGE_FAULT @0x0). Base-then-enable avoids that.
	 */
	if (!blk1_only) {
		writel(STREAM_CHANS, bar + STREAM_BLK0 + STREAM_OFF_CHANS);	/* 0x204 = 28 */
		writel(STREAM_SIZE_VAL, bar + STREAM_BLK0 + STREAM_OFF_SIZE);	/* 0x208 */
		writel(upper_32_bits(r0), bar + STREAM_BLK0 + STREAM_OFF_BASE_HI); /* 0x214 */
		writel(lower_32_bits(r0), bar + STREAM_BLK0 + STREAM_OFF_BASE_LO); /* 0x210 (low last) */
	}

	writel(STREAM_CHANS, bar + STREAM_BLK1 + STREAM_OFF_CHANS);	/* 0x304 */
	writel(STREAM_SIZE_VAL, bar + STREAM_BLK1 + STREAM_OFF_SIZE);	/* 0x308 */
	writel(upper_32_bits(r1), bar + STREAM_BLK1 + STREAM_OFF_BASE_HI);	/* 0x314 */
	writel(lower_32_bits(r1), bar + STREAM_BLK1 + STREAM_OFF_BASE_LO);	/* 0x310 */

	/*
	 * Enable via 0x20c — the sole *global* enable (0x30c is not a per-block enable; writing it reads
	 * back 0). In blk1_only mode block 0's base is left null (skipped above), so after the global
	 * enable block 0 read-faults at 0 (flags=0) while block 1 writes capture: the fault flags then
	 * attribute the write-to-null (flags=0x20) to block 1's engine specifically.
	 */
	writel(1, bar + STREAM_BLK0 + STREAM_OFF_CTRL);			/* 0x20c global enable */

	writel(0x1e70700, bar + REG_STREAM_IRQ_CFG2);			/* 0x10c */
	writel(0x7, bar + REG_STREAM_IRQ_ARM);				/* 0x110 arm */
	/*
	 * NB: the capture's 0x110=0x0 lands 13 s later (stream-stop), not as a pulse here — writing it
	 * now would disarm the period IRQs immediately. clarett_engine_stop() issues the 0x0 at teardown.
	 */

	c->stream_on = true;
	dev_info(&c->pci->dev,
		 "engine probe: started; %u-desc tables @ %pad / %pad, ptr0=0x%x ptr1=0x%x\n",
		 CLARETT_STREAM_NDESC, &r0, &r1,
		 readl(bar + STREAM_BLK0 + STREAM_OFF_PTR),
		 readl(bar + STREAM_BLK1 + STREAM_OFF_PTR));
	/* Diagnostic: did the base latch, and what does the device see at descriptor[0]? */
	dev_info(&c->pci->dev,
		 "engine regs: blk0 base=%08x:%08x ctrl=%08x ptr=%08x | blk1 base=%08x:%08x ctrl=%08x ptr=%08x | "
		 "tx_desc[0]=%016llx rx_desc[0]=%016llx\n",
		 readl(bar + STREAM_BLK0 + STREAM_OFF_BASE_HI),
		 readl(bar + STREAM_BLK0 + STREAM_OFF_BASE_LO),
		 readl(bar + STREAM_BLK0 + STREAM_OFF_CTRL),
		 readl(bar + STREAM_BLK0 + STREAM_OFF_PTR),
		 readl(bar + STREAM_BLK1 + STREAM_OFF_BASE_HI),
		 readl(bar + STREAM_BLK1 + STREAM_OFF_BASE_LO),
		 readl(bar + STREAM_BLK1 + STREAM_OFF_CTRL),
		 readl(bar + STREAM_BLK1 + STREAM_OFF_PTR),
		 le64_to_cpu(tx_tbl[0]), le64_to_cpu(rx_tbl[0]));
	schedule_delayed_work(&c->stream_report, msecs_to_jiffies(1000));
	return 0;
}

/* Halt the engine before the ring buffer is freed (a bad/continued DMA would fault the IOMMU). */
static void clarett_engine_stop(struct clarett *c)
{
	if (!c->stream_on)
		return;
	cancel_delayed_work_sync(&c->stream_report);
	writel(0, c->bar0 + STREAM_BLK0 + STREAM_OFF_CTRL);	/* disable ring 0 */
	writel(0, c->bar0 + STREAM_BLK1 + STREAM_OFF_CTRL);	/* disable ring 1 (blk1_only path) */
	writel(0, c->bar0 + REG_STREAM_IRQ_ARM);
	writel(0, c->bar0 + REG_STREAM_IRQ_CFG);
	readl(c->bar0 + STREAM_BLK0 + STREAM_OFF_CTRL);		/* flush posted writes */
	c->stream_on = false;
}

/*
 * MSI handler. One Linux IRQ per MSI vector, dispatched by vector index (dev_id).
 * The device signals control-plane events on vec0; we check the notification cause
 * (0x400) there. We must NOT read the mailbox cause (0x100) — that is read-to-clear
 * and racing clarett_fcp()'s poll would make mailbox commands time out (mailbox
 * completion stays polled). vec1/vec2 are the data-plane period-IRQ suspects — when the
 * engine-start probe is active we just count them (MSI is edge-triggered, so not clearing a
 * cause register can't storm); reading the block cause reg (0x200/0x300) is harmless observation.
 * Every vector returns IRQ_HANDLED so the core doesn't disable the MSI as spurious.
 */
static irqreturn_t clarett_irq(int irq, void *dev_id)
{
	struct clarett_irqctx *ic = dev_id;
	struct clarett *c = ic->c;

	if (ic->idx == CLARETT_VEC_EVENT) {
		u32 cause = readl(c->bar0 + REG_NOTIFY_CAUSE);	/* 0x400, read-to-clear */
		u32 ev = cause & (NOTIFY_DIM_MUTE | NOTIFY_MONITOR);

		if (ev) {
			atomic_or(ev, &c->notify_bits);
			schedule_work(&c->notify_work);
		}
	} else if (ic->idx == 1 || ic->idx == 2) {	/* data-plane period IRQs (probe) */
		readl(c->bar0 + (ic->idx == 1 ? STREAM_BLK0 : STREAM_BLK1));   /* read-to-clear/observe */
		atomic_inc(&c->period_irqs[ic->idx]);
	}
	return IRQ_HANDLED;
}

/*
 * On a notification (front-panel button), mirror the vendor flow: re-read the
 * monitor config region, then tell userspace the monitor controls may have changed.
 */
static void clarett_notify_work(struct work_struct *work)
{
	struct clarett *c = container_of(work, struct clarett, notify_work);
	u32 ev = atomic_xchg(&c->notify_bits, 0);
	int i, err;

	if (!ev)
		return;

	err = clarett_get_data(c, MONITOR_CFG_OFFSET, MONITOR_CFG_LEN);
	if (err) {
		dev_warn(&c->pci->dev, "notify 0x%x: monitor re-read failed (%d)\n",
			 ev, err);
	} else {
		const u8 *r = c->resp_buf;
		u32 echo;

		dma_rmb();	/* order the DMAed response before we read resp_buf */
		echo = r[FCP_RESP_ECHO_OFF] | r[FCP_RESP_ECHO_OFF + 1] << 8 |
		       r[FCP_RESP_ECHO_OFF + 2] << 16 | r[FCP_RESP_ECHO_OFF + 3] << 24;

		if (echo == (CMD_EXEC_FLAG | FCP_GET_DATA)) {
			const u8 *data = r + FCP_RESP_DATA_OFF;

			/* data[i] == config[MONITOR_CFG_OFFSET + i] */
			c->shadow[24]  = data[24  - MONITOR_CFG_OFFSET];
			c->shadow[28]  = data[28  - MONITOR_CFG_OFFSET];
			c->shadow[112] = data[112 - MONITOR_CFG_OFFSET];
		} else {
			/* No response DMAed (e.g. the first GET at load) — keep the shadow. */
			dev_dbg(&c->pci->dev,
				"notify 0x%x: no GET response (echo=0x%08x)\n", ev, echo);
		}
	}

	for (i = 0; i < c->n_ctls; i++)
		if (c->ctls[i].activate == MONITOR_ACTIVATE && c->ctls[i].kctl)
			snd_ctl_notify(c->card, SNDRV_CTL_EVENT_MASK_VALUE,
				       &c->ctls[i].kctl->id);

	dev_dbg(&c->pci->dev, "async notification handled: 0x%x\n", ev);
}

/* Enable MSI and hook the notification vector. Best-effort: on failure the driver
 * still works (control plane, polled mailbox) but without async notifications. */
static void clarett_setup_irq(struct clarett *c)
{
	struct pci_dev *pci = c->pci;
	int i, nvec, err;

	nvec = pci_alloc_irq_vectors(pci, CLARETT_NUM_VECTORS, CLARETT_NUM_VECTORS,
				     PCI_IRQ_MSI);
	if (nvec < 0) {
		dev_warn(&pci->dev,
			 "MSI alloc failed (%d); async notifications disabled\n", nvec);
		return;
	}

	for (i = 0; i < CLARETT_NUM_VECTORS; i++) {
		c->irq_ctx[i].c = c;
		c->irq_ctx[i].idx = i;
		err = request_irq(pci_irq_vector(pci, i), clarett_irq, 0,
				  KBUILD_MODNAME, &c->irq_ctx[i]);
		if (err) {
			dev_warn(&pci->dev,
				 "request_irq vec %d failed (%d); notifications disabled\n",
				 i, err);
			while (--i >= 0)
				free_irq(pci_irq_vector(pci, i), &c->irq_ctx[i]);
			pci_free_irq_vectors(pci);
			return;
		}
	}
	c->irq_ready = true;
}

static void clarett_teardown_irq(struct clarett *c)
{
	int i;

	if (!c->irq_ready)
		return;
	for (i = 0; i < CLARETT_NUM_VECTORS; i++)
		free_irq(pci_irq_vector(c->pci, i), &c->irq_ctx[i]);
	pci_free_irq_vectors(c->pci);
	c->irq_ready = false;
}

static int clarett_probe(struct pci_dev *pci, const struct pci_device_id *ent)
{
	struct snd_card *card;
	struct clarett *c;
	int err, seeded;

	err = snd_card_new(&pci->dev, SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1,
			   THIS_MODULE, sizeof(*c), &card);
	if (err < 0)
		return err;

	c = card->private_data;
	c->card = card;
	c->pci = pci;
	mutex_init(&c->mbox_lock);
	INIT_WORK(&c->notify_work, clarett_notify_work);
	atomic_set(&c->notify_bits, 0);
	INIT_DELAYED_WORK(&c->stream_report, clarett_stream_report);
	atomic_set(&c->period_irqs[1], 0);
	atomic_set(&c->period_irqs[2], 0);

	err = pcim_enable_device(pci);
	if (err)
		goto err_free;

	err = pcim_iomap_regions(pci, BIT(CLARETT_BAR), KBUILD_MODNAME);
	if (err)
		goto err_free;
	c->bar0 = pcim_iomap_table(pci)[CLARETT_BAR];

	pci_set_master(pci);

	err = dma_set_mask_and_coherent(&pci->dev, DMA_BIT_MASK(32));
	if (err)
		goto err_free;

	c->resp_size = PAGE_SIZE;
	c->resp_buf = dmam_alloc_coherent(&pci->dev, c->resp_size,
					  &c->resp_dma, GFP_KERNEL);
	if (!c->resp_buf) {
		err = -ENOMEM;
		goto err_free;
	}

	clarett_hw_init(c);

	/* Arm the device (full vendor bring-up). Required on a freshly power-cycled device:
	 * self-boot alone leaves config access (GET_DATA) and config-apply disabled. */
	clarett_arm_device(c);

	/* Seed the shadow from the device so mixer "get" reflects real state at load and the
	 * enable-byte RMW below is safe. Best-effort: if it fails we skip the enable writes. */
	seeded = clarett_seed_shadow(c);
	if (seeded)
		dev_warn(&pci->dev,
			 "config shadow seed failed (%d); leaving hardware mute/dim enables untouched\n",
			 seeded);

	err = clarett_create_controls(c);
	if (err)
		goto err_free;

	/* Make the global Mute/Dim controls actually affect Monitor Out 1-2. Needs the seeded
	 * shadow for a correct read-modify-write, so only attempt it when seeding succeeded. */
	if (!seeded) {
		err = clarett_enable_monitor_hw_controls(c);
		if (err)
			dev_warn(&pci->dev,
				 "could not enable monitor hardware mute/dim (%d)\n", err);
	}

	clarett_setup_irq(c);	/* best-effort; controls must exist first (snd_ctl_notify) */

	/* Opt-in data-plane experiment: start the audio engine and watch what happens. Best-effort;
	 * needs the IRQ handlers (above) hooked first so vec1/vec2 period IRQs are counted. */
	if (stream_probe && c->irq_ready) {
		err = clarett_engine_start(c);
		if (err)
			dev_warn(&pci->dev, "engine-start probe failed (%d)\n", err);
		err = 0;
	}

	strscpy(card->driver, "Clarett8PreX", sizeof(card->driver));
	strscpy(card->shortname, "Focusrite Clarett 8PreX", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "%s at %s, fw app 0x%08x", card->shortname, pci_name(pci),
		 c->fw_app);

	err = snd_card_register(card);
	if (err)
		goto err_free;

	pci_set_drvdata(pci, card);
	dev_info(&pci->dev,
		 "Clarett 8PreX: serial %08x%08x fw app 0x%08x fpga 0x%08x\n",
		 c->serial_hi, c->serial_lo, c->fw_app, c->fw_fpga);
	return 0;

err_free:
	clarett_engine_stop(c);			/* halt DMA before the ring buffer is freed */
	clarett_teardown_irq(c);		/* no-op unless setup_irq() succeeded */
	cancel_work_sync(&c->notify_work);
	snd_card_free(card);
	return err;
}

static void clarett_remove(struct pci_dev *pci)
{
	struct snd_card *card = pci_get_drvdata(pci);
	struct clarett *c = card->private_data;

	clarett_engine_stop(c);			/* halt streaming DMA before the buffer is freed */
	writel(0, c->bar0 + REG_IRQ0_ENABLE);	/* mask causes before freeing handlers */
	clarett_teardown_irq(c);		/* free MSI vectors / IRQ handlers */
	cancel_work_sync(&c->notify_work);	/* flush any in-flight notification work */
	snd_card_free(card);
	/* BAR mapping, DMA buffer and device enable are devres-managed */
}

static const struct pci_device_id clarett_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_FOCUSRITE, PCI_DEVICE_CLARETT) },
	{ }
};
MODULE_DEVICE_TABLE(pci, clarett_ids);

static struct pci_driver clarett_driver = {
	.name = KBUILD_MODNAME,
	.id_table = clarett_ids,
	.probe = clarett_probe,
	.remove = clarett_remove,
};
module_pci_driver(clarett_driver);

MODULE_DESCRIPTION("Focusrite Clarett 8PreX (Thunderbolt) — control plane");
MODULE_AUTHOR("Clarett RE project");
MODULE_LICENSE("GPL");
