#!/bin/bash
# Switch a libvirt VM between the stock QEMU and the trace-enabled build (tools/qemu-trace/build.sh).
#   sudo bash tools/qemu-trace/vm_trace.sh on  [domain]
#   sudo bash tools/qemu-trace/vm_trace.sh off [domain]
#   sudo bash tools/qemu-trace/vm_trace.sh show [domain]
# The VM is the domain argument, else $VM_DOMAIN (keep it through sudo: sudo VM_DOMAIN=... or sudo -E);
# there is no built-in default.
# "on" points <emulator> at $PREFIX/bin/qemu-system-x86_64 and adds, in a <qemu:commandline> block:
#   -trace enable=vfio_region_read -trace enable=vfio_region_write   BAR accesses (log backend, so they
#                                                                     land in /var/log/libvirt/qemu/<domain>.log)
#   -global vfio-pci.x-no-mmap=true                                   BARs not mmap'd, so every access traps;
#                                                                     -global, unlike <qemu:override>, also
#                                                                     reaches a device hot-plugged later
# "off" restores the system emulator and removes the block. Edits the SAVED definition (dumpxml --inactive)
# and takes effect at the VM's next start; the definition before each change is kept in $BACKUP.
# Verify a capture with:  virsh qemu-monitor-command <domain> --pretty \
#   '{"execute":"qom-get","arguments":{"path":"/machine/peripheral/ua-clarett","property":"x-no-mmap"}}'
set -eu
CMD=${1:?usage: $0 on|off|show [domain]}
DOM=${2:-${VM_DOMAIN:?name the libvirt VM: a second argument or VM_DOMAIN=<domain>}}
PREFIX=${PREFIX:-/opt/qemu-trace}
TRACE_QEMU=$PREFIX/bin/qemu-system-x86_64
SYS_QEMU=/usr/bin/qemu-system-x86_64
BACKUP=${BACKUP:-/var/lib/libvirt/qemu-trace-backup}
V="virsh -c qemu:///system"

cur=$(mktemp); new=$(mktemp); trap 'rm -f "$cur" "$new"' EXIT
$V dumpxml --inactive "$DOM" > "$cur"
case $CMD in
show)
	grep -E '<emulator>|qemu:arg' "$cur" || true
	exit 0 ;;
on)
	[ -x "$TRACE_QEMU" ] || { echo "$TRACE_QEMU not installed (tools/qemu-trace/build.sh)"; exit 1; }
	MODE=on ;;
off) MODE=off ;;
*) echo "usage: $0 on|off|show [domain]"; exit 1 ;;
esac
python3 -I - "$cur" "$new" "$MODE" "$TRACE_QEMU" "$SYS_QEMU" <<'EOF'
import re, sys
src, dst, mode, trace_q, sys_q = sys.argv[1:]
x = open(src).read()
NS = "xmlns:qemu='http://libvirt.org/schemas/domain/qemu/1.0'"
x = re.sub(r"\s*<qemu:commandline>.*?</qemu:commandline>", "", x, flags=re.S)  # drop any old block
if mode == "on":
    x = re.sub(r"<emulator>[^<]*</emulator>", f"<emulator>{trace_q}</emulator>", x)
    if "xmlns:qemu=" not in x:
        x = re.sub(r"<domain\b", f"<domain {NS}", x, count=1)
    args = ["-trace", "enable=vfio_region_read", "-trace", "enable=vfio_region_write",
            "-global", "vfio-pci.x-no-mmap=true"]
    block = "  <qemu:commandline>\n" + "".join(f"    <qemu:arg value='{a}'/>\n" for a in args) \
          + "  </qemu:commandline>\n"
    x = x.replace("</domain>", block + "</domain>")
else:
    x = re.sub(r"<emulator>[^<]*</emulator>", f"<emulator>{sys_q}</emulator>", x)
open(dst, "w").write(x)
EOF
mkdir -p "$BACKUP"
B=$BACKUP/$DOM.$(date +%Y%m%d-%H%M%S).xml
cp "$cur" "$B"
diff -u "$cur" "$new" || true
$V define "$new" >/dev/null
echo "defined $DOM (trace $MODE); previous definition saved as $B"
$V domstate "$DOM" | grep -q running && echo "takes effect at the next start: shut the VM down and start it again"
