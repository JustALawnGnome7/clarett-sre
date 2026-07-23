/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Focusrite Clarett 8PreX (Thunderbolt) ALSA driver — shared definitions.
 *
 * Register map and FCP framing are from the reverse-engineering notes in
 * ../spec/clarett-fcp-transport.md (confirmed against MMIO traces).
 * Control offsets/commands are from ../spec/clarett-control-plane.md.
 */
#ifndef CLARETT_H
#define CLARETT_H

#include <linux/types.h>
#include <linux/bitmap.h>		/* DECLARE_BITMAP, set_bit/test_bit — shadow_known */
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/atomic.h>
#include <linux/wait.h>		/* wait_queue_head_t — hwdep notification relay */
#include <linux/workqueue.h>
#include <linux/lcm.h>		/* lcm() — descriptor fragment alignment */
#include <linux/log2.h>		/* roundup_pow_of_two() — page-safe fragment slots */
#include <sound/core.h>

struct snd_kcontrol;
struct snd_pcm;
struct snd_pcm_substream;

/* --- BAR0 register map (confirmed) -------------------------------------- */
#define CLARETT_BAR              0
#define REG_CAPS                 0x000
#define REG_SERIAL_LO            0x010
#define REG_SERIAL_HI            0x014
#define REG_IRQ0_CAUSE           0x100   /* read-to-clear; bit DONE = mailbox complete */
#define REG_IRQ0_ENABLE          0x104   /* observed init value 0xf000003f            */
#define REG_NOTIFY_CAUSE         0x400   /* read-to-clear; carries the §11 notify mask */
#define REG_DOORBELL             0x408   /* write 1 = submit, 2 = ack/clear prior      */
#define REG_DMA_ADDR_LO          0x410   /* GET-response DMA buffer bus address (low 32)  */
#define REG_DMA_ADDR_HI          0x414   /* DMA buffer bus address (high 32) — confirmed  */
#define REG_INFO                 0x8000  /* read-only fw-info header (fw versions, ...) */
#define REG_MBOX                 0x8020  /* FCP request mailbox                         */

/*
 * Data-plane streaming registers (recovered from a streaming capture; data-plane spec §3b).
 * Two structurally identical ring blocks; block 0 (0x200) → MSI vec1, block 1 (0x300) → vec2.
 * `clarett_engine_start()` replays the captured stream-start sequence with our own ring buffer.
 */
#define REG_STREAM_IRQ_CFG       0x108   /* stream-start writes 0x10        */
#define REG_STREAM_IRQ_CFG2      0x10c   /* stream-start writes 0x1e70700   */
#define REG_STREAM_IRQ_ARM       0x110   /* stream-start writes 0x7 then 0x0 */
#define STREAM_BLK0              0x200   /* ring block 0 (vec1); +0x00 = cause (read-to-clear) */
#define STREAM_BLK1              0x300   /* ring block 1 (vec2) */
#define   STREAM_OFF_CHANS       0x04    /* channel count = 0x1c (28)       */
#define   STREAM_OFF_SIZE        0x08    /* size/period   = 0x1c0 [HYP]     */
#define   STREAM_OFF_CTRL        0x0c    /* enable bit    = 1               */
#define   STREAM_OFF_BASE_LO     0x10    /* ring base bus address low 32    */
#define   STREAM_OFF_BASE_HI     0x14    /* ring base bus address high 32   */
#define   STREAM_OFF_PTR         0x18    /* DMA position (read-only)        */
#define STREAM_CHANS             0x1c    /* 8PreX: 28 PCM channels/direction (populates clarett_8prex) */
#define STREAM_SIZE_VAL          0x1c0   /* 8PreX: bytes the engine DMAs per descriptor (0x208 reg)    */
/*
 * Descriptor ring (data-plane spec §3c). 0x210/0x214 (and 0x310/0x314) point at a table of bare
 * 8-byte little-endian guest-physical addresses, zero-terminated; each entry is one DMA fragment of
 * clarett_model.stream_frag bytes holding capture_channels-wide S32_LE (24-bit MSB-justified)
 * interleaved frames (frame stride = channels * 4). The probe lays CLARETT_STREAM_NDESC valid entries
 * (+ a zero terminator) per ring over one contiguous coherent buffer.
 *
 * STREAM_CHANS / STREAM_SIZE_VAL are the 8PreX values that populate clarett_8prex; all runtime stream
 * geometry is derived per-model from c->model via clarett_buf_bytes() &c. (defined below struct clarett).
 */
#define CLARETT_STREAM_NDESC     256            /* descriptors per ring (model-independent) */

/* --- PCM (data plane) --------------------------------------------------- */
#define CLARETT_PCM_RATE         48000          /* default rate, both models (see clocking enum) */
/*
 * Frames the engine advances per 0x300 period event = clarett_irq_period_frames() (one IRQ-flagged
 * descriptor consumed; spec §14). CALIBRATE on hardware: if the reported rate/pitch is off, the true
 * frames-per-event differs from CLARETT_IRQ_DESCS*CLARETT_FRAG_FRAMES — count 0x300 events/second at a
 * known 48 kHz and adjust. The plumbing is correct regardless of the exact value.
 */

/* FCP mailbox header layout, relative to REG_MBOX */
#define MBOX_CMD                 0x00    /* bit31 = execute flag | opcode               */
#define MBOX_SIZESEQ             0x04    /* size (low 16) | seq (high 16)               */
#define MBOX_ERROR               0x08
#define MBOX_PAD                 0x0c
#define MBOX_DATA                0x10

#define CMD_EXEC_FLAG            0x80000000u
#define IRQ_DONE_BIT             0x20000000u   /* mailbox-complete cause bit @ REG_IRQ0_CAUSE */
#define DOORBELL_SUBMIT          1
#define DOORBELL_ACK             2

/*
 * MSI: bare-metal /proc/interrupts shows the device delivers ALL control-plane
 * interrupts on vector 0 — both mailbox-done and front-panel notifications. The
 * cause registers (not the MSI vector index) distinguish them: 0x100 = mailbox
 * done (polled), 0x400 = notification mask. vec1/vec2/vec3 never fire here; they
 * are the data-plane (period IRQ) suspects.
 */
#define CLARETT_NUM_VECTORS      4
#define CLARETT_VEC_EVENT        0       /* the device signals control events on vec0 */

/*
 * REG_NOTIFY_CAUSE (0x400) is NOT an async event queue — it is a 2-bit command-phase/status
 * register. Correlating every FC boot capture (tools scratch: notify_correlate) shows it only ever
 * holds {0,1,2,3}: idle/ready = 0x3, then it dips 0x3->0x0 while a mailbox command is accepted and
 * blips 0x1->0x2 mid-command, returning to 0x3 at completion. FC POLLS it as flat status and
 * branches on nothing — it performs NO per-bit follow-up read (the Apollo-style "each notification
 * bit points to a descriptor block you must read" model was tested here and refuted). One capture
 * (4pre, at stream time) is the sole place bit3 (0x8) ever appears.
 *
 * Consequence for our ISR: vec0 fires on mailbox-DONE too, and at completion 0x400 reads its idle
 * 0x3 (== NOTIFY_MON_PRIMARY), so a completion MSI can be misread as a monitor event; the cmd_inflight
 * guard suppresses that self-reflection. But hardware (July 6 2026, response-logging on the 2Pre) shows
 * the guard is only a minor cleanup: the dominant "notification retried indefinitely" storm is the
 * DEVICE genuinely re-asserting 0x3 in us-scale bursts (8 in 234us, far faster than our ~30ms command
 * rate, inflight=0) because our GET returns empty (size=0) and never satisfies it — where FC's returns
 * real config and it goes quiet. So this is a dormant-backend symptom, not a driver bug (manifestation-
 * wall §5a). (The earlier 0x00200000/0x00400000 pair was an unverified §11 guess that never matched.)
 */
#define NOTIFY_MON_PRIMARY       0x00000003u  /* bit0|bit1 — raised on every monitor (mute/dim) event */
#define NOTIFY_MON_AUX           0x00200000u  /* bit21 — co-occurs intermittently */
#define NOTIFY_MONITOR_MASK      (NOTIFY_MON_PRIMARY | NOTIFY_MON_AUX)

/* Monitoring config region re-read on a notification (control-plane §9). */
#define MONITOR_CFG_OFFSET       24
#define MONITOR_CFG_LEN          92
#define MONITOR_ACTIVATE         2       /* DATA_CMD code shared by the monitor controls.
                                          * Trace-confirmed: mute@24 / dim@28 are 1-bit fields that
                                          * toggle 0/1 and commit with activate=2 (control-plane §9). */

/*
 * DATA_CMD{5} = flash / persist app config (control-plane §2, TRACE-confirmed: a monitor mute/dim
 * change emits a standalone DATA_CMD{5} on a debounce, with no preceding SET_DATA). A plain control
 * commit (DATA_CMD{activate}) applies the change live but RAM-only; this persists it across a power
 * cycle. The driver deliberately does NOT auto-issue it — persisting on every mixer tweak would wear
 * the device flash (the vendor app debounces). To add a deliberate "save" action, call
 * clarett_data_cmd(c, FCP_ACTIVATE_PERSIST).
 */
#define FCP_ACTIVATE_PERSIST     5

/*
 * Per-output "follow the monitor section" hardware-enable bits (command 3, control-plane §5).
 * The global Mute (offset 24) / Dim (offset 28) only affect an output whose enable bit is set —
 * the master flag alone does nothing. The driver force-enables the two monitor outputs at probe
 * so global Mute/Dim actually act on Monitor Out 1-2 (matching the USB unit's behaviour).
 * These bytes pack one bit per output, so they must be read-modify-written from a shadow seeded
 * from the device (clarett_seed_shadow), never blindly overwritten.
 *   byte 72: enable-hardware-mute (Monitor Out 1 = bit0, Monitor Out 2 = bit1)
 *   byte 73: enable-hardware-dim  (Monitor Out 1 = bit2, Monitor Out 2 = bit3)
 */
#define HWEN_ACTIVATE            3
#define HWEN_GAIN_OFFSET         52    /* enable-hardware-gain (SW/HW) base; byte 52+(out/2)*4, bit out%2 */
#define HWEN_MUTE_OFFSET         72
#define HWEN_DIM_OFFSET          73
#define HWEN_MONITOR_MUTE_MASK   0x03    /* Monitor Out 1-2 mute enables */
#define HWEN_MONITOR_DIM_MASK    0x0c    /* Monitor Out 1-2 dim enables  */

/* S/PDIF source select (XML <spdif-mode>): 2-bit fields, DATA_CMD activate 4. <input> @132 picks the
 * S/PDIF *input* to capture (matches scarlett2's "S/PDIF Source Capture Enum"); <output> @124 picks the
 * S/PDIF *output* connector. Enum None=0 / Optical=1 / RCA=2. Activate 4 [TRACE-CONFIRMED]. */
#define SPDIF_SOURCE_OFFSET      132
#define SPDIF_SOURCE_ACTIVATE    4
#define SPDIF_OUTPUT_OFFSET      124
#define SPDIF_OUTPUT_ACTIVATE    4

/* Hardware-meter source select (XML <meter-source> @184, DATA_CMD activate 8) + the per-band channel
 * index tables written alongside it (<hardware-meters> meters-l@136 / meters-m@146 / meters-h@156,
 * 10 bytes each). Enum is a bitmask value: Analogue=1 / S/PDIF=2 / ADAT1=4 / ADAT2=8. [TRACE-CONFIRMED] */
#define METER_SOURCE_OFFSET      184
#define METER_SOURCE_ACTIVATE    8
#define METER_TABLE_L_OFFSET     136
#define METER_TABLE_M_OFFSET     146
#define METER_TABLE_H_OFFSET     156
#define METER_TABLE_LEN          10

/* FCP "big" opcodes (low bits of cmd) — confirmed; == scarlett2 USB values */
#define FCP_GET_DATA             0x800000
#define FCP_SET_DATA             0x800001
#define FCP_DATA_CMD             0x800002

/*
 * GET_METER (0x001001): Focusrite Control polls this continuously (~24 Hz) the entire time it is
 * connected — the bulk of the trace "noise". It is not just a GUI meter read: it is the device's
 * required host heartbeat. With NO periodic poll the device accepts control writes (done=1, fcperr=0)
 * but never applies them to hardware (front-panel LEDs/preamp do not move), and the stream engine
 * stalls after one ring pass. Replaying FC's exact 8-byte payload {0x00300000, 0x00000001} as a
 * periodic heartbeat is what makes control changes physically manifest. See clarett_meter_work().
 */
#define FCP_GET_METER            0x001001
#define CLARETT_METER_POLL_MS    40

/* Debounced flash persist: after a control change, schedule a single DATA_CMD{PERSIST} this many ms
 * later (cancel+reschedule on each change) so a burst coalesces into one NVRAM write. Matches the
 * upstream scarlett2 driver's 2 s save debounce, and FC's own traced behaviour (a monitor change
 * emits a standalone DATA_CMD{5} on a debounce). See clarett_save_work() / FCP_ACTIVATE_PERSIST. */
#define CLARETT_SAVE_DELAY_MS    2000

/* SET_CLOCK (TRACE-CONFIRMED, control-plane §7): payload {u32 sample_rate, u32 clock_source}. */
#define FCP_SET_CLOCK            0x006003
#define CLARETT_CLOCK_INTERNAL   24
#define CLARETT_DEFAULT_RATE     48000

/*
 * CLOCK/SYNC category (0x006xxx) — these are QUERIES, not commands `[HW — 4Pre, July 20 2026]`.
 *
 * They were named FCP_STREAM_ENABLE/FCP_STREAM_COMMIT from watching the vendor issue them in-session
 * immediately before arming the engine, and that inference was WRONG: the category number is the
 * sync category (fcp-server: FCP_OPCODE_CATEGORY_SYNC = 0x006, SYNC_READ = 0x006004), and reading
 * them back on a live 4Pre returns state, not acknowledgement:
 *
 *   0x006004 -> 1        sync lock status (0 = unlocked, 1 = locked)   [== fcp-server SYNC_READ]
 *   0x006002 -> 48000    current rate (the rate we had just set)
 *   0x006005 -> 48000    rate
 *   0x006000 -> 0x30018  caps/bitmask (undecoded)
 *   0x006001 -> 44100    rate
 *   0x006003 -> 44100    rate
 *
 * So the vendor was POLLING whether its clock had locked, not enabling a stream — which also explains
 * its 3-second stall before streaming with zero MMIO writes in it. Consequence for us: the stream
 * handshake has NO enabling function beyond SET_CLOCK and the CONFIG_PUSH burst; issuing these three
 * is inert. They are kept (and still issued) only to keep our command stream byte-identical to the
 * vendor's, and because their responses are worth reading — a device reporting unlocked would explain
 * a dead engine. Ours reports LOCKED at 48000, so the data-plane stall is not a clock problem.
 */
#define FCP_SYNC_READ            0x006004   /* u32 lock status; was misnamed FCP_STREAM_ENABLE */
#define FCP_SYNC_RATE            0x006005   /* u32 rate; was misnamed FCP_STREAM_COMMIT */
/* Back-compat aliases: the old names appear in comments/specs written before the decode. */
#define FCP_STREAM_ENABLE        FCP_SYNC_READ
#define FCP_STREAM_COMMIT        FCP_SYNC_RATE

/*
 * Firmware init-handshake opcodes, replayed verbatim at probe (see clarett_init_handshake).
 * Observed at device attach from the vendor app and not fully decoded: CONFIG_PUSH registers
 * config items by id (arms the config space so SET_DATA writes actually reach hardware), and
 * GET_6x/GET_7x/READ_SEG are version/identity queries whose responses we ignore.
 */
#define FCP_READ_SEG             0x800005
#define FCP_INIT_2               0x000002
#define FCP_CONFIG_PUSH          0x005000
#define FCP_GET_60               0x006000
#define FCP_GET_61               0x006001
#define FCP_GET_62               0x006002
#define FCP_GET_70               0x007000
#define FCP_GET_71               0x007001
#define FCP_GET_72               0x007002
#define FCP_GET_73               0x007003

/*
 * Device bring-up opcodes seen in the vendor attach capture (8prex_full_init_mute.log).
 * Not fully decoded; the bring-up is replayed verbatim at probe (clarett_arm_device) from the
 * generated clarett_init_8prex.h, which precedes config writes actually taking effect on hardware.
 * Named here for documentation only — the replay table carries raw opcodes.
 *   0x000001 subsystem enable {u16 id}; 0x001000/0x002000/0x003000/0x004000 subsystem-count
 *   queries; 0x002002 SET_MIX {u16 mix, u16 coeff[30]}; 0x003002 SET_MUX; 0x004001/0x004005
 *   subsystem-4 setup; 0x005000 CONFIG_PUSH {u16 id}.
 */
#define FCP_INIT_1               0x000001
#define FCP_SET_MIX              0x002002
#define FCP_SET_MUX              0x003002

/* One replayed bring-up command: opcode + a [off, off+len) slice of clarett_init_blob[]. */
struct clarett_init_step {
	u32 opcode;
	u16 off;
	u16 len;
};

/*
 * One router patch: this destination pin is fed from this source pin (src 0 = unrouted). A model's
 * band-0 table names every destination it has exactly once — confirmed on all three captured models,
 * where the entry count is exactly (30 mixer inputs + physical outputs + capture channels).
 *
 * Used for a model with no captured bring-up blob: the arm necessarily replays another model's
 * routing (see clarett_apply_model_routing), and fcp-server refuses to create ANY routing control
 * unless every destination in its map is present in the device's live table.
 */
struct clarett_mux_entry {
	u16 src;
	u16 dst;
};

/*
 * Per-model descriptor (multi-model support). One const instance per supported
 * Clarett Thunderbolt variant, selected at probe and pinned as clarett.model.
 * Every value that differs between variants lives here; the mailbox/engine/mixer
 * *code* stays model-agnostic. Encodings are per-model (clean-room rule) — never
 * assume a value carries across models.
 *
 * NOTE: all Clarett Thunderbolt units reportedly share PCI id 1cb5:0002, so the
 * id_table cannot distinguish models; driver_data carries the default (2Pre)
 * and runtime disambiguation (fw-info / routing-count query) is a later step.
 */
struct clarett_out_gain {
	const char *name;	/* ALSA control name prefix, e.g. "Monitor 1"  */
	u8 offset;		/* config-space byte offset (SET_DATA target)  */
};

/*
 * Per-preamp input mode enum. mode_values is the per-model device encoding:
 * NEVER assume text index == device byte (8PreX Mic/Line/Inst = 0/1/2, but the
 * 2Pre's Line/Inst = 1/2 with no Mic). NULL = identity (index == device byte).
 */
struct clarett_preamp {
	const char * const *mode_texts;
	const u8 *mode_values;
	int n_modes;
};

struct clarett_model {
	const char *name;			/* human-readable model name ("Clarett 8PreX") */
	/*
	 * Stable machine-readable model slug ("clarett-8prex"), exposed at /proc/asound/cardN/clarett.
	 * The whole Thunderbolt line shares PCI id 1cb5:0002, so the PCI id cannot select a per-model
	 * control map; userspace (fcp-server) keys its model-specific maps on this slug instead. Unlike
	 * card->id it is never mangled for uniqueness, so it is a reliable contract. Keep it stable.
	 */
	const char *slug;

	/* control plane */
	const struct clarett_out_gain *out_gains;
	int n_out_gains;
	int n_analogue;				/* preamp count (air + mode controls) */
	const struct clarett_preamp *analogue;	/* [n_analogue] */
	/* Input-control naming. All models use in_prefix "Line In" (matching the scarlett2 names for
	 * the USB siblings). mode_label is "Level" for the USB models (their Line/Inst switch) but
	 * "Mode" for the 8PreX, whose Mic/Line/Inst mode is richer than scarlett2's Line/Inst "Level".
	 * Output-gain names carry the full "Line NN (descr)" string per model in out_gains[].name. */
	const char *in_prefix;			/* "Line In" (all models) */
	const char *mode_label;			/* "Level" (USB models) or "Mode" (8PreX) */
	/* "S/PDIF Source Capture Enum" (None/Optical/RCA @ SPDIF_SOURCE_OFFSET). Present where the
	 * device has a selectable S/PDIF input — 4Pre/8Pre/8PreX. The 2Pre has optical only (one
	 * option), so it gets no control, matching scarlett2 (which omits it for the 2Pre). */
	bool has_spdif_source;
	/* Hardware-meter source selector (8PreX only; others have one or no source). */
	const struct clarett_meter_source *meter_sources;
	int n_meter_sources;

	/* data plane / PCM geometry */
	u8 capture_channels;			/* block-1 RX stream width */
	u8 playback_channels;			/* block-0 TX stream width */
	u32 stream_frag;			/* legacy engine-start probe only (uniform per-descriptor DMA bytes);
						 * the PCM path derives per-direction fragments from channel counts */
	/*
	 * Buffer mode (data-plane spec §9, §13). The engine's ring base registers 0x210/0x310 point at EITHER
	 * a scatter-gather descriptor table (large buffers; the 8PreX RAM dump) OR a flat contiguous sample
	 * ring (small buffers; the 2Pre RAM dump was flat audio, no table). It is per-model: the 2Pre's engine
	 * consumes ZERO frames/period when handed a descriptor table (ctr frozen at 0) but its counter advances
	 * in flat mode (ctr=0x1c10, 28 passes). flat_buffer=true selects the flat path (0x210/0x310 -> sample
	 * ring directly, no table); false keeps the descriptor table. See clarett_pcm.c.
	 */
	bool flat_buffer;

	/*
	 * Per-channel stream-routing CONFIG_PUSH ids, re-issued in-session at PCM prepare (the device resets
	 * stream routing when idle; the probe-time push goes stale). Captured from the VM rate-change handshake
	 * (2pre_streamstart.log): one CONFIG_PUSH{u16 id} per stream channel. tx[] after GET_7.2, rx[] after
	 * GET_7.3, matching the wire order. NULL/0 = skip the burst (8PreX ids not yet captured).
	 */
	const u8 *stream_tx_ids;
	const u8 *stream_rx_ids;
	u8 n_stream_tx_ids;
	u8 n_stream_rx_ids;

	/* device bring-up replay (per-model; from fcp_decode.py --emit-init) */
	const u8 *init_blob;
	const struct clarett_init_step *init_seq;
	int n_init_steps;

	/* Band-0 router patch for a model with no init_blob (tools/gen_fcp_maps.py --emit-mux). */
	const struct clarett_mux_entry *mux_band0;
	int n_mux_band0;
};

/*
 * GET-response DMA layout (confirmed on hardware). The device DMAs the response
 * into resp_buf as a 16-byte FCP header followed by the requested bytes:
 *   resp[0..3]  = echoed cmd (CMD_EXEC_FLAG | opcode) — guard on this
 *   resp[4..5]  = size: # of payload bytes the device actually returned
 *   resp[16+i]  = config[offset + i]  for a GET_DATA{offset, len}
 * A failed/absent DMA leaves the echo word 0, so checking it avoids consuming a
 * stale buffer (seen on the first GET at load, which DMAs all zeroes). But the echo
 * word alone is NOT sufficient: our device answers GET_DATA with the header present
 * yet size=0 and NO payload — the config backend refuses our session (see below and
 * spec/clarett-manifestation-wall.md §5a/§7). So a reader must ALSO
 * require size > 0 before consuming resp[16+]; otherwise it copies stale buffer bytes.
 *
 * resp[8..11] is the FCP ERROR word: 0 = OK. A working session's responses carry 0
 * with real payload sizes (pmemsave of FC's live buffer, July 9 2026 — transport spec
 * §8). Our sessions get 0x3 on every response — a refusal code, NOT "success" (the
 * pre-July-9 reading, calibrated only on walled responses, had this backwards).
 */
#define FCP_RESP_ECHO_OFF        0
#define FCP_RESP_SIZE_OFF        4
#define FCP_RESP_SEQ_OFF         6      /* echoed request seq in the DMAed response header */
#define FCP_RESP_STATUS_OFF      8      /* FCP error word; see layout comment above */
#define FCP_RESP_DATA_OFF        16
#define FCP_RESP_ERR_OK          0x00   /* working-session responses */
#define FCP_RESP_ERR_WALLED      0x03   /* the refusal every command gets on our sessions */

#define CLARETT_MBOX_TIMEOUT_MS  100
#define CLARETT_MAX_PAYLOAD      64      /* clarett_set_data single-write cap (small configs) */
#define CLARETT_MBOX_DATA_MAX    1024    /* mailbox data region (MBOX_END - MBOX_DATA); SET_MUX = 412 */
#define CLARETT_CONFIG_SIZE      256     /* shadow of the device config/app space       */
#define CLARETT_APPSPACE_SIZE    8392    /* full persistent config/appspace: the arm's bulk
					  * GET_DATA reads span exactly [0, 8392) and its
					  * writebacks fall inside that range */

/* A hardware-meter source option: its device value and the three per-band channel-index tables the
 * host writes (@136/146/156) when selecting it, alongside SET_DATA{184}=value + DATA_CMD{8}. Kept as
 * per-model RE data (referenced by the model table); the control that consumed it is now fcp-server's. */
struct clarett_meter_source {
	const char *name;
	u8 value;			/* Analogue=1 / S/PDIF=2 / ADAT1=4 / ADAT2=8 */
	u8 tbl[3][10];			/* meters-l, meters-m, meters-h (per sample-rate band) */
};

#define CLARETT_N_METERS         48    /* GET_METER returns 48 u32 levels (num_meters=0x30)  */
#define CLARETT_METER_MAX        4095  /* meter level range 0..4095 (matches scarlett2)       */

struct clarett;

/* request_irq dev_id: identifies the card and which MSI vector fired */
struct clarett_irqctx {
	struct clarett *c;
	unsigned int idx;
};

/* --- per-card state ----------------------------------------------------- */
struct clarett {
	struct pci_dev *pci;
	struct snd_card *card;
	void __iomem *bar0;
	const struct clarett_model *model;	/* selected at probe; see struct clarett_model */

	struct mutex mbox_lock;		/* serialises FCP transactions */
	u16 seq;

	void *resp_buf;			/* coherent GET-response DMA buffer */
	dma_addr_t resp_dma;
	size_t resp_size;

	u32 serial_lo, serial_hi, fw_app, fw_fpga;

	/* MSI / async notifications (vec0). With the vendor mailbox cycle (default) the ISR
	 * IS the completion path: while cmd_inflight it reads the 0x100 mailbox cause (the
	 * vendor sweep's first read, MSI-paced) and completes mbox_done; clarett_fcp then
	 * finishes the sweep. legacy_mbox_cycle=1 restores the pure polled mailbox, where
	 * the ISR deliberately never touches 0x100. */
	bool irq_ready;
	bool ctl_ready;				/* controls registered; notify path may snd_ctl_notify */
	int n_vec;				/* MSI vectors actually allocated (<= CLARETT_NUM_VECTORS) */
	struct clarett_irqctx irq_ctx[CLARETT_NUM_VECTORS];
	struct work_struct notify_work;
	struct delayed_work save_work;		/* debounced DATA_CMD{PERSIST}; see CLARETT_SAVE_DELAY_MS */
	atomic_t notify_bits;
	struct completion mbox_done;		/* completed by the vec0 ISR on mailbox DONE */
	u32 mbox_cause;				/* 0x100 value the ISR consumed with DONE set */
	/*
	 * Set while a mailbox command is in flight (clarett_fcp submit->complete). vec0 fires on
	 * mailbox-DONE as well as front-panel notifications, and 0x400 reads its idle level (bit0|bit1
	 * = 0x3) at completion, so a completion MSI can be misread as a monitor event. This guard makes
	 * the ISR skip 0x400 while our own command is in flight, suppressing that self-reflection.
	 * NOTE (hardware-confirmed July 6 2026): this is only a MINOR contributor. On a walled device the
	 * dominant "notification retried indefinitely" storm is the DEVICE genuinely re-asserting 0x3 in
	 * us-scale bursts because our GET returns empty (size=0) and never satisfies it — the guard cannot
	 * stop that (the device fires in the idle gaps where inflight=0). See clarett_irq() and the
	 * REG_NOTIFY_CAUSE note; manifestation-wall §5a.
	 */
	atomic_t cmd_inflight;

	/* Periodic GET_METER heartbeat — the device requires it to apply control writes to
	 * hardware (and to sustain streaming). See FCP_GET_METER / clarett_meter_work(). */
	struct delayed_work meter_work;

	/*
	 * FCP hwdep level meter. fcp-server creates the "Level Meter"
	 * control via FCP_IOCTL_SET_METER_MAP: hwdep_meter_map[i] indexes the device's raw meter array
	 * (or -1 = no source) for output channel i; hwdep_meter_levels is the GET_METER scratch buffer
	 * (hwdep_n_meter_slots u32s). hwdep_meter_labels_tlv carries the channel-name TLV set by
	 * FCP_IOCTL_SET_METER_LABELS. All devm-allocated (freed at detach). Mirrors sound/usb/fcp.c. */
	struct mutex hwdep_lock;		/* serialises the hwdep meter ioctls vs the control callbacks */
	/*
	 * hwdep notification relay. A device notification (0x400 cause,
	 * detected in clarett_irq -> clarett_notify_work) sets hwdep_notify_event and wakes any
	 * fcp-server blocked in read()/poll(). Lock-free: atomic_or to accumulate, atomic_xchg to drain.
	 */
	wait_queue_head_t hwdep_notify_wait;
	atomic_t hwdep_notify_event;
	struct delayed_work hwdep_notify_dwork;	/* coalesces relay wakes (idle 0x400 storms ~30 Hz) */
	bool hwdep_ready;			/* dwork INIT'd: gates cancel (probe-error paths never got here) */
	struct snd_kcontrol *hwdep_meter_ctl;
	s16 *hwdep_meter_map;
	__le32 *hwdep_meter_levels;
	int hwdep_meter_channels;	/* map_size: channels the control exposes */
	int hwdep_n_meter_slots;	/* device raw meter count */
	unsigned int *hwdep_meter_labels_tlv;
	unsigned int hwdep_meter_labels_tlv_size;

	/*
	 * Data-plane engine-start probe (opt-in via the stream_probe module param). Not a PCM
	 * implementation — it programs the §3b ring registers with this buffer and watches whether
	 * the engine runs (vec1/vec2 period IRQs + DMA pointer advancing). See clarett_engine_start().
	 */
	bool stream_on;
	bool flat_buffer;		/* effective buffer mode (model default, overridable by force_flat) */
	u32 rx_slot;			/* RX descriptor fragment SLOT stride in bytes (>= audio bytes/fragment).
					 * = audio bytes when contiguous (rx_frag_pad=0); larger to break buffer
					 * contiguity (scatter-gather experiment for the page-drift glitch). */
	void *stream_buf;		/* coherent streaming ring buffer */
	dma_addr_t stream_dma;
	size_t stream_size;
	atomic_t period_irqs[CLARETT_NUM_VECTORS];   /* per-vector IRQ counts */
	struct delayed_work stream_report;	/* logs pointer/IRQ progress after start */
	struct task_struct *stream_svc;		/* polls/acks 0x300 to keep the engine clocked */
	atomic_t stream_periods;		/* running period count (servicer -> hw pointer) */
	u32 stream_ctr;				/* last 0x300 period counter (servicer-private) */
	u32 stream_ctr_step;			/* last positive ctr delta (reused across counter wraps) */
	bool stream_run;			/* servicer ACKs 0x300 only while set (PCM trigger gate) */

	/*
	 * PCM capture (data plane). The descriptor table maps the ALSA-managed DMA buffer into
	 * hardware fragments; the servicer kthread advances pcm_frames per 0x300 tick and calls
	 * snd_pcm_period_elapsed (clarett_pcm_tick). Capture = ring block 1 (0x300) only.
	 */
	struct snd_pcm *pcm;
	struct snd_pcm_substream *pcm_sub;	/* live capture substream (NULL when idle) */
	struct snd_pcm_substream *pcm_play_sub;	/* live playback substream (NULL when idle) */
	/*
	 * The hardware rings live in ONE contiguous coherent buffer (c->stream_buf), the exact layout the
	 * engine-start probe proved clocks — split allocations (separate table / ALSA buffer / TX ring) do
	 * NOT clock. Block 0 (silent dummy TX, full-duplex requirement) occupies the first half, block 1
	 * (capture) the second. Captured samples are memcpy'd from the block-1 RX area into the ALSA buffer
	 * each period (clarett_pcm_tick). FC always arms both blocks even for record-only (data-plane §9).
	 */
	struct mutex pcm_lock;			/* guards the tick's ring<->ALSA copies vs hw_free teardown */
	bool pcm_running;			/* capture trigger START..STOP: gate period delivery */
	bool play_running;			/* playback trigger START..STOP: gate period delivery */
	u64 pcm_frames;				/* engine frame clock since arm (shared by both directions) */
	u64 pcm_last_period;			/* last capture period index reported via period_elapsed */
	u64 play_last_period;			/* last playback period index reported via period_elapsed */

	/*
	 * Shadow of the config space backing mixer "get". Updated write-through on
	 * every put; the monitor bytes (24/28/112) are additionally refreshed from
	 * the DMAed GET response on a front-panel notification (clarett_notify_work),
	 * so those reflect live hardware state. Other bytes remain write-through.
	 */
	u8 shadow[CLARETT_CONFIG_SIZE];
	/*
	 * Per-byte "the shadow is known to match hardware" flags. A shadow byte is
	 * only authoritative once we've written it (write-through) or read it from a
	 * trusted live source (the 24/28/112 monitor refresh). The put handler's
	 * skip-if-unchanged optimisation is sound ONLY for known bytes: for a control
	 * the device does not report back (preamp Mode@166/Air@174), the seed leaves
	 * the shadow at 0, and skipping "set to 0" would silently drop a real change.
	 */
	DECLARE_BITMAP(shadow_known, CLARETT_CONFIG_SIZE);
};

static inline void clarett_put_le32(u8 *p, u32 v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static inline u32 clarett_get_le32(const u8 *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

static inline u16 clarett_get_le16(const u8 *p)
{
	return p[0] | p[1] << 8;
}

static inline void clarett_put_le16(u8 *p, u16 v)
{
	p[0] = v;
	p[1] = v >> 8;
}

/*
 * Runtime stream geometry, derived per-model from c->model. The hardware rings live in one contiguous
 * coherent buffer of 2 * clarett_ring_bytes(): block 0 (TX) then block 1 (RX), each a descriptor table
 * (clarett_tbl_bytes, model-independent) followed by CLARETT_STREAM_NDESC sample fragments. NOTE: TX and
 * RX share one stream_frag here (true on the 8PreX, where both directions are 28ch); per-direction
 * (asymmetric) geometry for narrower models is deferred until captured (step 5).
 */
static inline size_t clarett_tbl_bytes(void)
{
	return ALIGN((CLARETT_STREAM_NDESC + 1) * sizeof(__le64), 64);
}

static inline size_t clarett_buf_bytes(const struct clarett *c)
{
	return (size_t)CLARETT_STREAM_NDESC * c->model->stream_frag;
}

static inline size_t clarett_ring_bytes(const struct clarett *c)
{
	return clarett_tbl_bytes() + clarett_buf_bytes(c);
}

/*
 * Hardware IRQ period in bytes for a stream of `channels` (the 0x208/0x308 SIZE register value): one period
 * is 4 interleaved S32_LE frames. Holds for both models (8PreX 28ch -> 0x1c0, 2Pre TX 4ch -> 0x40 / RX 14ch
 * -> 0xe0). This is the IRQ granularity, decoupled from the descriptor fragment (4 periods per 8PreX fragment).
 */
static inline u32 clarett_period_bytes(u8 channels)
{
	return (u32)channels * 4 * 4;
}

/*
 * PCM descriptor-table geometry (per-direction), built to match the LIVE 2Pre vendor tables read out by
 * pmemsave (spec §14; tools/dma_classify.py). Every entry is a bare 8-byte LE bus address; the fragment is
 * exactly CLARETT_FRAG_FRAMES interleaved frames = channels*4*16 bytes, packed with NO 0x100 rounding
 * (2Pre TX 4ch->0x100, RX 14ch->0x380, 8PreX 28ch->0x700 — the vendor RX stride 0x380 is only 0x80-aligned,
 * disproving the earlier lcm(0x100,...) rule that doubled 14ch to 0x700). The RX ring carries a periodic
 * IRQ flag (bit1) every CLARETT_IRQ_DESCS descriptors — THIS is what raises the counted 0x300 period; a
 * ring flagged only at the end never advances the counter (the ctr=0 wall). The LAST entry adds the wrap
 * flag (bit0): TX 0x01, RX 0x03 (wrap|IRQ). No zero terminator.
 */
#define CLARETT_DESC_ALIGN	0x100	/* pad the table so the sample area starts 0x100-aligned (harmless) */
#define CLARETT_DESC_WRAP_TX	0x01	/* last-entry flag, block 0 (TX): bit0 = end-of-list/wrap */
#define CLARETT_DESC_WRAP_RX	0x03	/* last-entry flag, block 1 (RX): bit0 wrap | bit1 IRQ (spec §14) */
#define CLARETT_DESC_IRQ	0x02	/* periodic per-period IRQ marker on RX descriptors (bit1) */

/*
 * A descriptor covers exactly CLARETT_FRAG_FRAMES frames (the vendor's fragment is channels*4*16 on both
 * 2Pre directions and the 8PreX, verified by RAM dump — no alignment rounding). CLARETT_IRQ_DESCS is how
 * many descriptors the RX engine consumes between period IRQs; the vendor's 2Pre RX flags roughly every 14
 * (a fractional ~228-frame period). We pick a clean 16 (= 256 frames = 5.33 ms at 48k) since we own our
 * buffer/period; the exact count is our choice as long as RX descriptors carry the flag at this cadence.
 */
#define CLARETT_FRAG_FRAMES	16
#define CLARETT_IRQ_DESCS	16

static inline u32 clarett_frag_bytes(u8 channels)
{
	return (u32)channels * 4 * CLARETT_FRAG_FRAMES;
}
/* Frames advanced per 0x300 period IRQ (one IRQ-flagged descriptor consumed = CLARETT_IRQ_DESCS frags).
 * Used only as the ALSA period granularity; the actual capture advance is ctr-delta driven (below). */
static inline u32 clarett_irq_period_frames(void)
{
	return CLARETT_IRQ_DESCS * CLARETT_FRAG_FRAMES;
}
/*
 * Frames per 0x300 counter unit (spec §10/§14). Hardware-derived: the vendor steps +0xc/period == 192
 * frames == 4 ms at 48k, so one unit == 16 frames; our 2Pre steps +0xd (~208 frames/event ~= 48 kHz).
 * The capture path advances by (measured ctr delta) * this, self-calibrating to the real hardware period
 * regardless of the per-model step or our IRQ-marker spacing. Sanity cap so a glitched read can't
 * over-advance the ring: a real delta is ~12-13, never dozens.
 */
#define CLARETT_CTR_FRAMES	16
#define CLARETT_CTR_STEP_MAX	64
/* PCM descriptor table size: NDESC bare 8-byte entries, padded to keep the following sample area 0x100-aligned.
 * No +1 terminator slot — the wrap flag on the last entry is the terminator. */
static inline size_t clarett_pcm_tbl_bytes(void)
{
	return ALIGN((size_t)CLARETT_STREAM_NDESC * sizeof(__le64), CLARETT_DESC_ALIGN);
}
static inline size_t clarett_pcm_tx_samples(const struct clarett *c)
{
	return (size_t)CLARETT_STREAM_NDESC * clarett_frag_bytes(c->model->playback_channels);
}
/*
 * RX has TWO byte sizes once the scatter-gather experiment pads the fragments (c->rx_slot > audio bytes):
 *   _samples = the LOGICAL audio (contiguous frames) — the ALSA buffer and the per-period frame math.
 *   _dev     = the DEVICE sample area = NDESC slots of c->rx_slot each — what is allocated and what the
 *              descriptors stride over (with gaps between fragments when padded). Equal when unpadded.
 */
static inline size_t clarett_pcm_rx_samples(const struct clarett *c)
{
	return (size_t)CLARETT_STREAM_NDESC * clarett_frag_bytes(c->model->capture_channels);
}
static inline size_t clarett_pcm_rx_dev_bytes(const struct clarett *c)
{
	return (size_t)CLARETT_STREAM_NDESC * c->rx_slot;
}
/* One ring per direction = table + samples. The contiguous buffer is [TX ring][RX ring]; r1 = r0 + tx ring. */
static inline size_t clarett_pcm_tx_ring(const struct clarett *c)
{
	return clarett_pcm_tbl_bytes() + clarett_pcm_tx_samples(c);
}
static inline size_t clarett_pcm_rx_ring(const struct clarett *c)
{
	return clarett_pcm_tbl_bytes() + clarett_pcm_rx_dev_bytes(c);	/* device area (slotted) for allocation */
}

/*
 * Flat-buffer geometry (flat_buffer models, spec §9/§13). 0x210/0x310 point straight at a contiguous
 * sample ring — NO descriptor table. CLARETT_FLAT_FRAMES is the per-direction ring depth in frames; on
 * the 2Pre this makes the TX ring 1024*4ch*4 = 16 KB, exactly the VM's TX-base->RX-base gap
 * (0x680fb000-0x680f7000), and the RX ring 1024*14ch*4 = 56 KB. The engine wraps each ring at this depth
 * (VM counter wrapped ~0xf0 with these sizes). r1 = r0 + flat TX bytes, so RX abuts TX just as the VM's
 * two bases do. Sample-ring bytes = frames * channels * 4; that RX ring IS the ALSA capture buffer.
 */
#define CLARETT_FLAT_FRAMES	1024
static inline size_t clarett_flat_tx_bytes(const struct clarett *c)
{
	return (size_t)CLARETT_FLAT_FRAMES * c->model->playback_channels * 4;
}
static inline size_t clarett_flat_rx_bytes(const struct clarett *c)
{
	return (size_t)CLARETT_FLAT_FRAMES * c->model->capture_channels * 4;
}

/*
 * Mode-independent stream accessors — the PCM path uses these so it does not branch on flat_buffer
 * everywhere. total = both rings; rx_off = byte offset of the RX SAMPLE area (the engine's capture write
 * target, and the source of the per-period copy) within the contiguous buffer; rx_area = its size, which
 * is also the ALSA buffer size; r0/r1 = the two ring base addresses the engine is armed with (a table
 * base in descriptor mode, a sample base in flat mode).
 */
static inline size_t clarett_stream_tx_off(const struct clarett *c)
{
	return c->flat_buffer ? 0 : clarett_pcm_tbl_bytes();  /* flat: samples at 0; descr: past TX table */
}
static inline size_t clarett_stream_tx_area_bytes(const struct clarett *c)
{
	return c->flat_buffer ? clarett_flat_tx_bytes(c) : clarett_pcm_tx_samples(c);
}
static inline size_t clarett_stream_rx_off(const struct clarett *c)
{
	/* descr: past the TX ring and the RX table, PAGE-ALIGNED so each RX fragment slot (a power of two,
	 * <= PAGE) is page-contained — the fix for the 8-bytes-per-page capture drift (spec §15). */
	return c->flat_buffer
		? clarett_flat_tx_bytes(c)			     /* flat: RX samples abut TX samples */
		: ALIGN(clarett_pcm_tx_ring(c) + clarett_pcm_tbl_bytes(), PAGE_SIZE);
}
static inline size_t clarett_stream_rx_area_bytes(const struct clarett *c)
{
	return c->flat_buffer ? clarett_flat_rx_bytes(c) : clarett_pcm_rx_samples(c);
}
static inline size_t clarett_stream_r1_off(const struct clarett *c)
{
	/* base of block 1: its sample ring (flat) or its descriptor table (descriptor). */
	return c->flat_buffer ? clarett_flat_tx_bytes(c) : clarett_pcm_tx_ring(c);
}
static inline size_t clarett_stream_total_bytes(const struct clarett *c)
{
	return c->flat_buffer
		? clarett_flat_tx_bytes(c) + clarett_flat_rx_bytes(c)
		: clarett_stream_rx_off(c) + clarett_pcm_rx_dev_bytes(c);  /* RX samples are last; page-aligned */
}

/* mailbox.c */
int clarett_fcp(struct clarett *c, u32 opcode, const u8 *data, u16 len);
int clarett_fcp_cmd(struct clarett *c, u32 opcode, const u8 *req, u16 req_len,
		    u8 *resp, u16 resp_len);
int clarett_hwdep_init(struct clarett *c);	/* create the FCP hwdep (fcp-server transport) */
void clarett_hwdep_notify(struct clarett *c, u32 ev);	/* relay a device notification to fcp-server */
void clarett_hwdep_free(struct clarett *c);	/* stop the relay before c is freed (UAF guard) */
int clarett_get_data(struct clarett *c, u32 offset, u32 len);
int clarett_set_data(struct clarett *c, u32 offset, u32 len, const u8 *val);
int clarett_data_cmd(struct clarett *c, u32 activate);
int clarett_write_u8(struct clarett *c, u32 offset, u8 val, u32 activate);

/* BAR0 access wrappers. */
void clarett_wl(struct clarett *c, u32 off, u32 val);
u32 clarett_rl(struct clarett *c, u32 off);

int clarett_write_bits(struct clarett *c, u32 offset, u8 mask, u8 val, u32 activate);

/* mixer.c */

/* main.c — data-plane engine (shared with pcm.c) */
void clarett_engine_arm(struct clarett *c, dma_addr_t r0, dma_addr_t r1);
void clarett_engine_run(struct clarett *c);
void clarett_engine_stop(struct clarett *c);
int clarett_engine_start(struct clarett *c);

/* pcm.c */
int clarett_create_pcm(struct clarett *c);
void clarett_pcm_tick(struct clarett *c, u32 add_frames);

#endif /* CLARETT_H */
