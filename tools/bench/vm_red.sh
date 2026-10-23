#!/bin/bash
# Hot-plug a Red/Clarett unit into a RUNNING libvirt VM, or take it back.
#   bash vm_red.sh attach [card-id] [domain]    card-id default R8Line (a Clarett: C8PreX, C8Pre, ...)
#   bash vm_red.sh detach [card-id] [domain]
# The VM is the domain argument, else $VM_DOMAIN; there is no built-in default.
# Live only, never in the persistent domain: Windows does not boot with a Red 8Line attached, but takes
# it fine once it is up. managed='yes' makes libvirt unbind snd_clarett and bind vfio-pci on attach, and
# rebind snd_clarett on detach. The unit's PCI address is looked up each time (it changes on a replug);
# on detach it is read from the domain. Stop fcp-server@<card> first and take the card off PipeWire, so
# nothing holds it when the driver is unbound.
set -eu
CMD=${1:?usage: $0 attach|detach [card-id] [domain]}
ID=${2:-R8Line}
DOM=${3:-${VM_DOMAIN:?name the libvirt VM: a third argument or VM_DOMAIN=<domain>}}
V="virsh -c qemu:///system"
XML=$(mktemp --suffix=.xml); trap 'rm -f "$XML"' EXIT
hostdev() {	# bdf
	local b=${1#0000:}
	cat > "$XML" <<EOF
<hostdev mode='subsystem' type='pci' managed='yes'>
  <source><address domain='0x0000' bus='0x${b:0:2}' slot='0x${b:3:2}' function='0x${b:6:1}'/></source>
  <alias name='ua-clarett'/>
</hostdev>
EOF
}
case $CMD in
attach)
	CARD=$(grep -l "^$ID\$" /proc/asound/card*/id 2>/dev/null | head -1 | sed 's|/proc/asound/card\([0-9]*\)/id|\1|')
	[ -n "$CARD" ] || { echo "no card with id $ID (is it bound to snd_clarett?)"; exit 1; }
	BDF=$(basename "$(readlink -f /sys/class/sound/card$CARD/device)")
	[ "$(cat /sys/bus/pci/devices/$BDF/vendor):$(cat /sys/bus/pci/devices/$BDF/device)" = 0x1cb5:0x0002 ] \
		|| { echo "$BDF is not a 1cb5:0002"; exit 1; }
	if systemctl is-active -q "fcp-server@$CARD"; then
		echo "fcp-server@$CARD is running: sudo systemctl stop fcp-server@$CARD first"; exit 1
	fi
	if fuser -s /dev/snd/*C${CARD}D* 2>/dev/null; then
		echo "card $CARD is still open:"; fuser -v /dev/snd/*C${CARD}D* 2>&1 | tail -n +2
		echo "set its PipeWire profile to Off (pactl / GNOME Sound) and retry"; exit 1
	fi
	echo "attaching card $CARD ($ID) at $BDF to $DOM"
	hostdev "$BDF"
	$V attach-device "$DOM" "$XML" --live
	;;
detach)
	SRC=$($V dumpxml "$DOM" | grep -A3 "<hostdev mode='subsystem' type='pci'" | grep -A1 '<source>' \
		| grep -oE "bus='0x[0-9a-f]+' slot='0x[0-9a-f]+' function='0x[0-9a-f]'" | head -1)
	[ -n "$SRC" ] || { echo "$DOM has no PCI hostdev"; exit 1; }
	eval "$(sed "s/ /; /g" <<<"$SRC")"	# bus= slot= function=
	BDF=$(printf '0000:%02x:%02x.%x' "$bus" "$slot" "$function")
	echo "detaching $BDF from $DOM"
	hostdev "$BDF"
	$V detach-device "$DOM" "$XML" --live
	;;
*) echo "usage: $0 attach|detach [card-id] [domain]"; exit 1 ;;
esac
