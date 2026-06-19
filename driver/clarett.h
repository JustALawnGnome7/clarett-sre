/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Focusrite Clarett 8PreX (Thunderbolt) ALSA driver — shared definitions.
 *
 * Register map and FCP framing are from the reverse-engineering notes in
 * ../spec/clarett-8prex-fcp-transport.md (confirmed against MMIO traces).
 * Control offsets/commands are from ../spec/clarett-8prex-control-plane.md.
 */
#ifndef CLARETT_H
#define CLARETT_H

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>
#include <sound/core.h>

struct snd_kcontrol;

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
#define STREAM_CHANS             0x1c    /* 28 PCM channels per direction   */
#define STREAM_SIZE_VAL          0x1c0
#define CLARETT_STREAM_BUF       (128 * 1024)   /* coherent ring buffer (generous; avoids fault) */
#define CLARETT_STREAM_RING_GAP  0x4000         /* block-1 ring offset (capture spaced them 16 KB) */

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
#define NOTIFY_DIM_MUTE          0x00200000u
#define NOTIFY_MONITOR           0x00400000u

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
#define HWEN_MUTE_OFFSET         72
#define HWEN_DIM_OFFSET          73
#define HWEN_MONITOR_MUTE_MASK   0x03    /* Monitor Out 1-2 mute enables */
#define HWEN_MONITOR_DIM_MASK    0x0c    /* Monitor Out 1-2 dim enables  */

/* FCP "big" opcodes (low bits of cmd) — confirmed; == scarlett2 USB values */
#define FCP_GET_DATA             0x800000
#define FCP_SET_DATA             0x800001
#define FCP_DATA_CMD             0x800002

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
 * Device bring-up opcodes seen in the vendor attach capture (clarett_full_init_mute.log).
 * Not fully decoded; the bring-up is replayed verbatim at probe (clarett_arm_device) from the
 * generated clarett_init_seq.h, which precedes config writes actually taking effect on hardware.
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
 * GET-response DMA layout (confirmed on hardware). The device DMAs the response
 * into resp_buf as a 16-byte FCP header followed by the requested bytes:
 *   resp[0..3]  = echoed cmd (CMD_EXEC_FLAG | opcode) — guard on this
 *   resp[16+i]  = config[offset + i]  for a GET_DATA{offset, len}
 * A failed/absent DMA leaves the echo word 0, so checking it avoids consuming a
 * stale buffer (seen on the first GET at load, which DMAs all zeroes).
 */
#define FCP_RESP_ECHO_OFF        0
#define FCP_RESP_DATA_OFF        16

#define CLARETT_MBOX_TIMEOUT_MS  100
#define CLARETT_MAX_PAYLOAD      64      /* clarett_set_data single-write cap (small configs) */
#define CLARETT_MBOX_DATA_MAX    1024    /* mailbox data region (MBOX_END - MBOX_DATA); SET_MUX = 412 */
#define CLARETT_CONFIG_SIZE      256     /* shadow of the device config/app space       */

/* --- mixer control descriptor ------------------------------------------- */
enum clarett_ctl_type {
	CT_SWITCH,	/* 1 byte, 0/1 (optionally inverted)        */
	CT_GAIN,	/* 1 byte, 7-bit attenuation code = |dB|    */
	CT_ENUM,	/* 1 byte, enumerated                       */
};

struct clarett_ctl {
	char name[44];
	enum clarett_ctl_type type;
	u32 offset;			/* config-space byte offset (SET_DATA target) */
	u8  activate;			/* DATA_CMD activate code (XML "command")     */
	u8  invert;			/* CT_SWITCH: device 1 == "off" in ALSA terms */
	const char * const *texts;	/* CT_ENUM                                    */
	int n_texts;
	struct snd_kcontrol *kctl;	/* for snd_ctl_notify on async events         */
};

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

	struct mutex mbox_lock;		/* serialises FCP transactions */
	u16 seq;

	void *resp_buf;			/* coherent GET-response DMA buffer */
	dma_addr_t resp_dma;
	size_t resp_size;

	u32 serial_lo, serial_hi, fw_app, fw_fpga;

	/* MSI / async notifications (vec0). The mailbox stays polled — the ISR
	 * deliberately does not touch the 0x100 mailbox cause (see clarett_main.c). */
	bool irq_ready;
	struct clarett_irqctx irq_ctx[CLARETT_NUM_VECTORS];
	struct work_struct notify_work;
	atomic_t notify_bits;

	struct clarett_ctl *ctls;	/* descriptor array, lifetime = card */
	int n_ctls;

	/*
	 * Data-plane engine-start probe (opt-in via the stream_probe module param). Not a PCM
	 * implementation — it programs the §3b ring registers with this buffer and watches whether
	 * the engine runs (vec1/vec2 period IRQs + DMA pointer advancing). See clarett_engine_start().
	 */
	bool stream_on;
	void *stream_buf;		/* coherent streaming ring buffer */
	dma_addr_t stream_dma;
	size_t stream_size;
	atomic_t period_irqs[CLARETT_NUM_VECTORS];   /* per-vector IRQ counts */
	struct delayed_work stream_report;	/* logs pointer/IRQ progress after start */

	/*
	 * Shadow of the config space backing mixer "get". Updated write-through on
	 * every put; the monitor bytes (24/28/112) are additionally refreshed from
	 * the DMAed GET response on a front-panel notification (clarett_notify_work),
	 * so those reflect live hardware state. Other bytes remain write-through.
	 */
	u8 shadow[CLARETT_CONFIG_SIZE];
};

static inline void clarett_put_le32(u8 *p, u32 v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

/* mailbox.c */
int clarett_fcp(struct clarett *c, u32 opcode, const u8 *data, u16 len);
int clarett_get_data(struct clarett *c, u32 offset, u32 len);
int clarett_set_data(struct clarett *c, u32 offset, u32 len, const u8 *val);
int clarett_data_cmd(struct clarett *c, u32 activate);
int clarett_write_u8(struct clarett *c, u32 offset, u8 val, u32 activate);
int clarett_write_bits(struct clarett *c, u32 offset, u8 mask, u8 val, u32 activate);

/* mixer.c */
int clarett_create_controls(struct clarett *c);

#endif /* CLARETT_H */
