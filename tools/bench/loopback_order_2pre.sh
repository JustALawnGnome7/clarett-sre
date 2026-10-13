#!/bin/bash
# Clarett 2Pre capture-order and S/MUX check with the digital-loopback ramp. Run as your user:
#   bash loopback_order_2pre.sh <card>
# Playback PCM 3 (feeds no output and no mixer input on a 2Pre) carries a ramp; each leg routes it
# to one router capture destination and checks which ALSA capture channel it arrives on,
# sample-exact, and that the channels S/MUX removes at 96/192 kHz stay zero. Restores each route.
# Needs fcp-server running (the routing controls). Log: ./loopback_order.log
# For another model, change CH, LEGS and dead() below.
set -u
CARD=${1:?usage: $0 <card>}
D=$(dirname "$(readlink -f "$0")")
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
LOG=${BENCH_LOG_DIR:-$PWD}/loopback_order.log
PY="python3 -I $D/ramp_check.py"
CH=14
# leg = router destination : expected ALSA capture channel
LEGS="13:13 14:14 05:5"
# ALSA capture channels the device leaves dead at each rate (after the loopback reorder)
dead() { case $1 in 48000) echo -;; 96000) echo 9,10,11,12;; 192000) echo 7,8,9,10,11,12;; esac; }

echo "== $(date) loopback order card $CARD (srcversion $(cat /sys/module/snd_clarett/srcversion))" | tee -a "$LOG"
for R in 48000 96000 192000; do
	$PY gen "$W/ramp_$R.raw" $((R * 3)) 4 3
	for leg in $LEGS; do
		dst=${leg%:*}; exp=${leg#*:}
		ctl="PCM $dst Capture Enum"
		was=$(amixer -c "$CARD" cget name="$ctl" | sed -n 's/.*: values=//p')
		arecord -q -D hw:$CARD,0 -f S32_LE -c $CH -r $R -t raw -d 6 "$W/cap.raw" &
		rec=$!
		sleep 1
		amixer -q -c "$CARD" cset name="$ctl" 'PCM 3'	# set mid-capture, at this rate
		sleep 0.5
		aplay -q -D hw:$CARD,0 -f S32_LE -c 4 -r $R -t raw "$W/ramp_$R.raw"; prc=$?
		wait $rec; rrc=$?
		amixer -q -c "$CARD" cset name="$ctl" "$was"
		printf -- "-- %6s  PCM %s -> ch %-2s (aplay %s, arecord %s): %s\n" "$R" "$dst" "$exp" "$prc" "$rrc" \
			"$($PY check "$W/cap.raw" $CH "$exp" "$(dead $R)")" | tee -a "$LOG"
	done
done
