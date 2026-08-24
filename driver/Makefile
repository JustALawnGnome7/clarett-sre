# Out-of-tree build for the Clarett Thunderbolt driver.
#   make                  # build snd-clarett.ko against the running kernel
#   make KDIR=...         # build against a specific kernel tree
#   make modules_install  # install + depmod, so `modprobe snd-clarett` works (needs root)
#   make load             # load in place, pulling in the ALSA modules it needs
#   make dkms-install     # register with DKMS so it rebuilds on every kernel upgrade
#   make dist             # source tarball, the input to the RPM packaging
#   make clean

# Named once, for both halves of this file: the kbuild half turns it into the object
# list, the packaging half into the tarball/DKMS payload. Adding a source file here is
# the only edit either needs.
CLARETT_SRCS := clarett_main.c clarett_mailbox.c clarett_pcm.c clarett_hwdep.c clarett_midi.c
CLARETT_HDRS := clarett.h clarett_fcp_uapi.h

# Kbuild includes this file with `obj` set (along with src, M and KERNELRELEASE); a direct
# `make` has none of them.
#
# Testing KERNELRELEASE instead — the usual idiom, and what this used to do — is wrong here.
# DKMS rewrites the leading `make` of its MAKE[0] into `make -jN KERNELRELEASE=<kver>` and
# invokes the Makefile DIRECTLY, so the KERNELRELEASE test sends it into the kbuild half,
# which defines variables and declares no targets:
#
#     make[1]: *** No targets.  Stop.
#
# `obj` tells the two apart, because only kbuild sets it. Verified by printing all four in
# both invocations.
ifneq ($(obj),)

obj-m := snd-clarett.o
snd-clarett-objs := $(CLARETT_SRCS:.c=.o)

# CLARETT_VERSION is handed down by the outer half below, which reads it out of
# dkms.conf. Anything that invokes kbuild directly (rather than through this file's
# `all` target) won't set it, and a module that quietly reports a version it did not
# come from is worse than one that admits it doesn't know — hence the explicit
# unknown marker rather than a plausible-looking default.
CLARETT_VERSION ?= 0.0.0-unknown
ccflags-y += -DCLARETT_VERSION=\"$(CLARETT_VERSION)\"

else

KDIR ?= /lib/modules/$(shell uname -r)/build

# dkms.conf owns the version (see the comment there). Parse rather than duplicate, so
# there is exactly one place to bump and no way for the compiled-in MODULE_VERSION to
# disagree with the package that shipped it.
PKGNAME := snd-clarett
CLARETT_VERSION := $(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"$$/\1/p' $(CURDIR)/dkms.conf)

# LICENSES/ is a directory (holding the Linux-syscall-note exception that clarett_fcp_uapi.h's
# SPDX tag refers to), so anything consuming DISTFILES has to recurse — hence `cp -r` below.
DISTFILES := $(CLARETT_SRCS) $(CLARETT_HDRS) Makefile dkms.conf \
             LICENSE LICENSES README.md DEVELOPMENT.md
TARBALL   := $(PKGNAME)-$(CLARETT_VERSION).tar.gz
DKMS_SRC  := /usr/src/$(PKGNAME)-$(CLARETT_VERSION)

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

.PHONY: all clean modules_install load unload version dist dkms-install dkms-uninstall

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) CLARETT_VERSION=$(CLARETT_VERSION) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f $(PKGNAME)-*.tar.gz

version:
	@echo $(CLARETT_VERSION)

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

# Source tarball named the way rpmbuild expects (%{name}-%{version}/ prefix inside), so
# packaging/snd-clarett-kmod.spec can consume it unmodified. Only the files needed to
# build and understand the module go in — no build artefacts, no capture logs.
dist:
	@test -n "$(CLARETT_VERSION)" || { echo "no PACKAGE_VERSION in dkms.conf"; exit 1; }
	tar --transform 's,^,$(PKGNAME)-$(CLARETT_VERSION)/,' \
	    --owner=0 --group=0 --numeric-owner --sort=name \
	    -czf $(TARBALL) $(DISTFILES)
	@echo "wrote $(TARBALL)"

# DKMS wants the source under /usr/src/<name>-<version>/ and finds it by that exact name,
# so the copy and the registration both key off CLARETT_VERSION. Needs root.
dkms-install:
	@command -v dkms >/dev/null || \
		{ echo "dkms is not installed (Fedora: dnf install dkms)"; exit 1; }
	install -d $(DKMS_SRC)
	cp -r -t $(DKMS_SRC) $(DISTFILES)
	@# `dkms add` fails rather than no-ops when the version is already registered, and
	@# --force is not portable across dkms 2.x/3.x — so ask first.
	@dkms status -m $(PKGNAME) -v $(CLARETT_VERSION) | grep -q . || \
		dkms add -m $(PKGNAME) -v $(CLARETT_VERSION)
	dkms build -m $(PKGNAME) -v $(CLARETT_VERSION)
	dkms install -m $(PKGNAME) -v $(CLARETT_VERSION) --force
	@echo "installed: modprobe snd-clarett now works, and rebuilds on kernel upgrade"

dkms-uninstall:
	-dkms remove -m $(PKGNAME) -v $(CLARETT_VERSION) --all
	rm -rf $(DKMS_SRC)

endif
