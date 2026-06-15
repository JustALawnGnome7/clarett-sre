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
#include <sound/core.h>

/* --- BAR0 register map (confirmed) -------------------------------------- */
#define CLARETT_BAR              0
#define REG_CAPS                 0x000
#define REG_SERIAL_LO            0x010
#define REG_SERIAL_HI            0x014
#define REG_IRQ0_CAUSE           0x100   /* read-to-clear; bit DONE = mailbox complete */
#define REG_IRQ0_ENABLE          0x104   /* observed init value 0xf000003f            */
#define REG_DOORBELL             0x408   /* write 1 = submit, 2 = ack/clear prior      */
#define REG_DMA_ADDR_LO          0x410   /* GET-response DMA buffer bus address (low 32)  */
#define REG_DMA_ADDR_HI          0x414   /* DMA buffer bus address (high 32) — confirmed  */
#define REG_INFO                 0x8000  /* read-only fw-info header (fw versions, ...) */
#define REG_MBOX                 0x8020  /* FCP request mailbox                         */

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

/* FCP "big" opcodes (low bits of cmd) — confirmed; == scarlett2 USB values */
#define FCP_GET_DATA             0x800000
#define FCP_SET_DATA             0x800001
#define FCP_DATA_CMD             0x800002

#define CLARETT_MBOX_TIMEOUT_MS  100
#define CLARETT_MAX_PAYLOAD      64
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

	struct clarett_ctl *ctls;	/* descriptor array, lifetime = card */

	/*
	 * Write-through shadow of the config space. We cannot yet decode the
	 * DMA-delivered GET responses, so mixer "get" returns cached values.
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
int clarett_set_data(struct clarett *c, u32 offset, u32 len, const u8 *val);
int clarett_data_cmd(struct clarett *c, u32 activate);
int clarett_write_u8(struct clarett *c, u32 offset, u8 val, u32 activate);

/* mixer.c */
int clarett_create_controls(struct clarett *c);

#endif /* CLARETT_H */
