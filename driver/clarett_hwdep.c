// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clarett FCP hwdep — the transport seam for Geoffrey Bennett's user-space `fcp-server`.
 *
 * Presents the same hwdep ABI as the mainline USB FCP driver (sound/usb/fcp.c), so the unmodified
 * fcp-server can drive this Thunderbolt device: the kernel is a thin transport (relay FCP commands
 * to the mailbox, expose the level meter) and userspace implements the mixer/routing/metering.
 * The FCP wire packet is our mailbox packet exactly (opcode/size/seq/error/pad/data) and the FCP
 * opcodes are our opcodes, so FCP_IOCTL_CMD maps straight onto clarett_fcp_cmd().
 *
 * Created only when in_kernel_controls=0 (mutually exclusive with the in-kernel control set, which
 * would otherwise contend for c->seq and the mailbox). The device is still armed in-kernel at probe
 * (clarett_arm_device); this just hands the mailbox to userspace afterwards.
 *
 * STATUS: low-risk core (PVERSION + CMD). INIT (the real INIT_1/INIT_2 handshake + seq sync),
 * SET_METER_MAP/LABELS (drive the level meter), and the notification read/poll relay are TODO.
 */
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/hwdep.h>
#include "clarett.h"
#include "clarett_fcp_uapi.h"

/* Near-direct device access, matching fcp.c. */
static int clarett_hwdep_open(struct snd_hwdep *hw, struct file *file)
{
	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	return 0;
}

/* FCP_IOCTL_CMD: relay one FCP command to the mailbox and return its response payload. */
static int clarett_hwdep_cmd(struct clarett *c, struct fcp_cmd __user *arg)
{
	u16 resp_cap = c->resp_size - FCP_RESP_DATA_OFF;	/* response payload space in resp_buf */
	struct fcp_cmd cmd;
	void *data = NULL;
	int buf_size, err = 0;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	if (cmd.req_size > CLARETT_MBOX_DATA_MAX || cmd.resp_size > resp_cap)
		return -EINVAL;

	/* TODO: validate opcode (fcp.c refuses flash erase/write of the golden firmware segment). */

	buf_size = max(cmd.req_size, cmd.resp_size);
	if (buf_size) {
		data = kmalloc(buf_size, GFP_KERNEL);
		if (!data)
			return -ENOMEM;
	}
	if (cmd.req_size && copy_from_user(data, arg->data, cmd.req_size)) {
		err = -EFAULT;
		goto out;
	}

	err = clarett_fcp_cmd(c, cmd.opcode, data, cmd.req_size, data, cmd.resp_size);
	if (err)
		goto out;

	if (cmd.resp_size && copy_to_user(arg->data, data, cmd.resp_size))
		err = -EFAULT;
out:
	kfree(data);
	return err;
}

static int clarett_hwdep_ioctl(struct snd_hwdep *hw, struct file *file,
			       unsigned int cmd, unsigned long arg)
{
	struct clarett *c = hw->private_data;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case FCP_IOCTL_PVERSION:
		return put_user(FCP_HWDEP_VERSION, (int __user *)argp);
	case FCP_IOCTL_CMD:
		return clarett_hwdep_cmd(c, argp);
	case FCP_IOCTL_INIT:
	case FCP_IOCTL_SET_METER_MAP:
	case FCP_IOCTL_SET_METER_LABELS:
		return -EOPNOTSUPP;	/* TODO: next increment */
	}
	return -ENOIOCTLCMD;
}

int clarett_hwdep_init(struct clarett *c)
{
	struct snd_hwdep *hw;
	int err;

	err = snd_hwdep_new(c->card, "Focusrite Control", 0, &hw);
	if (err < 0)
		return err;

	/* fcp.c leaves iface at the default and fcp-server opens by device, not iface. */
	hw->private_data = c;
	hw->exclusive = 1;
	hw->ops.open = clarett_hwdep_open;
	hw->ops.ioctl = clarett_hwdep_ioctl;
	hw->ops.ioctl_compat = clarett_hwdep_ioctl;

	dev_info(&c->pci->dev, "FCP hwdep created (fcp-server transport; core ioctls PVERSION+CMD)\n");
	return 0;
}
