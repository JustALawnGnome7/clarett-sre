/*
 * Playback client that holds a deliberately SMALL queue ahead of the hardware, for the tx_guard
 * question. The driver's TX fill does not write within tx_guard frames of the engine's read position,
 * which assumes the application keeps more than that queued; aplay keeps its whole buffer full, so it
 * never probes the assumption. This client pins the lead where you ask, writing `chunk` frames
 * whenever the queue drops to lead - chunk. Plays the digital-loopback ramp ((n+1)*256) on one
 * channel and silence on the rest; an underrun is recovered and the ramp carries on.
 *   leadplay <hw:N,0> <channels> <ramp_ch> <rate> <buffer> <period> <lead> <chunk> <seconds>
 * Build: gcc -O2 -o leadplay leadplay.c -lasound
 */
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	snd_pcm_t *h;
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	snd_pcm_uframes_t B, P;
	unsigned int rate;
	long nch, rch, lead, chunk, qmin = 1 << 30, qmax = -1, underruns = 0;
	long long written = 0, target, i;
	int32_t *buf;
	int err;

	if (argc < 10) {
		fprintf(stderr, "usage: leadplay <dev> <channels> <ramp_ch> <rate> <buffer> <period> <lead> <chunk> <seconds>\n");
		return 2;
	}
	nch = atol(argv[2]); rch = atol(argv[3]); rate = atoi(argv[4]);
	B = atol(argv[5]); P = atol(argv[6]); lead = atol(argv[7]); chunk = atol(argv[8]);
	target = atoll(argv[9]) * rate;

	if ((err = snd_pcm_open(&h, argv[1], SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
		fprintf(stderr, "open: %s\n", snd_strerror(err)); return 1;
	}
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(h, hw);
	snd_pcm_hw_params_set_access(h, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(h, hw, SND_PCM_FORMAT_S32_LE);
	snd_pcm_hw_params_set_channels(h, hw, nch);
	snd_pcm_hw_params_set_rate(h, hw, rate, 0);
	if ((err = snd_pcm_hw_params_set_period_size(h, hw, P, 0)) < 0 ||
	    (err = snd_pcm_hw_params_set_buffer_size(h, hw, B)) < 0 ||
	    (err = snd_pcm_hw_params(h, hw)) < 0) {
		fprintf(stderr, "hw_params (buffer %lu period %lu): %s\n", (unsigned long)B,
			(unsigned long)P, snd_strerror(err));
		return 1;
	}
	snd_pcm_hw_params_get_buffer_size(hw, &B);
	snd_pcm_hw_params_get_period_size(hw, &P, 0);

	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(h, sw);
	snd_pcm_sw_params_set_start_threshold(h, sw, lead);	/* start once the lead is queued */
	snd_pcm_sw_params_set_avail_min(h, sw, 1);
	if ((err = snd_pcm_sw_params(h, sw)) < 0) {
		fprintf(stderr, "sw_params: %s\n", snd_strerror(err)); return 1;
	}

	buf = calloc(chunk * nch, 4);
	while (written < target) {
		snd_pcm_sframes_t avail;
		long queued;

		for (;;) {
			avail = snd_pcm_avail_update(h);
			if (avail < 0) {
				if (avail == -EPIPE) { underruns++; snd_pcm_prepare(h); continue; }
				fprintf(stderr, "avail: %s\n", snd_strerror((int)avail)); goto done;
			}
			queued = (long)B - avail;
			if (queued <= lead - chunk)
				break;
			usleep(150);
		}
		if (queued < qmin) qmin = queued;
		if (queued > qmax) qmax = queued;

		for (i = 0; i < chunk; i++)
			buf[i * nch + rch - 1] = (int32_t)(uint32_t)((written + i + 1) * 256);
		err = snd_pcm_writei(h, buf, chunk);
		if (err < 0) {
			if (err == -EPIPE) { underruns++; snd_pcm_prepare(h); continue; }
			fprintf(stderr, "writei: %s\n", snd_strerror(err)); break;
		}
		written += err;
	}
done:
	snd_pcm_drain(h);
	printf("buffer=%lu period=%lu lead=%ld chunk=%ld written=%lld underruns=%ld queued_min=%ld queued_max=%ld\n",
	       (unsigned long)B, (unsigned long)P, lead, chunk, written, underruns, qmin, qmax);
	snd_pcm_close(h);
	return 0;
}
