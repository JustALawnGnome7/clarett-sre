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
WP_CONFDIR  := $(DESTDIR)$(PREFIX)/share/wireplumber/wireplumber.conf.d

# Both halves of each model's map pair; fcp-server loads both from DATADIR.
# Matches every model the generator emits, not just the Clarett line -- the Red 8Line
# has a pair too, and a clarett-* glob left it out of `make install` while looking like
# it had worked.
FCP_MAPS := $(wildcard fcp-server-data/fcp-devmap-*.json) \
            $(wildcard fcp-server-data/fcp-alsa-map-*.json)
WP_DROPIN    := wireplumber/51-clarett-naming.conf
# The drop-in is generated from the driver's clarett_model table, so its per-model rules
# cannot drift from the card names the driver registers. Needs driver/ present.
GEN_WP       := tools/gen_wireplumber_conf.py

.PHONY: help install install-maps install-wireplumber uninstall \
        wireplumber-conf check-wireplumber-conf

# Default to help so a bare `make` never runs a root install by accident.
help:
	@echo "Clarett userspace install (PREFIX=$(PREFIX)):"
	@echo "  make install              maps + WirePlumber drop-in (needs root)"
	@echo "  make install-maps         maps    -> $(FCP_DATADIR)"
	@echo "  make install-wireplumber  drop-in -> $(WP_CONFDIR)"
	@echo "  make uninstall            remove what install placed"
	@echo
	@echo "  make wireplumber-conf        regenerate the drop-in from the model table"
	@echo "  make check-wireplumber-conf  fail if the drop-in is stale (CI)"
	@echo
	@echo "PREFIX must match the fcp-server install PREFIX (both default /usr/local)."
	@echo "Kernel module builds separately: make -C driver (see driver/README.md)."

install: install-maps install-wireplumber

install-maps:
	install -d $(FCP_DATADIR)
	install -m 644 $(FCP_MAPS) $(FCP_DATADIR)/

install-wireplumber:
	install -D -m 644 $(WP_DROPIN) $(WP_CONFDIR)/$(notdir $(WP_DROPIN))
	@echo "Restart WirePlumber to apply: systemctl --user restart wireplumber"

uninstall:
	rm -f $(addprefix $(FCP_DATADIR)/,$(notdir $(FCP_MAPS)))
	rm -f $(WP_CONFDIR)/$(notdir $(WP_DROPIN))

# Deliberately not a prerequisite of install-wireplumber: a package or a data-only
# install may not have driver/ checked out, and that should not block the install.
wireplumber-conf:
	python3 $(GEN_WP)

check-wireplumber-conf:
	python3 $(GEN_WP) --check
