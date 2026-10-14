#!/bin/bash
# Capture-order and S/MUX check with the digital-loopback ramp, for any Clarett. Run as your user:
#   bash loopback_order.sh <card>          (RAMP=<n> overrides the playback channel)
# The model comes from the card id. Playback PCM $RAMP carries a ramp; each leg routes it to one
# router capture destination and checks which ALSA capture channel it arrives on, sample-exact,
# and that the channels S/MUX removes at 96/192 kHz stay zero. Whatever the ramp channel already
# feeds (an output, a mixer input) is switched Off for the run and restored at exit, as is each
# leg's route. Needs fcp-server running (the routing controls). Log: ./loopback_order.log
set -u
CARD=${1:?usage: $0 <card>}
D=$(dirname "$(readlink -f "$0")")
W=$(mktemp -d)
LOG=${BENCH_LOG_DIR:-$PWD}/loopback_order.log
PY="python3 -I $D/ramp_check.py"
ID=$(cat /proc/asound/card$CARD/id)

# CH/PB = capture/playback width; LEGS = router destination:expected ALSA capture channel (both
# loopback destinations, then ADAT 1); DEAD96/DEAD192 = ALSA capture channels S/MUX leaves dead,
# after the driver's loopback reorder.
case $ID in
C2Pre)  CH=14 PB=4  R0=3 LEGS="13:13 14:14 05:5"  DEAD96=9,10,11,12 DEAD192=7,8,9,10,11,12;;
C4Pre|C8Pre)
	CH=20 PB=$([ $ID = C4Pre ] && echo 8 || echo 20) R0=7 LEGS="19:19 20:20 11:11"
	DEAD96=15,16,17,18 DEAD192=13,14,15,16,17,18;;
C8PreX) CH=28 PB=28 R0=7 LEGS="27:27 28:28 11:11"
	DEAD96=19,20,21,22,23,24,25,26 DEAD192=15,16,17,18,19,20,21,22,23,24,25,26;;
*) echo "unknown card id $ID"; exit 1;;
esac
RAMP=${RAMP:-$R0}
dead() { case $1 in 48000) echo -;; 96000) echo $DEAD96;; 192000) echo $DEAD192;; esac; }

# controls currently fed by 'PCM $RAMP': switch them Off for the run
amixer -c "$CARD" controls | sed -n "s/.*name='\(.*Enum\)'.*/\1/p" > "$W/enums"
: > "$W/restore"
while read -r n; do
	v=$(amixer -c "$CARD" cget name="$n" | sed -n 's/.*: values=//p' | head -1)
	amixer -c "$CARD" cget name="$n" | grep -q "Item #$v 'PCM $RAMP'$" || continue
	printf '%s\t%s\n' "$n" "$v" >> "$W/restore"
	amixer -q -c "$CARD" cset name="$n" 'Off'
done < "$W/enums"
restore() {
	while IFS=$'\t' read -r n v; do amixer -q -c "$CARD" cset name="$n" "$v"; done < "$W/restore"
	rm -rf "$W"
}
trap restore EXIT

echo "== $(date) loopback order card $CARD $ID, ramp on PCM $RAMP (srcversion $(cat /sys/module/snd_clarett/srcversion))" | tee -a "$LOG"
[ -s "$W/restore" ] && echo "   switched off for the run: $(cut -f1 "$W/restore" | paste -sd, -)" | tee -a "$LOG"
for R in 48000 96000 192000; do
	$PY gen "$W/ramp_$R.raw" $((R * 3)) "$PB" "$RAMP"
	for leg in $LEGS; do
		dst=${leg%:*}; exp=${leg#*:}
		ctl="PCM $dst Capture Enum"
		was=$(amixer -c "$CARD" cget name="$ctl" | sed -n 's/.*: values=//p')
		arecord -q -D hw:$CARD,0 -f S32_LE -c $CH -r $R -t raw -d 6 "$W/cap.raw" &
		rec=$!
		sleep 1
		amixer -q -c "$CARD" cset name="$ctl" "PCM $RAMP"	# set mid-capture, at this rate
		sleep 0.5
		aplay -q -D hw:$CARD,0 -f S32_LE -c "$PB" -r $R -t raw "$W/ramp_$R.raw"; prc=$?
		wait $rec; rrc=$?
		amixer -q -c "$CARD" cset name="$ctl" "$was"
		printf -- "-- %6s  PCM %s -> ch %-2s (aplay %s, arecord %s): %s\n" "$R" "$dst" "$exp" "$prc" "$rrc" \
			"$($PY check "$W/cap.raw" $CH "$exp" "$(dead $R)")" | tee -a "$LOG"
	done
done
