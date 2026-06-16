// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clarett 8PreX — FCP mailbox transport.
 *
 * One transaction (confirmed from trace): ack the previous completion via the
 * doorbell, fill the request mailbox (cmd/size+seq/error/pad/data), ring the
 * doorbell, then wait for the DONE bit in the IRQ-0 cause register. We poll the
 * cause register rather than taking MSIs — the data-plane work will add proper
 * MSI handling; for the (infrequent) control plane, polling is simplest and
 * avoids racing the read-to-clear cause register against an ISR.
 */
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include "clarett.h"

int clarett_fcp(struct clarett *c, u32 opcode, const u8 *data, u16 len)
{
	void __iomem *mb = c->bar0 + REG_MBOX;
	unsigned long deadline;
	u32 cause = 0, first_cause = 0, fcperr = 0;
	int i, ret = 0, polls = 0;

	if (len > CLARETT_MAX_PAYLOAD)
		return -EINVAL;

	mutex_lock(&c->mbox_lock);

	writel(DOORBELL_ACK, c->bar0 + REG_DOORBELL);

	writel(CMD_EXEC_FLAG | opcode, mb + MBOX_CMD);
	writel(((u32)c->seq << 16) | len, mb + MBOX_SIZESEQ);
	writel(0, mb + MBOX_ERROR);
	writel(0, mb + MBOX_PAD);

	/* payload, written as little-endian 32-bit words (last word zero-padded) */
	for (i = 0; i < len; i += 4) {
		u32 w = 0;
		int j;

		for (j = 0; j < 4 && i + j < len; j++)
			w |= (u32)data[i + j] << (8 * j);
		writel(w, mb + MBOX_DATA + i);
	}

	writel(DOORBELL_SUBMIT, c->bar0 + REG_DOORBELL);

	deadline = jiffies + msecs_to_jiffies(CLARETT_MBOX_TIMEOUT_MS);
	do {
		cause = readl(c->bar0 + REG_IRQ0_CAUSE);   /* read-to-clear */
		if (!polls)
			first_cause = cause;
		polls++;
		if (cause & IRQ_DONE_BIT)
			break;
		cpu_relax();
	} while (time_before(jiffies, deadline));

	fcperr = readl(mb + MBOX_ERROR);

	/* TODO(debug): remove once mailbox completion is trusted on hardware. */
	dev_info(&c->pci->dev,
		 "FCP op=0x%06x seq=%u first_cause=0x%08x polls=%d done=%d fcperr=0x%08x\n",
		 opcode, c->seq, first_cause, polls, !!(cause & IRQ_DONE_BIT), fcperr);

	if (!(cause & IRQ_DONE_BIT))
		ret = -ETIMEDOUT;
	else if (fcperr)
		ret = -EIO;

	c->seq++;
	mutex_unlock(&c->mbox_lock);
	return ret;
}

/* GET_DATA: request `len` bytes from config `offset`. The response is DMAed into
 * the buffer programmed at REG_DMA_ADDR_LO/HI (c->resp_buf), not returned via MMIO;
 * its byte layout is not yet decoded, so callers currently only dump it. */
int clarett_get_data(struct clarett *c, u32 offset, u32 len)
{
	u8 buf[8];

	clarett_put_le32(buf, offset);
	clarett_put_le32(buf + 4, len);
	return clarett_fcp(c, FCP_GET_DATA, buf, 8);
}

/* SET_DATA: write `len` bytes into the config space at `offset`. */
int clarett_set_data(struct clarett *c, u32 offset, u32 len, const u8 *val)
{
	u8 buf[8 + CLARETT_MAX_PAYLOAD];

	if (len > CLARETT_MAX_PAYLOAD)
		return -EINVAL;
	clarett_put_le32(buf, offset);
	clarett_put_le32(buf + 4, len);
	memcpy(buf + 8, val, len);
	return clarett_fcp(c, FCP_SET_DATA, buf, 8 + len);
}

/* DATA_CMD: commit a preceding SET_DATA (activate == the XML control "command"). */
int clarett_data_cmd(struct clarett *c, u32 activate)
{
	u8 buf[4];

	clarett_put_le32(buf, activate);
	return clarett_fcp(c, FCP_DATA_CMD, buf, 4);
}

/* Convenience: write a single config byte and commit it. */
int clarett_write_u8(struct clarett *c, u32 offset, u8 val, u32 activate)
{
	int err;

	if (offset >= CLARETT_CONFIG_SIZE)
		return -EINVAL;

	err = clarett_set_data(c, offset, 1, &val);
	if (err)
		return err;
	err = clarett_data_cmd(c, activate);
	if (err)
		return err;

	c->shadow[offset] = val;
	return 0;
}
