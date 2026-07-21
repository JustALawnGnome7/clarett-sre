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
 * This is the driver's only control surface. The device is still armed in-kernel at probe
 * (clarett_arm_device); this just hands the mailbox to userspace afterwards.
 *
 * STATUS: complete ABI — PVERSION + CMD + INIT + SET_METER_MAP/LABELS + the notification
 * read/poll relay. fcp-server can drive this device end-to-end (bench-testing pending).
 */
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <sound/core.h>
#include <sound/control.h>
#include <sound/hwdep.h>
#include <sound/tlv.h>
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
		/* Zeroed: the reply may be shorter than resp_size, and uninitialised kernel memory
		 * must never reach userspace through the gap. */
		data = kzalloc(buf_size, GFP_KERNEL);
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

/*
 * FCP_IOCTL_INIT: the protocol-start handshake. fcp-server calls this once, requires it to
 * succeed, and reads the firmware version from step2[8]; it ignores step0's content.
 *
 * The USB reference (fcp.c fcp_init) does: a STEP0 USB class request -> INIT_1 -> INIT_2, and
 * resets seq to 0. Two adaptations for this Thunderbolt device:
 *
 *  - STEP0 has no mailbox equivalent. On USB it is a vendor class request (FCP_USB_REQ_STEP0),
 *    not an FCP opcode; the device-info it returns (serial, firmware ids) is read from BAR
 *    registers here (REG_SERIAL_lo/hi and REG_INFO at probe), never over the wire. We return
 *    step0 zeroed; fcp-server only inspects step2, so this is honest, not a fabricated layout.
 *
 *  - We do NOT reset c->seq. fcp.c can zero it because fcp-server is the device's only mailbox
 *    user; here the in-kernel GET_METER heartbeat shares c->seq, so a reset would race it. The
 *    device only echoes seq (it does not require a 0 start), and fcp-server never inspects seq,
 *    so continuing the monotonic counter is correct and race-free.
 *
 * BENCH RISK (untested): our probe already armed the device in-kernel, which ran INIT_1/INIT_2
 * once this power cycle. fcp.c's fcp_reinit re-runs them on a live device, so they are meant to
 * be re-runnable, but INIT_1 is command #0 of the vendor session-start and re-issuing it on an
 * already-armed device is the top thing to verify (re-running the *full* arm is known to wedge).
 */
static int clarett_hwdep_init_cmd(struct clarett *c, struct fcp_init __user *arg)
{
	u16 resp_cap = c->resp_size - FCP_RESP_DATA_OFF;
	struct fcp_init init;
	u8 *resp;
	int buf_size, err;

	if (copy_from_user(&init, arg, sizeof(init)))
		return -EFAULT;

	/* Match fcp.c's bounds: each step response is a single byte-counted block. */
	if (init.step0_resp_size < 1 || init.step0_resp_size > 255 ||
	    init.step2_resp_size < 1 || init.step2_resp_size > 255 ||
	    init.step2_resp_size > resp_cap)
		return -EINVAL;

	buf_size = init.step0_resp_size + init.step2_resp_size;
	resp = kzalloc(buf_size, GFP_KERNEL);	/* step0 stays zero (see above) */
	if (!resp)
		return -ENOMEM;

	/* INIT_1: session handshake, no response payload. */
	err = clarett_fcp(c, init.init1_opcode, NULL, 0);
	if (err)
		goto out;

	/* INIT_2: returns the firmware-info block; fcp-server reads fw version at step2[8]. */
	err = clarett_fcp_cmd(c, init.init2_opcode, NULL, 0,
			      resp + init.step0_resp_size, init.step2_resp_size);
	if (err)
		goto out;

	if (copy_to_user(arg->resp, resp, buf_size))
		err = -EFAULT;
out:
	kfree(resp);
	return err;
}

/*** Level Meter (FCP_IOCTL_SET_METER_MAP / _LABELS) ***/

/*
 * fcp-server owns metering on this path: it reads the device's raw meter count, chooses a
 * channel->slot map, and installs both here. The "Level Meter" control is created on the first
 * SET_METER_MAP; its .get polls GET_METER (the same {pad,num_meters,magic=1} request the in-kernel
 * heartbeat uses) and projects the raw levels through the map. SET_METER_LABELS attaches an
 * FCP_CHANNEL_LABELS TLV naming each channel. Mirrors sound/usb/fcp.c's meter control.
 *
 * LEVELS ARE u16 `[HW — 4Pre, July 20 2026]`. This was u32 (48 u32s, inherited from the removed
 * in-kernel meter), which came from reading the request's num_meters=0x30 and assuming an element
 * width — never checked against a reply. Dumping an actual response shows each u32 is two identical
 * u16s: 0x020a020a = {522, 522}, 0x020e020e = {526, 526}. 522/526 are sane idle levels in the
 * 0..4095 range; 34210314 is not. So a u32 read merged two channels and clamped every one of them
 * to full scale — the old control would have pegged at 4095 across the board had anything ever read
 * it on hardware. Still to confirm: whether num_meters counts u16 slots (assumed here) or something
 * else, which a reply-length sweep settles.
 */
static int clarett_hwdep_meter_info(struct snd_kcontrol *kctl,
				    struct snd_ctl_elem_info *ui)
{
	struct clarett *c = kctl->private_data;

	ui->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	ui->count = c->hwdep_meter_channels;
	ui->value.integer.min = 0;
	ui->value.integer.max = CLARETT_METER_MAX;
	ui->value.integer.step = 1;
	return 0;
}

static int clarett_hwdep_meter_get(struct snd_kcontrol *kctl,
				   struct snd_ctl_elem_value *uc)
{
	struct clarett *c = kctl->private_data;
	int i, err, n = c->hwdep_n_meter_slots;
	u8 req[8];			/* {pad:u16=0, num_meters:u16, magic:u32=1} */

	clarett_put_le16(req, 0);
	clarett_put_le16(req + 2, n);
	clarett_put_le32(req + 4, 1);

	mutex_lock(&c->hwdep_lock);
	err = clarett_fcp_cmd(c, FCP_GET_METER, req, sizeof(req),
			      (u8 *)c->hwdep_meter_levels, n * sizeof(__le16));
	if (!err) {
		for (i = 0; i < c->hwdep_meter_channels; i++) {
			int idx = c->hwdep_meter_map[i];
			u32 v = idx < 0 ? 0 : le16_to_cpu(c->hwdep_meter_levels[idx]);

			uc->value.integer.value[i] = min(v, (u32)CLARETT_METER_MAX);
		}
	}
	mutex_unlock(&c->hwdep_lock);
	return err;
}

/* TLV read: hand back the channel-labels blob installed by SET_METER_LABELS. */
static int clarett_hwdep_meter_tlv(struct snd_kcontrol *kctl, int op_flag,
				   unsigned int size, unsigned int __user *tlv)
{
	struct clarett *c = kctl->private_data;
	int ret = 0;

	if (op_flag != SNDRV_CTL_TLV_OP_READ)
		return -EINVAL;

	mutex_lock(&c->hwdep_lock);
	if (c->hwdep_meter_labels_tlv_size) {
		if (size > c->hwdep_meter_labels_tlv_size)
			size = c->hwdep_meter_labels_tlv_size;
		if (copy_to_user(tlv, c->hwdep_meter_labels_tlv, size))
			ret = -EFAULT;
		else
			ret = size;
	}
	mutex_unlock(&c->hwdep_lock);
	return ret;
}

static const struct snd_kcontrol_new clarett_hwdep_meter_tmpl = {
	.iface  = SNDRV_CTL_ELEM_IFACE_PCM,
	.name   = "Level Meter",
	.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	.info   = clarett_hwdep_meter_info,
	.get    = clarett_hwdep_meter_get,
	.tlv    = { .c = clarett_hwdep_meter_tlv },
};

static int clarett_hwdep_validate_map(const s16 *map, int map_size, int slots)
{
	int i;

	for (i = 0; i < map_size; i++)
		if (map[i] < -1 || map[i] >= slots)
			return -EINVAL;
	return 0;
}

/* FCP_IOCTL_SET_METER_MAP: install the channel->slot map, creating the control on first call. */
static int clarett_hwdep_set_meter_map(struct clarett *c, struct fcp_meter_map __user *arg)
{
	u16 resp_cap = c->resp_size - FCP_RESP_DATA_OFF;
	struct fcp_meter_map map;
	s16 *tmp;
	int err;

	if (copy_from_user(&map, arg, sizeof(map)))
		return -EFAULT;

	/* Geometry is frozen once the control exists (fcp.c does the same). */
	if (c->hwdep_meter_ctl &&
	    (map.map_size != c->hwdep_meter_channels ||
	     map.meter_slots != c->hwdep_n_meter_slots))
		return -EINVAL;
	if (map.map_size < 1 || map.map_size > 255 ||
	    map.meter_slots < 1 || map.meter_slots > 255 ||
	    map.meter_slots * sizeof(__le16) > resp_cap)	/* GET_METER response must fit resp_buf */
		return -EINVAL;

	tmp = kmalloc_array(map.map_size, sizeof(s16), GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;
	if (copy_from_user(tmp, arg->map, map.map_size * sizeof(s16))) {
		err = -EFAULT;
		goto out_free;
	}
	err = clarett_hwdep_validate_map(tmp, map.map_size, map.meter_slots);
	if (err)
		goto out_free;

	mutex_lock(&c->hwdep_lock);
	if (!c->hwdep_meter_ctl) {
		s16 *new_map = devm_kmalloc_array(&c->pci->dev, map.map_size,
						  sizeof(s16), GFP_KERNEL);
		__le16 *levels = devm_kmalloc_array(&c->pci->dev, map.meter_slots,
						    sizeof(__le16), GFP_KERNEL);
		struct snd_kcontrol *kctl = NULL;

		if (new_map && levels)
			kctl = snd_ctl_new1(&clarett_hwdep_meter_tmpl, c);
		if (!kctl) {
			devm_kfree(&c->pci->dev, levels);	/* NULL-safe */
			devm_kfree(&c->pci->dev, new_map);
			err = -ENOMEM;
			goto out_unlock;
		}
		/* Geometry must be visible before the control is added (info() reads it). */
		c->hwdep_meter_channels = map.map_size;
		c->hwdep_n_meter_slots = map.meter_slots;
		c->hwdep_meter_map = new_map;
		c->hwdep_meter_levels = levels;
		err = snd_ctl_add(c->card, kctl);	/* frees kctl on failure */
		if (err) {
			c->hwdep_meter_map = NULL;
			c->hwdep_meter_levels = NULL;
			devm_kfree(&c->pci->dev, levels);
			devm_kfree(&c->pci->dev, new_map);
			goto out_unlock;
		}
		c->hwdep_meter_ctl = kctl;
	}
	memcpy(c->hwdep_meter_map, tmp, map.map_size * sizeof(s16));
out_unlock:
	mutex_unlock(&c->hwdep_lock);
out_free:
	kfree(tmp);
	return err;
}

/* FCP_IOCTL_SET_METER_LABELS: attach (or clear, size 0) the channel-name TLV on the meter control. */
static int clarett_hwdep_set_meter_labels(struct clarett *c, struct fcp_meter_labels __user *arg)
{
	struct fcp_meter_labels labels;
	unsigned int *tlv, tlv_size, data_size;
	int err = 0;

	if (copy_from_user(&labels, arg, sizeof(labels)))
		return -EFAULT;

	mutex_lock(&c->hwdep_lock);
	if (!c->hwdep_meter_ctl) {	/* map (hence the control) must be set first */
		err = -EINVAL;
		goto out;
	}

	if (!labels.labels_size) {	/* clear */
		if (c->hwdep_meter_labels_tlv) {
			c->hwdep_meter_ctl->vd[0].access &=
				~(SNDRV_CTL_ELEM_ACCESS_TLV_READ |
				  SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK);
			snd_ctl_notify(c->card, SNDRV_CTL_EVENT_MASK_INFO,
				       &c->hwdep_meter_ctl->id);
			devm_kfree(&c->pci->dev, c->hwdep_meter_labels_tlv);
			c->hwdep_meter_labels_tlv = NULL;
			c->hwdep_meter_labels_tlv_size = 0;
		}
		goto out;
	}

	if (labels.labels_size > 4096) {
		err = -EINVAL;
		goto out;
	}
	data_size = ALIGN(labels.labels_size, sizeof(unsigned int));
	tlv_size = sizeof(unsigned int) * 2 + data_size;	/* type + length words + payload */
	tlv = devm_kzalloc(&c->pci->dev, tlv_size, GFP_KERNEL);
	if (!tlv) {
		err = -ENOMEM;
		goto out;
	}
	tlv[0] = SNDRV_CTL_TLVT_FCP_CHANNEL_LABELS;
	tlv[1] = data_size;
	if (copy_from_user(&tlv[2], arg->labels, labels.labels_size)) {
		devm_kfree(&c->pci->dev, tlv);
		err = -EFAULT;
		goto out;
	}

	if (!c->hwdep_meter_labels_tlv) {	/* first labels: advertise TLV read */
		c->hwdep_meter_ctl->vd[0].access |=
			SNDRV_CTL_ELEM_ACCESS_TLV_READ |
			SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK;
		snd_ctl_notify(c->card, SNDRV_CTL_EVENT_MASK_INFO,
			       &c->hwdep_meter_ctl->id);
	} else {
		devm_kfree(&c->pci->dev, c->hwdep_meter_labels_tlv);
	}
	c->hwdep_meter_labels_tlv = tlv;
	c->hwdep_meter_labels_tlv_size = tlv_size;
out:
	mutex_unlock(&c->hwdep_lock);
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
		return clarett_hwdep_init_cmd(c, argp);
	case FCP_IOCTL_SET_METER_MAP:
		return clarett_hwdep_set_meter_map(c, argp);
	case FCP_IOCTL_SET_METER_LABELS:
		return clarett_hwdep_set_meter_labels(c, argp);
	}
	return -ENOIOCTLCMD;
}

/*** Notification relay ***/

/*
 * Relay a device notification to a waiting fcp-server (called from clarett_notify_work on the
 * hwdep path). fcp-server's read() returns a u32 bitmask and re-reads every control whose
 * devmap "notify-client" mask intersects it.
 *
 * Adaptation: the USB FCP device delivers that precise FCP notification bitmask in its interrupt
 * message; this Thunderbolt device only signals *that* a notification occurred, via the 0x400
 * cause register (ev = the monitor-mask cause bits) — the FCP notification word is not exposed on
 * any surface we can read. We therefore deliver an all-categories event (~0) so fcp-server does a
 * correct, if broad, re-read of all notifiable controls. If a real notification word is ever
 * decoded (e.g. a future capture or a DEVMAP field), carry it through here instead of the wildcard.
 */
/* Coalesced wake: fires ~200 ms after the last notification, so a burst collapses to one wake. */
static void clarett_hwdep_notify_wake(struct work_struct *work)
{
	struct clarett *c = container_of(work, struct clarett, hwdep_notify_dwork.work);

	wake_up_interruptible(&c->hwdep_notify_wait);
}

void clarett_hwdep_notify(struct clarett *c, u32 ev)
{
	/*
	 * The device asserts the 0x400 config-change notification steadily (~30 Hz idle), and we can
	 * only relay a wildcard (~0) since the FCP notification word is not exposed — so every wake
	 * makes fcp-server re-read *every* control. Coalesce: set the event now, but debounce the wake
	 * ~200 ms so a storm of idle notifications becomes one re-read, not thirty. A real change is
	 * delivered within the debounce window — imperceptible for a control refresh.
	 */
	if (!c->hwdep_ready)
		return;
	atomic_or(~0u, &c->hwdep_notify_event);
	mod_delayed_work(system_wq, &c->hwdep_notify_dwork, msecs_to_jiffies(200));
}

/*
 * Stop the relay. MUST run before c is freed: the debounced wake holds a timer pointing at c, and
 * because the device notifies at ~30 Hz idle one is essentially always armed — a surprise removal
 * (the device powered off on its Thunderbolt link) that frees c with the timer live fires
 * wake_up_interruptible() into freed memory. Call after the last thing that can re-arm the dwork
 * (clarett_notify_work), and note the flag also covers probe-error teardowns that never reached
 * clarett_hwdep_init(), where the delayed_work is still all zeroes.
 */
void clarett_hwdep_free(struct clarett *c)
{
	if (!c->hwdep_ready)
		return;
	c->hwdep_ready = false;
	cancel_delayed_work_sync(&c->hwdep_notify_dwork);
}

static long clarett_hwdep_read(struct snd_hwdep *hw, char __user *buf,
			       long count, loff_t *offset)
{
	struct clarett *c = hw->private_data;
	u32 event;
	int err;

	if (count < sizeof(event))
		return -EINVAL;

	/* card->shutdown terminates the wait on removal: snd_card_free() blocks until the last
	 * handle closes, so a reader parked here forever would hang the PCI remove thread. */
	err = wait_event_interruptible(c->hwdep_notify_wait,
				       atomic_read(&c->hwdep_notify_event) ||
				       c->card->shutdown);
	if (err)
		return err;
	if (c->card->shutdown)
		return -ENODEV;

	event = atomic_xchg(&c->hwdep_notify_event, 0);
	if (copy_to_user(buf, &event, sizeof(event)))
		return -EFAULT;

	return sizeof(event);
}

static __poll_t clarett_hwdep_poll(struct snd_hwdep *hw, struct file *file,
				   poll_table *wait)
{
	struct clarett *c = hw->private_data;

	poll_wait(file, &c->hwdep_notify_wait, wait);
	if (c->card->shutdown)
		return EPOLLHUP | EPOLLERR;
	return atomic_read(&c->hwdep_notify_event) ? EPOLLIN | EPOLLRDNORM : 0;
}

int clarett_hwdep_init(struct clarett *c)
{
	struct snd_hwdep *hw;
	int err;

	err = snd_hwdep_new(c->card, "Focusrite Control", 0, &hw);
	if (err < 0)
		return err;

	INIT_DELAYED_WORK(&c->hwdep_notify_dwork, clarett_hwdep_notify_wake);

	/* fcp.c leaves iface at the default and fcp-server opens by device, not iface. */
	hw->private_data = c;
	hw->exclusive = 1;
	hw->ops.open = clarett_hwdep_open;
	hw->ops.ioctl = clarett_hwdep_ioctl;
	hw->ops.ioctl_compat = clarett_hwdep_ioctl;
	hw->ops.read = clarett_hwdep_read;
	hw->ops.poll = clarett_hwdep_poll;

	c->hwdep_ready = true;		/* dwork is live from here; clarett_hwdep_free() must run */

	dev_info(&c->pci->dev,
		 "FCP hwdep created (fcp-server transport; PVERSION+CMD+INIT+METER+notify)\n");
	return 0;
}
