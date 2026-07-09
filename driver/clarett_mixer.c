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
	}
	return 0;
}

static int clarett_ctl_put(struct snd_kcontrol *kc,
			   struct snd_ctl_elem_value *uc)
{
	struct clarett *c = snd_kcontrol_chip(kc);
	const struct clarett_ctl *d = (const void *)kc->private_value;
	u8 dev, old = c->shadow[d->offset];
	int err;

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

	if (dev == old)
		return 0;

	err = clarett_write_u8(c, d->offset, dev, d->activate);
	if (err)
		return err;

	/*
	 * NOTE: this is byte-identical to Focusrite Control's per-toggle sequence (SET_DATA +
	 * DATA_CMD{activate}; FC's standalone DATA_CMD{5} is a once-at-end debounced persist, not
	 * per-toggle). On hardware the write completes (done=1, fcperr=0) but the front-panel state
	 * does not move — see spec/clarett-manifestation-wall.md.
	 */
	return 1;
}

int clarett_create_controls(struct clarett *c)
{
	const struct clarett_model *m = c->model;
	/* monitor + gains + air(per input) + mode(per input). Upper bound: some inputs are air-only
	 * (n_modes == 0, e.g. 4Pre Analogue 3-4) and get no mode control, so the actual count <= total. */
	const int total = 3 + m->n_out_gains + 2 * m->n_analogue;
	struct clarett_ctl *d;
	int i, n = 0, err;

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
	*d = (struct clarett_ctl){ .type = CT_GAIN, .offset = 112, .activate = 2 };
	scnprintf(d->name, sizeof(d->name), "Master Playback Volume");

	for (i = 0; i < m->n_out_gains; i++) {
		d = &c->ctls[n++];
		*d = (struct clarett_ctl){ .type = CT_GAIN, .offset = m->out_gains[i].offset, .activate = 1 };
		scnprintf(d->name, sizeof(d->name), "%s Playback Volume", m->out_gains[i].name);
	}

	/* Air @ 174+i and Mode @ 166+i are shared bases across models (XML diff). */
	for (i = 0; i < m->n_analogue; i++) {
		d = &c->ctls[n++];
		*d = (struct clarett_ctl){ .type = CT_SWITCH, .offset = 174 + i, .activate = 7 };
		scnprintf(d->name, sizeof(d->name), "Analogue %d Air Capture Switch", i + 1);
	}

	for (i = 0; i < m->n_analogue; i++) {
		if (m->analogue[i].n_modes == 0)	/* air-only input (no Line/Inst switch) — e.g. 4Pre 3-4 */
			continue;
		d = &c->ctls[n++];
		*d = (struct clarett_ctl){ .type = CT_ENUM, .offset = 166 + i, .activate = 6,
			.texts = m->analogue[i].mode_texts, .n_texts = m->analogue[i].n_modes,
			.values = m->analogue[i].mode_values };
		scnprintf(d->name, sizeof(d->name), "Analogue %d Mode Capture Enum", i + 1);
	}

	for (i = 0; i < n; i++) {
		struct snd_kcontrol *kctl;
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

		kctl = snd_ctl_new1(&kn, c);
		err = snd_ctl_add(c->card, kctl);	/* frees kctl on error */
		if (err < 0)
			return err;
		c->ctls[i].kctl = kctl;			/* for snd_ctl_notify() */
	}
	c->n_ctls = n;

	return 0;
}
