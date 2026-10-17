#!/bin/bash
# Clock Source changed while streaming. Run as your user:  bash clock_live.sh <card> [external source]
#  1. A 20 s digital-loopback ramp (raw hw, PipeWire's profile off): at 5 s the Clock Source goes to an
#     external source (default: the card's ADAT / ADAT 1, which should have nothing connected), at 12 s
#     back to Internal. Reports Sync Status after each change, whether both clients finish, where the
#     ramp breaks and whether it runs clean from the last break to its end, and the servicer telemetry.
#  2. The same two changes with PipeWire holding the card (its profile restored): Sync Status must
#     follow, which it could not while a change waited for the next stream arm.
# The ramp channel's other uses, the route, the profile and the Clock Source are restored at exit.
# Needs fcp-server (routing and Sync Status). Do not start alsa-scarlett-gui during it.
# Log: ./clock_live.log
set -u
CARD=${1:?usage: $0 <card> [external source]}
D=$(dirname "$(readlink -f "$0")")
W=$(mktemp -d)
LOG=${BENCH_LOG_DIR:-$PWD}/clock_live.log
PY="python3 -I $D/duplex_ramp.py"
ID=$(cat /proc/asound/card$CARD/id)
case $ID in
C2Pre)        CH=14 PB=4  R0=3  DST=13 EXP=13 EXT=ADAT;;
C4Pre)        CH=20 PB=8  R0=7  DST=19 EXP=19 EXT=ADAT;;
C8Pre)        CH=20 PB=20 R0=7  DST=19 EXP=19 EXT=ADAT;;
C8PreX)       CH=28 PB=28 R0=7  DST=27 EXP=27 EXT="ADAT 1";;
R8Line|R4Pre) CH=60 PB=64 R0=31 DST=31 EXP=31 EXT="ADAT 1";;
*) echo "unknown card id $ID"; exit 1;;
esac
EXT=${2:-$EXT}
RAMP=$R0 R=48000 S=20
PWCARD=$(pactl list cards short 2>/dev/null | awk -v b="pci-$(basename "$(readlink -f /sys/class/sound/card$CARD/device)" | tr ":" "_")" \
	'$2 ~ b {print $2}')
val() { amixer -c "$CARD" cget name="$1" | sed -n 's/.*: values=//p' | head -1; }
item() { local v; v=$(val "$1"); amixer -c "$CARD" cget name="$1" | sed -n "s/.*Item #$v '\(.*\)'$/\1/p"; }
say() { echo "$*" | tee -a "$LOG"; }
# amixer -q ignores an unknown item name, which leaves the clock on Internal and fakes a pass
amixer -c "$CARD" cget name='Clock Source' | grep -q "Item #[0-9]* '$EXT'$" || {
	echo "'$EXT' is not a Clock Source item on card $CARD:"; amixer -c "$CARD" cget name='Clock Source' | grep Item; exit 1; }

: > "$W/restore"
CLK0=$(val 'Clock Source')
printf '%s\t%s\n' 'Clock Source' "$CLK0" >> "$W/restore"
PWPROF=
if [ -n "$PWCARD" ]; then
	PWPROF=$(pactl list cards | awk -v n="$PWCARD" '$1=="Name:"{c=($2==n)} c&&/Active Profile:/{print $3; exit}')
	pactl set-card-profile "$PWCARD" off
fi
amixer -c "$CARD" controls | sed -n "s/.*name='\(.*Enum\)'.*/\1/p" > "$W/enums"
while read -r n; do
	v=$(val "$n")
	amixer -c "$CARD" cget name="$n" | grep -q "Item #$v 'PCM $RAMP'$" || continue
	printf '%s\t%s\n' "$n" "$v" >> "$W/restore"
	amixer -q -c "$CARD" cset name="$n" 'Off'
done < "$W/enums"
CTL="PCM $DST Capture Enum"
printf '%s\t%s\n' "$CTL" "$(val "$CTL")" >> "$W/restore"
amixer -q -c "$CARD" cset name="$CTL" "PCM $RAMP"
restore() {
	while IFS=$'\t' read -r n v; do amixer -q -c "$CARD" cset name="$n" "$v"; done < "$W/restore"
	[ -n "$PWCARD" ] && [ -n "$PWPROF" ] && pactl set-card-profile "$PWCARD" "$PWPROF"
	rm -rf "$W"
}
trap restore EXIT

say "== $(date) clock live card $CARD $ID, ramp PCM $RAMP -> \"$CTL\" -> ch $EXP, external '$EXT' (srcversion $(cat /sys/module/snd_clarett/srcversion))"
amixer -q -c "$CARD" cset name='Clock Source' Internal
sleep 1
T0=$(date '+%F %T')
say "-- 1. raw stream, ${S} s at $R Hz; Sync before: $(item 'Sync Status')"
arecord -D hw:$CARD,0 -f S32_LE -c $CH -r $R -t raw -d $((S + 2)) 2>"$W/rec.err" | $PY check $CH $EXP > "$W/res" &
rp=$!
sleep 1
$PY gen $((R * S)) $PB $RAMP | aplay -D hw:$CARD,0 -f S32_LE -c $PB -r $R -t raw 2>"$W/play.err" &
pp=$!
sleep 4
amixer -q -c "$CARD" cset name='Clock Source' "$EXT"; t1=$(date +%T.%N | cut -c1-12)
sleep 2; s1=$(item 'Sync Status')
sleep 5
amixer -q -c "$CARD" cset name='Clock Source' Internal; t2=$(date +%T.%N | cut -c1-12)
sleep 2; s2=$(item 'Sync Status')
dl=$(( $(date +%s) + S + 20 ))
for p in $pp $rp; do
	while kill -0 "$p" 2>/dev/null; do
		[ "$(date +%s)" -ge "$dl" ] && { say "!! HUNG: pid $p still alive; NOT touching the module (reboot)"; \
			ps -eLo pid,tid,stat,wchan:32,comm | grep -E 'aplay|arecord' | tee -a "$LOG"; trap - EXIT; exit 2; }
		sleep 0.2
	done
done
wait $pp; prc=$?; wait $rp
say "   $t1 -> '$EXT': Sync $s1;   $t2 -> Internal: Sync $s2"
say "   aplay rc $prc; $(grep -viE '^(Recording|Playing)' "$W/rec.err" "$W/play.err" | head -3 | tr '\n' ' ')"
say "   ramp: $(cat "$W/res")"
journalctl -k -o cat --since "$T0" | grep -E 'snd_clarett|WARNING|BUG' | sed 's/^/   /' | tee -a "$LOG" | head -20

if [ -n "$PWCARD" ] && [ -n "$PWPROF" ] && [ "$PWPROF" != off ]; then
	pactl set-card-profile "$PWCARD" "$PWPROF"
	sleep 3
	st=$(grep -h state /proc/asound/card$CARD/pcm0*/sub0/status | tr -s ' ' | tr '\n' ' ')
	say "-- 2. PipeWire holding the card (profile $PWPROF; $st)"
	amixer -q -c "$CARD" cset name='Clock Source' "$EXT"; sleep 2; s1=$(item 'Sync Status')
	amixer -q -c "$CARD" cset name='Clock Source' Internal; sleep 2; s2=$(item 'Sync Status')
	say "   -> '$EXT': Sync $s1;   -> Internal: Sync $s2   (expect Unlocked, then Locked)"
else
	say "-- 2. skipped: no PipeWire profile to restore for this card"
fi
