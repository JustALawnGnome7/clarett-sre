#!/bin/bash
# cat7_probe.sh <card> — read-only probe of FCP category 7 (0x007000-0x007003).
# Sends only the four opcodes the vendor sends. Stops fcp-server for the card,
# takes the card from PipeWire (profile off, restored after), turns on the
# driver's FCPr trace so the device's true reply size is logged. Streams with
# arecord (capture only, to /dev/null) to put the unit at each rate.
# Run as root. Needs tools/fcp_cmd.c built (FCP_CMD, default $BENCH_LOG_DIR/fcp_cmd).
# Output: $BENCH_LOG_DIR/cat7_<cardid>.log
set -u
card=${1:?usage: cat7_probe.sh <card number>}
hw=/dev/snd/hwC${card}D0
dir=${BENCH_LOG_DIR:-$PWD}
cmd=${FCP_CMD:-$dir/fcp_cmd}
[ -x "$cmd" ] || { echo "build $cmd first: gcc -O2 -o $cmd tools/fcp_cmd.c (or set FCP_CMD)"; exit 1; }
id=$(cat /proc/asound/card${card}/id)
bdf=$(basename "$(readlink -f /sys/class/sound/card${card}/device)")
out=$dir/cat7_${id}.log
dd=/sys/kernel/debug/dynamic_debug/control
user=${SUDO_USER:-$USER}
pa() { sudo -u "$user" XDG_RUNTIME_DIR=/run/user/$(id -u "$user") pactl "$@"; }
pcard=alsa_card.pci-${bdf//[:.]/_}
prof=$(pa list cards 2>/dev/null | awk -v n="Name: $pcard" '$0 ~ n {f=1} f && /Active Profile:/ {print $3; exit}')

pa set-card-profile "$pcard" off 2>/dev/null
systemctl stop "fcp-server@${card}" 2>/dev/null
sleep 1
ch=$(arecord -D hw:$card -f S32_LE -r 48000 --dump-hw-params -d 1 /dev/null 2>&1 \
	| awk '/^CHANNELS:/ {print $2; exit}')
echo 'format "FCPr" +p' > $dd
start=$(date '+%Y-%m-%d %H:%M:%S')
sleep 1

q() { echo "# $*"; $cmd $hw "$@"; }
reads() {	# the set read at each state
	q 0x007000 64
	for b in 00 01 02; do q 0x007001 64 $b; done
}

{
echo "# card $card id $id bdf $bdf capture channels $ch"
echo "## A: request shapes (idle)"
q 0x007000 64
q 0x007000 64 00
q 0x007000 64 00 00
q 0x007000 64 00 00 00 00
q 0x007000 64 01 00 00 00
for b in 00 01 02 03; do q 0x007001 64 $b; done
q 0x007001 64
q 0x007001 64 00 00
q 0x007001 64 00 00 00 00
for op in 0x007002 0x007003; do
	for b in 00 01 02 03; do q $op 1024 $b 00 00 00 40 00; done
	q $op 1024
	q $op 1024 00 00 00 00
	q $op 1024 00 00 00 00 01 00
	q $op 1024 00 00 00 00 00 00
	q $op 1024 00 00 00 00 40 00 00 00
done
echo "## B: after a stream at each rate (idle again)"
for r in 44100 48000 88200 96000 176400 192000; do
	echo "### rate $r: after"
	arecord -q -D hw:$card -c $ch -f S32_LE -r $r -d 1 /dev/null
	reads
	echo "### rate $r: during"
	arecord -q -D hw:$card -c $ch -f S32_LE -r $r -d 4 /dev/null &
	sleep 1.5
	reads
	q 0x007002 1024 00 00 00 00 40 00
	q 0x007003 1024 00 00 00 00 40 00
	wait
done
echo "## C: repeat of idle reads at the end (48 kHz stream last run before)"
arecord -q -D hw:$card -c $ch -f S32_LE -r 48000 -d 1 /dev/null
reads
} > "$out" 2>&1

sleep 1
echo 'format "FCPr" -p' > $dd
echo "## FCPr trace (category 7)" >> "$out"
journalctl -k --since "$start" -o cat | grep "$bdf" \
	| awk '/FCPr op=0x00700/ {print; k=1; next} /FCPr   payload/ && k {print} {k=0}' >> "$out"
systemctl start "fcp-server@${card}" 2>/dev/null
[ -n "$prof" ] && pa set-card-profile "$pcard" "$prof" 2>/dev/null
echo "wrote $out ($(grep -c '^op=' "$out") commands, $(grep -c 'FCPr op=0x00700' "$out") traced)"
