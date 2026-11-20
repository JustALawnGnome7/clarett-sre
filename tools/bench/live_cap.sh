#!/bin/bash
# Live capture position: a client that reads as soon as frames appear (livecap, snd_pcm_avail()
# every 250 us) must still get the digital-loopback ramp sample-exact, i.e. a frame the driver
# reports is already in memory and nothing stale from the ring's previous lap is handed over.
# Run as your user:  bash live_cap.sh <card>          (LEGS="rate:period ..." overrides the legs)
# Each leg: livecap at the leg's period with buffer $BUF (default 4 periods), aplay playing a 3 s ramp
# on the ramp channel at the same period. PASS = sample-exact; the livecap line says how many reads
# were shorter than a period (the point of the test: many, down to 16 frames). A leg whose livecap
# reports xruns broke the ramp by recovering, so read it against that window's stream-svc gapmax
# (the ASRock's 42 ms stall), not as a position fault. PipeWire's profile is set off, whatever the
# ramp channel feeds is switched off, and the loopback route set; all restored at exit. The ramp is
# generated to a file first (in $W, up to ~150 MB at 192 kHz), and aplay's underruns are logged.
# Needs fcp-server (routing controls) and livecap built at $BENCH_LOG_DIR/livecap
# (gcc -O2 -o livecap livecap.c -lasound). Log: ./live_cap.log
set -u
CARD=${1:?usage: $0 <card>}
D=$(dirname "$(readlink -f "$0")")
DIR=${BENCH_LOG_DIR:-$PWD}
LOG=$DIR/live_cap.log
LC=$DIR/livecap
PY="python3 -I $D/duplex_ramp.py"
W=$(mktemp -d)
[ -e /proc/asound/card$CARD/id ] || { echo "no card $CARD (cat /proc/asound/cards)"; exit 1; }
ID=$(cat /proc/asound/card$CARD/id)
[ -x "$LC" ] || { echo "build $LC first: gcc -O2 -o $LC $D/livecap.c -lasound"; exit 1; }

# CH/PB = capture/playback width, R0 = ramp playback channel (feeds no output by default),
# DST:EXP = loopback capture destination and the ALSA capture channel it arrives on.
case $ID in
C2Pre)        CH=14 PB=4  R0=3  DST=13 EXP=13 L0="48000:1024 48000:256 48000:64 96000:1024 192000:1024 192000:256";;
C4Pre)        CH=20 PB=8  R0=7  DST=19 EXP=19 L0="48000:1024 48000:256 48000:64 96000:1024 192000:1024 192000:256";;
C8Pre)        CH=20 PB=20 R0=7  DST=19 EXP=19 L0="48000:1024 48000:256 48000:64 96000:1024 192000:1024 192000:256";;
C8PreX)       CH=28 PB=28 R0=7  DST=27 EXP=27 L0="48000:1024 48000:256 48000:64 96000:1024 192000:1024 192000:256";;
# 31 -> 31 survives every speed's S/MUX tail; the Red's ring ceiling is 2048 frames (4 x 512)
R8Line|R4Pre) CH=60 PB=64 R0=31 DST=31 EXP=31 L0="48000:512 48000:128 48000:64 96000:512 96000:128 192000:512 192000:128";;
*) echo "unknown card id $ID"; exit 1;;
esac
RAMP=${RAMP:-$R0}
LEGS=${LEGS:-$L0}

PWCARD=$(pactl list cards short 2>/dev/null | awk -v b="pci-$(basename "$(readlink -f /sys/class/sound/card$CARD/device)" | tr ":" "_")" \
	'$2 ~ b {print $2}')
: > "$W/restore"
if [ -n "$PWCARD" ]; then
	PWPROF=$(pactl list cards | awk -v n="$PWCARD" '$1=="Name:"{c=($2==n)} c&&/Active Profile:/{print $3; exit}')
	pactl set-card-profile "$PWCARD" off
fi
amixer -c "$CARD" controls | sed -n "s/.*name='\(.*Enum\)'.*/\1/p" > "$W/enums"
while read -r n; do
	v=$(amixer -c "$CARD" cget name="$n" | sed -n 's/.*: values=//p' | head -1)
	amixer -c "$CARD" cget name="$n" | grep -q "Item #$v 'PCM $RAMP'$" || continue
	printf '%s\t%s\n' "$n" "$v" >> "$W/restore"
	amixer -q -c "$CARD" cset name="$n" 'Off'
done < "$W/enums"
CTL="PCM $DST Capture Enum"
printf '%s\t%s\n' "$CTL" "$(amixer -c "$CARD" cget name="$CTL" | sed -n 's/.*: values=//p')" >> "$W/restore"
amixer -q -c "$CARD" cset name="$CTL" "PCM $RAMP"
restore() {
	while IFS=$'\t' read -r n v; do amixer -q -c "$CARD" cset name="$n" "$v"; done < "$W/restore"
	[ -n "$PWCARD" ] && pactl set-card-profile "$PWCARD" "$PWPROF"
	rm -rf "$W"
}
trap restore EXIT

echo "== $(date) live capture position card $CARD $ID, ramp PCM $RAMP -> ch $EXP (srcversion $(cat /sys/module/snd_clarett/srcversion))" | tee -a "$LOG"
for leg in $LEGS; do
	r=${leg%:*} p=${leg#*:} b=${BUF:-$(( ${leg#*:} * 4 ))}
	# from a file: fed live through a pipe, a pause in the generator
	# underruns aplay at the short legs (a 2.67 ms buffer at 192k/128)
	$PY gen $((r * 3)) $PB $RAMP > "$W/ramp.raw"
	"$LC" hw:$CARD,0 $r $CH $p 5 $b 2>"$W/lc.err" | $PY check $CH $EXP $((r * 3)) > "$W/lc.res" &
	sleep 0.6
	aplay -q -D hw:$CARD,0 -t raw -f S32_LE -r $r -c $PB --period-size=$p --buffer-size=$b \
		"$W/ramp.raw" 2>"$W/play.err"
	wait
	ur=$(grep -c underrun "$W/play.err")
	echo "-- $r Hz period $p buffer $b: $(cat "$W/lc.res") | $(cat "$W/lc.err") | aplay underruns $ur" | tee -a "$LOG"
done
