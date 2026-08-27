// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fcp_flash_dump — enumerate the device's flash segments and dump their contents.
 *
 * Why: the host-readable appspace only shows bytes this host has written (see fcp_cfg_read), so a
 * used device's stored settings are invisible from there. The device restores them from flash, and
 * the FLASH category can read that flash back — so this is the only way to capture what a unit
 * actually holds before anything overwrites it.
 *
 * Segment names follow the scarlett2 convention: "App_Gold" (recovery, segment 0), "App_Settings"
 * (the persisted configuration) and "App_Upgrade" (firmware). App_Settings is the interesting one.
 *
 * READ ONLY by design: the FLASH category also has erase (0x004002) and write (0x004004) opcodes.
 * They are deliberately not implemented here — a bad write to App_Gold bricks the unit.
 *
 * Opcodes: FLASH_INFO 0x004000 {} -> {u32 count, u8 unknown[8]}
 *          FLASH_SEGMENT_INFO 0x004001 {u32 index} -> {u32 size, u32 flags, char name[16]}
 *          FLASH_READ 0x004005 {u32 segment, u32 offset, u32 len} -> len bytes
 *
 * Build:  gcc -O2 -o fcp_flash_dump tools/fcp_flash_dump.c
 * Run:    sudo ./fcp_flash_dump /dev/snd/hwC1D0                    (list segments)
 *         sudo ./fcp_flash_dump /dev/snd/hwC1D0 2 settings.bin     (dump segment 2 to a file)
 *         (stop fcp-server first — it holds the hwdep exclusively)
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

#define FLASH_INFO         0x004000
#define FLASH_SEGMENT_INFO 0x004001
#define FLASH_READ         0x004005

/* The mailbox data region caps a single transfer; scarlett2 uses the same 1024 for flash r/w. */
#define FLASH_RW_MAX 1024

/* Segment count sanity bound: scarlett2 accepts 1..5, and the Clarett init trace queries 0..3. */
#define SEGMENT_MAX 8

/* How many times a chunk may be re-read while two consecutive reads disagree. */
static const int read_retries = 8;

/* CLARETT_FLASH_NOVERIFY=1 disables the double read: one read per chunk. Use it to exercise the
 * transport's single-read behaviour, and compare the result against a verified dump. */
static int noverify;

/* Total re-reads a dump needed; a nonzero count means the transport handed back a stale response. */
static unsigned long mismatches;

/*
 * The Clarett's FLASH_INFO reply is {u32 total_size, u32 segment_count, u16, u16}, not the USB
 * layout ({u32 count, u8 unknown[8]}). Confirmed on a 2Pre: total_size 0x3f0000 is exactly the
 * sum of the six segments' sizes. The trailing pair reads 2 and 1000, meaning unknown.
 */
struct flash_info {
	__le32 total_size;
	__le32 count;
	__u8   unknown[4];
} __attribute__((packed));

struct segment_info {
	__le32 size;
	__le32 flags;
	char   name[16];
} __attribute__((packed));

/* One FCP round trip through the hwdep; the driver strips the response header for us. */
static int fcp(int fd, __u32 opcode, const void *req, int req_size, void *resp, int resp_size)
{
	int buf = req_size > resp_size ? req_size : resp_size;
	struct fcp_cmd *cmd = calloc(1, sizeof(*cmd) + buf);
	int err;

	if (!cmd)
		return -1;

	cmd->opcode = opcode;
	cmd->req_size = req_size;
	cmd->resp_size = resp_size;
	if (req_size)
		memcpy(cmd->data, req, req_size);

	err = ioctl(fd, FCP_IOCTL_CMD, cmd);
	if (err < 0)
		fprintf(stderr, "opcode 0x%06x: %s\n", opcode, strerror(errno));
	else if (resp_size)
		memcpy(resp, cmd->data, resp_size);

	free(cmd);
	return err;
}

static __u32 le32(__le32 v)
{
	const __u8 *b = (const __u8 *)&v;

	return b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24;
}

/*
 * Indices at or past the count do not fail — they alias onto the last segment — so the count from
 * FLASH_INFO is the only thing that bounds the enumeration.
 */
static int segment_count(int fd)
{
	struct flash_info info;
	__u32 count;

	if (fcp(fd, FLASH_INFO, NULL, 0, &info, sizeof(info)) < 0)
		return -1;

	count = le32(info.count);
	printf("flash: %u bytes total, %u segments (tail %02x %02x %02x %02x)\n",
	       le32(info.total_size), count,
	       info.unknown[0], info.unknown[1], info.unknown[2], info.unknown[3]);

	if (count < 1 || count > SEGMENT_MAX) {
		fprintf(stderr, "implausible flash segment count %u\n", count);
		return -1;
	}

	return count;
}

/* Segment names are not guaranteed NUL-terminated, and a fresh/odd segment can hold junk. */
static void print_name(FILE *f, const char *name, int len)
{
	for (int i = 0; i < len && name[i]; i++)
		fputc((unsigned char)name[i] >= 0x20 &&
		      (unsigned char)name[i] < 0x7f ? name[i] : '.', f);
}

static int read_segment_info(int fd, int seg, struct segment_info *info)
{
	__le32 req;
	__u8 *b = (__u8 *)&req;

	b[0] = seg; b[1] = seg >> 8; b[2] = seg >> 16; b[3] = seg >> 24;

	return fcp(fd, FLASH_SEGMENT_INFO, &req, sizeof(req), info, sizeof(*info));
}

static int list_segments(int fd, int count)
{
	for (int i = 0; i < count; i++) {
		struct segment_info info;

		/* Probing past the last segment is expected to fail; that is the terminator. */
		if (read_segment_info(fd, i, &info) < 0)
			return i ? 0 : 1;

		printf("  segment %d: size %8u  flags 0x%08x  name \"",
		       i, le32(info.size), le32(info.flags));
		print_name(stdout, info.name, sizeof(info.name));
		printf("\"\n");
	}
	return 0;
}

static int dump_segment(int fd, int seg, const char *path)
{
	struct segment_info info;
	__u32 size, off = 0;
	FILE *out;

	if (read_segment_info(fd, seg, &info) < 0)
		return 1;

	size = le32(info.size);
	if (!size) {
		fprintf(stderr, "segment %d reports zero size\n", seg);
		return 1;
	}

	out = fopen(path, "wb");
	if (!out) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return 1;
	}

	fprintf(stderr, "dumping segment %d (\"", seg);
	print_name(stderr, info.name, sizeof(info.name));
	fprintf(stderr, "\"), %u bytes -> %s\n", size, path);

	while (off < size) {
		__u32 req[3], len = size - off;
		__u8 buf[FLASH_RW_MAX], cmp[FLASH_RW_MAX];
		int tries;

		if (len > FLASH_RW_MAX)
			len = FLASH_RW_MAX;

		req[0] = seg;
		req[1] = off;
		req[2] = len;

		/*
		 * A chunk is believed once two reads of it agree, and the re-reads are counted so a
		 * dump reports whether the transport ever handed back a stale response. The check is
		 * independent of the mailbox, so a nonzero count points at the transport rather than at
		 * the flash.
		 */
		for (tries = 0; tries < read_retries; tries++) {
			if (fcp(fd, FLASH_READ, req, sizeof(req), buf, len) < 0)
				goto failed;
			if (noverify)
				break;
			if (fcp(fd, FLASH_READ, req, sizeof(req), cmp, len) < 0) {
failed:
				fprintf(stderr, "failed at offset %u; %u bytes written\n", off, off);
				fclose(out);
				return 1;
			}
			if (!memcmp(buf, cmp, len))
				break;
			mismatches++;
		}

		if (tries == read_retries) {
			fprintf(stderr,
				"offset %u never read the same twice in %d attempts; aborting\n",
				off, read_retries);
			fclose(out);
			return 1;
		}

		if (fwrite(buf, 1, len, out) != len) {
			fprintf(stderr, "short write to %s\n", path);
			fclose(out);
			return 1;
		}

		off += len;
	}

	if (fclose(out)) {
		fprintf(stderr, "close %s: %s\n", path, strerror(errno));
		return 1;
	}

	fprintf(stderr, "done: %u bytes (%lu re-reads)\n", size, mismatches);
	return 0;
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/snd/hwC1D0";
	int fd, count, ret;

	noverify = getenv("CLARETT_FLASH_NOVERIFY") &&
		   atoi(getenv("CLARETT_FLASH_NOVERIFY"));

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s (fcp-server still running?)\n",
			dev, strerror(errno));
		return 1;
	}

	if (argc > 2) {
		int seg = atoi(argv[2]);

		if (argc < 4) {
			fprintf(stderr, "usage: %s <hwdep> [segment outfile]\n", argv[0]);
			close(fd);
			return 1;
		}
		ret = dump_segment(fd, seg, argv[3]);
	} else {
		count = segment_count(fd);
		ret = count < 0 ? 1 : list_segments(fd, count);
	}

	close(fd);
	return ret;
}
