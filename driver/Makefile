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

# insmod does NOT resolve dependencies — it loads exactly the file named — so loading the .ko
# in place needs the modules it links against resident first. snd-pcm and snd-hwdep are usually
# already up (any onboard HD-Audio card pulls them in), but snd-rawmidi is only loaded once
# something needs MIDI, so a machine with no MIDI device fails with "Unknown symbol
# snd_rawmidi_*". Read the list out of the built module rather than keeping a second copy here,
# so it stays right when the module gains or drops a dependency.
#
# NOTE the -a on the modprobe below is REQUIRED, not decorative: modprobe's synopsis is
# `modprobe [modulename] [module parameters...]`, so without it only the FIRST name is treated
# as a module and the rest are passed to it as parameters. That fails silently — unknown module
# parameters are ignored with a warning rather than rejected — so it exits 0 having loaded one
# module, and the missing one only surfaces later as "Unknown symbol" from insmod.
MODDEPS = $(shell modinfo -F depends $(CURDIR)/snd-clarett.ko 2>/dev/null | tr ',' ' ')

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
	$(if $(strip $(MODDEPS)),modprobe -a $(MODDEPS))
	insmod $(CURDIR)/snd-clarett.ko $(ARGS)

unload:
	rmmod snd_clarett

endif
