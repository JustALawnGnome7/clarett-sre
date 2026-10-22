#!/bin/bash
# Build a trace-enabled QEMU for MMIO capture: the SAME source package as the installed Fedora QEMU,
# Fedora's patches applied, built with the "log" trace backend (Fedora ships the DTrace/SystemTap backend,
# so -trace enable=vfio_region_* does nothing with the stock binary). x86_64 system emulator only,
# installed beside the system copy under PREFIX, never over it.
#   bash tools/qemu-trace/build.sh            # download, verify, patch, configure, build (no root)
#   sudo make -C "$WORK/qemu-<ver>/build" install   # printed at the end, with the SELinux steps
# WORK (default ~/.cache/qemu-trace) holds the sources and build, outside the repo.
# Configure features cover what a libvirt Windows VM uses here: SPICE (display, audio, agent, usbredir),
# QXL, qemu-xhci, e1000e, ICH9 HDA, tpm-crb (swtpm), Secure Boot pflash, vfio.
set -eu
PREFIX=${PREFIX:-/opt/qemu-trace}
WORK=${WORK:-$HOME/.cache/qemu-trace}
NVR=$(rpm -q --qf '%{SOURCERPM}' qemu-system-x86-core)	# e.g. qemu-11.1.2-1.fc45.src.rpm
NVR=${NVR%.src.rpm}
mkdir -p "$WORK/srpm"; cd "$WORK/srpm"

if [ ! -f "$NVR.src.rpm" ]; then
	echo "== downloading $NVR.src.rpm"
	dnf download --source --enablerepo=updates-source --enablerepo=updates-testing-source "$NVR"
fi
echo "== verifying the package signature against Fedora's keys"
rpm -K "$NVR.src.rpm" | tee /dev/stderr | grep -q 'signatures OK' || { echo "signature check FAILED"; exit 1; }
rpm2cpio "$NVR.src.rpm" | cpio -idmu --quiet

SPEC=qemu.spec
TARBALL=$(ls qemu-*.tar.xz | head -1)
SRC=$WORK/${TARBALL%.tar.xz}
rm -rf "$SRC"; tar -C "$WORK" -xf "$TARBALL"
echo "== applying Fedora's patches (spec order)"
for p in $(sed -n 's/^Patch[0-9]*:[[:space:]]*//p' "$SPEC"); do
	p=$(basename "$p")
	[ -f "$p" ] || { echo "missing patch $p"; exit 1; }
	echo "   $p"; patch -d "$SRC" -p1 -s < "$p"
done

echo "== configuring"
cd "$SRC"
./configure --prefix="$PREFIX" --target-list=x86_64-softmmu --enable-trace-backends=log \
	--enable-kvm --enable-tpm --enable-spice --enable-spice-protocol --enable-usb-redir --enable-libusb \
	--enable-cap-ng --enable-seccomp --enable-linux-aio --enable-linux-io-uring --enable-fdt=system \
	--enable-numa --disable-docs --disable-werror
echo "== building"
make -j"$(nproc)"

B=$SRC/build/qemu-system-x86_64
$B -trace help 2>/dev/null | grep -qx vfio_region_read || { echo "vfio_region_read trace event MISSING"; exit 1; }
echo
echo "Built $($B --version | head -1), with the vfio_region_* trace events. Next, as root:"
echo "  sudo make -C $SRC/build install"
echo "  sudo semanage fcontext -a -t qemu_exec_t '$PREFIX/bin/qemu-system-x86_64'"
echo "  sudo restorecon -Rv $PREFIX"
