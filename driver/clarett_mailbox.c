// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clarett (Thunderbolt) — FCP mailbox transport.
 *
 * One transaction: fill the request mailbox (cmd/size+seq/error/pad/data), ring the
 * doorbell, wait for the device to report the request accepted and then its response
 * DMA landed, and ack. Both reports are REG_NOTIFY_CAUSE phase bits, delivered by the
 * vec0 ISR (see the irq_ready block in struct clarett).
 */
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/string.h>
#include "clarett.h"

/*
 * The trailing doorbell ack (0x408=2) means "response consumed, buffer free", so it must follow
 * the response DMA. Acking earlier is a protocol violation, which the device answers by refusing
 * the whole session from command #0 with a blanket err=3. A command whose response never lands is
 * therefore never acked.
 *
 * resp_trace logs one line per command: DONE latency, response-landing latency, echoed seq, FCP
 * status word (resp+8), size.
 */
static bool resp_trace;
module_param(resp_trace, bool, 0444);
MODULE_PARM_DESC(resp_trace,
	"Log per-command response telemetry: DONE and response-landing latencies, echo, FCP "
	"status (resp+8), size. Default 0.");

/*
 * Backstop for the response's echoed opcode appearing in resp_buf after the device has reported
 * the transfer complete. Runtime-writable so it can be raised against a loaded module.
 */
static uint resp_timeout_ms = CLARETT_MBOX_TIMEOUT_MS;
module_param(resp_timeout_ms, uint, 0644);
MODULE_PARM_DESC(resp_timeout_ms,
	"Deadline (ms) for a command's response to appear in the DMA buffer before the trailing ack "
	"is withheld (default 100). resp_trace=1 prints the landing latency.");

/*
 * Wait for THIS command's response to land in resp_buf. Returns the matched echo word
 * (CMD_EXEC_FLAG | opcode — never zero), or 0 if nothing landed before the deadline;
 * *land_us gets the submit->landing latency (-1 on timeout). Called under mbox_lock.
 *
 * A response is identified by its echoed opcode AND sequence number. Opcodes repeat — GET_METER
 * every tick — so a response abandoned by an earlier command can arrive later carrying the opcode
 * of the one now waiting; the sequence number separates them. A device holding an unretired command
 * answers it in place of every later one, so its stale sequence number fails this match and the
 * waiting command times out, which is what clarett_fcp() reports as a wedge.
 */
static u32 clarett_resp_wait(struct clarett *c, u32 exp_echo, u16 exp_seq,
			     ktime_t t_submit, s64 *land_us)
{
	const u8 *r = c->resp_buf;
	unsigned long deadline = jiffies + msecs_to_jiffies(max(resp_timeout_ms, 1u));
	ktime_t spin_until = ktime_add_us(ktime_get(), 500);
	u32 echo;

	for (;;) {
		u16 rseq;

		dma_rmb();	/* order the DMAed response before we read resp_buf */
		echo = r[FCP_RESP_ECHO_OFF] | r[FCP_RESP_ECHO_OFF + 1] << 8 |
		       r[FCP_RESP_ECHO_OFF + 2] << 16 | (u32)r[FCP_RESP_ECHO_OFF + 3] << 24;
		rseq = r[FCP_RESP_SEQ_OFF] | r[FCP_RESP_SEQ_OFF + 1] << 8;

		if (echo == exp_echo && rseq == exp_seq) {
			*land_us = ktime_us_delta(ktime_get(), t_submit);
			return echo;
		}
		if (time_after(jiffies, deadline)) {
			*land_us = -1;
			return 0;
		}
		if (ktime_before(ktime_get(), spin_until))
			cpu_relax();
		else
			usleep_range(20, 50);
	}
}

/*
 * Core mailbox transaction. If resp_out/resp_len are given, the response *payload* (the bytes after
 * the 16-byte echoed header) is copied out under mbox_lock on success — race-free, unlike reading
 * c->resp_buf after the call. clarett_fcp() is the response-less wrapper; clarett_fcp_cmd() is the
 * hwdep CMD path that needs the payload.
 */
static int __clarett_fcp(struct clarett *c, u32 opcode, const u8 *data, u16 len,
			 u8 *resp_out, u16 resp_len)
{
	u32 resp_echo = 0, fcp_status = 0;
	bool accepted = false, answered = false;
	ktime_t t_submit;
	s64 land_us = -1;
	int i, ret = 0;

	if (len > CLARETT_MBOX_DATA_MAX)
		return -EINVAL;

	/*
	 * A removed device answers every MMIO read with ~0 and swallows writes, so the gated cycle
	 * would wait out its full timeout for a response that can never land — once per command, for
	 * every command still queued when the Thunderbolt cable is pulled or the unit is switched
	 * off. Fail fast instead; the PCI core sets this the moment the link goes.
	 */
	if (pci_dev_is_disconnected(c->pci))
		return -ENODEV;

	if (READ_ONCE(c->mbox_dead))
		return -ENODEV;

	mutex_lock(&c->mbox_lock);

	/* zero the response header so clarett_resp_wait can't match a stale echo
	 * (the arm repeats opcodes back-to-back, CONFIG_PUSH x122) */
	memset(c->resp_buf, 0, FCP_RESP_DATA_OFF);
	dma_wmb();

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

	/* Arm the waits before submitting, so an interrupt that arrives immediately has somewhere
	 * to land. */
	reinit_completion(&c->mbox_accepted);
	reinit_completion(&c->mbox_landed);

	t_submit = ktime_get();
	clarett_wl(c, REG_DOORBELL, DOORBELL_SUBMIT);

	accepted = wait_for_completion_timeout(&c->mbox_accepted,
					       msecs_to_jiffies(CLARETT_ACCEPT_TIMEOUT_MS));
	if (accepted)
		answered = wait_for_completion_timeout(&c->mbox_landed,
						       msecs_to_jiffies(CLARETT_ANSWER_TIMEOUT_MS));

	if (answered) {
		resp_echo = clarett_resp_wait(c, CMD_EXEC_FLAG | opcode, c->seq,
					      t_submit, &land_us);
		if (resp_echo) {
			/* The device reports command-level failures in the FCP status word
			 * (resp+8), NOT the mailbox error register — a rejected write/commit
			 * lands a nonzero status here while DONE and the echo both look fine.
			 * Read it now (response has landed) so callers, incl. the hwdep CMD
			 * ioctl, see the real result instead of a false success. */
			dma_rmb();
			fcp_status = clarett_get_le32((const u8 *)c->resp_buf +
						      FCP_RESP_STATUS_OFF);
			clarett_wl(c, REG_DOORBELL, DOORBELL_ACK);
		} else {
			dev_warn_ratelimited(&c->pci->dev,
				"FCP op=0x%06x seq=%u: response never landed; ack withheld\n",
				opcode, c->seq);
		}
	} else if (accepted) {
		/* Ack releases the phase machine for the next command; see the deadline note
		 * in clarett.h for the outcome semantics. */
		clarett_wl(c, REG_DOORBELL, DOORBELL_ACK);
		dev_warn_ratelimited(&c->pci->dev,
			"FCP op=0x%06x seq=%u: accepted but unanswered after %u ms; outcome unknown\n",
			opcode, c->seq, CLARETT_ANSWER_TIMEOUT_MS);
	}

	if (accepted) {
		c->mbox_strikes = 0;
	} else if (++c->mbox_strikes >= CLARETT_MBOX_STRIKES && !c->mbox_dead) {
		WRITE_ONCE(c->mbox_dead, true);
		dev_err(&c->pci->dev,
			"mailbox dead: %u consecutive commands not accepted; failing all further "
			"commands. Reload or rebind the driver to recover.\n",
			c->mbox_strikes);
	}

	/* No mailbox read here, matching the vendor, which never reads any mailbox register. The real
	 * error channel is resp+8 in the DMA buffer; a read here would race the device's response DMA. */

	/* Per-transaction trace. dev_dbg (not dev_info): the meter poll runs at ~24 Hz, so
	 * info-level here would flood the log. Enable via dynamic debug when diagnosing the mailbox. */
	dev_dbg(&c->pci->dev,
		"FCP op=0x%06x seq=%u accepted=%d answered=%d status=0x%08x\n",
		opcode, c->seq, accepted, answered, fcp_status);

	if (resp_trace) {
		const u8 *r = c->resp_buf;

		if (resp_echo) {
			u32 size = r[FCP_RESP_SIZE_OFF] | r[FCP_RESP_SIZE_OFF + 1] << 8;

			dev_info(&c->pci->dev,
				 "FCPr op=0x%06x seq=%u resp=%lldus rseq=%u err=%u size=%u\n",
				 opcode, c->seq, land_us,
				 r[FCP_RESP_SEQ_OFF] | r[FCP_RESP_SEQ_OFF + 1] << 8,
				 r[FCP_RESP_STATUS_OFF], size);
			/* Payload head for every answered non-meter command: the raw material for
			 * cross-model diffing (model auto-detect: which query's answer encodes the
			 * model?). 32 bytes is enough to see counts/ids; GET_METER excluded (24 Hz). */
			if (size && opcode != FCP_GET_METER)
				dev_info(&c->pci->dev, "FCPr   payload=%*ph\n",
					 (int)min(size, 32u), r + FCP_RESP_DATA_OFF);
		}
		else
			dev_info(&c->pci->dev,
				 "FCPr op=0x%06x seq=%u resp=NONE (ack withheld)\n",
				 opcode, c->seq);
	}

	/*
	 * Wedge detection, for the readiness poll: no response matched this command's opcode and
	 * sequence number, which covers both silence and a device answering an earlier unretired
	 * command with its stale sequence.
	 */
	c->mbox_wedged = !resp_echo;

	if (!resp_echo)
		ret = -ETIMEDOUT;
	/*
	 * NOTE: fcp_status (resp+8) is NOT a clean pass/fail on the Clarett — INIT_1 re-run on an
	 * already-armed device returns a nonzero status yet the command works (INIT_2 still answers
	 * the firmware version). So we do NOT fail on it; it is surfaced in the dev_dbg line below and
	 * via resp_trace for diagnosis. Real command rejection is judged by outcome, not this word.
	 */

	/* Copy the response payload out while still holding the lock (resp_buf is stable until the next
	 * command, which can't start before we unlock). Only on success and only if the caller asked. */
	if (!ret && resp_out && resp_len) {
		const u8 *r = c->resp_buf;
		u16 have;

		dma_rmb();
		have = r[FCP_RESP_SIZE_OFF] | r[FCP_RESP_SIZE_OFF + 1] << 8;
		/*
		 * Honour the response's own size. The device answers some commands with LESS than the
		 * caller asked for — MUX_READ caps every reply at 28 entries (112 bytes) however large
		 * a count is requested — and copying the full requested length then hands the caller
		 * whatever the PREVIOUS command left in resp_buf. That is stale DMA content presented
		 * as device data (it disguised the MUX_READ cap as a truncated routing table for some
		 * time), and via the hwdep it is also a kernel-memory disclosure. Zero-fill the tail so
		 * a short reply is unmistakably short. size == 0 is left alone: not every command fills
		 * the field, and a zero-length answer is judged by outcome (see the note above).
		 */
		if (have && have < resp_len) {
			memcpy(resp_out, r + FCP_RESP_DATA_OFF, have);
			memset(resp_out + have, 0, resp_len - have);
			dev_dbg(&c->pci->dev, "op 0x%06x: short response %u < %u requested\n",
				opcode, have, resp_len);
		} else {
			memcpy(resp_out, r + FCP_RESP_DATA_OFF, resp_len);
		}
	}

	c->seq++;

	mutex_unlock(&c->mbox_lock);
	return ret;
}

int clarett_fcp(struct clarett *c, u32 opcode, const u8 *data, u16 len)
{
	return __clarett_fcp(c, opcode, data, len, NULL, 0);
}

/* hwdep FCP_IOCTL_CMD: run one command and return `resp_len` bytes of its response payload. */
int clarett_fcp_cmd(struct clarett *c, u32 opcode, const u8 *req, u16 req_len,
		    u8 *resp, u16 resp_len)
{
	return __clarett_fcp(c, opcode, req, req_len, resp, resp_len);
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

/*
 * Schedule the debounced NVRAM commit (a single DATA_CMD{FCP_ACTIVATE_PERSIST}) so a config change
 * survives a power cycle — the device owns its state (upstream scarlett2 policy). mod_delayed_work
 * coalesces a burst (e.g. a slider drag) into one save CLARETT_SAVE_DELAY_MS after the LAST change.
 * Gated on ctl_ready: the arm replay and the probe-time monitor-enable RMW run before the card is up
 * and must NOT commit flash on every load. Called both from the in-kernel write path below and from
 * the hwdep relay when fcp-server commits a config change (clarett_hwdep_cmd) — the device
 * auto-persists only some config (routing) and not the rest (output gains, S/PDIF source), so
 * without this the latter revert to the flash default on a power cycle.
 */
void clarett_schedule_persist(struct clarett *c)
{
	if (READ_ONCE(c->ctl_ready))
		mod_delayed_work(system_wq, &c->save_work,
				 msecs_to_jiffies(CLARETT_SAVE_DELAY_MS));
}

/* Convenience: write a single config byte and commit it. The commit (DATA_CMD{activate})
 * applies the change live but RAM-only — it is NOT persisted across a power cycle. Persistence
 * is a separate DATA_CMD{FCP_ACTIVATE_PERSIST} (command 5), scheduled here on a debounce. */
static int __clarett_write_u8(struct clarett *c, u32 offset, u8 val, u32 activate, bool save)
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
	set_bit(offset, c->shadow_known);	/* write-through: the shadow now matches hardware */

	/* Persist to the device's NVRAM on a debounce so the change survives a power cycle (device owns
	 * the state); nosave writes are mirrors, not user intent, and skip it. See the helper. */
	if (save)
		clarett_schedule_persist(c);
	return 0;
}

int clarett_write_u8(struct clarett *c, u32 offset, u8 val, u32 activate)
{
	return __clarett_write_u8(c, offset, val, activate, true);
}

/*
 * As clarett_write_u8, but does NOT schedule the NVRAM save. For bytes the driver maintains as a
 * MIRROR of some other state rather than at the user's request — currently the SW gains of outputs
 * under hardware control (clarett_hw_gain_follow), which retrack the front-panel knob and would
 * otherwise commit the device's flash on every movement of it.
 */
int clarett_write_u8_nosave(struct clarett *c, u32 offset, u8 val, u32 activate)
{
	return __clarett_write_u8(c, offset, val, activate, false);
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
