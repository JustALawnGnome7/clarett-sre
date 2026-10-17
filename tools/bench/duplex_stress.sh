#!/bin/bash
# Simultaneous capture + playback stress, checked with the digital-loopback ramp. Run as your user:
#   bash duplex_stress.sh <card>          (SHORT=1 for a ~3 min smoke run)
# Playback PCM $RAMP carries a streamed ramp at the full playback width, routed to the first loopback
# capture destination; the full-width capture is checked live (duplex_ramp.py). Parts:
#   A  sustained duplex: long legs at 48/96/192 kHz, plus a small-buffer leg
#   B  churn: N cycles of capture and playback started and stopped in the same instant, at mixed
#      rates; after each, both clients must exit and no servicer thread may survive
#   C  a playback open at a different rate while capture runs: narrowed to the running rate, unbroken
# The first stream after a change to a FASTER rate logs late=1..3 in its first stream-svc window: the
# device runs the old rate for ~20-30 ms after SET_CLOCK while the threshold already uses the new one.
# Whatever the ramp channel feeds is switched Off for the run, PipeWire's card profile is set to off,
# and both are restored at exit. A client still alive 15 s past its deadline is reported HUNG and the
# run stops (a D-state close: reboot, do not rmmod). Kernel lines from snd_clarett during the run are
# appended to the log. Needs fcp-server (routing controls). Log: ./duplex_stress.log
set -u
CARD=${1:?usage: $0 <card>}
D=$(dirname "$(readlink -f "$0")")
W=$(mktemp -d)
LOG=${BENCH_LOG_DIR:-$PWD}/duplex_stress.log
PY="python3 -I $D/duplex_ramp.py"
ID=$(cat /proc/asound/card$CARD/id)
SHORT=${SHORT:-0}

# CH/PB = capture/playback width, R0 = ramp playback channel (feeds no output by default),
# DST:EXP = loopback capture destination and the ALSA capture channel it arrives on.
case $ID in
C2Pre)       CH=14 PB=4  R0=3 DST=13 EXP=13;;
C4Pre)       CH=20 PB=8  R0=7 DST=19 EXP=19;;
C8Pre)       CH=20 PB=20 R0=7 DST=19 EXP=19;;
C8PreX)      CH=28 PB=28 R0=7 DST=27 EXP=27;;
# 31 -> 31 survives every speed's S/MUX tail. The churn runs each app's default geometry: the driver
# gives both directions the Red's shorter (2048-frame playback) ring as the ceiling, so the defaults
# agree and two clients configuring in the same instant cannot trip the duplex period lock.
# CHURN_XA="--period-size=... --buffer-size=..." pins a geometry instead.
R8Line|R4Pre) CH=60 PB=64 R0=31 DST=31 EXP=31;;
*) echo "unknown card id $ID"; exit 1;;
esac
RAMP=${RAMP:-$R0}
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
	[ -n "$PWCARD" ] && pactl set-card-profile "$PWCARD" "$PWPROF"
	rm -rf "$W"
}
trap restore EXIT

T0=$(date '+%Y-%m-%d %H:%M:%S')
FAILS=0
svc() { ps -eLo comm | grep -c '^clarett-svc'; }
say() { echo "$*" | tee -a "$LOG"; }

# wait for pids until a deadline (s); HUNG -> dump state and exit without touching the module
reap() {
	local dl=$(( $(date +%s) + $1 )); shift
	for p in "$@"; do
		while kill -0 "$p" 2>/dev/null; do
			if [ "$(date +%s)" -ge "$dl" ]; then
				say "!! HUNG: pid $p still alive past its deadline; NOT touching the module (reboot)"
				ps -eLo pid,tid,stat,wchan:32,comm | grep -E 'aplay|arecord|clarett' | tee -a "$LOG"
				trap - EXIT; exit 2
			fi
			sleep 0.2
		done
	done
}

# one duplex leg: rate, seconds of ramp, capture lead/tail (s), extra aplay/arecord args, label,
# and INFO=1 to report without counting a failure (non-RT clients on a tiny buffer)
leg() {
	local R=$1 S=$2 LEAD=$3 XA=$4 lbl=$5 info=${6:-0} fr=$(( $1 * $2 ))
	arecord -D hw:$CARD,0 -f S32_LE -c $CH -r $R -t raw -d $(( S + 2 * LEAD )) $XA 2>"$W/rec.err" \
		| $PY check $CH $EXP $fr > "$W/res" &
	local rp=$!
	sleep "$LEAD"
	$PY gen $fr $PB $RAMP | aplay -D hw:$CARD,0 -f S32_LE -c $PB -r $R -t raw $XA 2>"$W/play.err" &
	local pp=$!
	reap $(( S + 2 * LEAD + 15 )) $pp $rp
	local x="$(grep -ciE 'underrun|overrun|xrun' "$W/rec.err" "$W/play.err" | awk -F: '{s+=$2} END{print s+0}')"
	local e="$(grep -viE '^(Recording|Playing)' "$W/rec.err" "$W/play.err" | grep -v ':$' | head -3 | tr '\n' ' ')"
	grep -q '^PASS' "$W/res" && [ "$x" = 0 ] || [ "$info" = 1 ] || FAILS=$((FAILS + 1))
	say "-- $lbl: $(cat "$W/res"); xruns $x${e:+; stderr: $e}"
}

say "== $(date) duplex stress card $CARD $ID, ${CH}ch capture + ${PB}ch playback, ramp PCM $RAMP -> \"$CTL\" -> ch $EXP (srcversion $(cat /sys/module/snd_clarett/srcversion))"
# Every card's servicer is named clarett-svc, so another card streaming (PipeWire on a second unit) shows
# up here too: take the count now as the baseline, and require each check to return to it.
BASE=$(svc)
[ "$BASE" = 0 ] || say "   baseline: $BASE clarett-svc thread(s) from other cards; checks compare against it"

if [ "$SHORT" = 1 ]; then A="48000:30 96000:20 192000:20"; ASM=20; NB=10; else A="48000:300 96000:120 192000:120"; ASM=120; NB=30; fi
say "-- A. sustained duplex"
for a in $A; do leg ${a%:*} ${a#*:} 1 "" "${a%:*} Hz ${a#*:} s, default buffer"; done
leg 48000 $ASM 1 "--period-size=64 --buffer-size=256" "48000 Hz $ASM s, period 64 / buffer 256 (non-RT clients; informational)" 1
[ $(svc) = "$BASE" ] || say "!! clarett-svc thread left after part A"

say "-- B. churn: $NB simultaneous start/stop cycles"
RATES=(48000 96000 192000 44100 88200 176400)
bf=0; left=0
for i in $(seq 1 $NB); do
	R=${RATES[$(( (i - 1) % 6 ))]}; fr=$(( R * 3 ))
	arecord -D hw:$CARD,0 -f S32_LE -c $CH -r $R -t raw -d 3 ${CHURN_XA:-} 2>"$W/rec.err" | $PY check $CH $EXP > "$W/res" &
	rp=$!
	$PY gen $fr $PB $RAMP | aplay -D hw:$CARD,0 -f S32_LE -c $PB -r $R -t raw ${CHURN_XA:-} 2>"$W/play.err" &
	pp=$!
	reap 20 $pp $rp
	sleep 0.3
	n=$(svc)
	r=$(cat "$W/res")
	ok=1; grep -q '^PASS' "$W/res" || ok=0; [ "$n" = "$BASE" ] || { ok=0; left=$((left + 1)); }
	grep -qiE 'error|busy|underrun|overrun' "$W/rec.err" "$W/play.err" && ok=0
	[ $ok = 1 ] || { bf=$((bf + 1)); say "   cycle $i $R Hz: $r; svc threads $n; $(cat "$W/rec.err" "$W/play.err" | grep -v '^\(Recording\|Playing\)' | head -2 | tr '\n' ' ')"; }
done
[ $bf = 0 ] || FAILS=$((FAILS + 1))
say "-- B: $((NB - bf))/$NB cycles clean (ramp unbroken, both exited, no servicer left; $left left a thread)"

say "-- C. playback opened at 96 kHz while capture runs at 48 kHz"
arecord -D hw:$CARD,0 -f S32_LE -c $CH -r 48000 -t raw -d 5 2>"$W/rec.err" | $PY check $CH $EXP 96000 > "$W/res" &
rp=$!
sleep 1
$PY gen 96000 $PB $RAMP | aplay -D hw:$CARD,0 -f S32_LE -c $PB -r 96000 -t raw 2>"$W/play.err"; c96=$?
reap 20 $rp
say "   aplay exit $c96 ($(grep -v '^Playing' "$W/play.err" | head -1)); capture: $(cat "$W/res")"
# expected: the open is narrowed to the running rate (aplay warns got = 48000Hz), and the 96000 frames
# arrive as one unbroken ramp at 48 kHz
{ [ $c96 = 0 ] && grep -q 'got = 48000Hz' "$W/play.err" && grep -q '^PASS' "$W/res"; } || FAILS=$((FAILS + 1))

[ $(svc) = "$BASE" ] || { say "!! clarett-svc thread left at the end"; FAILS=$((FAILS + 1)); }
say "-- kernel (snd_clarett / WARN / hung) since $T0:"
journalctl -k -o cat --since "$T0" | grep -E 'snd_clarett|WARNING|BUG|hung_task|blocked for' | sed 's/^/   /' | tee -a "$LOG" | head -40
say "== done: $FAILS failing part(s)/leg(s)"
