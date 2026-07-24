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
#include <linux/ktime.h>
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

static bool mbox_err_read;
module_param(mbox_err_read, bool, 0444);
MODULE_PARM_DESC(mbox_err_read,
	"Read MBOX_ERROR (0x8028) after each command completion. The vendor driver NEVER reads any "
	"mailbox register (cold trace: zero mailbox reads across the whole bring-up), and our read "
	"lands while the device is still DMAing its async response — the sole per-command BAR-surface "
	"difference from the vendor (wall spec 7). Default 0 = vendor-faithful (no read); 1 restores "
	"the old readback for A/B.");

static bool legacy_mbox_cycle;
module_param(legacy_mbox_cycle, bool, 0444);
MODULE_PARM_DESC(legacy_mbox_cycle,
	"Use the old mailbox cycle: leading doorbell ACK + tight 0x100-only completion poll. The "
	"vendor's cycle (default 0) is submit -> sweep ALL five cause blocks in order "
	"0x100,0x300,0x200,0x400,0x500 until DONE -> one confirming sweep -> TRAILING ack. The old "
	"cycle's leading ACK made our first-ever doorbell write to a fresh device an ack to a mailbox "
	"that never carried a command — an out-of-protocol token the vendor never sends, at the exact "
	"pre-command-#0 point where the session gate decides (wall spec 7).");

/*
 * THE WALL CROSSING (wall spec §8, July 16 2026) — why the trailing ack is gated on the
 * response landing. The trailing doorbell ack (0x408=2) means "response consumed, buffer
 * free", NOT "completion observed": the device DMAs its response asynchronously AFTER the
 * BAR DONE bit, and acking before it lands is a protocol violation the device answers with
 * a blanket err=3 refusal of the whole session from command #0. No trace could show this —
 * every vendor capture ran under ~20 us/access MMIO trapping, so the response had always
 * landed by ack time (>=242 us after submit); our native-speed ack fired ~us after DONE.
 * The gated cycle below is therefore the DEFAULT, not a lever: pre-submit, zero the
 * response header (so repeated opcodes cannot false-match a stale echo — and FC's buffer
 * arrives zeroed from Windows anyway); after the DONE sweep, wait for THIS command's
 * echoed opcode in resp_buf; only then ack. A response that never arrives is never acked
 * (the vendor never acks an incomplete command). Confirmed 3/3 on fresh DC power-cycles:
 * gated arms (err=0 + physical manifestation), ungated walls (seed -5).
 *
 * resp_trace: one log line per command — DONE latency, response-landing latency, echoed
 * seq, FCP status word (resp+8), size. The wall's onset instrument; kept as the mailbox's
 * one diagnostic lever. (GET_METER adds ~24 lines/s; meter_poll_ms=0 for a readable log.)
 */
static bool resp_trace;
module_param(resp_trace, bool, 0444);
MODULE_PARM_DESC(resp_trace,
	"Log per-command response telemetry: DONE and response-landing latencies, echo, FCP "
	"status (resp+8), size. For mapping the wall's onset across fresh boots. Use with "
	"meter_poll_ms=0. Default 0.");

/*
 * True for a stream period-cause block (0x200 TX / 0x300 RX) while the PCM engine is streaming. The
 * mailbox skips these in its DONE sweep so it does not read-to-clear a period event the stream servicer
 * is waiting for (an audible gap). When idle they read 0 and are swept normally.
 */
static inline bool clarett_stream_cause(const struct clarett *c, u16 reg)
{
	return c->stream_on && (reg == STREAM_BLK0 || reg == STREAM_BLK1);
}

/*
 * Wait for THIS command's response to land in resp_buf. Returns the matched echo word
 * (CMD_EXEC_FLAG | opcode — never zero), or 0 if nothing landed before the deadline;
 * *land_us gets the submit->landing latency (-1 on timeout). Spins briefly (a landing,
 * when it happens, is expected within ~150 us), then backs off to sleeps; called under
 * mbox_lock, sleeping is fine.
 */
static u32 clarett_resp_wait(struct clarett *c, u32 exp_echo, ktime_t t_submit, s64 *land_us)
{
	const u8 *r = c->resp_buf;
	unsigned long deadline = jiffies + msecs_to_jiffies(CLARETT_MBOX_TIMEOUT_MS);
	ktime_t spin_until = ktime_add_us(ktime_get(), 500);
	u32 echo;

	for (;;) {
		dma_rmb();	/* order the DMAed response before we read resp_buf */
		echo = r[FCP_RESP_ECHO_OFF] | r[FCP_RESP_ECHO_OFF + 1] << 8 |
		       r[FCP_RESP_ECHO_OFF + 2] << 16 | (u32)r[FCP_RESP_ECHO_OFF + 3] << 24;
		if (echo == exp_echo) {
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
	unsigned long deadline;
	u32 cause = 0, first_cause = 0, fcperr = 0, resp_echo = 0, fcp_status = 0;
	ktime_t t_submit;
	s64 done_us = -1, land_us = -1;
	int i, ret = 0, polls = 0;

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

	mutex_lock(&c->mbox_lock);

	/* Old cycle only: the leading ACK. The vendor NEVER acks before a submit — its ack
	 * trails each completed command (below). On a fresh device the leading ACK was the
	 * first doorbell token the device ever received from us: an ack with nothing to ack. */
	if (legacy_mbox_cycle)
		clarett_wl(c, REG_DOORBELL, DOORBELL_ACK);

	if (resp_prefill >= 0) {
		memset(c->resp_buf, resp_prefill & 0xff, c->resp_size);
		dma_wmb();	/* fill visible to the device before the doorbell submit */
	}
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

	/* Mark the command in flight BEFORE submitting: vec0 fires on mailbox-DONE, and the ISR must
	 * suppress its notify path for our own completion (clarett_irq / the 0x400 note in clarett.h).
	 * With the vendor cycle the ISR is also the completion consumer (reads 0x100, completes
	 * mbox_done). Held until just before mutex_unlock so it still covers a completion MSI
	 * delivered after the poll below observes DONE. */
	reinit_completion(&c->mbox_done);
	atomic_set(&c->cmd_inflight, 1);

	t_submit = ktime_get();
	clarett_wl(c, REG_DOORBELL, DOORBELL_SUBMIT);

	deadline = jiffies + msecs_to_jiffies(CLARETT_MBOX_TIMEOUT_MS);
	if (legacy_mbox_cycle) {
		do {
			cause = clarett_rl(c, REG_IRQ0_CAUSE);   /* read-to-clear */
			if (!polls)
				first_cause = cause;
			polls++;
			if (cause & IRQ_DONE_BIT)
				break;
			cpu_relax();
		} while (time_before(jiffies, deadline));
	} else {
		/* Vendor cycle (cold trace, every command): after submit the working driver's
		 * first sweep (~40 us later) sees DONE in 0x100, sweeps the remaining four cause
		 * blocks (order 0x300,0x200,0x400,0x500 — 0x400 reads the 0x3 command-phase value
		 * and is read-to-cleared), does one confirming full sweep, then the TRAILING ack.
		 * We reach DONE with the long-proven tight 0x100 poll and only then sweep: a
		 * first-cut continuous five-block sweep DURING command processing hammered the
		 * read-to-clear phase regs at bus speed and caused arm-command timeouts (-110) —
		 * the one behavior change any lever ever produced; don't reintroduce it. */
		static const u16 sweep[] = { REG_IRQ0_CAUSE, STREAM_BLK1, STREAM_BLK0,
					     REG_NOTIFY_CAUSE, 0x500 };
		int s;

		/* Completion discovery, vendor-style: wait for the vec0 MSI; the ISR performs
		 * the sweep's first 0x100 read (MSI-paced — the vendor reads 0x100 exactly twice
		 * per command; a tight poll here reads it dozens of times). Poll only as a
		 * fallback (MSI not granted, or a lost/raced interrupt). polls==1 in the dev_dbg
		 * line verifies the MSI path was taken. */
		if (c->irq_ready &&
		    wait_for_completion_timeout(&c->mbox_done,
						msecs_to_jiffies(CLARETT_MBOX_TIMEOUT_MS))) {
			cause = c->mbox_cause;
			first_cause = cause;
			polls = 1;
		}
		if (!(cause & IRQ_DONE_BIT)) {
			do {	/* >=1 read even if the MSI wait consumed the deadline */
				cause = clarett_rl(c, REG_IRQ0_CAUSE);   /* fallback poll, read-to-clear */
				if (!polls)
					first_cause = cause;
				polls++;
				if (cause & IRQ_DONE_BIT)
					break;
				cpu_relax();
			} while (time_before(jiffies, deadline));
		}

		if (cause & IRQ_DONE_BIT) {
			done_us = ktime_us_delta(ktime_get(), t_submit);
			/*
			 * Skip the stream period-cause blocks (0x200 TX / 0x300 RX) while the PCM engine is
			 * streaming: the stream servicer owns them (read-to-clear), and a mailbox read here
			 * STEALS a pending period event — an audible gap in capture/playback. This is why control
			 * traffic during playback (meter poll, fcp-server, the mixer GUI) caused skipping. The
			 * mailbox ack only needs 0x100 + the response landing + the trailing doorbell ack; the
			 * stream blocks are irrelevant to it. clarett_stream_cause() flags the two to skip.
			 */
			for (s = 1; s < ARRAY_SIZE(sweep); s++)
				if (!clarett_stream_cause(c, sweep[s]))
					clarett_rl(c, sweep[s]);	/* rest of the DONE sweep */
			for (s = 0; s < ARRAY_SIZE(sweep); s++)
				if (!clarett_stream_cause(c, sweep[s]))
					clarett_rl(c, sweep[s]);	/* confirming full sweep */
			resp_echo = clarett_resp_wait(c, CMD_EXEC_FLAG | opcode,
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
		}
		/* on timeout: no ack — the vendor never acks an incomplete command */
	}

	/* Vendor-faithful default: NO mailbox read here. FC never reads any mailbox register;
	 * this read raced the device's async response DMA on every command since day one
	 * (fcperr always read 0 for us anyway — the real error channel is resp+8 in the DMA
	 * buffer). Lever mbox_err_read=1 restores it. */
	if (mbox_err_read)
		fcperr = clarett_rl(c, REG_MBOX + MBOX_ERROR);

	/* Per-transaction trace. dev_dbg (not dev_info): the GET_METER heartbeat runs at ~24 Hz, so
	 * info-level here would flood the log. Enable via dynamic debug when diagnosing the mailbox. */
	dev_dbg(&c->pci->dev,
		"FCP op=0x%06x seq=%u first_cause=0x%08x polls=%d done=%d fcperr=0x%08x status=0x%08x\n",
		opcode, c->seq, first_cause, polls, !!(cause & IRQ_DONE_BIT), fcperr, fcp_status);

	if (resp_trace) {
		const u8 *r = c->resp_buf;

		if (resp_echo) {
			u32 size = r[FCP_RESP_SIZE_OFF] | r[FCP_RESP_SIZE_OFF + 1] << 8;

			dev_info(&c->pci->dev,
				 "FCPr op=0x%06x seq=%u done=%lldus resp=%lldus rseq=%u err=%u size=%u\n",
				 opcode, c->seq, done_us, land_us,
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
				 "FCPr op=0x%06x seq=%u done=%lldus resp=NONE (ack withheld)\n",
				 opcode, c->seq, done_us);
	}

	if (!(cause & IRQ_DONE_BIT))
		ret = -ETIMEDOUT;
	else if (fcperr)
		ret = -EIO;
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

	atomic_set(&c->cmd_inflight, 0);	/* completion window closed; idle-gap events may resume */

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

/* Convenience: write a single config byte and commit it. The commit (DATA_CMD{activate})
 * applies the change live but RAM-only — it is NOT persisted across a power cycle. Persistence
 * is a separate DATA_CMD{FCP_ACTIVATE_PERSIST} (command 5), intentionally not issued here. */
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

	/* Persist to the device's NVRAM on a debounce so the change survives a power cycle (device
	 * owns the state). Gated on ctl_ready: the arm replay and the probe-time monitor-enable RMW
	 * run before the card is up and must NOT trigger a flash write on every load. mod_delayed_work
	 * pushes the single save out to CLARETT_SAVE_DELAY_MS after the LAST change, coalescing bursts. */
	if (save && READ_ONCE(c->ctl_ready))
		mod_delayed_work(system_wq, &c->save_work,
				 msecs_to_jiffies(CLARETT_SAVE_DELAY_MS));
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
