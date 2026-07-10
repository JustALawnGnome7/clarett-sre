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
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/string.h>
#include <linux/jiffies.h>
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

static bool enable_pcm;
module_param(enable_pcm, bool, 0444);
MODULE_PARM_DESC(enable_pcm,
		 "Register a capture PCM device (28ch S32_LE @48k, ring block 1) driven by the 0x300 "
		 "servicer. Experimental data-plane bring-up; mutually exclusive with stream_probe.");

static int rekick;
module_param(rekick, int, 0444);
MODULE_PARM_DESC(rekick,
		 "Stall re-kick when 0x300 freezes mid-stream (the one-ring-pass stall): 0=off, "
		 "1=rewrite 0x110 arm, 2=rewrite ring bases + 0x110, 3=rewrite 0x20c enable + 0x110, "
		 "4=re-issue activate=5 stream commit (mailbox).");

static int rekick_ms = 20;
module_param(rekick_ms, int, 0444);
MODULE_PARM_DESC(rekick_ms, "Stall threshold for rekick: ms with no new 0x300 period before kicking.");

static int meter_poll_ms = CLARETT_METER_POLL_MS;
module_param(meter_poll_ms, int, 0444);
MODULE_PARM_DESC(meter_poll_ms,
		 "Period (ms) of the GET_METER heartbeat the device requires to APPLY control writes to "
		 "hardware. Focusrite Control polls ~every 40 ms continuously; without it our byte-correct "
		 "writes complete (done=1) but the front-panel preamp/monitor state never moves. Default 40. "
		 "Set 0 to disable (for A/B testing the hypothesis).");

static int dma_bits = 32;
module_param(dma_bits, int, 0444);
MODULE_PARM_DESC(dma_bits,
		 "DMA coherent mask width (bits). Default 32 lands the stream buffer at the top of 32-bit "
		 "IOVA space (~0xffe00000); the VM's buffers sat mid-range (~1.5-1.9 GB). Lower this (e.g. "
		 "31 -> <2GB, 30 -> <1GB) to force the allocation into the VM's range and test whether the "
		 "engine's burst-then-stall is sensitive to buffer address. Long shot: PTR advances at "
		 "0xffe00000 with no fault, so the high address is not obviously the blocker.");

static bool monitor_enables = true;
module_param(monitor_enables, bool, 0444);
MODULE_PARM_DESC(monitor_enables,
		 "At probe, write the monitor HW-enable bits (0x48/0x49, cmd3) so global Mute/Dim affect "
		 "Monitor Out 1-2. Default true. FC's captured 2Pre control session does NOT send these, so "
		 "they are additive writes we make beyond FC. Set 0 to make our command stream an exact "
		 "SUBSET of FC's (combine with inject_clock=0) and A/B whether any extra write we make is "
		 "wedging control manifestation. If toggles still don't manifest with both off, the on-wire "
		 "surface is fully exhausted and the gap is conclusively off-wire DMA.");

static bool premailbox_reads = true;
module_param(premailbox_reads, bool, 0444);
MODULE_PARM_DESC(premailbox_reads,
		 "Replay the vendor's exact pre-mailbox BAR0 READ sequence at attach (caps/serial/fw-header/"
		 "cause-blocks/0x514/0x58c) before the first FCP command. Motivated by the July-10 2026 cold gdb "
		 "ladder: the working device answers error=0 from mailbox command #0, so the accept-vs-refuse gate "
		 "is set PRE-mailbox (manifestation-wall §7). Pre-mailbox WRITES already match FC byte-for-byte; "
		 "this read set is the sole remaining host-visible pre-mailbox difference. Default true; set 0 for "
		 "the old (walled) read-minimal probe to A/B whether the reads flip GET_DATA to error=0.");

static bool error_probe;
module_param(error_probe, bool, 0444);
MODULE_PARM_DESC(error_probe,
		 "Diagnostic (manifestation-wall §7): after bring-up, send a few deliberately MALFORMED FCP "
		 "commands (bad offset, zero length, unknown opcode) alongside a valid GET_DATA and log each "
		 "response's DMA error word (resp+8) + size. If the malformed commands return a DIFFERENT code "
		 "than the valid one's error=3, the device parses per-command (error=3 = a specific semantic "
		 "rejection); if ALL return error=3/size=0 identically, it is a blanket out-of-band session refusal. "
		 "Off by default (sends junk commands). One-shot at probe. MUST be combined with meter_poll_ms=0: the "
		 "meter-poll worker otherwise races the probe on the shared resp_buf and c->seq (its GET_METER "
		 "responses land in the buffer and it bumps the seq), corrupting per-command attribution.");

static bool premailbox_causes = true;
module_param(premailbox_causes, bool, 0444);
MODULE_PARM_DESC(premailbox_causes,
		 "Within premailbox_reads, gate the four READ-TO-CLEAR cause-block reads (0x100/0x200/0x300/"
		 "0x400). Bisection lever for the 2026-07-10 finding that premailbox_reads=1 makes the Analogue-2 "
		 "gain LED flash red at probe (first-ever physical response). The info/version reads are "
		 "side-effect-free; the cause-block reads are read-to-clear, so they are the prime suspect. Set 0 "
		 "(with premailbox_reads=1) to drop just the cause reads and see if the LED flash stops — isolating "
		 "whether a read-to-clear is the trigger. Default true.");

static bool inject_clock = true;
module_param(inject_clock, bool, 0444);
MODULE_PARM_DESC(inject_clock,
		 "Inject SET_CLOCK{48000,Internal} (op 0x6003) before the first CONFIG_PUSH during arm. "
		 "Default true (data-plane: the device latches the rate while processing the push). FC's "
		 "2Pre control-only session sends NO 0x6003, so our injection is an init-sequence deviation "
		 "from FC that is present even during pure control tests. Set 0 to make arm an exact FC "
		 "replay and A/B whether the injected clock is why control toggles complete (done=1) but "
		 "never physically manifest.");

static const struct clarett_model clarett_8prex, clarett_2pre, clarett_4pre, clarett_8pre;	/* defined below; selected by clarett_pick_model() */

/*
 * Model selection. The whole Clarett Thunderbolt line shares PCI id 1cb5:0002 and presents a
 * byte-identical PCIe interface (MMIO regs, FCP query responses, config-space, even the dummy serial),
 * so the model is NOT auto-detectable from the device — it must be named. Default 2Pre (the primary
 * bench/RE unit). There is no userspace shortcut either: the line is entirely Thunderbolt 2 (discontinued
 * before any TB3 model), and TB2 units are firmware-tunneled rather than enumerated as kernel-managed TB
 * routers, so they never expose a DROM device_name in sysfs to disambiguate by.
 */
static char *model;
module_param(model, charp, 0444);
MODULE_PARM_DESC(model,
		 "Force interface model: \"2pre\" (default), \"4pre\", \"8pre\", or \"8prex\". All Clarett "
		 "Thunderbolt units share PCI id 1cb5:0002 and are indistinguishable from the PCIe side, so the "
		 "model must be specified explicitly (e.g. via /etc/modprobe.d/).");

static const struct clarett_model *clarett_pick_model(const struct pci_device_id *ent)
{
	if (model) {
		if (sysfs_streq(model, "2pre"))
			return &clarett_2pre;
		if (sysfs_streq(model, "4pre"))
			return &clarett_4pre;
		if (sysfs_streq(model, "8pre"))		/* distinct from 8prex — fewer streams, combo-jack inputs */
			return &clarett_8pre;
		if (sysfs_streq(model, "8prex"))
			return &clarett_8prex;
		pr_warn("snd_clarett: unknown model=\"%s\"; falling back to default\n", model);
	}
	return (const struct clarett_model *)ent->driver_data;
}

void clarett_wl(struct clarett *c, u32 off, u32 val)
{
	writel(val, c->bar0 + off);
}

u32 clarett_rl(struct clarett *c, u32 off)
{
	return readl(c->bar0 + off);
}

static void clarett_hw_init(struct clarett *c)
{
	void __iomem *bar = c->bar0;

	/*
	 * The pre-mailbox writes below (0x510/0x500 device-enable, DMA-response address, 0x104 cause latch)
	 * match FC's cold attach byte-for-byte. What did NOT match, until premailbox_reads, is the vendor's
	 * READ set at attach: the cold gdb ladder (2026-07-10; manifestation-wall §7) proved the working
	 * device already answers FCP error=0 from mailbox command #0, so the accept/refuse gate is decided
	 * BEFORE the first command — and the only host-visible pre-mailbox difference is that the vendor
	 * reads caps/0x4/0x8/0x514/0x58c, all four cause blocks, and the full fw-info header, which our
	 * read-minimal probe never issued. This branch replays the vendor's EXACT pre-mailbox read+write
	 * order (from /tmp/ladder_trace.log) in case a status/version/read-to-clear-cause read is part of an
	 * attach handshake. readl() returns are discarded except serial/fw (kept for dev_info).
	 *
	 * INTER-ACCESS TIMING is matched to the vendor too. The cold-ladder trace shows the vendor spaces
	 * these register GROUPS by ~0.8-8 ms of real driver-side pause, reproduced below with usleep_range
	 * (hw_init runs in probe/process context, so sleeping is fine). The ~17-20 us *intra*-burst spacing
	 * in the trace is x-no-mmap trap overhead — a VM measurement artifact (~100 ns on native hardware) —
	 * so those accesses are left back-to-back. Gaps vary boot-to-boot with scheduling; these are the
	 * measured cold-boot representatives. Tests whether the pre-mailbox gate is timing-sensitive (the read
	 * set alone, issued back-to-back, did not flip it — §7).
	 */
	if (premailbox_reads) {
		u32 r000, r004, r008, r514, r58c_a, r58c_b;

		readl(bar + REG_INFO);			/* 0x8000 — vendor's first touch */
		r004 = readl(bar + 0x004);
		r008 = readl(bar + 0x008);
		r000 = readl(bar + REG_CAPS);		/* 0x000 */
		c->serial_hi = readl(bar + REG_SERIAL_HI);	/* 0x014 (vendor reads hi before lo) */
		c->serial_lo = readl(bar + REG_SERIAL_LO);	/* 0x010 */
		usleep_range(7000, 7200);		/* vendor gap ~7.07 ms */
		r514 = readl(bar + 0x514);
		usleep_range(1300, 1400);		/* ~1.30 ms */
		clarett_wl(c, 0x510, 0x8);
		r58c_a = readl(bar + 0x58c);		/* ~21 us (native) — back-to-back */
		usleep_range(780, 850);			/* ~0.78 ms */
		clarett_wl(c, 0x500, 0x8);
		usleep_range(880, 950);			/* ~0.88 ms */
		clarett_wl(c, REG_IRQ0_ENABLE, 0xf000003f);	/* 0x104 — vendor writes this BEFORE the DMA addr */
		usleep_range(5640, 5800);		/* ~5.64 ms */
		clarett_wl(c, REG_DMA_ADDR_LO, lower_32_bits(c->resp_dma));
		usleep_range(1850, 1950);		/* ~1.85 ms */
		clarett_wl(c, REG_DMA_ADDR_HI, upper_32_bits(c->resp_dma));
		usleep_range(3200, 3350);		/* ~3.22 ms */
		if (premailbox_causes) {
			/*
			 * Read-to-clear cause blocks — CONFIRMED trigger of the Analogue-2 gain LED flash
			 * (premailbox_causes=0 → no flash). These are read-to-clear, so on a cold boot they
			 * report (and clear) whatever the device latched at power-on. Log the values: the
			 * pending cause here is the physical event the device signals, cleared by this read.
			 * Vendor order is 0x100, 0x300, 0x200, 0x400.
			 */
			u32 c100 = readl(bar + REG_IRQ0_CAUSE);
			u32 c300 = readl(bar + STREAM_BLK1);
			u32 c200 = readl(bar + STREAM_BLK0);
			u32 c400 = readl(bar + REG_NOTIFY_CAUSE);
			u32 c500 = readl(bar + 0x500);

			dev_info(&c->pci->dev,
				 "pre-mailbox causes: 0x100=0x%08x 0x300=0x%08x 0x200=0x%08x 0x400=0x%08x 0x500=0x%08x\n",
				 c100, c300, c200, c400, c500);
		} else {
			readl(bar + 0x500);
		}
		usleep_range(8220, 8400);		/* ~8.22 ms */
		r58c_b = readl(bar + 0x58c);
		dev_info(&c->pci->dev,
			 "pre-mailbox regs: caps(0x0)=0x%08x 0x4=0x%08x 0x8=0x%08x 0x514=0x%08x 0x58c=0x%08x/0x%08x\n",
			 r000, r004, r008, r514, r58c_a, r58c_b);
		c->fw_app  = readl(bar + REG_INFO + 0x00);	/* 0x8000 */
		c->fw_fpga = readl(bar + REG_INFO + 0x04);	/* 0x8004 */
		readl(bar + REG_INFO + 0x08);		/* rest of the 8-word fw-info header */
		readl(bar + REG_INFO + 0x0c);
		readl(bar + REG_INFO + 0x10);
		readl(bar + REG_INFO + 0x14);
		readl(bar + REG_INFO + 0x18);
		readl(bar + REG_INFO + 0x1c);
	} else {
		/*
		 * Read-minimal baseline (the known-walled probe). 0x510/0x500 are the vendor's first two
		 * writes at device open (global clock/converter-subsystem enable); the DMA-response address
		 * goes to REG_DMA_ADDR_LO/HI (HI = bus-address high32 — hardcoding the trace's 0x2 faulted the
		 * IOMMU); 0x104 latches interrupt causes (completion is still polled, clarett_mailbox.c).
		 */
		c->serial_lo = readl(bar + REG_SERIAL_LO);
		c->serial_hi = readl(bar + REG_SERIAL_HI);
		c->fw_app    = readl(bar + REG_INFO + 0);
		c->fw_fpga   = readl(bar + REG_INFO + 4);

		clarett_wl(c, 0x510, 0x8);
		clarett_wl(c, 0x500, 0x8);
		clarett_wl(c, REG_DMA_ADDR_LO, lower_32_bits(c->resp_dma));
		clarett_wl(c, REG_DMA_ADDR_HI, upper_32_bits(c->resp_dma));
		clarett_wl(c, REG_IRQ0_ENABLE, 0xf000003f);
	}

	memset(c->shadow, 0, sizeof(c->shadow));

	/*
	 * TODO: the firmware init handshake observed at boot (INIT_2 plus the
	 * 0x5000/0x6000/0x7000 command sequence) is not yet decoded. The mailbox
	 * accepts config commands without replaying it in testing, but a robust
	 * bring-up probably needs to understand/replay that sequence.
	 */
}

/* Generated per-model bring-up replay tables (clarett_init_blob_8prex[] / clarett_init_seq_8prex[]). */
#include "clarett_init_8prex.h"
/* clarett_init_blob_2pre[] / clarett_init_seq_2pre[] (fcp_decode.py --emit-init --init-model 2pre). */
#include "clarett_init_2pre.h"
/* clarett_init_blob_4pre[] / clarett_init_seq_4pre[] (fcp_decode.py --emit-init --init-model 4pre). */
#include "clarett_init_4pre.h"

/*
 * Replay the vendor device bring-up captured at attach from a freshly power-cycled device
 * (8prex_full_init_mute.log), regenerated into clarett_init_8prex.h by `fcp_decode.py --emit-init`:
 * every non-meter command up to the monitor-mute write, minus the bulk 8 KB config read/writeback.
 * Self-boot does NOT arm config access (GET_DATA fails); this host init arms it. Must run against a
 * device in its fresh power-on state — re-initializing an already-armed device wedges it instead.
 * Best-effort: failures are logged and the sequence continues.
 */
static int clarett_arm_device(struct clarett *c)
{
	int i, err, fails = 0;
	bool clk_sent = false;

	for (i = 0; i < c->model->n_init_steps; i++) {
		const struct clarett_init_step *s = &c->model->init_seq[i];

		/*
		 * SET_CLOCK before CONFIG_PUSH. FC issues 0x6003 at device-open, ahead of the config/routing
		 * push, in every capture; the device appears to latch the clock into its audio subsystem while
		 * processing that push, so a SET_CLOCK sent only later (engine_start) never engages period
		 * generation (0x300 stays flat). Inject it just before the first 0x5000, matching the order.
		 */
		if (inject_clock && !clk_sent && s->opcode == 0x005000) {
			u8 clk[8];

			clarett_put_le32(clk,     CLARETT_DEFAULT_RATE);
			clarett_put_le32(clk + 4, CLARETT_CLOCK_INTERNAL);
			err = clarett_fcp(c, FCP_SET_CLOCK, clk, sizeof(clk));
			dev_info(&c->pci->dev, "arm: SET_CLOCK{%u, Internal} before CONFIG_PUSH -> %d\n",
				 CLARETT_DEFAULT_RATE, err);
			clk_sent = true;
		}

		err = clarett_fcp(c, s->opcode, c->model->init_blob + s->off, s->len);
		if (err) {
			dev_warn(&c->pci->dev, "arm[%d] op 0x%06x failed: %d\n",
				 i, s->opcode, err);
			fails++;
		}
	}
	if (fails)
		dev_warn(&c->pci->dev, "arm: %d/%d steps failed\n",
			 fails, c->model->n_init_steps);

	return 0;
}

/*
 * Error-code discrimination probe (manifestation-wall §7). Send a valid GET_DATA plus three
 * deliberately malformed commands and log each response's DMA error word (resp+8) and size.
 * Our walled device returns error=3/size=0 to valid commands; if the malformed ones return the
 * SAME (error=3/size=0) the device blanket-refuses the session out-of-band (not parsing our
 * commands), whereas a DIFFERENT code (or a no-response timeout, echo=0) means it parses each
 * command and error=3 is a specific semantic rejection. Reads resp+8 directly because the BAR
 * MBOX_ERROR word (which clarett_fcp's return reflects) reads 0 for us — the real error is in
 * the DMAed response.
 */
static void clarett_error_probe(struct clarett *c)
{
	static const struct {
		const char *name;
		u32 opcode;
		u8 data[8];
		u16 len;
	} cmds[] = {
		/* Discrimination set (July 10): valid/zero-len GET_DATA → err=3 (real header);
		 * bad-offset / unknown-opcode → NO DMA response (device parses + drops). Kept as controls. */
		{ "GET_DATA{24,4} valid",     FCP_GET_DATA, { 24,0,0,0,  4,0,0,0 }, 8 },
		{ "GET_DATA bad-offset",      FCP_GET_DATA, { 0,0,0xff,0xff, 4,0,0,0 }, 8 },
		{ "unknown opcode 0x0000ff",  0x0000ff,     { 0 }, 0 },
		/* Opcode survey: the vendor's cold ladder got err=0 + real data on ALL of these. Does the
		 * device answer ANY of them for us (err=0), or is everything denied? READ_SEG is the segment
		 * "open" the vendor issues at seq 0; the GET_7/GET_6 are device queries; CONFIG_PUSH{id} is the
		 * per-id NAME query (vendor id 0x1e -> "ADAT 8"); GET_DATA{0xc8} hits the persistent appspace. */
		{ "READ_SEG{0,8}",            FCP_READ_SEG, { 0,0,0,0,  8,0,0,0 }, 8 },
		{ "GET_7.1{0}",               0x007001,     { 0 }, 1 },
		{ "GET_6.2",                  0x006002,     { 0 }, 0 },
		{ "CONFIG_PUSH{0x1e}",        0x005000,     { 0x1e, 0 }, 2 },
		{ "GET_DATA{0xc8,8}",         FCP_GET_DATA, { 0xc8,0,0,0, 8,0,0,0 }, 8 },
	};
	const u8 *r = c->resp_buf;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(cmds); i++) {
		u32 exp_echo = CMD_EXEC_FLAG | cmds[i].opcode;
		u16 exp_seq = c->seq;		/* the seq clarett_fcp will stamp on this command */
		unsigned long deadline;
		u32 echo = 0, err = 0;
		u16 size = 0, rseq = 0;
		bool landed = false;

		/*
		 * The device DMAs its response ASYNCHRONOUSLY, a little after the BAR done bit clarett_fcp
		 * polls — so reading resp_buf immediately races and catches the PREVIOUS command's late
		 * response. Wait for THIS command's response by matching the echoed opcode (unique per command
		 * after the memset). NOTE (July 10): on a refusal the device writes err=3 and does NOT echo the
		 * request seq (it writes seq=0), so we match echo ONLY; a timeout is a genuine no-response.
		 * Requires meter_poll_ms=0 (else the meter worker steals resp_buf / bumps seq).
		 */
		memset(c->resp_buf, 0, 32);
		ret = clarett_fcp(c, cmds[i].opcode, cmds[i].data, cmds[i].len);

		deadline = jiffies + msecs_to_jiffies(50);
		do {
			dma_rmb();
			echo = r[0] | r[1] << 8 | r[2] << 16 | r[3] << 24;
			rseq = r[FCP_RESP_SEQ_OFF] | r[FCP_RESP_SEQ_OFF + 1] << 8;
			if (echo == exp_echo) {
				landed = true;
				break;
			}
			usleep_range(200, 300);
		} while (time_before(jiffies, deadline));

		size = r[4] | r[5] << 8;
		err  = r[8] | r[9] << 8 | r[10] << 16 | r[11] << 24;
		dev_info(&c->pci->dev,
			 "error_probe: %-20s fcp_ret=%d %s echo=0x%08x seq=%u(exp %u) err=%u size=%u payload=%*ph\n",
			 cmds[i].name, ret, landed ? "RESP" : "NO-RESP",
			 echo, rseq, exp_seq, err, size, 8, r + FCP_RESP_DATA_OFF);
	}
}

/*
 * Seed the config shadow from the device. clarett_hw_init() zeroes the shadow, but the
 * command-3 enable bytes (72/73) pack one bit per output, so toggling the monitor outputs'
 * bits needs the real current bytes for a safe read-modify-write. GET the monitoring region
 * (offset 24, 92 bytes — covers 24/28/52/72-74/112) and copy the DMAed response in. The first
 * GET after programming the DMA address can come back empty (echo word 0), so retry briefly.
 * Guard on BOTH the echo word AND the response size: our device returns the header with size=0
 * and no payload (config backend dormant — see manifestation wall), and copying that would seed
 * the shadow with stale buffer bytes. Require size >= the bytes we read.
 */
static int clarett_seed_shadow(struct clarett *c)
{
	const u8 *r = c->resp_buf;
	u32 echo;
	u16 size;
	int err, attempt, i;

	for (attempt = 0; attempt < 3; attempt++) {
		err = clarett_get_data(c, MONITOR_CFG_OFFSET, MONITOR_CFG_LEN);
		if (err)
			return err;

		dma_rmb();	/* order the DMAed response before we read resp_buf */
		echo = r[FCP_RESP_ECHO_OFF] | r[FCP_RESP_ECHO_OFF + 1] << 8 |
		       r[FCP_RESP_ECHO_OFF + 2] << 16 | r[FCP_RESP_ECHO_OFF + 3] << 24;
		size = r[FCP_RESP_SIZE_OFF] | r[FCP_RESP_SIZE_OFF + 1] << 8;
		if (echo == (CMD_EXEC_FLAG | FCP_GET_DATA) && size >= MONITOR_CFG_LEN) {
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
static void clarett_ring_layout(struct clarett *c, size_t *tbl, size_t *smp, size_t *ring)
{
	*tbl = clarett_tbl_bytes();
	*smp = clarett_buf_bytes(c);
	*ring = clarett_ring_bytes(c);
}

/* 1 s after engine-start, log whether DMA advanced: period IRQs, pointer regs, capture-buffer writes. */
static void clarett_stream_report(struct work_struct *work)
{
	struct clarett *c = container_of(work, struct clarett, stream_report.work);
	size_t tbl, smp, ring, i;
	const u8 *rx_smp;
	bool rx_data = false;
	u32 c1_hits = 0, c2_hits = 0, c2_min = 0xffffffff, c2_max = 0;

	/*
	 * The real streaming signal is the block cause register, which the Windows driver POLLS (not
	 * MSI): during playback 0x300 returns 0x80000000 | period-counter stepping by 0xc, while
	 * 0x218/0x318 stay static (0x12/0x4 — the same values our engine reaches). Poll 0x200/0x300
	 * here in a ~40 ms burst; bit31-set reads with an advancing low counter == periods firing.
	 */
	for (i = 0; i < 2000; i++) {
		u32 c1 = readl(c->bar0 + STREAM_BLK0);		/* 0x200 cause (read-to-clear) */
		u32 c2 = readl(c->bar0 + STREAM_BLK1);		/* 0x300 cause (read-to-clear) */

		if (c1 & 0x80000000)
			c1_hits++;
		if (c2 & 0x80000000) {
			u32 ctr = c2 & 0x7fffffff;

			c2_hits++;
			if (ctr < c2_min)
				c2_min = ctr;
			if (ctr > c2_max)
				c2_max = ctr;
		}
		udelay(20);
	}
	if (c2_min == 0xffffffff)
		c2_min = 0;

	clarett_ring_layout(c, &tbl, &smp, &ring);
	rx_smp = (const u8 *)c->stream_buf + ring + tbl;	/* block-1 (capture) sample area */
	for (i = 0; i < smp; i++) {
		if (rx_smp[i] != 0xAA) {	/* any byte the device overwrote (incl. zeros) */
			rx_data = true;
			break;
		}
	}

	dev_info(&c->pci->dev,
		 "engine probe @1s: vec1=%d vec2=%d IRQs; ptr0=0x%x ptr1=0x%x; "
		 "cause-poll 0x200:hits=%u 0x300:hits=%u ctr=0x%x..0x%x; capture-buf=%s\n",
		 atomic_read(&c->period_irqs[1]), atomic_read(&c->period_irqs[2]),
		 readl(c->bar0 + STREAM_BLK0 + STREAM_OFF_PTR),
		 readl(c->bar0 + STREAM_BLK1 + STREAM_OFF_PTR),
		 c1_hits, c2_hits, c2_min, c2_max,
		 rx_data ? "WRITTEN (marker gone)" : "untouched (marker intact)");
}

/*
 * Persistent servicing engine. The data-plane DMA engine is flow-controlled: it raises a period on the
 * 0x300 cause register and waits for the host to ACK by reading it (read-to-clear), exactly as the
 * Windows driver does — it polls the cause block the whole time audio plays. Without this the engine
 * does ~4 descriptors and stalls; with it, it clocks for the stream lifetime (verified: counter climbs
 * monotonically while serviced). This kthread IS the device's required runtime; the future PCM path
 * calls snd_pcm_period_elapsed from here. Poll at ~5-10 kHz (the proven rate; Windows uses ~400 Hz, so
 * there is wide margin to slow this down once the counter->frame mapping is pinned).
 */
/*
 * Stall re-kick (experiment, module param `rekick`). The engine streams exactly one ring pass (~248
 * periods) then freezes even under continuous ACKing — Focusrite Control arms once and streams for
 * minutes, so the engine *can* wrap; we are missing whatever lets it. A FULL re-arm does NOT restart a
 * stalled engine (tested: re-arm after a pass => 0 periods), so try lighter nudges that don't disable
 * it. Re-writing the ring bases re-fetches the descriptor table from the top; 0x110 re-arms the period
 * IRQ. Methods are swept by `rekick` to find one (if any) that resumes the counter.
 */
static void clarett_stream_rekick(struct clarett *c)
{
	void __iomem *bar = c->bar0;

	switch (rekick) {
	case 4:	/* re-issue the stream-start commit (mailbox); heavier than a register poke */
		clarett_data_cmd(c, 5);
		break;
	case 2:	/* rewrite both ring bases (rewind descriptor walk) then re-arm */
		writel(readl(bar + STREAM_BLK0 + STREAM_OFF_BASE_HI), bar + STREAM_BLK0 + STREAM_OFF_BASE_HI);
		writel(readl(bar + STREAM_BLK0 + STREAM_OFF_BASE_LO), bar + STREAM_BLK0 + STREAM_OFF_BASE_LO);
		writel(readl(bar + STREAM_BLK1 + STREAM_OFF_BASE_HI), bar + STREAM_BLK1 + STREAM_OFF_BASE_HI);
		writel(readl(bar + STREAM_BLK1 + STREAM_OFF_BASE_LO), bar + STREAM_BLK1 + STREAM_OFF_BASE_LO);
		writel(0x7, bar + REG_STREAM_IRQ_ARM);
		break;
	case 3:	/* rewrite global enable then re-arm */
		writel(1, bar + STREAM_BLK0 + STREAM_OFF_CTRL);
		writel(0x7, bar + REG_STREAM_IRQ_ARM);
		break;
	default: /* 1: just re-arm the period IRQ */
		writel(0x7, bar + REG_STREAM_IRQ_ARM);
		break;
	}
}

static int clarett_stream_service(void *data)
{
	struct clarett *c = data;
	void __iomem *bar = c->bar0;
	unsigned long next_log = jiffies + msecs_to_jiffies(2000);
	unsigned long last_tick = jiffies;
	u32 wraps = 0, rekicks = 0;
	bool seen = false;

	while (!kthread_should_stop()) {
		u32 c2;

		/*
		 * Gate ACKing on stream_run. The engine is armed (and prefilled ~4 descriptors) by
		 * clarett_engine_arm, but stays paused until the PCM trigger flips stream_run — reading
		 * 0x300 (read-to-clear) IS the period ACK that releases it, so withholding the read keeps
		 * the engine quiescent between prepare and START. The probe path sets stream_run at start.
		 */
		if (!READ_ONCE(c->stream_run)) {
			usleep_range(300, 600);
			continue;
		}

		/*
		 * Mirror Windows' cause-block poll: read 0x200/0x300/0x500 each loop. We skip 0x100 (mailbox
		 * cause — read-to-clear, racing clarett_fcp's poll) and 0x400 (notify).
		 */
		readl(bar + STREAM_BLK0);		/* 0x200 TX cause */
		c2 = readl(bar + STREAM_BLK1);		/* 0x300 read-to-clear = period ack */
		readl(bar + 0x500);			/* 0x500 IRQ summary */
		if (c2 & 0x80000000) {
			u32 ctr = c2 & 0x7fffffff;

			if (seen && ctr < c->stream_ctr)
				wraps++;
			c->stream_ctr = ctr;
			seen = true;
			last_tick = jiffies;
			atomic_inc(&c->stream_periods);
			clarett_pcm_tick(c);		/* advance PCM pointer / period_elapsed (no-op if idle) */
		} else if (rekick && seen &&
			   time_after(jiffies, last_tick + msecs_to_jiffies(rekick_ms))) {
			/* Stalled mid-stream: counter frozen for rekick_ms while still ACKing. Nudge it. */
			if (!rekicks)
				dev_info(&c->pci->dev,
					 "stall dump: caps=%08x irq0=%08x blk0=%08x blk1=%08x p0=%08x p1=%08x info=%08x (0xffffffff == dead/off-bus)\n",
					 readl(bar + REG_CAPS), readl(bar + REG_IRQ0_CAUSE),
					 readl(bar + STREAM_BLK0), readl(bar + STREAM_BLK1),
					 readl(bar + STREAM_BLK0 + STREAM_OFF_PTR),
					 readl(bar + STREAM_BLK1 + STREAM_OFF_PTR),
					 readl(bar + REG_INFO));
			clarett_stream_rekick(c);
			rekicks++;
			last_tick = jiffies;		/* don't re-kick until another rekick_ms elapses */
		}
		if (time_after(jiffies, next_log)) {
			dev_info(&c->pci->dev,
				 "stream-svc: periods=%d ctr=0x%x wraps=%u rekicks=%u\n",
				 atomic_read(&c->stream_periods), c->stream_ctr, wraps, rekicks);
			next_log = jiffies + msecs_to_jiffies(2000);
		}
		usleep_range(100, 200);
	}
	dev_info(&c->pci->dev, "stream-svc: stopped (periods=%d ctr=0x%x wraps=%u rekicks=%u)\n",
		 atomic_read(&c->stream_periods), c->stream_ctr, wraps, rekicks);
	return 0;
}

/*
 * Arm the data-plane engine over caller-provided descriptor-table bases (data-plane spec §3b/§9).
 * r0 = block-0 (TX/playback) table base, r1 = block-1 (RX/capture) table base; pass 0 to skip a block
 * (capture-only uses r0=0, the proven blk1_only config). Replays SET_CLOCK, the 12-register stream
 * arm (base-before-enable), and the DATA_CMD{5} commit. Sleeps (mailbox FCP) — call from prepare or
 * probe context, never from the atomic PCM trigger. Leaves the engine armed-and-committed but paused:
 * it prefills a few descriptors and waits for the servicer to ACK 0x300 (gated by stream_run).
 */
void clarett_engine_arm(struct clarett *c, dma_addr_t r0, dma_addr_t r1)
{
	void __iomem *bar = c->bar0;

	/*
	 * Faithful replica of the VM's register-only arm — the exact 14-write sequence that brings 0x300 alive
	 * (2pre_streamstart.log @line 29158: 0x110=0 stop, then 0x100=0xf, 0x108, 0x20c=1, per-block geometry,
	 * 0x10c, 0x110=7, and 0x300 immediately reads 0x8000000c, ticking +0xc/period). Two corrections vs the
	 * old order: (1) ack the cause block (0x100=0xf) first — we previously only READ 0x100, never wrote it;
	 * (2) the global enable (0x20c=1) comes BEFORE the geometry/base writes, as the VM does (DMA only starts
	 * on the 0x110=7 arm, which stays last, so there is no null-base fault).
	 *
	 * NO per-arm FCP. The VM issues SET_CLOCK / CONFIG_PUSH / 0x6004 / 0x6005 only in the in-session
	 * handshake (clarett_stream_handshake, run from PCM prepare BEFORE this), and issues NO DATA_CMD at all
	 * during 2Pre stream start. The old SET_CLOCK + DATA_CMD{5} here were redundant/wrong (the device latches
	 * the clock during the handshake's CONFIG_PUSH) and one SET_CLOCK was timing out (-110). A null base arg
	 * skips that block (capture-only passes r0=0).
	 */
	writel(0xf, bar + REG_IRQ0_CAUSE);				/* 0x100 = 0xf: ack cause block */
	writel(0x10, bar + REG_STREAM_IRQ_CFG);				/* 0x108 */
	writel(1, bar + STREAM_BLK0 + STREAM_OFF_CTRL);			/* 0x20c global enable (before geometry) */

	if (r0) {
		writel(c->model->playback_channels, bar + STREAM_BLK0 + STREAM_OFF_CHANS); /* 0x204 */
		writel(clarett_period_bytes(c->model->playback_channels),
		       bar + STREAM_BLK0 + STREAM_OFF_SIZE);				  /* 0x208 period */
		writel(upper_32_bits(r0), bar + STREAM_BLK0 + STREAM_OFF_BASE_HI); /* 0x214 */
		writel(lower_32_bits(r0), bar + STREAM_BLK0 + STREAM_OFF_BASE_LO); /* 0x210 (low last) */
	}
	if (r1) {
		writel(c->model->capture_channels, bar + STREAM_BLK1 + STREAM_OFF_CHANS);  /* 0x304 */
		writel(clarett_period_bytes(c->model->capture_channels),
		       bar + STREAM_BLK1 + STREAM_OFF_SIZE);				  /* 0x308 period */
		writel(upper_32_bits(r1), bar + STREAM_BLK1 + STREAM_OFF_BASE_HI); /* 0x314 */
		writel(lower_32_bits(r1), bar + STREAM_BLK1 + STREAM_OFF_BASE_LO); /* 0x310 */
	}

	writel(0x1e70700, bar + REG_STREAM_IRQ_CFG2);			/* 0x10c */
	writel(0x7, bar + REG_STREAM_IRQ_ARM);				/* 0x110 arm (0x0 is stream-stop) */

	c->stream_on = true;
}

/* Start the persistent 0x300 servicer kthread. The caller flips stream_run to release ACKing. */
void clarett_engine_run(struct clarett *c)
{
	atomic_set(&c->stream_periods, 0);
	c->stream_ctr = 0;
	c->stream_svc = kthread_run(clarett_stream_service, c, "clarett-svc");
	if (IS_ERR(c->stream_svc)) {
		dev_warn(&c->pci->dev, "stream servicer failed to start: %ld\n", PTR_ERR(c->stream_svc));
		c->stream_svc = NULL;
	}
}

/*
 * Data-plane engine-start probe (data-plane spec §9, opt-in via stream_probe). Replays the captured
 * §3b stream-start register sequence, but now with a valid descriptor table per §3c: 0x210/0x214 point
 * at a zeroed-terminated array of 8-byte bus addresses, each naming one STREAM_SIZE_VAL fragment of our
 * coherent buffer. Then watches whether the engine runs (vec1/vec2 IRQs, advancing pointer, the device
 * writing the capture buffer). NOT a PCM implementation. The point is to test whether starting the
 * engine makes the control plane physically manifest (e.g. the Mute LED).
 */
int clarett_engine_start(struct clarett *c)
{
	void __iomem *bar = c->bar0;
	size_t tbl, smp, ring;
	__le64 *tx_tbl, *rx_tbl;
	dma_addr_t tx_smp, rx_smp, r0, r1;
	unsigned int i;

	clarett_ring_layout(c, &tbl, &smp, &ring);
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
		tx_tbl[i] = cpu_to_le64(tx_smp + (dma_addr_t)i * c->model->stream_frag);
		rx_tbl[i] = cpu_to_le64(rx_smp + (dma_addr_t)i * c->model->stream_frag);
	}

	/*
	 * The live vendor descriptor table (RAM dump of the Windows TX ring) flags ONLY its last entry
	 * with bit 0 set; every other entry is 0x100-aligned (low bit clear). That bit is the engine's
	 * end-of-list / ring-wrap marker. Our driver previously relied on the zero terminator alone, and
	 * the playback engine's per-descriptor status writeback bursts to base 0 without it. Fragment
	 * addresses are even (FRAG=0x1c0, page-aligned base), so bit 0 is free to carry the flag.
	 */
	tx_tbl[CLARETT_STREAM_NDESC - 1] |= cpu_to_le64(1);
	rx_tbl[CLARETT_STREAM_NDESC - 1] |= cpu_to_le64(1);

	/*
	 * Mark the RX (capture) sample area with 0xAA so the report can tell "device wrote silence (zeros)"
	 * from "device never wrote" — with nothing plugged in, a working capture writes near-zero samples
	 * that a plain non-zero scan would miss.
	 */
	memset((u8 *)c->stream_buf + ring + tbl, 0xAA, smp);

	r0 = c->stream_dma;		/* block-0 descriptor-table base */
	r1 = c->stream_dma + ring;	/* block-1 descriptor-table base */

	/* Arm+commit the engine (SET_CLOCK, 12-register sequence, DATA_CMD{5}); blk1_only skips block 0. */
	clarett_engine_arm(c, blk1_only ? 0 : r0, r1);
	WRITE_ONCE(c->stream_run, true);	/* probe streams immediately — no PCM trigger to gate it */
	clarett_engine_run(c);

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
void clarett_engine_stop(struct clarett *c)
{
	if (!c->stream_on)
		return;
	WRITE_ONCE(c->stream_run, false);		/* stop the servicer ACKing 0x300 */
	if (c->stream_svc) {
		kthread_stop(c->stream_svc);		/* stop acking 0x300 before the engine is torn down */
		c->stream_svc = NULL;
	}
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
		u32 ev = cause & NOTIFY_MONITOR_MASK;
		bool inflight = atomic_read(&c->cmd_inflight);

		/* vec0 also fires on mailbox-DONE, and 0x400 reads its idle level 0x3 (== NOTIFY_MON_PRIMARY)
		 * at completion time (see the REG_NOTIFY_CAUSE note in clarett.h). Skipping the notify path
		 * while our own command is in flight suppresses that self-reflection. NOTE (hardware July 6
		 * 2026): this is minor — the bulk of the "notification retried indefinitely" storm is the
		 * DEVICE genuinely re-asserting 0x3 (us-scale bursts, inflight=0) because our GET is empty;
		 * the guard can't stop that. Real front-panel events also arrive in the idle gaps. */
		if (ev && !inflight) {
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
		/* Rate-limited: on a walled device the periodic unsatisfied config-change notification
		 * (cause 0x3 ~every few s) makes this GET time out repeatedly; dev_warn would flood dmesg. */
		dev_warn_ratelimited(&c->pci->dev, "notify 0x%x: monitor re-read failed (%d)\n",
				     ev, err);
	} else {
		const u8 *r = c->resp_buf;
		u32 echo;
		u16 size;

		dma_rmb();	/* order the DMAed response before we read resp_buf */
		echo = r[FCP_RESP_ECHO_OFF] | r[FCP_RESP_ECHO_OFF + 1] << 8 |
		       r[FCP_RESP_ECHO_OFF + 2] << 16 | r[FCP_RESP_ECHO_OFF + 3] << 24;
		size = r[FCP_RESP_SIZE_OFF] | r[FCP_RESP_SIZE_OFF + 1] << 8;

		/* Guard on echo AND size: our device returns echo+0x03 success with size=0 and no
		 * payload (config backend dormant), so consuming resp[16+] would copy stale buffer
		 * bytes into the monitor shadow. Require the full region before refreshing. */
		if (echo == (CMD_EXEC_FLAG | FCP_GET_DATA) && size >= MONITOR_CFG_LEN) {
			const u8 *data = r + FCP_RESP_DATA_OFF;

			/* data[i] == config[MONITOR_CFG_OFFSET + i] */
			c->shadow[24]  = data[24  - MONITOR_CFG_OFFSET];
			c->shadow[28]  = data[28  - MONITOR_CFG_OFFSET];
			c->shadow[112] = data[112 - MONITOR_CFG_OFFSET];
		} else {
			/* Empty/absent payload — keep the write-through shadow. On our device this is
			 * the normal case (echo present, size=0: config backend dormant); echo=0 is the
			 * older first-GET-at-load case. Either way, don't seed the shadow with stale bytes. */
			dev_dbg(&c->pci->dev,
				"notify 0x%x: empty GET response (echo=0x%08x size=%u) — shadow kept\n",
				ev, echo, size);
		}
	}

	for (i = 0; i < c->n_ctls; i++)
		if (c->ctls[i].activate == MONITOR_ACTIVATE && c->ctls[i].kctl)
			snd_ctl_notify(c->card, SNDRV_CTL_EVENT_MASK_VALUE,
				       &c->ctls[i].kctl->id);

	dev_dbg(&c->pci->dev, "async notification handled: 0x%x\n", ev);
}

/*
 * GET_METER heartbeat. Focusrite Control polls GET_METER continuously while connected, and that poll
 * turns out to be the device's required host heartbeat: without it, control writes complete (done=1,
 * fcperr=0) but never reach hardware (front-panel state frozen). We replay FC's exact 8-byte payload
 * and re-arm ourselves every meter_poll_ms. The response (DMAed meter levels) is ignored — only the
 * periodic transaction matters. Self-requeuing delayed_work; cancelled at remove.
 */
static void clarett_meter_work(struct work_struct *work)
{
	struct clarett *c = container_of(work, struct clarett, meter_work.work);
	static const u8 meter_req[8] = { 0x00, 0x00, 0x30, 0x00, 0x01, 0x00, 0x00, 0x00 };
	int delay = meter_poll_ms > 0 ? meter_poll_ms : CLARETT_METER_POLL_MS;

	if (meter_poll_ms > 0)
		clarett_fcp(c, FCP_GET_METER, meter_req, sizeof(meter_req));

	if (meter_poll_ms > 0)
		schedule_delayed_work(&c->meter_work, msecs_to_jiffies(delay));
}

/* Enable MSI and hook the notification vector. Best-effort: on failure the driver
 * still works (control plane, polled mailbox) but without async notifications. */
static void clarett_setup_irq(struct clarett *c)
{
	struct pci_dev *pci = c->pci;
	int i, nvec, err;

	/* Request up to CLARETT_NUM_VECTORS but accept as few as 1: under vfio passthrough the guest
	 * often cannot grant all 4 (seen: -ENOSPC for min=max=4), and hard-failing left us with ZERO
	 * interrupts — a real divergence from FC, which always has MSI up. With MSI collapsed to fewer
	 * vectors the device funnels all causes to the lower vector(s); clarett_irq reads the cause regs
	 * regardless, so notifications still work. */
	nvec = pci_alloc_irq_vectors(pci, 1, CLARETT_NUM_VECTORS, PCI_IRQ_MSI);
	if (nvec < 0) {
		dev_warn(&pci->dev,
			 "MSI alloc failed (%d); async notifications disabled\n", nvec);
		return;
	}
	c->n_vec = nvec;
	/* Always record the achieved count: 4/4 matches FC (session_notes.log: Enable+ Count=4/4);
	 * fewer means the platform (typically vfio passthrough) collapsed them and causes funnel to
	 * the allocated vectors. Logged unconditionally so every run pins down what it actually got. */
	dev_info(&pci->dev, "MSI: got %d/%d vectors%s\n", nvec, CLARETT_NUM_VECTORS,
		 nvec < CLARETT_NUM_VECTORS ? " (causes funnel to the allocated ones)" : "");

	for (i = 0; i < nvec; i++) {
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
	for (i = 0; i < c->n_vec; i++)
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
	/* Shared PCI id across the line → the match can't pick the model; the model= param does
	 * (clarett_pick_model), defaulting to the id_table's 2Pre. See clarett_pick_model(). */
	c->model = clarett_pick_model(ent);
	dev_info(&pci->dev, "model: %s\n", c->model->name);
	mutex_init(&c->mbox_lock);
	INIT_WORK(&c->notify_work, clarett_notify_work);
	INIT_DELAYED_WORK(&c->meter_work, clarett_meter_work);
	atomic_set(&c->notify_bits, 0);
	atomic_set(&c->cmd_inflight, 0);
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

	if (dma_bits < 28 || dma_bits > 64)
		dma_bits = 32;
	err = dma_set_mask_and_coherent(&pci->dev, DMA_BIT_MASK(dma_bits));
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

	/* Start the GET_METER heartbeat. FC polls continuously from connect onward; the device
	 * needs it to apply control writes to hardware. Run it for the rest of probe too, so the
	 * monitor-enable writes below take effect like a real session. */
	if (meter_poll_ms > 0)
		schedule_delayed_work(&c->meter_work,
				      msecs_to_jiffies(meter_poll_ms > 0 ? meter_poll_ms : CLARETT_METER_POLL_MS));

	/* Seed the shadow from the device so mixer "get" reflects real state at load and the
	 * enable-byte RMW below is safe. Best-effort: if it fails we skip the enable writes. */
	seeded = clarett_seed_shadow(c);
	if (seeded)
		dev_warn(&pci->dev,
			 "config shadow seed failed (%d); leaving hardware mute/dim enables untouched\n",
			 seeded);

	/* Diagnostic: characterize the FCP error=3 refusal (blanket session block vs per-command).
	 * Requires meter_poll_ms=0 so the meter worker doesn't race the shared resp_buf/seq (see param desc). */
	if (error_probe) {
		if (meter_poll_ms > 0)
			dev_warn(&pci->dev,
				 "error_probe needs meter_poll_ms=0 (meter worker races the shared response buffer); results unreliable\n");
		clarett_error_probe(c);
	}

	err = clarett_create_controls(c);
	if (err)
		goto err_free;

	/* Make the global Mute/Dim controls actually affect Monitor Out 1-2. Needs the seeded
	 * shadow for a correct read-modify-write, so only attempt it when seeding succeeded. */
	if (!seeded && monitor_enables) {
		err = clarett_enable_monitor_hw_controls(c);
		if (err)
			dev_warn(&pci->dev,
				 "could not enable monitor hardware mute/dim (%d)\n", err);
	}

	clarett_setup_irq(c);	/* best-effort; controls must exist first (snd_ctl_notify) */

	/* Experimental capture PCM (data-plane bring-up). Owns the engine, so it excludes stream_probe. Both
	 * models use the per-direction descriptor path (geometry derived from channel counts), so no per-model
	 * gate is needed beyond enable_pcm. */
	if (enable_pcm) {
		err = clarett_create_pcm(c);
		if (err)
			dev_warn(&pci->dev, "PCM create failed (%d); continuing mixer-only\n", err);
		err = 0;
	}

	/* Opt-in data-plane experiment: start the audio engine and watch what happens. Best-effort;
	 * needs the IRQ handlers (above) hooked first so vec1/vec2 period IRQs are counted. */
	if (stream_probe && c->model->stream_frag && !enable_pcm && c->irq_ready) {
		err = clarett_engine_start(c);
		if (err)
			dev_warn(&pci->dev, "engine-start probe failed (%d)\n", err);
		err = 0;
	}

	strscpy(card->driver, "Clarett", sizeof(card->driver));
	snprintf(card->shortname, sizeof(card->shortname), "Focusrite %s", c->model->name);
	snprintf(card->longname, sizeof(card->longname),
		 "%s at %s, fw app 0x%08x", card->shortname, pci_name(pci),
		 c->fw_app);

	err = snd_card_register(card);
	if (err)
		goto err_free;

	pci_set_drvdata(pci, card);
	dev_info(&pci->dev,
		 "%s: serial %08x%08x fw app 0x%08x fpga 0x%08x\n",
		 c->model->name, c->serial_hi, c->serial_lo, c->fw_app, c->fw_fpga);
	return 0;

err_free:
	cancel_delayed_work_sync(&c->meter_work);	/* stop the heartbeat before teardown */
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

	cancel_delayed_work_sync(&c->meter_work);	/* stop the heartbeat poll */
	clarett_engine_stop(c);			/* halt streaming DMA before the buffer is freed */
	writel(0, c->bar0 + REG_IRQ0_ENABLE);	/* mask causes before freeing handlers */
	clarett_teardown_irq(c);		/* free MSI vectors / IRQ handlers */
	cancel_work_sync(&c->notify_work);	/* flush any in-flight notification work */
	snd_card_free(card);
	/* BAR mapping, DMA buffer and device enable are devres-managed */
}

/* --- per-model descriptors ---------------------------------------------- */

static const struct clarett_out_gain clarett_8prex_gains[] = {
	{ "Monitor 1", 32 }, { "Monitor 2", 33 },
	{ "Line 3", 36 }, { "Line 4", 37 }, { "Line 5", 40 }, { "Line 6", 41 },
	{ "Line 7", 44 }, { "Line 8", 45 }, { "Line 9", 48 }, { "Line 10", 49 },
};

static const char * const clarett_mode_mli[] = { "Mic", "Line", "Inst" };
static const char * const clarett_mode_ml[]  = { "Mic", "Line" };

/* Analogue 1-2 add Inst; 3-8 are Mic/Line. Device byte == text index (identity). */
static const struct clarett_preamp clarett_8prex_preamps[] = {
	{ clarett_mode_mli, NULL, 3 }, { clarett_mode_mli, NULL, 3 },
	{ clarett_mode_ml,  NULL, 2 }, { clarett_mode_ml,  NULL, 2 },
	{ clarett_mode_ml,  NULL, 2 }, { clarett_mode_ml,  NULL, 2 },
	{ clarett_mode_ml,  NULL, 2 }, { clarett_mode_ml,  NULL, 2 },
};

static const struct clarett_model clarett_8prex = {
	.name = "Clarett 8PreX",
	.out_gains = clarett_8prex_gains,
	.n_out_gains = ARRAY_SIZE(clarett_8prex_gains),
	.n_analogue = 8,
	.analogue = clarett_8prex_preamps,
	.capture_channels = STREAM_CHANS,
	.playback_channels = STREAM_CHANS,
	.stream_frag = STREAM_SIZE_VAL,
	.init_blob = clarett_init_blob_8prex,
	.init_seq = clarett_init_seq_8prex,
	.n_init_steps = ARRAY_SIZE(clarett_init_seq_8prex),
};

/*
 * Clarett 2Pre (Thunderbolt). Control-plane values from the XML diff against the 8PreX
 * (vendor-reference/Devices/Clarett 2Pre.xml): shared offsets/commands, the first 4 of the 8PreX output
 * gains, 2 combo-jack preamps with the Line/Inst encoding (Line=1, Inst=2 — Mic is auto-detected by the
 * jack, not a software mode; see clarett_mode_li. clarett_ctl.values handles the value mapping).
 * Channel counts 4 playback / 14 record are HARDWARE-CONFIRMED (GET_7.2=0x04 / GET_7.3=0x0e in the boot
 * trace). The bring-up replay is the captured 2Pre attach (clarett_init_2pre.h). Selected via the model=
 * param: the whole Clarett TB line shares PCI id 1cb5:0002 and an identical PCIe interface, so the model
 * is NOT auto-detectable (verified: every MMIO reg / FCP response / config read / PCI config byte is
 * identical to the 8PreX, and these TB2 units expose no DROM device_name to disambiguate by either).
 * PCM uses the per-direction descriptor path (shared with the 8PreX): on hardware the engine dereferences
 * the ring base as a descriptor table (the 0xAA flat-buffer attempt faulted at 0xaaaa.. — descriptor mode
 * is the engine's default and is not flipped by the stream-config FCP handshake we can replay). Asymmetric
 * TX 4ch / RX 14ch: periods 0x40 / 0xe0, descriptor fragments 0x100 / 0x700 (clarett_frag_bytes). The 2Pre's
 * descriptor geometry is INFERRED (the VM uses flat mode for it); see clarett.h / spec §9 step 5.
 */
static const struct clarett_out_gain clarett_2pre_gains[] = {
	{ "Monitor 1", 32 }, { "Monitor 2", 33 }, { "Line 3", 36 }, { "Line 4", 37 },
};

/*
 * Combo-jack input mode (shared by 2Pre / 4Pre / 8Pre). These models use a single combined XLR/TRS jack
 * per input, so the connector auto-selects Mic (XLR inserted) vs the 1/4" path — Mic is NOT a software
 * option; the mode control only chooses Line vs Inst for the 1/4" path. So the enum is {Line=1, Inst=2}
 * with no Mic(0). (The 8PreX, by contrast, has SEPARATE XLR and 1/4" ports, so software must pick
 * Mic/Line/Inst explicitly — see clarett_mode_mli.)
 */
static const char * const clarett_mode_li[] = { "Line", "Inst" };
static const u8 clarett_mode_li_vals[] = { 1, 2 };

static const struct clarett_preamp clarett_2pre_preamps[] = {
	{ clarett_mode_li, clarett_mode_li_vals, 2 },
	{ clarett_mode_li, clarett_mode_li_vals, 2 },
};

/* Per-channel stream-routing CONFIG_PUSH ids, captured verbatim from the 2Pre rate-change handshake
 * (2pre_streamstart.log): 4 TX (playback) + 14 RX (record) channels, re-pushed at PCM prepare. */
static const u8 clarett_2pre_stream_tx[] = { 0x2b, 0x2c, 0x2d, 0x2e };
static const u8 clarett_2pre_stream_rx[] = {
	0x0d, 0x0e, 0x15, 0x16, 0x27, 0x28, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e,
};

static const struct clarett_model clarett_2pre = {
	.name = "Clarett 2Pre",
	.out_gains = clarett_2pre_gains,
	.n_out_gains = ARRAY_SIZE(clarett_2pre_gains),
	.n_analogue = 2,
	.analogue = clarett_2pre_preamps,
	.capture_channels = 14,			/* record-outputs pin count (12 record + 2 loopback) */
	.playback_channels = 4,			/* playback pin count */
	.stream_frag = 0,			/* legacy engine-start probe unused on the 2Pre; PCM uses
						 * clarett_frag_bytes() per direction */
	.stream_tx_ids = clarett_2pre_stream_tx,
	.n_stream_tx_ids = ARRAY_SIZE(clarett_2pre_stream_tx),
	.stream_rx_ids = clarett_2pre_stream_rx,
	.n_stream_rx_ids = ARRAY_SIZE(clarett_2pre_stream_rx),
	.init_blob = clarett_init_blob_2pre,
	.init_seq = clarett_init_seq_2pre,
	.n_init_steps = ARRAY_SIZE(clarett_init_seq_2pre),
};

/*
 * Clarett 4Pre (Thunderbolt). Control plane from vendor-reference/Devices/Clarett 4Pre.xml [XML],
 * cross-checked against a live FC capture (4pre_boot_to_stream_end.log) [TRACE]. Selected via model=4pre;
 * the whole TB line shares PCI id 1cb5:0002 and is not auto-detectable. See
 * spec/clarett-control-plane.md §4 and -data-plane.md §3b.
 *
 *   [TRACE] channel counts 8 playback / 20 record (GET_7.2=0x08 / GET_7.3=0x14, read 6x; XML-consistent:
 *           8 Playback pins, 18 record + 2 loopback = 20 record-output pins).
 *   [TRACE] stream-routing CONFIG_PUSH ids — captured verbatim from the in-session rate handshake
 *           (#674-#702): 8 TX after GET_7.2, 20 RX after GET_7.3, in wire order.
 *   [XML]   inputs: only Analogue 1-2 have a mode (Line=1/Inst=2; Mic is auto-detected by the combo
 *           XLR/TRS jack, not a software option — see clarett_mode_li), at mode@166/167 cmd 6;
 *           Analogue 1-4 each have Air at air@174..177 cmd 7; Analogue 5-8 have no preamp controls.
 *           So n_analogue=4 (the Air-capable inputs); 3-4 are air-only (n_modes=0). The Analogue-1
 *           mode/air encoding is additionally [TRACE]-confirmed (the "Line In 1" toggles in this capture).
 *   [XML]   output gains (cmd 1, 8-bit): Monitor 1-2 @ 32/33, Line 3-4 @ 36/37, Headphone 2 L/R @ 40/41.
 *           S/PDIF outputs carry no gain. Six gains total (no 44/45 pair on this model).
 */
static const struct clarett_out_gain clarett_4pre_gains[] = {		/* [XML] */
	{ "Monitor 1", 32 }, { "Monitor 2", 33 },
	{ "Line 3", 36 }, { "Line 4", 37 },
	{ "Headphone 2 L", 40 }, { "Headphone 2 R", 41 },
};

/* [XML] Analogue 1-2 Line/Inst (combo jack auto-detects Mic; Analogue-1 also [TRACE]-confirmed); 3-4 air-only (n_modes=0). */
static const struct clarett_preamp clarett_4pre_preamps[] = {
	{ clarett_mode_li, clarett_mode_li_vals, 2 },
	{ clarett_mode_li, clarett_mode_li_vals, 2 },
	{ NULL, NULL, 0 },
	{ NULL, NULL, 0 },
};

/* [TRACE] per-channel stream-routing CONFIG_PUSH ids, captured from the 4Pre rate handshake
 * (4pre_boot_to_stream_end.log #674-#702): 8 TX (playback) + 20 RX (record), re-pushed at PCM prepare. */
static const u8 clarett_4pre_stream_tx[] = { 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32 };
static const u8 clarett_4pre_stream_rx[] = {
	0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
	0x27, 0x28, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e,
};

static const struct clarett_model clarett_4pre = {
	.name = "Clarett 4Pre",
	.out_gains = clarett_4pre_gains,
	.n_out_gains = ARRAY_SIZE(clarett_4pre_gains),
	.n_analogue = 4,
	.analogue = clarett_4pre_preamps,
	.capture_channels = 20,			/* [TRACE] GET_7.3=0x14 record-outputs pin count */
	.playback_channels = 8,			/* [TRACE] GET_7.2=0x08 playback pin count */
	.stream_frag = 0,			/* PCM uses clarett_frag_bytes() per direction (asymmetric) */
	.stream_tx_ids = clarett_4pre_stream_tx,
	.n_stream_tx_ids = ARRAY_SIZE(clarett_4pre_stream_tx),
	.stream_rx_ids = clarett_4pre_stream_rx,
	.n_stream_rx_ids = ARRAY_SIZE(clarett_4pre_stream_rx),
	.init_blob = clarett_init_blob_4pre,
	.init_seq = clarett_init_seq_4pre,
	.n_init_steps = ARRAY_SIZE(clarett_init_seq_4pre),
};

/*
 * Clarett 8Pre (Thunderbolt) — a DISTINCT model from the 8PreX (do not confuse). Control plane from
 * vendor-reference/Devices/Clarett 8Pre.xml [XML]. We have no 8Pre trace capture, so this descriptor
 * is control-plane only: it registers the mixer but has no bring-up replay or stream-routing ids, so it
 * will NOT arm config access (GET_DATA wedged) or stream PCM until an 8Pre boot/stream is captured (then
 * emit clarett_init_8pre.h via fcp_decode.py --emit-init --init-model 8pre and fill the stream id tables).
 *
 * Differences from the 8PreX [XML] — these are physical: the 8Pre uses combo XLR/TRS jacks (the jack
 * auto-detects Mic when an XLR is inserted), whereas the 8PreX has SEPARATE XLR + 1/4" ports per input
 * (so its mode must be software-selected). Hence:
 *   - inputs: all 8 carry Air @ 174..181 (cmd 7), but only Analogue 1-2 have a mode (Line=1/Inst=2 for
 *     the 1/4" path; Mic is jack-auto, not a software option) @ 166/167 (cmd 6); Analogue 3-8 are
 *     air-only. (The 8PreX, with discrete ports, exposes software Mic/Line[/Inst] on all 8.)
 *   - streams: 20 playback / 20 record (the 8PreX is 28/28 — the 8Pre has a single ADAT bank).
 *   - outputs: IDENTICAL 10-gain map to the 8PreX (Monitor 1-2 @ 32/33, Line 3-10 @ 36..49), so the
 *     clarett_8prex_gains[] table is reused verbatim.
 */
static const struct clarett_preamp clarett_8pre_preamps[] = {	/* [XML] 1-2 Line/Inst, 3-8 air-only */
	{ clarett_mode_li, clarett_mode_li_vals, 2 },
	{ clarett_mode_li, clarett_mode_li_vals, 2 },
	{ NULL, NULL, 0 }, { NULL, NULL, 0 },
	{ NULL, NULL, 0 }, { NULL, NULL, 0 },
	{ NULL, NULL, 0 }, { NULL, NULL, 0 },
};

static const struct clarett_model clarett_8pre = {
	.name = "Clarett 8Pre",
	.out_gains = clarett_8prex_gains,	/* [XML] identical 10-output gain map to the 8PreX */
	.n_out_gains = ARRAY_SIZE(clarett_8prex_gains),
	.n_analogue = 8,
	.analogue = clarett_8pre_preamps,
	.capture_channels = 20,			/* [XML] 18 record + 2 loopback (untraced; no 8Pre capture) */
	.playback_channels = 20,		/* [XML] Playback 1-20 (untraced) */
	.stream_frag = 0,
	/* no .init_blob / .stream_*_ids: requires an 8Pre capture — see the comment above. */
};

static const struct pci_device_id clarett_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_FOCUSRITE, PCI_DEVICE_CLARETT),
	  .driver_data = (kernel_ulong_t)&clarett_2pre },
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
