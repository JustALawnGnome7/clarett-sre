#!/bin/bash
# Mailbox phase check. Run as root, alsa-scarlett-gui open:  sudo bash phase_check.sh <card> <bdf>
#  1. 3 s of per-command mailbox trace: phase bits per command, timeouts, never-landed.
#  2. A 25 s capture while the monitor knob is turned: events relayed mid-stream.
#  3. The same capture hands-off: should relay nothing.
# Log: ./phase_check.log
set -u
CARD=${1:?usage: $0 <card> <bdf>}; BDF=${2:?usage: $0 <card> <bdf>}
LOG=${BENCH_LOG_DIR:-$PWD}/phase_check.log
DD=/sys/kernel/debug/dynamic_debug/control
cursor() { journalctl -k -n0 --show-cursor | sed -n 's/^-- cursor: //p'; }
dbg_off() {
	echo 'format "FCP op=" -p' > $DD
	echo 'format "async notification handled" -p' > $DD
	echo 'format "stream-svc:" -p' > $DD
}
trap 'dbg_off; kill ${REC:-} 2>/dev/null' EXIT
CH=$(journalctl -k -q | grep "snd_clarett $BDF: .*serial" | tail -1 | sed -n 's/.*PCM [0-9]*\/\([0-9]*\)ch.*/\1/p')
[ -n "$CH" ] || { echo "no registration line for $BDF"; exit 1; }

echo "== $(date) phase check card $CARD ($BDF, ${CH}ch)" | tee -a "$LOG"

cur=$(cursor)
echo 'format "FCP op=" +p' > $DD
sleep 3
echo 'format "FCP op=" -p' > $DD
k=$(journalctl -k -q --after-cursor "$cur" | grep "snd_clarett $BDF: ")
l=$(grep "FCP op=.*phase=" <<<"$k")
{
	echo "-- 1. $(grep -c . <<<"$l") commands in 3 s; by opcode / done / phase:"
	grep -oE "op=0x[0-9a-f]+|done=[01]|phase=0x[0-9a-f]+" <<<"$l" | paste - - - | sort | uniq -c | sed 's/^/   /'
	echo "   never-landed: $(grep -c "never landed" <<<"$k")   timeouts/errors: $(grep -cE -- "-110|timed out" <<<"$k")"
} | tee -a "$LOG"

capture() {	# $1 = label
	cur=$(cursor)
	echo 'format "async notification handled" +p' > $DD
	echo 'format "stream-svc:" +p' > $DD
	arecord -q -D hw:$CARD,0 -f S32_LE -r 48000 -c "$CH" -d 25 /dev/null &
	REC=$!
	wait $REC; rc=$?
	REC=
	sleep 1
	echo 'format "async notification handled" -p' > $DD
	echo 'format "stream-svc:" -p' > $DD
	k=$(journalctl -k -q --after-cursor "$cur" | grep "snd_clarett $BDF: ")
	{
		echo "-- $1: arecord exit $rc; relayed events:"
		grep -o "handled: 0x[0-9a-f]*" <<<"$k" | sort | uniq -c | sed 's/^/   /'
		echo "   stream-svc windows: $(grep -c 'stream-svc: periods' <<<"$k"); anomalous (late/overrun/badreads > 0):"
		grep "stream-svc: periods" <<<"$k" | grep -E "late=[1-9]|overrun=[1-9]|badreads=[1-9]" | sed 's/^.*stream-svc:/   stream-svc:/' | head -5
		grep "stream-svc: stopped" <<<"$k" | sed 's/^.*stream-svc:/   stream-svc:/'
	} | tee -a "$LOG"
}

echo
echo ">>> Part 2: a 25 s capture starts now. TURN THE MONITOR KNOB for ~15 s, then leave it."
echo "    Watch alsa-scarlett-gui: Monitor volume and the HW-controlled outputs should follow."
capture "2. knob"
echo
read -r -p ">>> Part 3: hands off the unit for 25 s. Press Enter to start. " _
capture "3. hands-off"
