// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fcp_cfg_read — dump bytes of the device's configuration space via GET_DATA (0x800000).
 *
 * Why: an ALSA control shows the value userspace chose, never the byte that reached the device.
 * Where the two differ — an enum with explicit device values, an inverted volume — nothing else
 * can tell a correct write from a plausible-looking wrong one. Physical confirmation doesn't
 * discriminate either: a preamp relay clicks on ANY mode change, so it cannot distinguish
 * {Line=1, Inst=2} from {Mic=0, Line=1}. Reading the byte back can, and did (July 21 2026:
 * Inst -> 02, Line -> 01 at offsets 166/167 on a 2Pre).
 *
 * Two things to know before believing a reading:
 *   - Bytes this host has never written read 0 whatever the hardware is doing. The device
 *     restores its own state from flash without mirroring it into the host-readable appspace
 *     (see spec/clarett-control-plane.md and the config-ownership notes), so 0 means "unwritten",
 *     not "Mic".
 *   - Setting a control to the value it already holds writes nothing at all: no change, no
 *     SET_DATA. Toggle away and back if you need a byte written deliberately.
 *
 * Build: gcc -O2 -o fcp_cfg_read tools/fcp_cfg_read.c
 * Run:   sudo ./fcp_cfg_read /dev/snd/hwC5D0 166 2   (stop fcp-server first: the hwdep is exclusive)
 *
 * Useful offsets (spec/clarett-control-plane.md): 24 master mute, 28 dim, 32+ output gains
 * (strided), 166+i preamp mode, 174+i air.
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
#define GET_DATA      0x800000

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/snd/hwC5D0";
	unsigned off = argc > 2 ? strtoul(argv[2], NULL, 0) : 166;
	unsigned len = argc > 3 ? strtoul(argv[3], NULL, 0) : 2;
	struct fcp_cmd *cmd;
	__u32 req[2];
	int fd, i;

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s (fcp-server still running?)\n", dev, strerror(errno));
		return 1;
	}

	cmd = calloc(1, sizeof(*cmd) + len);
	req[0] = off;
	req[1] = len;
	cmd->opcode = GET_DATA;
	cmd->req_size = sizeof(req);
	cmd->resp_size = len;
	memcpy(cmd->data, req, sizeof(req));

	if (ioctl(fd, FCP_IOCTL_CMD, cmd) < 0) {
		fprintf(stderr, "GET_DATA{%u,%u}: %s\n", off, len, strerror(errno));
		return 1;
	}

	/* 16 bytes to a line, each tagged with its absolute offset: dumping a range at two settings of
	 * some physical control and diffing the two is how you find which byte backs it, and that only
	 * works if a changed byte shows up as one changed line. */
	for (i = 0; i < (int)len; i++) {
		if (i % 16 == 0)
			printf("%s%4u:", i ? "\n" : "", off + i);
		printf(" %02x", cmd->data[i]);
	}
	printf("\n");

	close(fd);
	return 0;
}
