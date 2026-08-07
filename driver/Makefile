# Out-of-tree build for the Clarett 8PreX driver.
#   make            # build snd-clarett.ko against the running kernel
#   make KDIR=...    # build against a specific kernel tree
#   make clean

ifneq ($(KERNELRELEASE),)

obj-m := snd-clarett.o
snd-clarett-objs := clarett_main.o clarett_mailbox.o clarett_pcm.o clarett_hwdep.o clarett_midi.o

# clarett_main.c #includes the de-blobbed per-model bring-up headers; Kbuild's auto-dep tracking
# misses regenerated headers, so force a recompile when they change.
$(obj)/clarett_main.o: $(src)/clarett_arm.h $(src)/clarett_arm_2pre.h $(src)/clarett_arm_4pre.h \
                       $(src)/clarett_arm_8pre.h $(src)/clarett_arm_8prex.h

else

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

endif
