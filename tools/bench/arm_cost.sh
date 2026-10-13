#!/bin/bash
# Mailbox cost of a stream start. Run as root, alsa-scarlett-gui CLOSED:
#   sudo bash arm_cost.sh <card> <bdf> [label] [N]
# N short captures; per start: commands from the first after the open to "engine armed", their
# opcodes, and the time that took. Then N simultaneous capture+playback starts, exit codes only.
# Log: ./arm_cost.log, tagged with the loaded module's srcversion.
set -u
CARD=${1:?usage: $0 <card> <bdf> [label] [N]}; BDF=${2:?usage}; LABEL=${3:-run}; N=${4:-3}
LOG=${BENCH_LOG_DIR:-$PWD}/arm_cost.log
DD=/sys/kernel/debug/dynamic_debug/control
cursor() { journalctl -k -n0 --show-cursor | sed -n 's/^-- cursor: //p'; }
dbg() { for f in "FCP op=" "engine armed" "stream-handshake:"; do echo "format \"$f\" $1" > $DD; done; }
trap 'dbg -p' EXIT
CH=$(journalctl -k -q | grep "snd_clarett $BDF: .*serial" | tail -1 | sed -n 's/.*PCM \([0-9]*\)\/\([0-9]*\)ch.*/\1 \2/p')
[ -n "$CH" ] || { echo "no registration line for $BDF"; exit 1; }
PB=${CH% *}; RC=${CH#* }
echo "== $(date) arm cost card $CARD ($BDF) [$LABEL] srcversion $(cat /sys/module/snd_clarett/srcversion)" | tee -a "$LOG"
dbg +p
for i in $(seq 1 "$N"); do
	sleep 1
	cur=$(cursor)
	arecord -q -D hw:$CARD,0 -f S32_LE -r 48000 -c "$RC" -d 1 /dev/null; rc=$?
	sleep 0.5
	k=$(journalctl -k -q -o short-unix --after-cursor "$cur" | grep "snd_clarett $BDF: ")
	arm=$(grep -n "engine armed" <<<"$k" | head -1 | cut -d: -f1)
	if [ -z "$arm" ]; then echo "-- start $i: no 'engine armed' line (arecord rc $rc)" | tee -a "$LOG"; continue; fi
	pre=$(head -n "$arm" <<<"$k" | grep "FCP op=.*phase=")
	t0=$(head -1 <<<"$pre" | awk '{print $1}'); t1=$(sed -n "${arm}p" <<<"$k" | awk '{print $1}')
	{
		echo "-- start $i (arecord rc $rc): $(grep -c . <<<"$pre") commands before arm, $(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.1f", (b-a)*1000}') ms first command -> armed"
		grep -oE "op=0x[0-9a-f]+" <<<"$pre" | sort | uniq -c | sed 's/^/     /'
		grep -E "never landed|-110" <<<"$k" | sed 's/^/   /'
	} | tee -a "$LOG"
done
dbg -p
echo "-- simultaneous capture+playback starts:" | tee -a "$LOG"
for i in $(seq 1 "$N"); do
	sleep 1
	arecord -q -D hw:$CARD,0 -f S32_LE -r 48000 -c "$RC" -d 3 /dev/null & r=$!
	aplay -q -D hw:$CARD,0 -f S32_LE -r 48000 -c "$PB" -d 3 /dev/zero & p=$!
	wait $r; rr=$?; wait $p; pr=$?
	echo "   $i: arecord rc $rr, aplay rc $pr" | tee -a "$LOG"
done
