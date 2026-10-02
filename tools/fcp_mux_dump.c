// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fcp_mux_dump — print one band of the device's routing table as the device holds it:
 * MUX_INFO (0x003000) for the band sizes, then MUX_READ (0x003001) in 28-entry windows, exactly as
 * fcp-server reads it.
 *
 * Why: fcp-server writes every routing change into all three rate tables (single, double and quad
 * speed), but only ever reads the single-speed one back. What actually landed in the double/quad
 * tables -- where S/MUX renumbers a second ADAT port -- is visible only by reading them directly.
 *
 * Each entry is (src_pin << 12) | dst_pin. Pass destination pins (hex) to print only those entries.
 *
 * Build: gcc -O2 -o fcp_mux_dump tools/fcp_mux_dump.c
 * Run:   sudo ./fcp_mux_dump /dev/snd/hwC3D0 1 0x204 0x613     (band 1 = double speed)
 * Stop fcp-server first: the hwdep is exclusive.
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
#define MUX_INFO 0x003000
#define MUX_READ 0x003001
#define CHUNK    28		/* entries a single MUX_READ reply carries */

static int fcp(int fd, __u32 op, const void *req, __u16 req_size, void *resp, __u16 resp_size)
{
	size_t n = req_size > resp_size ? req_size : resp_size;
	struct fcp_cmd *cmd = calloc(1, sizeof(*cmd) + n);
	int err;

	cmd->opcode = op;
	cmd->req_size = req_size;
	cmd->resp_size = resp_size;
	if (req_size)
		memcpy(cmd->data, req, req_size);
	err = ioctl(fd, FCP_IOCTL_CMD, cmd);
	if (!err && resp)
		memcpy(resp, cmd->data, resp_size);
	free(cmd);
	return err;
}

int main(int argc, char **argv)
{
	__u16 info[6] = { 0 };
	__u32 *tab;
	int fd, band, size, i, j;

	if (argc < 3) {
		fprintf(stderr, "usage: %s DEV BAND [DST_PIN...]   (BAND 0/1/2 = single/double/quad)\n",
			argv[0]);
		return 2;
	}
	band = atoi(argv[2]);
	if (band < 0 || band > 2) {
		fprintf(stderr, "band must be 0, 1 or 2\n");
		return 2;
	}
	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s (fcp-server still running?)\n", argv[1], strerror(errno));
		return 1;
	}
	if (fcp(fd, MUX_INFO, NULL, 0, info, sizeof(info)) < 0) {
		perror("MUX_INFO");
		return 1;
	}
	size = info[band];
	tab = calloc(size, sizeof(*tab));
	for (i = 0; i < size; i += CHUNK) {
		int n = size - i < CHUNK ? size - i : CHUNK;
		struct { __u8 offset, pad, count, mux_num; } req = { i, 0, n, band };

		if (fcp(fd, MUX_READ, &req, sizeof(req), tab + i, n * sizeof(*tab)) < 0) {
			perror("MUX_READ");
			return 1;
		}
	}
	printf("band %d: %d entries (sizes %d/%d/%d)\n", band, size, info[0], info[1], info[2]);
	for (i = 0; i < size; i++) {
		unsigned dst = tab[i] & 0xfff, src = (tab[i] >> 12) & 0xfff;
		int show = argc == 3;

		for (j = 3; j < argc; j++)
			if (strtoul(argv[j], NULL, 0) == dst)
				show = 1;
		if (show)
			printf("  slot %3d: dst 0x%03x <- src 0x%03x\n", i, dst, src);
	}
	close(fd);
	return 0;
}
