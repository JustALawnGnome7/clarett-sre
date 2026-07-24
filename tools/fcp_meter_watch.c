// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fcp_meter_watch — poll GET_METER and show which raw slots carry signal.
 *
 * Why: fcp-server's meter support needs a "peak-index" on each source/destination in the device map,
 * naming the raw GET_METER slot that channel's level appears in. USB FCP devices get that from the
 * device's own devmap; the Clarett Thunderbolt line does not serve one (DEVMAP_INFO returns size 0),
 * so the mapping has to be measured. Without it fcp-server logs "No meters found" and creates no
 * Level Meter control, and the kernel-side meter (clarett_hwdep.c) sits unused.
 *
 * Method: put signal on one known input at a time and watch which slot moves. The display keeps a
 * per-slot peak across the whole run and flags slots that ever exceeded the noise threshold, so a
 * single tap on a cable is enough to identify a channel. Nothing is written to the device.
 *
 * Build:  gcc -O2 -o fcp_meter_watch tools/fcp_meter_watch.c
 * Run:    sudo ./fcp_meter_watch /dev/snd/hwC5D0 [seconds] [slots]   (stop fcp-server first)
 *
 * The slot count is per model and MUST cover the whole array or the tail is silently invisible:
 * the 2Pre serves 48, but a 4Pre's Mixer Input 30 is expected around slot 57. Default 48 (the
 * 2Pre / Focusrite Control value); pass more when probing a larger model.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/types.h>

struct fcp_cmd {
	__u32 opcode;
	__u16 req_size;
	__u16 resp_size;
	__u8  data[];
};
#define FCP_IOCTL_CMD _IOWR('S', 0x65, struct fcp_cmd)

#define GET_METER   0x001001
#define N_SLOTS_DEF 48		/* num_meters=0x30, as Focusrite Control requests on the 2Pre */
#define N_SLOTS_MAX 256
#define MARGIN      40		/* a slot must exceed its own baseline by this to count as "moved" */
#define BASE_TICKS  10		/* first second of samples establishes each slot's idle baseline */

/*
 * A slot is a 32-bit word carrying a 16-BIT level, replicated into both halves (0x020a020a = 522).
 * Mask the low half; a plain u32 read is ~8000x over the 0..4095 full scale.
 */
#define LEVEL(w)    ((w) & 0xffff)

/* Request is {u16 pad, u16 num_meters, u32 magic=1} — the same payload the driver's heartbeat uses. */
static int meter_read(int fd, __u32 *out, int n_slots)
{
	struct fcp_cmd *cmd = calloc(1, sizeof(*cmd) + n_slots * sizeof(__u32));
	__u8 req[8] = { 0, 0, n_slots & 0xff, n_slots >> 8, 1, 0, 0, 0 };
	int err;

	if (!cmd)
		return -ENOMEM;

	cmd->opcode = GET_METER;
	cmd->req_size = sizeof(req);
	cmd->resp_size = n_slots * sizeof(__u32);
	memcpy(cmd->data, req, sizeof(req));

	err = ioctl(fd, FCP_IOCTL_CMD, cmd) < 0 ? -errno : 0;
	if (!err)
		memcpy(out, cmd->data, n_slots * sizeof(__u32));

	free(cmd);
	return err;
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/snd/hwC5D0";
	int seconds = argc > 2 ? atoi(argv[2]) : 20;
	int n_slots = argc > 3 ? atoi(argv[3]) : N_SLOTS_DEF;
	__u32 now[N_SLOTS_MAX], peak[N_SLOTS_MAX], base[N_SLOTS_MAX];
	int fd, i, ticks = 0;

	if (n_slots < 1 || n_slots > N_SLOTS_MAX) {
		fprintf(stderr, "slots must be 1..%d\n", N_SLOTS_MAX);
		return 1;
	}

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s (fcp-server still running?)\n", dev, strerror(errno));
		return 1;
	}
	memset(peak, 0, sizeof(peak));
	memset(base, 0, sizeof(base));

	printf("Watching %d meter slots for %ds.\n", n_slots, seconds);
	printf("The first %d samples set each slot's idle baseline; after that, put signal on ONE\n",
	       BASE_TICKS);
	printf("input and watch which slot rises. Levels are 0..4095.\n\n");

	for (; ticks < seconds * 10; ticks++) {
		if (meter_read(fd, now, n_slots)) {
			fprintf(stderr, "GET_METER failed: %s\n", strerror(errno));
			return 1;
		}
		for (i = 0; i < n_slots; i++) {
			__u32 lvl = LEVEL(now[i]);

			/* Baseline = the highest idle reading seen in the first second. Everything here
			 * sits on a noise floor of several hundred, so absolute thresholds are useless:
			 * what identifies a channel is rising above ITS OWN idle level. */
			if (ticks < BASE_TICKS) {
				if (lvl > base[i])
					base[i] = lvl;
				continue;
			}
			if (lvl > peak[i])
				peak[i] = lvl;
		}
		if (ticks < BASE_TICKS) {
			printf("\rmeasuring baseline...");
			fflush(stdout);
			goto tick;
		}

		/* Live line: only slots currently above their own baseline, so a moving channel stands out. */
		printf("\r\033[K");
		for (i = 0; i < n_slots; i++)
			if (LEVEL(now[i]) > base[i] + MARGIN)
				printf(" [%02d]=%-4u", i, LEVEL(now[i]));
		fflush(stdout);
tick:

		nanosleep(&(struct timespec){ .tv_nsec = 100000000 }, NULL);
	}

	printf("\n\nSlot:  baseline -> peak  (rise)\n");
	for (i = 0; i < n_slots; i++)
		printf("  %02d: %5u -> %5u  (%+d)%s\n", i, base[i], peak[i],
		       (int)peak[i] - (int)base[i],
		       peak[i] > base[i] + MARGIN ? "   <== MOVED" : "");

	printf("\nSlots that rose more than %d above their baseline:", MARGIN);
	for (i = 0; i < n_slots; i++)
		if (peak[i] > base[i] + MARGIN)
			printf(" %d", i);
	printf("\n");

	close(fd);
	return 0;
}
