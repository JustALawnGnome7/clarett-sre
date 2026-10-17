#!/bin/bash
# Analogue round-trip latency (retest item 5). Needs a cable from Analogue Output $OUT to Analogue input
# $OUT (default 1). Run as your user:  bash rt_latency.sh <card> [legs]
# For the run: PipeWire's profile off; Analogue Output $OUT fed straight from playback PCM $OUT, on software
# volume at 0 dB (so the front-panel knob does not decide the level); capture PCM $OUT from Analogue $OUT;
# the digital-loopback reference PCM $REF -> capture "PCM $DST"; every other route from those two playback
# channels Off. All restored at exit. Each leg runs rtlat (linked duplex, playback full) and prints the
# click offsets per click: analogue = output -> cable -> input, digital = inside the device. legs =
# "rate:period ..." (buffer 4 periods), default 48/96/192 kHz at a 1.33 ms and a 5.33 ms period.
# Needs rtlat built at $BENCH_LOG_DIR/rtlat (gcc -O2 -o rtlat rtlat.c -lasound -lm) and fcp-server.
# Log: ./rt_latency.log
set -u
CARD=${1:?usage: $0 <card> [legs]}
LEGS=${2:-"48000:64 48000:256 96000:128 96000:512 192000:256 192000:1024"}
DIR=${BENCH_LOG_DIR:-$PWD}
LOG=$DIR/rt_latency.log
RT=$DIR/rtlat
OUT=${OUT:-1}
[ -x "$RT" ] || { echo "build $RT first: gcc -O2 -o $RT $(dirname "$(readlink -f "$0")")/rtlat.c -lasound -lm"; exit 1; }
[ -e /proc/asound/card$CARD/id ] || { echo "no card $CARD (cat /proc/asound/cards)"; exit 1; }
ID=$(cat /proc/asound/card$CARD/id)
case $ID in
C2Pre)   CH=14 PB=4  REF=3 DST=13 EXP=13;;
C4Pre)   CH=20 PB=8  REF=7 DST=19 EXP=19;;
C8Pre)   CH=20 PB=20 REF=7 DST=19 EXP=19;;
C8PreX)  CH=28 PB=28 REF=7 DST=27 EXP=27;;
*) echo "unsupported card id $ID"; exit 1;;
esac
W=$(mktemp -d)
PWCARD=$(pactl list cards short 2>/dev/null | awk -v b="pci-$(basename "$(readlink -f /sys/class/sound/card$CARD/device)" | tr ":" "_")" \
	'$2 ~ b {print $2}')
: > "$W/restore"
save() { printf '%s\t%s\n' "$1" "$(amixer -c "$CARD" cget name="$1" | sed -n 's/.*: values=//p' | head -1)" >> "$W/restore"; }
if [ -n "$PWCARD" ]; then
	PWPROF=$(pactl list cards | awk -v n="$PWCARD" '$1=="Name:"{c=($2==n)} c&&/Active Profile:/{print $3; exit}')
	pactl set-card-profile "$PWCARD" off
fi
amixer -c "$CARD" controls | sed -n "s/.*name='\(.*Enum\)'.*/\1/p" > "$W/enums"
while read -r n; do
	v=$(amixer -c "$CARD" cget name="$n" | sed -n 's/.*: values=//p' | head -1)
	amixer -c "$CARD" cget name="$n" | grep -qE "Item #$v 'PCM ($OUT|$REF)'$" || continue
	save "$n"; amixer -q -c "$CARD" cset name="$n" 'Off'
done < "$W/enums"
for n in "Analogue Output $OUT Playback Enum" "Line Out $OUT Volume Control Playback Enum" "Line $OUT Playback Volume" \
	 "PCM $(printf %02d $OUT) Capture Enum" "PCM $DST Capture Enum"; do save "$n"; done
amixer -q -c "$CARD" cset name="Line Out $OUT Volume Control Playback Enum" off
amixer -q -c "$CARD" cset name="Line $OUT Playback Volume" 127
amixer -q -c "$CARD" cset name="Analogue Output $OUT Playback Enum" "PCM $OUT"
amixer -q -c "$CARD" cset name="PCM $(printf %02d $OUT) Capture Enum" "Analogue $OUT"
amixer -q -c "$CARD" cset name="PCM $DST Capture Enum" "PCM $REF"
restore() {
	tac "$W/restore" | while IFS=$'\t' read -r n v; do amixer -q -c "$CARD" cset name="$n" "$v"; done
	[ -n "$PWCARD" ] && pactl set-card-profile "$PWCARD" "$PWPROF"
	rm -rf "$W"
}
trap restore EXIT

say() { echo "$*" | tee -a "$LOG"; }
say "== $(date) round-trip latency card $CARD $ID: Analogue Out $OUT -> cable -> Analogue In $OUT (capture ch $OUT); digital reference PCM $REF -> ch $EXP (srcversion $(cat /sys/module/snd_clarett/srcversion))"
sleep 0.5
for l in $LEGS; do
	R=${l%:*} P=${l#*:}
	res=$("$RT" hw:$CARD,0 $PB $CH $R $P $(( 4 * P )) $OUT $OUT $REF $EXP 8 2>&1)
	a=$(sed -n 's/.*analogue \(-\{0,1\}[0-9]*\) frames.*/\1/p' <<<"$res" | sort -n | uniq -c | awk '{printf "%s%sx%s", s, $1, $2; s=" "}')
	d=$(sed -n 's/.*digital \(-\{0,1\}[0-9]*\) frames.*/\1/p' <<<"$res" | sort -n | uniq -c | awk '{printf "%s%sx%s", s, $1, $2; s=" "}')
	pk=$(sed -n 's/.*analogue -\{0,1\}[0-9]* frames (peak \([-0-9.]*\) dBFS).*/\1/p' <<<"$res" | head -1)
	x=$(sed -n 's/.*xruns=\([0-9]*\).*/\1/p' <<<"$res")
	say "-- $R Hz period $P buffer $(( 4 * P )): analogue [$a] frames, digital [$d] frames, analogue peak ${pk:-?} dBFS, xruns ${x:-?}"
	[ -n "$a" ] || say "   $(head -3 <<<"$res" | tr '\n' ' ')"
done
