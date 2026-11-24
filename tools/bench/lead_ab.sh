#!/bin/bash
# Short-lead playback A/B between two driver builds: the tx_guard_lead.sh cell "leadplay period 32"
# (leadplay holding a 48-frame lead in 16-frame writes, arecord capturing at the same geometry, the
# loopback ramp counted on the capture), REPS reps x SECS s per build, in blocks A B A B so drift over
# the run hits both builds alike. Per rep it also counts the servicer's 2 s windows with a gap of
# 40 ms or more (stream-svc telemetry, switched on for the run), so a host stall can be told from the
# driver: on the ASRock a gap that lands in a stall window is the platform.
# Run as your user (asks for sudo once, for the module swaps and the kernel log):
#   bash lead_ab.sh <card id, e.g. R8Line> <A.ko> <B.ko> [reps per block, default 5] [secs, default 20]
# The card is left on build B. Needs fcp-server (started by udev on each load) and leadplay built at
# $BENCH_LOG_DIR/leadplay. Log: ./lead_ab.log
set -u
ID=${1:?usage: $0 <card id> <A.ko> <B.ko> [reps] [secs]}
declare -A KO=([A]=$(readlink -f "${2:?A.ko}") [B]=$(readlink -f "${3:?B.ko}"))
REPS=${4:-5}; SECS=${5:-20}
D=$(dirname "$(readlink -f "$0")")
DIR=${BENCH_LOG_DIR:-$PWD}
LOG=$DIR/lead_ab.log
LP=$DIR/leadplay
PY="python3 -I $D/duplex_ramp.py"
W=$(mktemp -d)
for b in A B; do [ -f "${KO[$b]}" ] || { echo "no module ${KO[$b]}"; exit 1; }; done
[ -x "$LP" ] || { echo "build $LP first: gcc -O2 -o $LP $D/leadplay.c -lasound"; exit 1; }
case $ID in
C2Pre)        CH=14 PB=4  RAMP=3  DST=13 EXP=13;;
C4Pre)        CH=20 PB=8  RAMP=7  DST=19 EXP=19;;
C8Pre)        CH=20 PB=20 RAMP=7  DST=19 EXP=19;;
C8PreX)       CH=28 PB=28 RAMP=7  DST=27 EXP=27;;
R8Line|R4Pre) CH=60 PB=64 RAMP=31 DST=31 EXP=31;;
*) echo "unknown card id $ID"; exit 1;;
esac
CTL="PCM $DST Capture Enum" PER=32 BUF=128 TOTAL=$((SECS * 48000))
cardno() { awk -v id="$ID" '$2 == "["id {print $1; exit}' /proc/asound/cards; }
C=$(cardno); [ -n "$C" ] || { echo "no card $ID (cat /proc/asound/cards)"; exit 1; }
BDF=$(basename "$(readlink -f /sys/class/sound/card$C/device)")
sudo -v || exit 1
( while sleep 50; do sudo -n true || exit; done ) & KEEP=$!

# what the ramp channel feeds is switched off and the loopback route set, after every load
route() {
	local n v
	: > "$W/restore"
	amixer -c "$C" controls | sed -n "s/.*name='\(.*Enum\)'.*/\1/p" > "$W/enums"
	while read -r n; do
		v=$(amixer -c "$C" cget name="$n" | sed -n 's/.*: values=//p' | head -1)
		amixer -c "$C" cget name="$n" | grep -q "Item #$v 'PCM $RAMP'$" || continue
		printf '%s\t%s\n' "$n" "$v" >> "$W/restore"
		amixer -q -c "$C" cset name="$n" 'Off'
	done < "$W/enums"
	printf '%s\t%s\n' "$CTL" "$(amixer -c "$C" cget name="$CTL" | sed -n 's/.*: values=//p')" >> "$W/restore"
	amixer -q -c "$C" cset name="$CTL" "PCM $RAMP"
}
unroute() {
	[ -s "$W/restore" ] || return 0
	while IFS=$'\t' read -r n v; do amixer -q -c "$C" cset -- name="$n" "$v" 2>/dev/null; done < "$W/restore"
	: > "$W/restore"
}
pw() { pactl list cards short 2>/dev/null | awk -v b="pci-$(tr ':' '_' <<<"$BDF")" '$2 ~ b {print $2}'; }
PWCARD=$(pw); PWPROF=
[ -n "$PWCARD" ] && PWPROF=$(pactl list cards | awk -v n="$PWCARD" '$1=="Name:"{c=($2==n)} c&&/Active Profile:/{print $3; exit}')
finish() {
	unroute
	PWCARD=$(pw)
	[ -n "$PWCARD" ] && [ -n "$PWPROF" ] && pactl set-card-profile "$PWCARD" "$PWPROF"
	kill $KEEP 2>/dev/null
	rm -rf "$W"
}
trap finish EXIT

load() {	# load <ko>: swap the module, wait for fcp-server's routing controls, take the card from PipeWire
	local i
	PWCARD=$(pw); [ -n "$PWCARD" ] && pactl set-card-profile "$PWCARD" off
	sudo systemctl stop 'fcp-server@*' alsa-state.service 2>/dev/null
	sudo rmmod snd_clarett && sudo insmod "$1" || { echo "module swap failed"; exit 1; }
	echo 'format "stream-svc:" +p' | sudo tee /sys/kernel/debug/dynamic_debug/control >/dev/null
	for i in $(seq 100); do
		C=$(cardno)
		[ -n "$C" ] && amixer -c "$C" cget name="$CTL" >/dev/null 2>&1 && break
		sleep 0.2
	done
	[ -n "$C" ] || { echo "card $ID did not come back"; exit 1; }
	sudo systemctl start alsa-state.service 2>/dev/null
	sleep 2
	PWCARD=$(pw); [ -n "$PWCARD" ] && pactl set-card-profile "$PWCARD" off
	route
	sleep 1
}

run() {	# run <build> <rep>
	local res pl t0 st ur
	t0=$(date +%s.%N)
	arecord -q -D hw:$C,0 -f S32_LE -c $CH -r 48000 -t raw -d $((SECS + 2)) \
		--period-size=$PER --buffer-size=$BUF 2>/dev/null | $PY count $CH $EXP > "$W/res" &
	local rp=$!
	sleep 0.5
	pl=$("$LP" hw:$C,0 $PB $RAMP 48000 $BUF $PER 48 16 $SECS 2>&1)
	wait $rp
	sleep 2.5	# the last 2 s window
	st=$(sudo -n journalctl -k -o cat --since "@$t0" | grep 'stream-svc: periods=' |
		sed -n 's/.*gapmax=\([0-9]*\)us.*/\1/p' | awk '$1 >= 40000 {n++} END {print n + 0}')
	res=$(cat "$W/res")
	ur=$(sed -n 's/.*underruns=\([0-9]*\).*/\1/p' <<<"$pl")
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$(sed -n 's/.*ramp \([0-9]*\) frames.*/\1/p' <<<"$res")" \
		"$(sed -n 's/.*gaps=\([0-9]*\).*/\1/p' <<<"$res")" "$(sed -n 's/.*breaks=\([0-9]*\).*/\1/p' <<<"$res")" \
		"${ur:-?}" "$st" >> "$W/all"
	echo "-- $1 rep $2 ($(cat /sys/module/snd_clarett/srcversion)): $res; leadplay underruns ${ur:-? ($pl)}; stall windows $st" | tee -a "$LOG"
}

: > "$W/all"
echo "#### $(date) $ID at $BDF: A = ${KO[A]}, B = ${KO[B]}; leadplay period $PER/$BUF, 48-frame lead, $REPS x $SECS s per block" | tee -a "$LOG"
n=0
for blk in A B A B; do
	unroute; load "${KO[$blk]}"
	for r in $(seq "$REPS"); do run $blk $((n / 2 * REPS + r)); done
	n=$((n + 1))
done
echo "-- summary per build (frames of $TOTAL / gaps / breaks / leadplay underruns / stall windows, per rep):" | tee -a "$LOG"
for b in A B; do
	awk -F'\t' -v b=$b -v T=$TOTAL '$1 == b {f = f " " $3; g = g " " $4; k = k " " $5; u = u " " $6; s = s " " $7
		G += $4; K += $5; U += $6; S += $7; if ($3 < T) L += T - $3; n++}
		END {printf "   %s frames:%s\n     gaps:%s (total %d)\n     breaks:%s (total %d)\n     underruns:%s (total %d)\n     stall windows:%s (total %d)\n     frames lost: %d of %d\n",
			b, f, g, G, k, K, u, U, s, S, L, n * T}' "$W/all" | tee -a "$LOG"
done
echo "(the card is left on build B)" | tee -a "$LOG"
