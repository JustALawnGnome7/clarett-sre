# Clarett — userspace install
#
# Installs the device-specific userspace artifacts that fcp-server and
# WirePlumber consume at runtime, so they no longer have to be copied by hand:
#   - the per-model FCP maps (devmap + alsa-map)  -> fcp-server's DATADIR
#   - the WirePlumber card-naming drop-in         -> WirePlumber's conf.d
#
# The kernel module is separate: build it with `make -C driver` and load it with
# insmod (see driver/README.md). This Makefile covers only the userspace data.
#
# PREFIX MUST match the PREFIX fcp-server was built/installed with, because
# fcp-server looks for its maps in $(PREFIX)/share/fcp-server (its compiled-in
# DATADIR). fcp-support is installed with PREFIX=/usr, so default to that; a
# mismatch means fcp-server silently won't find the Clarett maps.

PREFIX  ?= /usr
DESTDIR ?=

FCP_DATADIR := $(DESTDIR)$(PREFIX)/share/fcp-server
WP_CONFDIR  := $(DESTDIR)$(PREFIX)/share/wireplumber/wireplumber.conf.d

# Both halves of each model's map pair; fcp-server loads both from DATADIR.
CLARETT_MAPS := $(wildcard fcp-server-data/fcp-devmap-clarett-*.json) \
                $(wildcard fcp-server-data/fcp-alsa-map-clarett-*.json)
WP_DROPIN    := wireplumber/51-clarett-naming.conf

.PHONY: help install install-maps install-wireplumber uninstall

# Default to help so a bare `make` never runs a root install by accident.
help:
	@echo "Clarett userspace install (PREFIX=$(PREFIX)):"
	@echo "  make install              maps + WirePlumber drop-in (needs root under /usr)"
	@echo "  make install-maps         maps    -> $(FCP_DATADIR)"
	@echo "  make install-wireplumber  drop-in -> $(WP_CONFDIR)"
	@echo "  make uninstall            remove what install placed"
	@echo
	@echo "PREFIX must match the fcp-server install PREFIX (default /usr)."
	@echo "Kernel module builds separately: make -C driver (see driver/README.md)."

install: install-maps install-wireplumber

install-maps:
	install -d $(FCP_DATADIR)
	install -m 644 $(CLARETT_MAPS) $(FCP_DATADIR)/

install-wireplumber:
	install -D -m 644 $(WP_DROPIN) $(WP_CONFDIR)/$(notdir $(WP_DROPIN))
	@echo "Restart WirePlumber to apply: systemctl --user restart wireplumber"

uninstall:
	rm -f $(addprefix $(FCP_DATADIR)/,$(notdir $(CLARETT_MAPS)))
	rm -f $(WP_CONFDIR)/$(notdir $(WP_DROPIN))
