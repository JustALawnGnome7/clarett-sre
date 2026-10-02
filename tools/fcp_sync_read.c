// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fcp_sync_read — print the raw answers to the clock/sync queries: 0x006004 (fcp-server's SYNC_READ,
 * which it collapses to a 0/1 "Sync Status"), 0x006002 and 0x006005 (the live sample rate).
 *
 * Why: 0x006004 is not a clean lock flag -- it has read 1 or 3 depending on model, stream state and
 * rate -- and only the raw word can be tabulated against the conditions that produce it. One line per
 * call, so a script can sample it while it varies one condition at a time.
 *
 * Build: gcc -O2 -o fcp_sync_read tools/fcp_sync_read.c
 * Run:   sudo ./fcp_sync_read /dev/snd/hwC3D0 [label]
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

static int query(int fd, __u32 op, __u32 *val)
{
	struct fcp_cmd *cmd = calloc(1, sizeof(*cmd) + sizeof(*val));
	int err;

	cmd->opcode = op;
	cmd->resp_size = sizeof(*val);
	err = ioctl(fd, FCP_IOCTL_CMD, cmd);
	if (!err)
		memcpy(val, cmd->data, sizeof(*val));
	free(cmd);
	return err;
}

int main(int argc, char **argv)
{
	static const __u32 ops[] = { 0x006004, 0x006002, 0x006005 };
	unsigned i;
	int fd;

	if (argc < 2) {
		fprintf(stderr, "usage: %s DEV [label]\n", argv[0]);
		return 2;
	}
	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s (fcp-server still running?)\n", argv[1], strerror(errno));
		return 1;
	}
	printf("%-28s", argc > 2 ? argv[2] : "");
	for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
		__u32 v = 0;

		if (query(fd, ops[i], &v) < 0)
			printf("  %06x=err(%s)", ops[i], strerror(errno));
		else
			printf("  %06x=0x%08x", ops[i], v);
	}
	printf("\n");
	close(fd);
	return 0;
}
