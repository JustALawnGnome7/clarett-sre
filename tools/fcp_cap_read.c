// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fcp_cap_read — ask the device which FCP opcode categories it supports.
 *
 * Why: fcp-server refuses to start unless CAP_READ reports the INIT and DATA categories as
 * supported ("Device does not support required INIT category"), and the failing read is invisible
 * from userspace otherwise — fcp-server prints the verdict, not the byte. This tool shows the raw
 * per-category answer, plus a GET_DATA probe, which together separate the two ways that check can
 * fail: a device that never took the vendor bring-up (GET_DATA refused too) from an armed device
 * whose capability bytes come back zero.
 *
 * CAP_READ is opcode 0x000001 — the same "subsystem enable/query" opcode the vendor init replays.
 * Request is a u16 category; the reply is one byte, non-zero = supported.
 *
 * Build:  gcc -O2 -o fcp_cap_read tools/fcp_cap_read.c
 * Run:    sudo ./fcp_cap_read /dev/snd/hwC3D0        (stop fcp-server first — it holds the hwdep)
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

#define CAP_READ    0x000001
#define GET_DATA    0x800000

static const struct {
	int category;
	const char *name;
	const char *note;
} cats[] = {
	{ 0x000, "INIT",    "required by fcp-server" },
	{ 0x001, "METER",   "level meter" },
	{ 0x002, "MIX",     "mixer gain matrix" },
	{ 0x003, "MUX",     "routing" },
	{ 0x004, "FLASH",   "firmware update" },
	{ 0x006, "SYNC",    "clock sync status" },
	{ 0x009, "ESP_DFU", "" },
	{ 0x800, "DATA",    "required by fcp-server" },
};

/* One FCP round trip through the hwdep; the driver strips the response header for us. */
static int fcp(int fd, __u32 opcode, const void *req, int req_size, void *resp, int resp_size)
{
	int buf = req_size > resp_size ? req_size : resp_size;
	struct fcp_cmd *cmd = calloc(1, sizeof(*cmd) + buf);
	int err;

	if (!cmd)
		return -ENOMEM;

	cmd->opcode = opcode;
	cmd->req_size = req_size;
	cmd->resp_size = resp_size;
	if (req_size)
		memcpy(cmd->data, req, req_size);

	err = ioctl(fd, FCP_IOCTL_CMD, cmd) < 0 ? -errno : 0;
	if (!err && resp_size)
		memcpy(resp, cmd->data, resp_size);

	free(cmd);
	return err;
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/snd/hwC3D0";
	__u8 cfg[8];
	__u32 get_data_req[2] = { 0, sizeof(cfg) };
	int fd, i, err, ok = 0;

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s (fcp-server still running?)\n", dev, strerror(errno));
		return 1;
	}

	printf("CAP_READ (opcode 0x%06x), one byte per category:\n\n", CAP_READ);
	for (i = 0; i < (int)(sizeof(cats) / sizeof(cats[0])); i++) {
		__u16 req = cats[i].category;
		__u8 resp = 0;

		err = fcp(fd, CAP_READ, &req, sizeof(req), &resp, sizeof(resp));
		if (err) {
			printf("  0x%03x %-8s  COMMAND FAILED: %s\n",
			       cats[i].category, cats[i].name, strerror(-err));
			continue;
		}
		printf("  0x%03x %-8s  %s (0x%02x)%s%s%s\n",
		       cats[i].category, cats[i].name,
		       resp ? "supported" : "NOT SUPPORTED", resp,
		       cats[i].note[0] ? "   [" : "", cats[i].note, cats[i].note[0] ? "]" : "");
		if (resp)
			ok++;
	}

	/* The same probe the driver's clarett_is_armed() uses: a fresh device refuses GET_DATA. */
	err = fcp(fd, GET_DATA, get_data_req, sizeof(get_data_req), cfg, sizeof(cfg));
	printf("\nGET_DATA{off=0,len=8}: ");
	if (err) {
		printf("FAILED: %s  => the device is NOT armed (bring-up never took).\n", strerror(-err));
	} else {
		printf("ok  %02x %02x %02x %02x %02x %02x %02x %02x\n",
		       cfg[0], cfg[1], cfg[2], cfg[3], cfg[4], cfg[5], cfg[6], cfg[7]);
		printf("  => the mailbox answers config reads, so the session is up.\n");
	}

	if (!ok)
		printf("\nNo category reported supported: the vendor bring-up did not run or did not take.\n"
		       "Reload with force_arm=1 against a freshly power-cycled device.\n");

	close(fd);
	return 0;
}
