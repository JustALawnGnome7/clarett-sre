#!/bin/bash
# Swap the loaded snd-clarett for another build, for an A/B with the bench scripts. Run as your user
# (asks for sudo):  bash load_build.sh <path/to/snd-clarett.ko | installed> [card id to wait for]
# Takes every Clarett/Red card from PipeWire (profile off, as your user: a root shell cannot reach
# the session's PipeWire), stops fcp-server and alsa-state, reloads the module, switches the
# stream-svc telemetry on (DYNDBG=0 leaves it off), restarts alsa-state, and waits until that card's
# routing controls answer (fcp-server is started by udev). The cards are left with PipeWire's profile
# off; `pactl set-card-profile <card> pro-audio` (or the GUI) hands one back.
set -u
KO=${1:?usage: $0 <snd-clarett.ko|installed> [card id]}
ID=${2:-}
[ "$KO" = installed ] || { KO=$(readlink -f "$KO"); [ -f "$KO" ] || { echo "no module $KO"; exit 1; }; }
sudo -v || exit 1
for c in $(awk '$3 == "Clarett" {print $1}' /proc/asound/cards); do
	bdf=$(basename "$(readlink -f /sys/class/sound/card$c/device)")
	pw=$(pactl list cards short 2>/dev/null | awk -v b="pci-$(tr ':' '_' <<<"$bdf")" '$2 ~ b {print $2}')
	[ -n "$pw" ] && pactl set-card-profile "$pw" off
done
sudo systemctl stop 'fcp-server@*' alsa-state.service 2>/dev/null
sudo rmmod snd_clarett 2>/dev/null
if [ "$KO" = installed ]; then sudo modprobe snd-clarett; else sudo insmod "$KO"; fi || { echo "load failed"; exit 1; }
[ "${DYNDBG:-1}" = 1 ] && echo 'format "stream-svc:" +p' | sudo tee /sys/kernel/debug/dynamic_debug/control >/dev/null
sudo systemctl start alsa-state.service 2>/dev/null
if [ -n "$ID" ]; then
	for i in $(seq 100); do
		c=$(awk -v id="$ID" '$2 == "["id {print $1; exit}' /proc/asound/cards)
		[ -n "$c" ] && amixer -c "$c" controls 2>/dev/null | grep -q 'Capture Enum' && break
		sleep 0.2
	done
	[ -n "$c" ] || { echo "card $ID did not come back"; exit 1; }
	sleep 2
	bdf=$(basename "$(readlink -f /sys/class/sound/card$c/device)")
	pw=$(pactl list cards short 2>/dev/null | awk -v b="pci-$(tr ':' '_' <<<"$bdf")" '$2 ~ b {print $2}')
	[ -n "$pw" ] && pactl set-card-profile "$pw" off
	echo "card $c ($ID), srcversion $(cat /sys/module/snd_clarett/srcversion)"
fi
