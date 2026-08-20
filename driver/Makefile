# Out-of-tree build for the Clarett 8PreX driver.
#   make            # build snd-clarett.ko against the running kernel
#   make KDIR=...    # build against a specific kernel tree
#   make clean

ifneq ($(KERNELRELEASE),)

obj-m := snd-clarett.o
snd-clarett-objs := clarett_main.o clarett_mailbox.o clarett_pcm.o clarett_hwdep.o clarett_midi.o

else

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

endif
