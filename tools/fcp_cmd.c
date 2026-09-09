// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fcp_cmd — send one arbitrary FCP command through the hwdep and hex-dump the response.
 *
 * The generic bench tool: every other fcp_*.c wraps a fixed opcode, this one takes the opcode and
 * request bytes on the command line, so an undecoded query can be tried on live hardware without
 * writing a tool for it. The response is dumped as hex and as little-endian u32 words.
 *
 * Build:  gcc -O2 -o fcp_cmd tools/fcp_cmd.c
 * Run:    sudo ./fcp_cmd /dev/snd/hwC1D0 <opcode> <resp_len> [req byte ...]
 *         sudo ./fcp_cmd /dev/snd/hwC1D0 0x007001 20 01       # identity query, band 1
 *         sudo ./fcp_cmd /dev/snd/hwC1D0 0x005000 64 0d 00    # port descriptor for id 0x0d
 * Stop fcp-server first — it holds the hwdep exclusively.
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

#define MAX_BYTES 1024

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s <hwdep> <opcode> <resp_len> [req byte ...]\n", prog);
	exit(2);
}

int main(int argc, char **argv)
{
	struct fcp_cmd *cmd;
	__u32 opcode;
	int fd, i, req_len, resp_len, buf;

	if (argc < 4)
		usage(argv[0]);
	opcode = strtoul(argv[2], NULL, 0);
	resp_len = strtol(argv[3], NULL, 0);
	req_len = argc - 4;
	if (resp_len < 0 || resp_len > MAX_BYTES || req_len > MAX_BYTES)
		usage(argv[0]);

	buf = req_len > resp_len ? req_len : resp_len;
	cmd = calloc(1, sizeof(*cmd) + buf);
	if (!cmd)
		return 1;
	cmd->opcode = opcode;
	cmd->req_size = req_len;
	cmd->resp_size = resp_len;
	for (i = 0; i < req_len; i++)
		cmd->data[i] = strtoul(argv[4 + i], NULL, 16);

	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s (fcp-server still running?)\n", argv[1], strerror(errno));
		return 1;
	}

	printf("op=0x%06x req[%d]=", opcode, req_len);
	for (i = 0; i < req_len; i++)
		printf("%02x", cmd->data[i]);
	if (ioctl(fd, FCP_IOCTL_CMD, cmd) < 0) {
		printf("  FAILED: %s\n", strerror(errno));
		return 1;
	}
	printf("\n  hex:");
	for (i = 0; i < resp_len; i++)
		printf("%s%02x", i % 16 ? " " : "\n    ", cmd->data[i]);
	printf("\n  u32:");
	for (i = 0; i + 4 <= resp_len; i += 4)
		printf("%s0x%08x", i % 32 ? " " : "\n    ",
		       cmd->data[i] | cmd->data[i + 1] << 8 | cmd->data[i + 2] << 16 |
		       (__u32)cmd->data[i + 3] << 24);
	printf("\n  ascii: ");
	for (i = 0; i < resp_len; i++)
		putchar(cmd->data[i] >= 0x20 && cmd->data[i] < 0x7f ? cmd->data[i] : '.');
	putchar('\n');

	close(fd);
	free(cmd);
	return 0;
}
