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
#include <sound/core.h>
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

	/* Latch interrupt causes (observed init value). We poll the cause register
	 * for mailbox completion rather than taking MSIs (see clarett_mailbox.c).
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
	snd_card_free(card);
	return err;
}

static void clarett_remove(struct pci_dev *pci)
{
	struct snd_card *card = pci_get_drvdata(pci);
	struct clarett *c = card->private_data;

	writel(0, c->bar0 + REG_IRQ0_ENABLE);	/* mask interrupts */
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
