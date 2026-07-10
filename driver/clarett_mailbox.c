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
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/string.h>
#include "clarett.h"

/*
 * Manifestation-wall A/B — per-command response-buffer hygiene. The device provably reacts
 * to host DMA-buffer *contents* (data-plane §9: the 0xAA RX pre-fill was the lone difference
 * that made the engine clock), and this buffer is the only host address the device knows at
 * init. FC's 4 KB common buffer arrives zeroed from Windows; ours is zeroed at alloc but then
 * holds the previous response between commands. -1 = leave it alone (baseline); 0..255 = fill
 * the whole buffer with that byte before every submit (0 mirrors FC's fresh common buffer;
 * 170/0xAA restores the §5a emptiness instrument — any byte still 0xAA after completion was
 * not written by the device).
 */
static int resp_prefill = -1;
module_param(resp_prefill, int, 0444);
MODULE_PARM_DESC(resp_prefill,
	"Fill the response DMA buffer with this byte before each command (-1=off/baseline, 0=zero like FC's fresh buffer, 170=0xAA emptiness marker).");

int clarett_fcp(struct clarett *c, u32 opcode, const u8 *data, u16 len)
{
	unsigned long deadline;
	u32 cause = 0, first_cause = 0, fcperr = 0;
	int i, ret = 0, polls = 0;

	if (len > CLARETT_MBOX_DATA_MAX)
		return -EINVAL;

	mutex_lock(&c->mbox_lock);

	clarett_wl(c, REG_DOORBELL, DOORBELL_ACK);

	if (resp_prefill >= 0) {
		memset(c->resp_buf, resp_prefill & 0xff, c->resp_size);
		dma_wmb();	/* fill visible to the device before the doorbell submit */
	}

	clarett_wl(c, REG_MBOX + MBOX_CMD, CMD_EXEC_FLAG | opcode);
	clarett_wl(c, REG_MBOX + MBOX_SIZESEQ, ((u32)c->seq << 16) | len);
	clarett_wl(c, REG_MBOX + MBOX_ERROR, 0);
	clarett_wl(c, REG_MBOX + MBOX_PAD, 0);

	/* payload, written as little-endian 32-bit words (last word zero-padded) */
	for (i = 0; i < len; i += 4) {
		u32 w = 0;
		int j;

		for (j = 0; j < 4 && i + j < len; j++)
			w |= (u32)data[i + j] << (8 * j);
		clarett_wl(c, REG_MBOX + MBOX_DATA + i, w);
	}

	/* Mark the command in flight BEFORE submitting: vec0 fires on mailbox-DONE, and the ISR must
	 * suppress its notify path for our own completion (clarett_irq / the 0x400 note in clarett.h).
	 * Held until just before mutex_unlock so it still covers a completion MSI delivered after the
	 * poll below observes DONE. */
	atomic_set(&c->cmd_inflight, 1);

	clarett_wl(c, REG_DOORBELL, DOORBELL_SUBMIT);

	deadline = jiffies + msecs_to_jiffies(CLARETT_MBOX_TIMEOUT_MS);
	do {
		cause = clarett_rl(c, REG_IRQ0_CAUSE);   /* read-to-clear */
		if (!polls)
			first_cause = cause;
		polls++;
		if (cause & IRQ_DONE_BIT)
			break;
		cpu_relax();
	} while (time_before(jiffies, deadline));

	fcperr = clarett_rl(c, REG_MBOX + MBOX_ERROR);

	/* Per-transaction trace. dev_dbg (not dev_info): the GET_METER heartbeat runs at ~24 Hz, so
	 * info-level here would flood the log. Enable via dynamic debug when diagnosing the mailbox. */
	dev_dbg(&c->pci->dev,
		"FCP op=0x%06x seq=%u first_cause=0x%08x polls=%d done=%d fcperr=0x%08x\n",
		opcode, c->seq, first_cause, polls, !!(cause & IRQ_DONE_BIT), fcperr);

	if (!(cause & IRQ_DONE_BIT))
		ret = -ETIMEDOUT;
	else if (fcperr)
		ret = -EIO;

	c->seq++;

	atomic_set(&c->cmd_inflight, 0);	/* completion window closed; idle-gap events may resume */

	mutex_unlock(&c->mbox_lock);
	return ret;
}

/* GET_DATA: request `len` bytes from config `offset`. The response is DMAed into
 * the buffer programmed at REG_DMA_ADDR_LO/HI (c->resp_buf), not returned via MMIO.
 * The response layout is decoded: a 16-byte echoed FCP header (guard on resp[0]) then
 * the requested bytes at FCP_RESP_DATA_OFF (see clarett.h; clarett_notify_work refreshes
 * the monitor shadow from it). */
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

/* Convenience: write a single config byte and commit it. The commit (DATA_CMD{activate})
 * applies the change live but RAM-only — it is NOT persisted across a power cycle. Persistence
 * is a separate DATA_CMD{FCP_ACTIVATE_PERSIST} (command 5), intentionally not issued here. */
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

/* Read-modify-write the `mask` bits of a shared config byte to the value in `val`, then commit.
 * Used for the command-3 enable bytes (72/73) that pack one bit per output: the shadow MUST have
 * been seeded from the device first (clarett_seed_shadow) so the other outputs' bits survive. */
int clarett_write_bits(struct clarett *c, u32 offset, u8 mask, u8 val, u32 activate)
{
	u8 updated;

	if (offset >= CLARETT_CONFIG_SIZE)
		return -EINVAL;

	updated = (c->shadow[offset] & ~mask) | (val & mask);
	if (updated == c->shadow[offset])
		return 0;
	return clarett_write_u8(c, offset, updated, activate);
}
