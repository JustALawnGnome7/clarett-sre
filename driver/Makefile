# Out-of-tree build for the Clarett 8PreX driver.
#   make            # build snd-clarett.ko against the running kernel
#   make KDIR=...    # build against a specific kernel tree
#   make clean

ifneq ($(KERNELRELEASE),)

obj-m := snd-clarett.o
snd-clarett-objs := clarett_main.o clarett_mailbox.o clarett_mixer.o

# clarett_main.c #includes the generated clarett_init_seq.h; Kbuild's auto-dep tracking
# misses regenerated headers, so force a recompile when it changes.
$(obj)/clarett_main.o: $(src)/clarett_init_seq.h

else

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

endif
