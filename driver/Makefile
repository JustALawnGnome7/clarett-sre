# Out-of-tree build for the Clarett Thunderbolt driver.
#   make                  # build snd-clarett.ko against the running kernel
#   make KDIR=...         # build against a specific kernel tree
#   make modules_install  # install + depmod, so `modprobe snd-clarett` works (needs root)
#   make load             # load in place, pulling in the ALSA modules it needs
#   make clean

ifneq ($(KERNELRELEASE),)

obj-m := snd-clarett.o
snd-clarett-objs := clarett_main.o clarett_mailbox.o clarett_pcm.o clarett_hwdep.o clarett_midi.o

else

KDIR ?= /lib/modules/$(shell uname -r)/build

# The ALSA cores this module links against. insmod does NOT resolve dependencies — it loads
# exactly the file named — so loading the .ko in place needs these resident first. snd-pcm and
# snd-hwdep are usually already up (any onboard HD-Audio card pulls them in), but snd-rawmidi
# is only loaded when something needs MIDI, so a machine with no MIDI device fails with
# "Unknown symbol snd_rawmidi_*". Either use `make load`, or install and use modprobe.
ALSA_DEPS := snd-pcm snd-hwdep snd-rawmidi

.PHONY: all clean modules_install load unload

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

modules_install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
	depmod -a

# Deliberately NOT dependent on `all`: this is run under sudo, and building as root would leave
# root-owned objects behind and break the next unprivileged `make`. Build first, then load.
load:
	@test -f $(CURDIR)/snd-clarett.ko || \
		{ echo "snd-clarett.ko not built — run 'make' first (as yourself, not root)"; exit 1; }
	modprobe $(ALSA_DEPS)
	insmod $(CURDIR)/snd-clarett.ko $(ARGS)

unload:
	rmmod snd_clarett

endif
