/*
 * Capture client that reads frames as soon as the driver reports them, not at period boundaries:
 * the test for a live capture position. Non-blocking; asks the driver for its position with
 * snd_pcm_avail() (hwsync) every 250 us and reads whatever is there, 16 frames or more.
 * snd_pcm_avail_update() would NOT do: it never calls the driver, so it only moves at period events.
 * Raw S32_LE to stdout; on stderr, how many reads were shorter than a period, and xruns.
 *   livecap <device> <rate> <channels> <period> <seconds> [buffer, default 4 periods]
 * Build: gcc -O2 -o livecap livecap.c -lasound
 */
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	snd_pcm_t *h;
	snd_pcm_hw_params_t *hw;
	snd_pcm_uframes_t period, buffer;
	unsigned int rate, ch;
	long total, got = 0, reads = 0, partial = 0, xruns = 0, minread = 1 << 30;
	int32_t *buf;

	if (argc != 6 && argc != 7) {
		fprintf(stderr, "usage: livecap <dev> <rate> <ch> <period> <secs> [buffer]\n");
		return 2;
	}
	rate = atoi(argv[2]);
	ch = atoi(argv[3]);
	period = atol(argv[4]);
	buffer = argc == 7 ? (snd_pcm_uframes_t)atol(argv[6]) : period * 4;
	total = (long)rate * atol(argv[5]);
	if (snd_pcm_open(&h, argv[1], SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK) < 0)
		return perror("open"), 1;
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(h, hw);
	snd_pcm_hw_params_set_access(h, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(h, hw, SND_PCM_FORMAT_S32_LE);
	snd_pcm_hw_params_set_channels(h, hw, ch);
	snd_pcm_hw_params_set_rate(h, hw, rate, 0);
	snd_pcm_hw_params_set_period_size(h, hw, period, 0);
	snd_pcm_hw_params_set_buffer_size(h, hw, buffer);
	if (snd_pcm_hw_params(h, hw) < 0)
		return fprintf(stderr, "livecap: hw_params failed (period %lu buffer %lu)\n",
			       period, buffer), 1;
	buf = malloc(buffer * ch * 4);
	snd_pcm_start(h);
	while (got < total) {
		snd_pcm_sframes_t a = snd_pcm_avail(h), r;

		if (a < 0) {
			xruns++;
			snd_pcm_prepare(h);
			snd_pcm_start(h);
			continue;
		}
		if (a < 16) {
			usleep(250);
			continue;
		}
		if (a > (snd_pcm_sframes_t)buffer)
			a = buffer;
		if (a > total - got)
			a = total - got;
		r = snd_pcm_readi(h, buf, a);
		if (r < 0) {
			xruns++;
			snd_pcm_prepare(h);
			snd_pcm_start(h);
			continue;
		}
		fwrite(buf, ch * 4, r, stdout);
		got += r;
		reads++;
		if (r < (snd_pcm_sframes_t)period)
			partial++;
		if (r < minread)
			minread = r;
	}
	fprintf(stderr, "livecap: %ld frames in %ld reads, %ld shorter than the %lu-frame period (smallest %ld), xruns %ld\n",
		got, reads, partial, period, minread, xruns);
	snd_pcm_close(h);
	return 0;
}
