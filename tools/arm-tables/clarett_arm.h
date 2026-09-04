/* SPDX-License-Identifier: GPL-2.0 */
/*
 * De-blobbed device bring-up: a typed command list replaces the opaque captured init blob.
 *
 * Each bring-up step is a struct clarett_arm_step; clarett_arm_emit() serializes its payload into
 * a byte buffer. The serialization is byte-identical to the vendor MMIO capture the blob came from
 * — verified offline for every model against the original capture (a userspace harness reconstructs
 * the command stream from these tables and diffs it against the captured blob).
 *
 * The per-model tables live in arm_<model>.h (generated from the vendor capture), where
 * <model> carries the product line: arm_clarett_8prex, arm_red_8line.
 * Dependency-free (only shifts + memcpy/memset) so the same builder validates in userspace and runs
 * in the kernel. Provide u8/u16/u32 before including.
 */
#ifndef CLARETT_ARM_H
#define CLARETT_ARM_H

enum clarett_arm_kind {
	CARM_NONE,	/* zero-length payload (INIT_2, identity/count queries — op carries the meaning) */
	CARM_ID,	/* le16(a): CONFIG_PUSH / SUBSYS_ENABLE id */
	CARM_U32,	/* le32(a): DATA_CMD activate / SUBSYS4_SET index */
	CARM_RANGE,	/* le32(a) le32(b): GET_DATA / READ_SEG {offset, len} */
	CARM_MIX,	/* le16(a=bus) then dlen le16 coeffs from .data (const u16 *) */
	CARM_MUX,	/* dlen le32 routing words from .data (const u32 *); word 0 is the band header */
	CARM_WB,	/* le32(a=off) le32(b=len) then dlen header bytes from .data then (b-dlen) zeros */
	CARM_RAW,	/* dlen opaque bytes verbatim from .data (const u8 *) — undecoded queries */
};

struct clarett_arm_step {
	u32 op;			/* FCP opcode */
	u8 kind;		/* enum clarett_arm_kind */
	u32 a, b;		/* inline scalars (see per-kind notes above) */
	const void *data;	/* backing table for CARM_MIX/MUX/WB/RAW; NULL otherwise */
	u16 dlen;		/* element count in .data (coeffs / words / bytes) */
};

/* Serialize one step's payload into buf; returns the byte length. buf must hold the largest
 * payload (a full writeback: 8 + b bytes) — callers size it at CLARETT_MBOX_DATA_MAX. */
static inline int clarett_arm_emit(const struct clarett_arm_step *s, u8 *buf)
{
	u16 i;

	switch (s->kind) {
	case CARM_NONE:
		return 0;
	case CARM_ID:
		buf[0] = s->a; buf[1] = s->a >> 8;
		return 2;
	case CARM_U32:
		buf[0] = s->a; buf[1] = s->a >> 8; buf[2] = s->a >> 16; buf[3] = s->a >> 24;
		return 4;
	case CARM_RANGE:
		buf[0] = s->a; buf[1] = s->a >> 8; buf[2] = s->a >> 16; buf[3] = s->a >> 24;
		buf[4] = s->b; buf[5] = s->b >> 8; buf[6] = s->b >> 16; buf[7] = s->b >> 24;
		return 8;
	case CARM_MIX: {
		const u16 *c = s->data;

		buf[0] = s->a; buf[1] = s->a >> 8;
		for (i = 0; i < s->dlen; i++) {
			buf[2 + 2 * i] = c[i];
			buf[3 + 2 * i] = c[i] >> 8;
		}
		return 2 + 2 * s->dlen;
	}
	case CARM_MUX: {
		const u32 *w = s->data;

		for (i = 0; i < s->dlen; i++) {
			buf[4 * i]     = w[i];
			buf[4 * i + 1] = w[i] >> 8;
			buf[4 * i + 2] = w[i] >> 16;
			buf[4 * i + 3] = w[i] >> 24;
		}
		return 4 * s->dlen;
	}
	case CARM_WB:
		buf[0] = s->a; buf[1] = s->a >> 8; buf[2] = s->a >> 16; buf[3] = s->a >> 24;
		buf[4] = s->b; buf[5] = s->b >> 8; buf[6] = s->b >> 16; buf[7] = s->b >> 24;
		if (s->dlen)
			memcpy(buf + 8, s->data, s->dlen);
		memset(buf + 8 + s->dlen, 0, s->b - s->dlen);
		return 8 + s->b;
	case CARM_RAW:
		memcpy(buf, s->data, s->dlen);
		return s->dlen;
	}
	return -1;
}

#endif /* CLARETT_ARM_H */
