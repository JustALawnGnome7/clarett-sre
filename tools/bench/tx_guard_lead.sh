#!/bin/bash
# The tx_guard question: does a playback client holding a SHORT lead tear the audio at a small guard?
# Run as your user (sudo is asked for once, to set tx_guard):  bash tx_guard_lead.sh <card> [reps] [secs]
# Grid, one variable at a time, each cell repeated: client (leadplay holding a 48-frame lead in
# 16-frame chunks | aplay keeping its buffer full) x period (32, buffer 128 | 64, buffer 256) x
# tx_guard (16, 32, 64). Each run plays the digital-loopback ramp on the ramp channel while arecord
# captures at the same geometry; the capture is counted (duplex_ramp.py count): `breaks` = steps that
# are not +256 between two ramp samples (torn or misplaced audio, the driver's failure), `gaps` =
# silences inside the ramp (the client's own underruns). On an older driver, on a host with a ~42 ms
# platform freeze, leadplay at period 32 gave 38k-71k breaks per 25 s at guard 16/32 and single
# digits at 64; aplay never did. Needs leadplay built at $BENCH_LOG_DIR/leadplay
# (gcc -O2 -o leadplay leadplay.c -lasound) and fcp-server (routing controls). tx_guard is restored
# to its value at start. Log: ./tx_guard_lead.log
set -u
CARD=${1:?usage: $0 <card> [reps] [secs]}; REPS=${2:-3}; SECS=${3:-20}
D=$(dirname "$(readlink -f "$0")")
DIR=${BENCH_LOG_DIR:-$PWD}
LOG=$DIR/tx_guard_lead.log
LP=$DIR/leadplay
GP=/sys/module/snd_clarett/parameters/tx_guard
PY="python3 -I $D/duplex_ramp.py"
W=$(mktemp -d)
[ -e /proc/asound/card$CARD/id ] || { echo "no card $CARD (cat /proc/asound/cards)"; exit 1; }
ID=$(cat /proc/asound/card$CARD/id)
[ -x "$LP" ] || { echo "build $LP first: gcc -O2 -o $LP $D/leadplay.c -lasound"; exit 1; }

# CH/PB = capture/playback width, R0 = ramp playback channel (feeds no output by default),
# DST:EXP = loopback capture destination and the ALSA capture channel it arrives on.
case $ID in
C2Pre)        CH=14 PB=4  R0=3  DST=13 EXP=13;;
C4Pre)        CH=20 PB=8  R0=7  DST=19 EXP=19;;
C8Pre)        CH=20 PB=20 R0=7  DST=19 EXP=19;;
C8PreX)       CH=28 PB=28 R0=7  DST=27 EXP=27;;
R8Line|R4Pre) CH=60 PB=64 R0=31 DST=31 EXP=31;;
*) echo "unknown card id $ID"; exit 1;;
esac
RAMP=${RAMP:-$R0}

sudo -v || exit 1
( while sleep 60; do sudo -n true || exit; done ) & KEEP=$!
G0=$(cat $GP)

PWCARD=$(pactl list cards short 2>/dev/null | awk -v b="pci-$(basename "$(readlink -f /sys/class/sound/card$CARD/device)" | tr ":" "_")" \
	'$2 ~ b {print $2}')
# PipeWire off the card, ramp channel's other uses off, loopback route on; all restored at exit
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
	echo "$G0" | sudo -n tee $GP >/dev/null
	[ -n "$PWCARD" ] && pactl set-card-profile "$PWCARD" "$PWPROF"
	kill $KEEP 2>/dev/null
	rm -rf "$W"
}
trap restore EXIT

say() { echo "$*" | tee -a "$LOG"; }

run() {	# client period guard
	local cl=$1 per=$2 g=$3 buf=$(( $2 * 4 )) pl ur res
	echo "$g" | sudo -n tee $GP >/dev/null
	arecord -D hw:$CARD,0 -f S32_LE -c $CH -r 48000 -t raw -d $(( SECS + 2 )) \
		--period-size=$per --buffer-size=$buf 2>"$W/rec.err" | $PY count $CH $EXP > "$W/res" &
	local rp=$!
	sleep 0.5
	if [ "$cl" = leadplay ]; then
		pl=$("$LP" hw:$CARD,0 $PB $RAMP 48000 $buf $per 48 16 $SECS 2>&1)
		ur=$(sed -n 's/.*underruns=\([0-9]*\).*/\1/p' <<<"$pl")
	else
		$PY gen $(( SECS * 48000 )) $PB $RAMP | aplay -D hw:$CARD,0 -f S32_LE -c $PB -r 48000 -t raw \
			--period-size=$per --buffer-size=$buf 2>"$W/play.err"
		ur=$(grep -c underrun "$W/play.err")
		pl=$(grep -viE '^Playing|underrun' "$W/play.err" | head -1)
	fi
	wait $rp
	res=$(cat "$W/res")
	printf '%s\t%s\t%s\t%s\n' "$cl" "$per" "$g" "$(sed -n 's/.*breaks=\([0-9]*\).*/\1/p' <<<"$res")" >> "$W/all"
	say "$(printf -- '-- %-8s period %-2s guard %-2s: %s; client underruns %s' "$cl" "$per" "$(cat $GP)" "$res" "${ur:-?}")$( [ -n "${ur:-}" ] || echo "; $pl")"
}

say "== $(date) tx_guard lead test card $CARD $ID, ramp PCM $RAMP -> ch $EXP, $REPS reps x $SECS s, 48 kHz (srcversion $(cat /sys/module/snd_clarett/srcversion), tx_guard was $G0)"
for r in $(seq 1 "$REPS"); do
	say "- rep $r"
	for cl in leadplay aplay; do for per in 32 64; do for g in 16 32 64; do run $cl $per $g; done; done; done
done
say "-- summary: breaks per run"
sort -k1,1 -k2,2n -k3,3n "$W/all" | awk -F'\t' '{k=$1" period "$2" guard "$3; v[k]=v[k] (v[k]==""?"":" ") $4; if(!(k in o)){o[k]=++n; ks[n]=k}}
	END {for(i=1;i<=n;i++) printf "   %-30s %s\n", ks[i], v[ks[i]]}' | tee -a "$LOG"
