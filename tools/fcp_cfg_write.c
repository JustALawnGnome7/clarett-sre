// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fcp_cfg_write — write bytes of the device's configuration space and commit them, then read them
 * back: SET_DATA (0x800001) {offset, len, bytes}, DATA_CMD (0x800002) {activate}, GET_DATA.
 *
 * Why: to test a config byte no map exposes yet, without first inventing a control for it. The
 * read-back is the point — a write the device refuses, clamps or later overrides shows up as a
 * mismatch here, which an ALSA control's cached value would hide. The companion of fcp_cfg_read.
 *
 * Values are bytes (decimal or 0x..), or s16:N for a signed 16-bit little-endian value (the Red's
 * gains: s16:-40 = d8 ff). Activate 0 skips the DATA_CMD (stage only).
 *
 * Build: gcc -O2 -o fcp_cfg_write tools/fcp_cfg_write.c
 * Run:   sudo ./fcp_cfg_write /dev/snd/hwC3D0 24 1 s16:-40     (Red: output 1 gain, activate 1)
 *        sudo ./fcp_cfg_write /dev/snd/hwC3D0 90 3 0           (Red: output 1 enable-hw-control)
 * Stop fcp-server first: the hwdep is exclusive. It re-reads the device when it restarts.
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
#define SET_DATA      0x800001
#define DATA_CMD      0x800002

static int fcp(int fd, __u32 op, const void *req, __u16 req_size, void *resp, __u16 resp_size)
{
	size_t n = req_size > resp_size ? req_size : resp_size;
	struct fcp_cmd *cmd = calloc(1, sizeof(*cmd) + n);
	int err;

	cmd->opcode = op;
	cmd->req_size = req_size;
	cmd->resp_size = resp_size;
	memcpy(cmd->data, req, req_size);
	err = ioctl(fd, FCP_IOCTL_CMD, cmd);
	if (!err && resp)
		memcpy(resp, cmd->data, resp_size);
	free(cmd);
	return err;
}

static void dump(const char *tag, unsigned off, const __u8 *b, unsigned len)
{
	unsigned i;

	printf("%s %u:", tag, off);
	for (i = 0; i < len; i++)
		printf(" %02x", b[i]);
	printf("\n");
}

int main(int argc, char **argv)
{
	__u8 val[64], back[64], req[8 + sizeof(val)];
	unsigned off, act, len = 0;
	__u32 hdr[2], a;
	int fd, i;

	if (argc < 5) {
		fprintf(stderr, "usage: %s DEV OFFSET ACTIVATE VALUE... (VALUE = byte or s16:N)\n", argv[0]);
		return 2;
	}
	off = strtoul(argv[2], NULL, 0);
	act = strtoul(argv[3], NULL, 0);
	for (i = 4; i < argc; i++) {
		if (!strncmp(argv[i], "s16:", 4)) {
			int v = strtol(argv[i] + 4, NULL, 0);

			if (len + 2 > sizeof(val))
				break;
			val[len++] = v & 0xff;
			val[len++] = (v >> 8) & 0xff;
		} else {
			if (len + 1 > sizeof(val))
				break;
			val[len++] = strtoul(argv[i], NULL, 0);
		}
	}

	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s (fcp-server still running?)\n", argv[1], strerror(errno));
		return 1;
	}

	hdr[0] = off;
	hdr[1] = len;
	if (fcp(fd, GET_DATA, hdr, sizeof(hdr), back, len) == 0)
		dump("before", off, back, len);

	memcpy(req, hdr, sizeof(hdr));
	memcpy(req + sizeof(hdr), val, len);
	dump("write ", off, val, len);
	if (fcp(fd, SET_DATA, req, sizeof(hdr) + len, NULL, 0) < 0) {
		fprintf(stderr, "SET_DATA{%u,%u}: %s\n", off, len, strerror(errno));
		return 1;
	}
	if (act) {
		a = act;
		if (fcp(fd, DATA_CMD, &a, sizeof(a), NULL, 0) < 0) {
			fprintf(stderr, "DATA_CMD{%u}: %s\n", act, strerror(errno));
			return 1;
		}
	}

	usleep(100000);		/* let the device apply the commit before reading it back */
	if (fcp(fd, GET_DATA, hdr, sizeof(hdr), back, len) < 0) {
		fprintf(stderr, "GET_DATA{%u,%u}: %s\n", off, len, strerror(errno));
		return 1;
	}
	dump("after ", off, back, len);
	if (memcmp(back, val, len))
		printf("** read-back differs from what was written **\n");

	close(fd);
	return 0;
}
