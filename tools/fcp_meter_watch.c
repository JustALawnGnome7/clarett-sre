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
 * Run:    sudo ./fcp_meter_watch /dev/snd/hwC5D0 [seconds]   (stop fcp-server first)
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
#define N_SLOTS     48		/* num_meters=0x30, as Focusrite Control requests */
#define NOISE       8		/* below this a slot is treated as idle (range is 0..4095) */

/* Request is {u16 pad, u16 num_meters, u32 magic=1} — the same payload the driver's heartbeat uses. */
static int meter_read(int fd, __u32 *out)
{
	struct fcp_cmd *cmd = calloc(1, sizeof(*cmd) + N_SLOTS * sizeof(__u32));
	__u8 req[8] = { 0, 0, N_SLOTS & 0xff, N_SLOTS >> 8, 1, 0, 0, 0 };
	int err;

	if (!cmd)
		return -ENOMEM;

	cmd->opcode = GET_METER;
	cmd->req_size = sizeof(req);
	cmd->resp_size = N_SLOTS * sizeof(__u32);
	memcpy(cmd->data, req, sizeof(req));

	err = ioctl(fd, FCP_IOCTL_CMD, cmd) < 0 ? -errno : 0;
	if (!err)
		memcpy(out, cmd->data, N_SLOTS * sizeof(__u32));

	free(cmd);
	return err;
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/snd/hwC5D0";
	int seconds = argc > 2 ? atoi(argv[2]) : 20;
	__u32 now[N_SLOTS], peak[N_SLOTS];
	int fd, i, ticks = 0;

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s (fcp-server still running?)\n", dev, strerror(errno));
		return 1;
	}
	memset(peak, 0, sizeof(peak));

	printf("Watching %d meter slots for %ds. Put signal on ONE input and note which slot moves.\n",
	       N_SLOTS, seconds);
	printf("(Ctrl-C to stop early; the peak table prints at the end.)\n\n");

	for (; ticks < seconds * 10; ticks++) {
		if (meter_read(fd, now)) {
			fprintf(stderr, "GET_METER failed: %s\n", strerror(errno));
			return 1;
		}
		for (i = 0; i < N_SLOTS; i++)
			if (now[i] > peak[i])
				peak[i] = now[i];

		/* Live line: only the slots currently above the noise floor, so a moving channel stands out. */
		printf("\r\033[K");
		for (i = 0; i < N_SLOTS; i++)
			if (now[i] > NOISE)
				printf(" [%02d]=%-4u", i, now[i]);
		fflush(stdout);

		nanosleep(&(struct timespec){ .tv_nsec = 100000000 }, NULL);
	}

	printf("\n\nPer-slot peak over the run:\n");
	for (i = 0; i < N_SLOTS; i++) {
		if (i % 8 == 0)
			printf("\n  %02d:", i);
		printf(" %5u", peak[i]);
	}
	printf("\n\nSlots that saw signal (peak > %d):", NOISE);
	for (i = 0; i < N_SLOTS; i++)
		if (peak[i] > NOISE)
			printf(" %d", i);
	printf("\n");

	close(fd);
	return 0;
}
