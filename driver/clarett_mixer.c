// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clarett 8PreX — ALSA mixer controls.
 *
 * A representative subset of the control-plane spec, all single-byte fields so
 * each maps to one SET_DATA{offset,1,value} + DATA_CMD{activate} pair:
 *   - master mute / monitor dim / master monitor gain
 *   - the 10 analogue output gains
 *   - per analogue input: Air switch and Mic/Line[/Inst] mode
 *
 * "get" returns the write-through shadow (device readback via DMA isn't decoded
 * yet). Packed bitfields (the per-output hardware gain/dim/mute enables) are not
 * implemented here — they need read-modify-write of shared bytes; see TODO.
 */
#include <sound/control.h>
#include <sound/tlv.h>
#include "clarett.h"

/* 7-bit attenuation code == |dB|, 1 dB/step, 0x00 = 0 dB .. 0x7f = -127 dB */
static const DECLARE_TLV_DB_SCALE(clarett_gain_tlv, -12700, 100, 0);

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
	case CT_ENUM:
		return snd_ctl_enum_info(ui, 1, d->n_texts, d->texts);
	}
	return -EINVAL;
}

static int clarett_ctl_get(struct snd_kcontrol *kc,
			   struct snd_ctl_elem_value *uc)
{
	struct clarett *c = snd_kcontrol_chip(kc);
	const struct clarett_ctl *d = (const void *)kc->private_value;
	u8 dev = c->shadow[d->offset];

	switch (d->type) {
	case CT_SWITCH:
		uc->value.integer.value[0] = d->invert ? !dev : !!dev;
		break;
	case CT_GAIN:
		/* device stores attenuation; ALSA value rises with loudness */
		uc->value.integer.value[0] = CLARETT_GAIN_MAX - dev;
		break;
	case CT_ENUM:
		uc->value.enumerated.item[0] = dev;
		break;
	}
	return 0;
}

static int clarett_ctl_put(struct snd_kcontrol *kc,
			   struct snd_ctl_elem_value *uc)
{
	struct clarett *c = snd_kcontrol_chip(kc);
	const struct clarett_ctl *d = (const void *)kc->private_value;
	u8 dev, old = c->shadow[d->offset];

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
		dev = item;
		break;
	}
	default:
		return -EINVAL;
	}

	if (dev == old)
		return 0;
	return clarett_write_u8(c, d->offset, dev, d->activate) ? : 1;
}

int clarett_create_controls(struct clarett *c)
{
	static const char * const mode3[] = { "Mic", "Line", "Inst" };
	static const char * const mode2[] = { "Mic", "Line" };
	static const struct { const char *name; u32 off; } outs[] = {
		{ "Monitor 1", 32 }, { "Monitor 2", 33 },
		{ "Line 3", 36 }, { "Line 4", 37 }, { "Line 5", 40 },
		{ "Line 6", 41 }, { "Line 7", 44 }, { "Line 8", 45 },
		{ "Line 9", 48 }, { "Line 10", 49 },
	};
	const int total = 3 + ARRAY_SIZE(outs) + 8 + 8;
	struct clarett_ctl *d;
	int i, n = 0, err;

	c->ctls = devm_kcalloc(&c->pci->dev, total, sizeof(*c->ctls), GFP_KERNEL);
	if (!c->ctls)
		return -ENOMEM;

	d = &c->ctls[n++];
	*d = (struct clarett_ctl){ .type = CT_SWITCH, .offset = 24, .activate = 2, .invert = 1 };
	scnprintf(d->name, sizeof(d->name), "Master Playback Switch");

	d = &c->ctls[n++];
	*d = (struct clarett_ctl){ .type = CT_SWITCH, .offset = 28, .activate = 2 };
	scnprintf(d->name, sizeof(d->name), "Monitor Dim Playback Switch");

	d = &c->ctls[n++];
	*d = (struct clarett_ctl){ .type = CT_GAIN, .offset = 112, .activate = 2 };
	scnprintf(d->name, sizeof(d->name), "Master Playback Volume");

	for (i = 0; i < ARRAY_SIZE(outs); i++) {
		d = &c->ctls[n++];
		*d = (struct clarett_ctl){ .type = CT_GAIN, .offset = outs[i].off, .activate = 1 };
		scnprintf(d->name, sizeof(d->name), "%s Playback Volume", outs[i].name);
	}

	for (i = 0; i < 8; i++) {
		d = &c->ctls[n++];
		*d = (struct clarett_ctl){ .type = CT_SWITCH, .offset = 174 + i, .activate = 7 };
		scnprintf(d->name, sizeof(d->name), "Analogue %d Air Capture Switch", i + 1);
	}

	for (i = 0; i < 8; i++) {
		d = &c->ctls[n++];
		*d = (struct clarett_ctl){ .type = CT_ENUM, .offset = 166 + i, .activate = 6 };
		if (i < 2) {
			d->texts = mode3;
			d->n_texts = 3;
		} else {
			d->texts = mode2;
			d->n_texts = 2;
		}
		scnprintf(d->name, sizeof(d->name), "Analogue %d Mode Capture Enum", i + 1);
	}

	for (i = 0; i < n; i++) {
		struct snd_kcontrol_new kn = {
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = c->ctls[i].name,
			.info = clarett_ctl_info,
			.get = clarett_ctl_get,
			.put = clarett_ctl_put,
			.private_value = (unsigned long)&c->ctls[i],
		};

		if (c->ctls[i].type == CT_GAIN) {
			kn.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
				    SNDRV_CTL_ELEM_ACCESS_TLV_READ;
			kn.tlv.p = clarett_gain_tlv;
		}

		err = snd_ctl_add(c->card, snd_ctl_new1(&kn, c));
		if (err < 0)
			return err;
	}

	return 0;
}
