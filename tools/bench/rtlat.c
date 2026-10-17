/*
 * Round-trip latency through a cable (retest item 5): capture and playback LINKED, so both start on the
 * same engine frame, playback kept full (lead = buffer), and a -12 dBFS one-sample click played every
 * half second on two playback channels: the analogue one (cabled output -> input) and a digital-loopback
 * reference (routed back inside the device). For each click it finds the capture frame of the largest
 * |sample| on each capture channel after it, and prints the offsets from the frame the click was written
 * at. Device latency = capture frame - playback frame; an app's round trip adds its playback lead
 * (here the buffer). Analogue minus digital = what the converters and the analogue path add.
 *   rtlat <hw:N,0> <pb_ch> <cap_ch> <rate> <period> <buffer> <out_ch> <in_ch> <ref_out> <ref_in> [clicks]
 * Channels are 1-based. Build: gcc -O2 -o rtlat rtlat.c -lasound -lm
 */
#include <alsa/asoundlib.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int setup(snd_pcm_t *h, int ch, unsigned int rate, snd_pcm_uframes_t *per, snd_pcm_uframes_t *buf)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	int err;

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(h, hw);
	snd_pcm_hw_params_set_access(h, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(h, hw, SND_PCM_FORMAT_S32_LE);
	snd_pcm_hw_params_set_channels(h, hw, ch);
	snd_pcm_hw_params_set_rate(h, hw, rate, 0);
	if ((err = snd_pcm_hw_params_set_period_size(h, hw, *per, 0)) < 0 ||
	    (err = snd_pcm_hw_params_set_buffer_size(h, hw, *buf)) < 0 ||
	    (err = snd_pcm_hw_params(h, hw)) < 0)
		return err;
	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(h, sw);
	snd_pcm_sw_params_set_start_threshold(h, sw, LONG_MAX);	/* started explicitly, linked */
	snd_pcm_sw_params_set_avail_min(h, sw, *per);
	return snd_pcm_sw_params(h, sw);
}

int main(int argc, char **argv)
{
	snd_pcm_t *pb, *cap;
	snd_pcm_uframes_t per, buf, per2, buf2;
	unsigned int rate;
	int pch, cch, oc, ic, roc, ric, nclick, err, xruns = 0;
	long spacing, total, i, k;
	long long rd = 0, wr = 0;
	int32_t *pbuf, *cbuf;
	long *ka, *ja, *jd;
	int32_t *peak_a, *peak_d;

	if (argc < 11) {
		fprintf(stderr, "usage: rtlat <dev> <pb_ch> <cap_ch> <rate> <period> <buffer> <out_ch> <in_ch> <ref_out> <ref_in> [clicks]\n");
		return 2;
	}
	pch = atoi(argv[2]); cch = atoi(argv[3]); rate = atoi(argv[4]);
	per = atol(argv[5]); buf = atol(argv[6]);
	oc = atoi(argv[7]); ic = atoi(argv[8]); roc = atoi(argv[9]); ric = atoi(argv[10]);
	nclick = argc > 11 ? atoi(argv[11]) : 8;
	spacing = rate / 2;
	total = (nclick + 2) * spacing;

	if ((err = snd_pcm_open(&pb, argv[1], SND_PCM_STREAM_PLAYBACK, 0)) < 0 ||
	    (err = snd_pcm_open(&cap, argv[1], SND_PCM_STREAM_CAPTURE, 0)) < 0) {
		fprintf(stderr, "open: %s\n", snd_strerror(err)); return 1;
	}
	per2 = per; buf2 = buf;
	if ((err = setup(pb, pch, rate, &per, &buf)) < 0 || (err = setup(cap, cch, rate, &per2, &buf2)) < 0) {
		fprintf(stderr, "setup: %s\n", snd_strerror(err)); return 1;
	}
	if ((err = snd_pcm_link(cap, pb)) < 0) {
		fprintf(stderr, "link: %s\n", snd_strerror(err)); return 1;
	}
	pbuf = calloc(buf * pch, 4);
	cbuf = calloc(per * cch, 4);
	ka = calloc(nclick, sizeof(long)); ja = calloc(nclick, sizeof(long)); jd = calloc(nclick, sizeof(long));
	peak_a = calloc(nclick, 4); peak_d = calloc(nclick, 4);
	for (i = 0; i < nclick; i++) {
		ka[i] = (i + 1) * spacing;
		ja[i] = jd[i] = -1;
	}

	/* prefill the whole playback buffer with silence, then start both streams on one frame */
	snd_pcm_prepare(pb);
	if (snd_pcm_writei(pb, pbuf, buf) != (snd_pcm_sframes_t)buf) {
		fprintf(stderr, "prefill failed\n"); return 1;
	}
	wr = buf;
	if ((err = snd_pcm_start(cap)) < 0) {
		fprintf(stderr, "start: %s\n", snd_strerror(err)); return 1;
	}

	while (rd < total) {
		snd_pcm_sframes_t n = snd_pcm_readi(cap, cbuf, per);

		if (n < 0) {
			xruns++;
			fprintf(stderr, "capture: %s\n", snd_strerror((int)n));
			break;
		}
		/* search: each click's window is [k, k + spacing) in capture frames */
		for (i = 0; i < n; i++) {
			long long f = rd + i;
			int c = (int)(f / spacing) - 1;
			int32_t a, d;

			if (c < 0 || c >= nclick)
				continue;
			a = abs(cbuf[i * cch + ic - 1]);
			d = abs(cbuf[i * cch + ric - 1]);
			if (a > peak_a[c]) { peak_a[c] = a; ja[c] = f; }
			if (d > peak_d[c]) { peak_d[c] = d; jd[c] = f; }
		}
		rd += n;

		/* write the next n frames of playback (frame index wr ...), clicks on oc and roc */
		memset(pbuf, 0, n * pch * 4);
		for (i = 0; i < n; i++) {
			long long f = wr + i;

			for (k = 0; k < nclick; k++)
				if (f == ka[k]) {
					pbuf[i * pch + oc - 1] = 0x20000000;	/* -12 dBFS */
					pbuf[i * pch + roc - 1] = 0x20000000;
				}
		}
		if (snd_pcm_writei(pb, pbuf, n) != n) {
			xruns++;
			fprintf(stderr, "playback write failed\n");
			break;
		}
		wr += n;
	}

	printf("rate=%u period=%lu buffer=%lu xruns=%d\n", rate, (unsigned long)per, (unsigned long)buf, xruns);
	for (i = 0; i < nclick; i++)
		printf("click %ld: analogue %ld frames (peak %.1f dBFS), digital %ld frames (peak %.1f dBFS)\n", i,
		       ja[i] - ka[i], peak_a[i] ? 20 * log10(peak_a[i] / 2147483648.0) : -999.0,
		       jd[i] - ka[i], peak_d[i] ? 20 * log10(peak_d[i] / 2147483648.0) : -999.0);
	snd_pcm_close(pb);
	snd_pcm_close(cap);
	return xruns ? 1 : 0;
}
