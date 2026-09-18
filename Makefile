# Clarett — userspace install
#
# Installs the per-model FCP maps (devmap + alsa-map) into fcp-server's DATADIR,
# so they no longer have to be copied by hand.
#
# The kernel module is separate: it lives in the snd-clarett submodule; build it with
# `make -C snd-clarett` (see snd-clarett/README.md). This Makefile covers only the userspace data.
#
# Two pieces of userspace data are deliberately NOT here, because they are keyed on
# names the driver registers and so ship with it: the ALSA card config
# (`sudo make -C snd-clarett alsa-install`) and the WirePlumber card-naming drop-in
# (`sudo make -C snd-clarett wireplumber-install`).
#
# PREFIX MUST match the PREFIX fcp-server was built/installed with, because
# fcp-server looks for its maps in $(PREFIX)/share/fcp-server (its compiled-in
# DATADIR). The default matches fcp-support's own default (/usr/local), so
# neither side needs PREFIX spelled out; a mismatch means fcp-server silently
# won't find the Clarett maps.
#
# Installing under BOTH prefixes is worse than picking the wrong one: systemd and
# udev search /usr/local first, so a stale /usr/local install silently shadows a
# fresh /usr one. Uninstall the old prefix before switching.

PREFIX  ?= /usr/local
DESTDIR ?=

FCP_DATADIR := $(DESTDIR)$(PREFIX)/share/fcp-server

# Both halves of each model's map pair; fcp-server loads both from DATADIR.
# Matches every model the generator emits, not just the Clarett line -- the Red 8Line
# has a pair too, and a clarett-* glob left it out of `make install` while looking like
# it had worked.
FCP_MAPS := $(wildcard fcp-server-data/fcp-devmap-*.json) \
            $(wildcard fcp-server-data/fcp-alsa-map-*.json)

.PHONY: help install install-maps uninstall

# Default to help so a bare `make` never runs a root install by accident.
help:
	@echo "Clarett userspace install (PREFIX=$(PREFIX)):"
	@echo "  make install              maps -> $(FCP_DATADIR) (needs root)"
	@echo "  make uninstall            remove what install placed"
	@echo
	@echo "PREFIX must match the fcp-server install PREFIX (both default /usr/local)."
	@echo "Kernel module builds separately: make -C snd-clarett (see snd-clarett/README.md)."
	@echo "ALSA card config (lists the card in apps): sudo make -C snd-clarett alsa-install"
	@echo "Per-model names in PipeWire/GNOME: sudo make -C snd-clarett wireplumber-install"

install: install-maps

install-maps:
	install -d $(FCP_DATADIR)
	install -m 644 $(FCP_MAPS) $(FCP_DATADIR)/

uninstall:
	rm -f $(addprefix $(FCP_DATADIR)/,$(notdir $(FCP_MAPS)))
