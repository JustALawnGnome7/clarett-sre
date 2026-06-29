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
#include <linux/string.h>
#include "clarett.h"

int clarett_fcp(struct clarett *c, u32 opcode, const u8 *data, u16 len)
{
	unsigned long deadline;
	u32 cause = 0, first_cause = 0, fcperr = 0;
	int i, ret = 0, polls = 0;

	if (len > CLARETT_MBOX_DATA_MAX)
		return -EINVAL;

	mutex_lock(&c->mbox_lock);

	clarett_wl(c, REG_DOORBELL, DOORBELL_ACK);

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

	/* Optional post-command settle delay (cmd_delay_us): mimic FC's x-no-mmap pacing to test whether
	 * arming/manifestation needs time to latch between commands. Process context under mbox_lock, so
	 * sleeping is fine; small slack lets the scheduler coalesce the timer. */
	if (clarett_cmd_delay_us > 0)
		usleep_range(clarett_cmd_delay_us, clarett_cmd_delay_us + (clarett_cmd_delay_us >> 2) + 1);

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

/*
 * Diagnostic: after a control write, GET_DATA the same offset and log whether the device's own
 * config RAM now reflects the value we wrote. Splits "device discards our write" (stale read) from
 * "device accepts it but never physically applies" (matching read, LED still frozen). Run with
 * meter_poll_ms=0 so the GET_METER heartbeat can't overwrite resp_buf between the GET and the read.
 */
void clarett_verify_write(struct clarett *c, u32 offset, u8 expected)
{
	const u8 *r = c->resp_buf;
	u32 echo;
	int err;
	u8 got;
	bool dmaed;

	/* Read a 16-byte window starting at `offset` (single-byte GETs may behave differently from
	 * the region reads the device expects); the target byte is the first data byte. */
	err = clarett_get_data(c, offset, 16);
	if (err) {
		dev_info(&c->pci->dev, "verify off=%u: GET_DATA failed (%d)\n", offset, err);
		return;
	}
	dma_rmb();
	echo = r[FCP_RESP_ECHO_OFF] | r[FCP_RESP_ECHO_OFF + 1] << 8 |
	       r[FCP_RESP_ECHO_OFF + 2] << 16 | r[FCP_RESP_ECHO_OFF + 3] << 24;
	dmaed = (echo == (CMD_EXEC_FLAG | FCP_GET_DATA));
	got = r[FCP_RESP_DATA_OFF];

	dev_info(&c->pci->dev,
		 "verify off=%u: wrote=0x%02x device-reads=0x%02x echo=0x%08x (%s)\n",
		 offset, expected, got, echo,
		 !dmaed ? "NO GET RESPONSE (read untrustworthy)" :
		 got == expected ? "ACCEPTED into config RAM" : "DISCARDED / stale");
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
