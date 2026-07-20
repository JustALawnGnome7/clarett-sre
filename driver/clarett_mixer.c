// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clarett 8PreX — ALSA mixer controls.
 *
 * A representative subset of the control-plane spec, all single-byte fields so
 * each maps to one SET_DATA{offset,1,value} + DATA_CMD{activate} pair:
 *   - master mute / monitor dim / master monitor gain
 *   - the per-model analogue output gains (clarett_model.out_gains)
 *   - per analogue input: Air switch and Mic/Line[/Inst] mode
 *
 * "get" returns the shadow: write-through on put, plus the monitor bytes are
 * refreshed from the DMAed GET response on a front-panel notification. Packed
 * bitfields (the per-output hardware gain/dim/mute enables) are not implemented
 * here — they need read-modify-write of shared bytes; see TODO.
 */
#include <sound/control.h>
#include <sound/tlv.h>
#include <linux/moduleparam.h>
#include "clarett.h"

/* Log each control put's actual SET_DATA{offset,value}. For correlating the write side with the
 * seed_dump read side: toggle each preamp control once with this on, then reload with seed_dump=1
 * and diff — reveals the true per-input read/write offset map (and any write off-by-one). */
static bool put_trace;
module_param(put_trace, bool, 0444);
MODULE_PARM_DESC(put_trace,
		 "Log each mixer put's control name, SET_DATA offset, and byte value. Default 0.");

/* 7-bit attenuation code == |dB|, 1 dB/step, 0x00 = 0 dB .. 0x7f = -127 dB */
static const DECLARE_TLV_DB_SCALE(clarett_gain_tlv, -12700, 100, 0);

#define CLARETT_GAIN_MAX 127

/*
 * Mixer gain: ALSA value 0..184 in 0.5 dB steps (−80..+12 dB, value 160 = 0 dB), matching scarlett2.
 * The device coefficient is a 16-bit linear amplitude = floor(8192 * 10^(dB/20)) (0x2000 = unity),
 * clamped to the Clarett max 0x3fd9 (+6 dB) — so values above +6 dB all map to the ceiling. This
 * table is derived from that formula (offline); it is not copied from any driver.
 */
static const DECLARE_TLV_DB_SCALE(clarett_mix_tlv, -8000, 50, 0);

static const u16 clarett_mix_coeff[CLARETT_MIX_MAX_VALUE + 1] = {
	0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001,
	0x0001, 0x0001, 0x0001, 0x0001, 0x0002, 0x0002, 0x0002, 0x0002, 0x0002, 0x0002, 0x0002, 0x0003,
	0x0003, 0x0003, 0x0003, 0x0003, 0x0004, 0x0004, 0x0004, 0x0004, 0x0005, 0x0005, 0x0005, 0x0006,
	0x0006, 0x0006, 0x0007, 0x0007, 0x0008, 0x0008, 0x0009, 0x0009, 0x000a, 0x000a, 0x000b, 0x000c,
	0x000c, 0x000d, 0x000e, 0x000f, 0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0017, 0x0018,
	0x0019, 0x001b, 0x001d, 0x001e, 0x0020, 0x0022, 0x0024, 0x0026, 0x0029, 0x002b, 0x002e, 0x0030,
	0x0033, 0x0036, 0x0039, 0x003d, 0x0041, 0x0044, 0x0049, 0x004d, 0x0051, 0x0056, 0x005b, 0x0061,
	0x0067, 0x006d, 0x0073, 0x007a, 0x0081, 0x0089, 0x0091, 0x009a, 0x00a3, 0x00ad, 0x00b7, 0x00c2,
	0x00cd, 0x00d9, 0x00e6, 0x00f4, 0x0103, 0x0112, 0x0122, 0x0133, 0x0146, 0x0159, 0x016d, 0x0183,
	0x019a, 0x01b2, 0x01cc, 0x01e7, 0x0204, 0x0223, 0x0243, 0x0266, 0x028a, 0x02b1, 0x02da, 0x0305,
	0x0333, 0x0363, 0x0397, 0x03cd, 0x0407, 0x0444, 0x0485, 0x04c9, 0x0512, 0x055f, 0x05b0, 0x0607,
	0x0662, 0x06c3, 0x0729, 0x0796, 0x0809, 0x0883, 0x0904, 0x098d, 0x0a1e, 0x0ab8, 0x0b5a, 0x0c06,
	0x0cbd, 0x0d7e, 0x0e4b, 0x0f24, 0x1009, 0x10fd, 0x11fe, 0x130f, 0x1430, 0x1563, 0x16a7, 0x17ff,
	0x196b, 0x1aec, 0x1c85, 0x1e35, 0x2000, 0x21e5, 0x23e7, 0x2608, 0x2849, 0x2aac, 0x2d33, 0x2fe1,
	0x32b7, 0x35b8, 0x38e7, 0x3c46, 0x3fd9, 0x3fd9, 0x3fd9, 0x3fd9, 0x3fd9, 0x3fd9, 0x3fd9, 0x3fd9,
	0x3fd9, 0x3fd9, 0x3fd9, 0x3fd9, 0x3fd9,
};

/* Device coefficient -> nearest ALSA value: the table is monotic, so return the first value whose
 * coefficient is >= the stored one (scarlett2's convention). */
static unsigned int clarett_mix_coeff_to_value(u16 coeff)
{
	unsigned int v;

	for (v = 0; v < CLARETT_MIX_MAX_VALUE; v++)
		if (clarett_mix_coeff[v] >= coeff)
			return v;
	return CLARETT_MIX_MAX_VALUE;
}

#define CLARETT_GAIN_MAX 127

static int clarett_ctl_info(struct snd_kcontrol *kc,
			    struct snd_ctl_elem_info *ui)
{
	const struct clarett_ctl *d = (const void *)kc->private_value;

	switch (d->type) {
	case CT_SWITCH:
		return snd_ctl_boolean_mono_info(kc, ui);
	case CT_GAIN:
		ui->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
		ui->count = 1;
		ui->value.integer.min = 0;
		ui->value.integer.max = CLARETT_GAIN_MAX;
		return 0;
	case CT_MIX:
		ui->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
		ui->count = 1;
		ui->value.integer.min = 0;
		ui->value.integer.max = CLARETT_MIX_MAX_VALUE;
		return 0;
	case CT_METER:
		ui->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
		ui->count = CLARETT_N_METERS;
		ui->value.integer.min = 0;
		ui->value.integer.max = CLARETT_METER_MAX;
		return 0;
	case CT_ENUM:
	case CT_ROUTE:
	case CT_METERSRC:
		return snd_ctl_enum_info(ui, 1, d->n_texts, d->texts);
	}
	return -EINVAL;
}

/* Map a device byte to its enum item index (inverse of d->values). Identity when values is NULL. */
static unsigned int clarett_enum_item(const struct clarett_ctl *d, u8 dev)
{
	int i;

	if (!d->values)
		return dev < (u8)d->n_texts ? dev : 0;
	for (i = 0; i < d->n_texts; i++)
		if (d->values[i] == dev)
			return i;
	return 0;
}

static int clarett_ctl_get(struct snd_kcontrol *kc,
			   struct snd_ctl_elem_value *uc)
{
	struct clarett *c = snd_kcontrol_chip(kc);
	const struct clarett_ctl *d = (const void *)kc->private_value;
	u8 dev = c->shadow[d->offset];

	/* Sub-byte control: reduce the masked bit to a 0/1 the switch below treats as the device value
	 * (single-bit enums like SW/HW use identity texts, so item 0/1 falls straight out). */
	if (d->mask)
		dev = !!(dev & d->mask);

	switch (d->type) {
	case CT_SWITCH:
		uc->value.integer.value[0] = d->invert ? !dev : !!dev;
		break;
	case CT_GAIN:
		/* device stores attenuation; ALSA value rises with loudness */
		uc->value.integer.value[0] = CLARETT_GAIN_MAX - dev;
		break;
	case CT_ENUM:
		uc->value.enumerated.item[0] = clarett_enum_item(d, dev);
		break;
	case CT_ROUTE:
		uc->value.enumerated.item[0] = d->route_val;	/* read-only, fixed at probe */
		break;
	case CT_MIX:
		uc->value.integer.value[0] =
			clarett_mix_coeff_to_value(clarett_get_le16(c->mix_rows[d->mix_row] + d->offset));
		break;
	case CT_METER: {
		int i;

		for (i = 0; i < CLARETT_N_METERS; i++)
			uc->value.integer.value[i] = c->meter_levels[i];
		break;
	}
	case CT_METERSRC: {
		const struct clarett_meter_source *ms = c->model->meter_sources;
		int i;

		uc->value.enumerated.item[0] = 0;
		for (i = 0; i < c->model->n_meter_sources; i++)
			if (ms[i].value == c->shadow[d->offset]) {
				uc->value.enumerated.item[0] = i;
				break;
			}
		break;
	}
	}
	return 0;
}

/*
 * Follow scarlett2: an output's volume fader is read-only while its SW/HW select is HW (the hardware
 * monitor knob owns the level). Flip the fader kctl's WRITE access and notify userspace on change.
 * Safe before snd_card_register (empty notify list) and before the kctl exists (no-op).
 */
static void clarett_set_fader_writable(struct clarett *c, struct clarett_ctl *vd, bool writable)
{
	struct snd_kcontrol *k = vd ? vd->kctl : NULL;

	if (!k || !!(k->vd[0].access & SNDRV_CTL_ELEM_ACCESS_WRITE) == writable)
		return;
	if (writable)
		k->vd[0].access |= SNDRV_CTL_ELEM_ACCESS_WRITE;
	else
		k->vd[0].access &= ~SNDRV_CTL_ELEM_ACCESS_WRITE;
	snd_ctl_notify(c->card, SNDRV_CTL_EVENT_MASK_INFO, &k->id);
}

/*
 * Routing put: point a destination at a new source. Edit this destination's entry (src field) in
 * each sample-rate band's SET_MUX payload, then resend all three commands — the arm's known-good
 * matrix plus this one delta, matching FC's routing-change cycle (3 SET_MUX, no other command).
 */
static int clarett_route_put(struct clarett *c, struct clarett_ctl *d, unsigned int item)
{
	u16 src, dst = d->route_dst;
	int b, ret = 0;

	if (item >= (unsigned int)d->n_texts)
		return -EINVAL;
	if (item == d->route_val)
		return 0;
	src = c->mux_src_pins[item];

	for (b = 0; b < 3; b++) {
		u8 *p = c->mux_band[b];
		u32 len = c->mux_band_len[b], k;
		int err;

		if (!p)
			continue;
		for (k = 4; k + 4 <= len; k += 4)
			if ((clarett_get_le32(p + k) & 0xfff) == dst) {
				clarett_put_le32(p + k, ((u32)src << 12) | dst);
				break;
			}
		err = clarett_fcp(c, FCP_SET_MUX, p, len);
		if (err && !ret)
			ret = err;
	}
	if (ret)
		return ret;
	d->route_val = item;
	return 1;
}

/*
 * Mixer gain put: set one input's coefficient in one mix bus and resend that bus's SET_MIX row.
 * The row is seeded from the arm blob (all unity), so a change is that row plus one coefficient delta.
 */
static int clarett_mix_put(struct clarett *c, struct clarett_ctl *d, long v)
{
	u8 *row = c->mix_rows[d->mix_row];
	u16 coeff;
	int err;

	if (v < 0 || v > CLARETT_MIX_MAX_VALUE)
		return -EINVAL;
	coeff = clarett_mix_coeff[v];
	if (clarett_get_le16(row + d->offset) == coeff)
		return 0;
	clarett_put_le16(row + d->offset, coeff);
	err = clarett_fcp(c, FCP_SET_MIX, row, c->mix_row_len);
	return err ? err : 1;
}

/*
 * Meter-source put: select which channel set the hardware meters show. Replay FC's cycle — write the
 * three per-band channel-index tables (@136/146/156), then the source byte (@184), then one
 * DATA_CMD{8} that commits the lot.
 */
static int clarett_metersrc_put(struct clarett *c, struct clarett_ctl *d, unsigned int item)
{
	const struct clarett_meter_source *ms;
	int err;

	if (item >= (unsigned int)d->n_texts)
		return -EINVAL;
	ms = &c->model->meter_sources[item];
	if (c->shadow[d->offset] == ms->value)
		return 0;

	err = clarett_set_data(c, METER_TABLE_L_OFFSET, METER_TABLE_LEN, ms->tbl[0]);
	if (!err)
		err = clarett_set_data(c, METER_TABLE_M_OFFSET, METER_TABLE_LEN, ms->tbl[1]);
	if (!err)
		err = clarett_set_data(c, METER_TABLE_H_OFFSET, METER_TABLE_LEN, ms->tbl[2]);
	if (!err)
		err = clarett_set_data(c, METER_SOURCE_OFFSET, 1, &ms->value);
	if (!err)
		err = clarett_data_cmd(c, d->activate);
	if (err)
		return err;
	c->shadow[d->offset] = ms->value;
	return 1;
}

static int clarett_ctl_put(struct snd_kcontrol *kc,
			   struct snd_ctl_elem_value *uc)
{
	struct clarett *c = snd_kcontrol_chip(kc);
	const struct clarett_ctl *d = (const void *)kc->private_value;
	u8 dev, old;
	int err;

	if (d->type == CT_ROUTE)
		return clarett_route_put(c, (struct clarett_ctl *)d,
					 uc->value.enumerated.item[0]);
	if (d->type == CT_MIX)
		return clarett_mix_put(c, (struct clarett_ctl *)d, uc->value.integer.value[0]);
	if (d->type == CT_METERSRC)
		return clarett_metersrc_put(c, (struct clarett_ctl *)d,
					    uc->value.enumerated.item[0]);
	old = c->shadow[d->offset];

	switch (d->type) {
	case CT_SWITCH: {
		int v = !!uc->value.integer.value[0];

		dev = d->invert ? !v : v;
		break;
	}
	case CT_GAIN: {
		long v = uc->value.integer.value[0];

		if (v < 0 || v > CLARETT_GAIN_MAX)
			return -EINVAL;
		dev = CLARETT_GAIN_MAX - v;
		break;
	}
	case CT_ENUM: {
		unsigned int item = uc->value.enumerated.item[0];

		if (item >= (unsigned int)d->n_texts)
			return -EINVAL;
		dev = d->values ? d->values[item] : item;
		break;
	}
	default:
		return -EINVAL;
	}

	/* Sub-byte control: merge the computed 0/1 into just the masked bit, preserving the rest of the
	 * byte (byte 52+ pack two outputs' SW/HW bits). dev is 0/1 here for masked switch/enum. */
	if (d->mask)
		dev = (old & ~d->mask) | (dev ? d->mask : 0);

	/*
	 * Skip a redundant write only when the shadow is KNOWN to match hardware. For a control the
	 * device does not report back (preamp Mode/Air), the seed leaves the shadow at 0, so a genuine
	 * "set to 0" (e.g. Air off, or Mode→Line which maps to a non-zero byte only by luck) would be
	 * silently dropped against the fictional 0. The first put of such a control always writes,
	 * establishing a real hardware value; write-through then makes the byte known for later skips.
	 */
	if (dev == old && test_bit(d->offset, c->shadow_known))
		return 0;

	if (put_trace)
		dev_info(&c->pci->dev, "put: %-32s offset=%u dev=0x%02x (old=0x%02x activate=%u)\n",
			 d->name, d->offset, dev, old, d->activate);

	err = clarett_write_u8(c, d->offset, dev, d->activate);
	if (err)
		return err;

	/* SW/HW select changed: make the governed fader read-only in HW, writable in SW. */
	if (d->vol_link)
		clarett_set_fader_writable(c, d->vol_link, !(dev & d->mask));

	/*
	 * NOTE: this is byte-identical to Focusrite Control's per-toggle sequence (SET_DATA +
	 * DATA_CMD{activate}; FC's standalone DATA_CMD{5} is a once-at-end debounced persist, not
	 * per-toggle). On hardware the write completes (done=1, fcperr=0) but the front-panel state
	 * does not move — see spec/clarett-manifestation-wall.md.
	 */
	return 1;
}

/* --- Read-only routing (mux) view — Phase 1 -------------------------------------------------------
 * Decode the model's default routing from the init blob's band-0 SET_MUX and expose each OUTPUT
 * destination (analogue / S/PDIF / ADAT out) as a read-only enum showing its source. No mux writes
 * yet (Phase 2). Pins are direction-scoped 12-bit values; entry = (src_pin << 12) | dst_pin. Source
 * names come from the confident, XML-verified ranges (0x408/9 as a *source* is S/PDIF in, etc.).
 */
static void clarett_mux_src_name(u16 pin, char *buf, size_t len)
{
	if (!pin)
		strscpy(buf, "Off", len);
	else if (pin >= 0x400 && pin <= 0x407)
		scnprintf(buf, len, "Analogue %u", pin - 0x400 + 1);
	else if (pin == 0x408 || pin == 0x409)
		scnprintf(buf, len, "S/PDIF %u", pin - 0x408 + 1);
	else if (pin >= 0x200 && pin <= 0x20f)
		scnprintf(buf, len, "ADAT %u", pin - 0x200 + 1);
	else if (pin >= 0x300 && pin <= 0x30f)
		scnprintf(buf, len, "Mix %c", 'A' + (pin - 0x300));
	else if (pin >= 0x600 && pin <= 0x61f)
		scnprintf(buf, len, "Playback %u", pin - 0x600 + 1);
	else
		scnprintf(buf, len, "0x%03x", pin);
}

/* Destination name (XML-accurate) + whether it is a capture-side destination (its control uses the
 * "Capture Enum" suffix; outputs use "Playback Enum"). Returns false for an unrecognised pin. */
static bool clarett_mux_dst_name(u16 pin, char *buf, size_t len, bool *is_capture)
{
	*is_capture = false;
	if (pin == 0x408 || pin == 0x409)
		scnprintf(buf, len, "Monitor Output %u", pin - 0x408 + 1);
	else if (pin >= 0x400 && pin <= 0x407)
		scnprintf(buf, len, "Line Output %u", pin - 0x400 + 3);
	else if (pin == 0x186 || pin == 0x187)
		scnprintf(buf, len, "S/PDIF Output %u", pin - 0x186 + 1);
	else if (pin >= 0x200 && pin <= 0x20f)
		scnprintf(buf, len, "ADAT Output %u", pin - 0x200 + 1);
	else if (pin >= 0x600 && pin <= 0x61f)
		{ *is_capture = true; scnprintf(buf, len, "PCM %02u", pin - 0x600 + 1); }
	else if (pin >= 0x300 && pin <= 0x31f)
		{ *is_capture = true; scnprintf(buf, len, "Mixer Input %02u", pin - 0x300 + 1); }
	else
		return false;
	return true;
}

/* Locate the model's band-0 SET_MUX payload in the init blob; return the entry array + count. */
static const u8 *clarett_band0_mux(const struct clarett_model *m, u32 *n_entries)
{
	int i;

	for (i = 0; i < m->n_init_steps; i++) {
		const struct clarett_init_step *s = &m->init_seq[i];
		const u8 *p = m->init_blob + s->off;

		if (s->opcode == FCP_SET_MUX && s->len >= 4 && (clarett_get_le32(p) >> 16) == 0) {
			*n_entries = (s->len - 4) / 4;
			return p + 4;
		}
	}
	return NULL;
}

/* Number of routing controls this model will add (destinations in the band-0 table). */
static int clarett_count_routing(const struct clarett_model *m)
{
	u32 n_mux = 0, i;
	const u8 *mux = clarett_band0_mux(m, &n_mux);
	char nm[24];
	bool cap;
	int cnt = 0;

	for (i = 0; mux && i < n_mux; i++) {
		u32 e = clarett_get_le32(mux + i * 4);

		if (e && clarett_mux_dst_name(e & 0xfff, nm, sizeof(nm), &cap))
			cnt++;
	}
	return cnt;
}

/* Seed the mutable per-band SET_MUX payloads from the arm blob (verbatim copies a routing put edits
 * one entry of and resends). Returns false if the model has no routing table. */
static bool clarett_seed_mux_bands(struct clarett *c)
{
	const struct clarett_model *m = c->model;
	bool any = false;
	int i, b;

	for (i = 0; i < m->n_init_steps; i++) {
		const struct clarett_init_step *s = &m->init_seq[i];
		const u8 *p = m->init_blob + s->off;

		if (s->opcode != FCP_SET_MUX || s->len < 4)
			continue;
		b = clarett_get_le32(p) >> 16;			/* band index from the header */
		if (b < 0 || b >= 3 || c->mux_band[b])
			continue;
		c->mux_band[b] = devm_kmemdup(&c->pci->dev, p, s->len, GFP_KERNEL);
		if (c->mux_band[b]) {
			c->mux_band_len[b] = s->len;
			any = true;
		}
	}
	return any;
}

/* Build writable routing controls: one enum per destination, with a shared source list (item ->
 * pin + name). Appends to c->ctls, advancing *np. Best-effort on allocation failure. */
static void clarett_create_routing_ctls(struct clarett *c, int *np)
{
	const struct clarett_model *m = c->model;
	u32 n_mux = 0, i, j, n_src = 0;
	const u8 *mux = clarett_band0_mux(m, &n_mux);
	u16 srcs[96];
	const char **texts;
	u16 *pins;
	char nm[24];
	bool cap;
	int n = *np;

	if (!mux || !clarett_seed_mux_bands(c))
		return;

	/* Distinct sources across the whole band-0 table -> the shared enum item list (includes Off,
	 * src pin 0, since unrouted mixer inputs carry it). */
	for (i = 0; i < n_mux; i++) {
		u32 e = clarett_get_le32(mux + i * 4);
		u16 src = (e >> 12) & 0xfff;

		if (!e)
			continue;
		for (j = 0; j < n_src; j++)
			if (srcs[j] == src)
				break;
		if (j == n_src && n_src < ARRAY_SIZE(srcs))
			srcs[n_src++] = src;
	}
	if (!n_src)
		return;

	texts = devm_kcalloc(&c->pci->dev, n_src, sizeof(*texts), GFP_KERNEL);
	pins = devm_kcalloc(&c->pci->dev, n_src, sizeof(*pins), GFP_KERNEL);
	if (!texts || !pins)
		return;
	for (j = 0; j < n_src; j++) {
		char *s = devm_kzalloc(&c->pci->dev, 24, GFP_KERNEL);

		if (!s)
			return;
		clarett_mux_src_name(srcs[j], s, 24);
		texts[j] = s;
		pins[j] = srcs[j];
	}
	c->mux_src_pins = pins;

	for (i = 0; i < n_mux; i++) {
		u32 e = clarett_get_le32(mux + i * 4);
		u16 dst = e & 0xfff, src = (e >> 12) & 0xfff;
		struct clarett_ctl *d;

		if (!e || !clarett_mux_dst_name(dst, nm, sizeof(nm), &cap))
			continue;
		for (j = 0; j < n_src && srcs[j] != src; j++)
			;
		d = &c->ctls[n++];
		*d = (struct clarett_ctl){ .type = CT_ROUTE, .texts = texts, .n_texts = n_src,
			.route_val = j, .route_dst = dst };
		scnprintf(d->name, sizeof(d->name), "%s %s Enum", nm, cap ? "Capture" : "Playback");
	}
	*np = n;
}

/* Seed the mutable per-bus SET_MIX rows from the arm blob; sets c->n_mix and c->mix_row_len. */
static bool clarett_seed_mix_rows(struct clarett *c)
{
	const struct clarett_model *m = c->model;
	int i;

	for (i = 0; i < m->n_init_steps; i++) {
		const struct clarett_init_step *s = &m->init_seq[i];
		const u8 *p = m->init_blob + s->off;
		u16 mixn;

		if (s->opcode != FCP_SET_MIX || s->len < 4)
			continue;
		mixn = clarett_get_le16(p);
		if (mixn >= CLARETT_MAX_MIXES || c->mix_rows[mixn])
			continue;
		c->mix_rows[mixn] = devm_kmemdup(&c->pci->dev, p, s->len, GFP_KERNEL);
		if (c->mix_rows[mixn]) {
			c->mix_row_len = s->len;
			if (mixn + 1 > c->n_mix)
				c->n_mix = mixn + 1;
		}
	}
	return c->n_mix > 0;
}

/* Number of mixer-gain controls this model adds: n_mix buses * n_input coefficients per bus. */
static int clarett_count_mix(const struct clarett_model *m)
{
	int i, n_mix = 0, n_in = 0;

	for (i = 0; i < m->n_init_steps; i++) {
		const struct clarett_init_step *s = &m->init_seq[i];

		if (s->opcode == FCP_SET_MIX && s->len >= 4) {
			n_mix++;
			n_in = (s->len - 2) / 2;
		}
	}
	return n_mix * n_in;
}

/* Build the mixer gain matrix: one "Mix X Input NN Playback Volume" per (bus, input). */
static void clarett_create_mix_ctls(struct clarett *c, int *np)
{
	int n = *np, mix, in, n_in;

	if (!clarett_seed_mix_rows(c))
		return;
	n_in = (c->mix_row_len - 2) / 2;
	for (mix = 0; mix < c->n_mix; mix++) {
		if (!c->mix_rows[mix])
			continue;
		for (in = 0; in < n_in; in++) {
			struct clarett_ctl *d = &c->ctls[n++];

			*d = (struct clarett_ctl){ .type = CT_MIX, .mix_row = mix,
				.offset = 2 + in * 2 };
			scnprintf(d->name, sizeof(d->name),
				  "Mix %c Input %02d Playback Volume", 'A' + mix, in + 1);
		}
	}
	*np = n;
}

int clarett_create_controls(struct clarett *c)
{
	const struct clarett_model *m = c->model;
	/* monitor(3) + per-output {volume, mute, SW/HW} (3x gains) + air + mode (per input) + optional
	 * S/PDIF source + routing + mixer + Level Meter. Upper bound: some inputs are air-only
	 * (n_modes == 0, e.g. 4Pre Analogue 3-4) and get no mode control, so the actual count <= total. */
	const int total = 3 + 3 * m->n_out_gains + 2 * m->n_analogue +
			  (m->has_spdif_source ? 2 : 0) + clarett_count_routing(m) +
			  clarett_count_mix(m) + 1 /* Level Meter */ +
			  (m->n_meter_sources > 1 ? 1 : 0);
	struct clarett_ctl *d;
	int i, n = 0, err, gain_base;

	c->ctls = devm_kcalloc(&c->pci->dev, total, sizeof(*c->ctls), GFP_KERNEL);
	if (!c->ctls)
		return -ENOMEM;

	/*
	 * Monitor section (command 2). Trace-confirmed against 8prex_monitor_mutedim.log:
	 * mute @ offset 24 and dim @ offset 28 are 1-bit fields that toggle 0/1 and commit
	 * with activate=2 (both directions verified). Named "Mute"/"Dim" to match the USB unit
	 * (scarlett2). For these to physically affect Monitor Out 1-2 the per-output enable bits
	 * (bytes 72/73, command 3) must be set — see clarett_enable_monitor_hw_controls().
	 */
	d = &c->ctls[n++];
	*d = (struct clarett_ctl){ .type = CT_SWITCH, .offset = 24, .activate = 2, .invert = 1 };
	scnprintf(d->name, sizeof(d->name), "Mute Playback Switch");

	d = &c->ctls[n++];
	*d = (struct clarett_ctl){ .type = CT_SWITCH, .offset = 28, .activate = 2 };
	scnprintf(d->name, sizeof(d->name), "Dim Playback Switch");

	d = &c->ctls[n++];
	/* Read-only reflection of the hardware monitor-volume knob: offset 112 is one of the monitor
	 * bytes (24/28/112) the device refreshes into the shadow on a front-panel notification, and the
	 * knob is the master — software can't override it. Named/typed to match the in-kernel scarlett2
	 * driver's "Master HW Playback Volume" (its R/O SCARLETT2_CONFIG_MASTER_VOLUME control). */
	*d = (struct clarett_ctl){ .type = CT_GAIN, .offset = 112, .activate = 2, .readonly = 1 };
	scnprintf(d->name, sizeof(d->name), "Master HW Playback Volume");

	gain_base = n;				/* volume fader for output i is at ctls[gain_base + i] */
	for (i = 0; i < m->n_out_gains; i++) {
		d = &c->ctls[n++];
		*d = (struct clarett_ctl){ .type = CT_GAIN, .offset = m->out_gains[i].offset, .activate = 1 };
		scnprintf(d->name, sizeof(d->name), "%s Playback Volume", m->out_gains[i].name);
	}

	/* Per-output mute (XML <enable-hardware-mute>; scarlett2 MUTE_SWITCH). On the Clarett this is the
	 * output's "obey the master Mute" bit: bit set = the output is muted whenever the global Mute is
	 * on (the master flag alone does nothing until an output opts in). Bit at HWEN_MUTE_OFFSET +
	 * (i/8), bit i%8 — bit-RMW via mask, command 3. */
	for (i = 0; i < m->n_out_gains; i++) {
		d = &c->ctls[n++];
		*d = (struct clarett_ctl){ .type = CT_SWITCH, .offset = HWEN_MUTE_OFFSET + (i >> 3),
			.mask = 1 << (i & 7), .activate = HWEN_ACTIVATE };
		scnprintf(d->name, sizeof(d->name), "Line %02d Mute Playback Switch", i + 1);
	}

	/* Per-output SW/HW volume-control select (XML <enable-hardware-gain>; scarlett2 SW_HW_SWITCH).
	 * Bit set = HW (output follows the hardware monitor knob), clear = SW. The bit lives at
	 * HWEN_GAIN_OFFSET + (i/2)*4, bit i%2 — two outputs share each byte, so it is a bit-RMW (mask).
	 * Enum {SW, HW} identity, matching scarlett2's "Line Out NN Volume Control Playback Enum". */
	for (i = 0; i < m->n_out_gains; i++) {
		static const char * const swhw_texts[] = { "SW", "HW" };

		d = &c->ctls[n++];
		*d = (struct clarett_ctl){ .type = CT_ENUM, .offset = HWEN_GAIN_OFFSET + (i / 2) * 4,
			.mask = 1 << (i % 2), .activate = HWEN_ACTIVATE,
			.texts = swhw_texts, .n_texts = ARRAY_SIZE(swhw_texts),
			.vol_link = &c->ctls[gain_base + i] };
		scnprintf(d->name, sizeof(d->name),
			  "Line Out %02d Volume Control Playback Enum", i + 1);
	}

	/* Air @ 174+i and Mode @ 166+i are shared bases across models (XML diff). Names follow the
	 * per-model scheme (m->in_prefix / m->mode_label): the USB models match scarlett2's
	 * "Line In N Air/Level ..."; the 8PreX keeps "Analogue N Air/Mode ...". */
	for (i = 0; i < m->n_analogue; i++) {
		d = &c->ctls[n++];
		*d = (struct clarett_ctl){ .type = CT_SWITCH, .offset = 174 + i, .activate = 7 };
		scnprintf(d->name, sizeof(d->name), "%s %d Air Capture Switch", m->in_prefix, i + 1);
	}

	for (i = 0; i < m->n_analogue; i++) {
		if (m->analogue[i].n_modes == 0)	/* air-only input (no Line/Inst switch) — e.g. 4Pre 3-4 */
			continue;
		d = &c->ctls[n++];
		*d = (struct clarett_ctl){ .type = CT_ENUM, .offset = 166 + i, .activate = 6,
			.texts = m->analogue[i].mode_texts, .n_texts = m->analogue[i].n_modes,
			.values = m->analogue[i].mode_values };
		scnprintf(d->name, sizeof(d->name), "%s %d %s Capture Enum",
			  m->in_prefix, i + 1, m->mode_label);
	}

	/* S/PDIF input source (XML <spdif-mode>): 2-bit field @132, DATA_CMD activate 4. Enum and name
	 * match scarlett2's Clarett "S/PDIF Source Capture Enum" ({None, Optical, RCA} = values 0/1/2). */
	if (m->has_spdif_source) {
		static const char * const spdif_texts[] = { "None", "Optical", "RCA" };
		static const u8 spdif_vals[] = { 0, 1, 2 };

		d = &c->ctls[n++];
		*d = (struct clarett_ctl){ .type = CT_ENUM, .offset = SPDIF_SOURCE_OFFSET,
			.activate = SPDIF_SOURCE_ACTIVATE, .texts = spdif_texts,
			.n_texts = ARRAY_SIZE(spdif_texts), .values = spdif_vals };
		scnprintf(d->name, sizeof(d->name), "S/PDIF Source Capture Enum");

		d = &c->ctls[n++];
		*d = (struct clarett_ctl){ .type = CT_ENUM, .offset = SPDIF_OUTPUT_OFFSET,
			.activate = SPDIF_OUTPUT_ACTIVATE, .texts = spdif_texts,
			.n_texts = ARRAY_SIZE(spdif_texts), .values = spdif_vals };
		scnprintf(d->name, sizeof(d->name), "S/PDIF Output Mode Playback Enum");
	}

	/* Routing patchbay: one source-selection enum per destination. */
	clarett_create_routing_ctls(c, &n);

	/* Mixer gain matrix: one gain per (mix bus, input slot). */
	clarett_create_mix_ctls(c, &n);

	/* Level meter: one read-only multi-channel control fed by the GET_METER heartbeat. */
	d = &c->ctls[n++];
	*d = (struct clarett_ctl){ .type = CT_METER, .readonly = 1 };
	scnprintf(d->name, sizeof(d->name), "Level Meter");

	/* Meter Source: which channel set the hardware meters display (8PreX only, >1 source). */
	if (m->n_meter_sources > 1) {
		const char **mtexts = devm_kcalloc(&c->pci->dev, m->n_meter_sources,
						   sizeof(*mtexts), GFP_KERNEL);

		if (mtexts) {
			for (i = 0; i < m->n_meter_sources; i++)
				mtexts[i] = m->meter_sources[i].name;
			d = &c->ctls[n++];
			*d = (struct clarett_ctl){ .type = CT_METERSRC, .offset = METER_SOURCE_OFFSET,
				.activate = METER_SOURCE_ACTIVATE, .texts = mtexts,
				.n_texts = m->n_meter_sources };
			scnprintf(d->name, sizeof(d->name), "Meter Source Capture Enum");
		}
	}

	for (i = 0; i < n; i++) {
		struct snd_kcontrol *kctl;
		bool ro = c->ctls[i].readonly;
		struct snd_kcontrol_new kn = {
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = c->ctls[i].name,
			.info = clarett_ctl_info,
			.get = clarett_ctl_get,
			.put = ro ? NULL : clarett_ctl_put,
			.private_value = (unsigned long)&c->ctls[i],
		};

		if (c->ctls[i].type == CT_GAIN) {
			kn.access = (ro ? SNDRV_CTL_ELEM_ACCESS_READ
					: SNDRV_CTL_ELEM_ACCESS_READWRITE) |
				    SNDRV_CTL_ELEM_ACCESS_TLV_READ;
			kn.tlv.p = clarett_gain_tlv;
		} else if (c->ctls[i].type == CT_MIX) {
			kn.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
				    SNDRV_CTL_ELEM_ACCESS_TLV_READ;
			kn.tlv.p = clarett_mix_tlv;
		} else if (c->ctls[i].type == CT_METER) {
			/* value changes continuously with no notification — mark volatile so
			 * userspace re-reads on every access. */
			kn.access = SNDRV_CTL_ELEM_ACCESS_READ |
				    SNDRV_CTL_ELEM_ACCESS_VOLATILE;
		} else if (ro) {
			kn.access = SNDRV_CTL_ELEM_ACCESS_READ;
		}

		kctl = snd_ctl_new1(&kn, c);
		err = snd_ctl_add(c->card, kctl);	/* frees kctl on error */
		if (err < 0)
			return err;
		c->ctls[i].kctl = kctl;			/* for snd_ctl_notify() */
	}
	c->n_ctls = n;

	/* Initial fader writability from the seeded SW/HW state: HW -> the fader is read-only. All
	 * kctls now exist and the shadow is seeded (seed runs before create_controls in probe). */
	for (i = 0; i < n; i++)
		if (c->ctls[i].vol_link)
			clarett_set_fader_writable(c, c->ctls[i].vol_link,
						   !(c->shadow[c->ctls[i].offset] & c->ctls[i].mask));

	return 0;
}
