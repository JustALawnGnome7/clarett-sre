// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fcp_mux_probe — probe the Clarett's MUX_READ (0x003001) windowing behaviour.
 *
 * Why this exists: reading a routing table of N entries in one MUX_READ returns only the first 28
 * (112 bytes); everything past that is stale response-buffer content, not device data. The request
 * carries an "offset" field that fcp-server always sets to 0, so the open question is whether the
 * device windows its reply — i.e. whether offset selects the starting entry, letting the full table
 * be read in 28-entry chunks — or whether it simply stores nothing beyond entry 28.
 *
 * This asks the device directly: dump a window at several offsets and compare. If offset works, the
 * window at 28 continues the table; if it does not, it repeats entries 0..27 or returns nothing.
 *
 * Build:  gcc -O2 -o fcp_mux_probe tools/fcp_mux_probe.c
 * Run:    sudo ./fcp_mux_probe /dev/snd/hwC5D0 [band]      (stop fcp-server first — hwdep is exclusive)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/types.h>

struct fcp_cmd {
	__u32 opcode;
	__u16 req_size;
	__u16 resp_size;
	__u8  data[];
};
#define FCP_IOCTL_CMD _IOWR('S', 0x65, struct fcp_cmd)

#define MUX_READ 0x003001
#define CHUNK    28		/* entries a single reply appears to carry */

/* Request layout as fcp-server builds it: {u8 offset, u8 pad, u8 count, u8 mux_num}. */
struct mux_req {
	__u8 offset;
	__u8 pad;
	__u8 count;
	__u8 mux_num;
};

static int mux_read(int fd, int band, int offset, int count, __u32 *out)
{
	size_t need = sizeof(struct fcp_cmd) + count * sizeof(__u32);
	struct fcp_cmd *cmd = calloc(1, need);
	struct mux_req req = { .offset = offset, .pad = 0,
			       .count = count, .mux_num = band };
	int err;

	if (!cmd)
		return -ENOMEM;

	cmd->opcode = MUX_READ;
	cmd->req_size = sizeof(req);
	cmd->resp_size = count * sizeof(__u32);
	memcpy(cmd->data, &req, sizeof(req));

	/* Poison the reply area so device-written words are distinguishable from untouched ones. */
	memset(cmd->data, 0xAA, cmd->resp_size);
	memcpy(cmd->data, &req, sizeof(req));

	err = ioctl(fd, FCP_IOCTL_CMD, cmd);
	if (err < 0)
		err = -errno;
	else
		memcpy(out, cmd->data, cmd->resp_size);

	free(cmd);
	return err;
}

static void dump(const char *tag, const __u32 *v, int n)
{
	printf("%s:", tag);
	for (int i = 0; i < n; i++) {
		if (i % 8 == 0)
			printf("\n  ");
		printf(" %03x %03x ", v[i] >> 12, v[i] & 0xfff);
	}
	printf("\n");
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/snd/hwC5D0";
	int band = argc > 2 ? atoi(argv[2]) : 0;
	__u32 base[CHUNK], win[CHUNK];
	int fd, err;

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s (is fcp-server still running?)\n",
			dev, strerror(errno));
		return 1;
	}

	printf("== band %d, %d entries per request ==\n", band, CHUNK);

	err = mux_read(fd, band, 0, CHUNK, base);
	if (err < 0) {
		fprintf(stderr, "MUX_READ offset=0 failed: %s\n", strerror(-err));
		return 1;
	}
	dump("offset 0", base, CHUNK);

	for (int off = CHUNK; off <= CHUNK * 2; off += CHUNK) {
		char tag[32];

		err = mux_read(fd, band, off, CHUNK, win);
		if (err < 0) {
			fprintf(stderr, "MUX_READ offset=%d failed: %s\n", off, strerror(-err));
			continue;
		}
		snprintf(tag, sizeof(tag), "offset %d", off);
		dump(tag, win, CHUNK);

		if (!memcmp(win, base, sizeof(base)))
			printf("  -> IDENTICAL to offset 0: the offset field is ignored\n");
		else
			printf("  -> DIFFERS from offset 0: the device windows its reply\n");
	}

	close(fd);
	return 0;
}
