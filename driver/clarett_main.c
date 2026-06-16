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
	 * vec3 (clarett_setup_irq). Enabling causes here only latches status — the
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

/*
 * MSI handler. One Linux IRQ per MSI vector, dispatched by vector index (dev_id).
 * Only vec3 (async notifications) is acted on; vec0 stays polled by clarett_fcp(),
 * so we deliberately do NOT read its cause register here (that would race the poll's
 * read-to-clear). vec1/vec2 are the data-plane period IRQs (not used yet). Every
 * vector returns IRQ_HANDLED so the core doesn't disable the MSI as spurious.
 */
static irqreturn_t clarett_irq(int irq, void *dev_id)
{
	struct clarett_irqctx *ic = dev_id;
	struct clarett *c = ic->c;

	if (ic->idx == CLARETT_VEC_NOTIFY) {
		u32 cause = readl(c->bar0 + REG_IRQ_CAUSE(CLARETT_VEC_NOTIFY)); /* read-to-clear */
		u32 ev = cause & (NOTIFY_DIM_MUTE | NOTIFY_MONITOR);

		if (ev) {
			atomic_or(ev, &c->notify_bits);
			schedule_work(&c->notify_work);
		}
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
		dma_rmb();	/* order the DMAed response before we read resp_buf */
		/*
		 * TODO(get-decode): the GET-response byte layout in resp_buf is not yet
		 * confirmed (raw config bytes at offset 0, vs an FCP header + data at
		 * offset 16). Once this hexdump resolves it on hardware, update
		 * c->shadow[24]/[28]/[112] from resp_buf here so "get" reflects the new
		 * physical state; until then it still returns the write-through shadow.
		 */
		print_hex_dump(KERN_INFO, "clarett monitor GET: ",
			       DUMP_PREFIX_OFFSET, 16, 1, c->resp_buf, 32, false);
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
	int err;

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

	err = clarett_create_controls(c);
	if (err)
		goto err_free;

	clarett_setup_irq(c);	/* best-effort; controls must exist first (snd_ctl_notify) */

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
	clarett_teardown_irq(c);		/* no-op unless setup_irq() succeeded */
	cancel_work_sync(&c->notify_work);
	snd_card_free(card);
	return err;
}

static void clarett_remove(struct pci_dev *pci)
{
	struct snd_card *card = pci_get_drvdata(pci);
	struct clarett *c = card->private_data;

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
