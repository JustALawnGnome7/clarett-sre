#!/bin/bash
# Stalls at stream start, A/B between two driver builds, interleaved so time of day hits both alike.
# Run as your user (asks for sudo, for the module swaps):
#   bash start_stall_ab.sh <card id, e.g. R8Line> <A.ko> <B.ko> [blocks, default 6] [streams per block, default 12]
# Blocks alternate A B A B ...; each block reloads its build (load_build.sh, stream-svc telemetry on) and
# runs short duplex streams (arecord + aplay with each app's default geometry, 4 s of loopback ramp),
# rotating 48/96/192 kHz so most streams follow a rate change, as in the runs that stalled. Per stream:
# the ramp check, and from the kernel log the first 2 s window's gapmax/readmax (and posmax, on a build
# that logs it), the stream's worst gap, and any aplay underrun (the client falling behind). A "stall"
# is a window gap of 40 ms or more.
# Log: ./start_stall_ab.log. The card is left on the last block's build with PipeWire's profile off.
set -u
ID=${1:?usage: $0 <card id> <A.ko> <B.ko> [blocks] [streams]}
declare -A KO=([A]=$(readlink -f "${2:?A.ko}") [B]=$(readlink -f "${3:?B.ko}"))
BLOCKS=${4:-6}; N=${5:-12}
D=$(dirname "$(readlink -f "$0")")
DIR=${BENCH_LOG_DIR:-$PWD}
LOG=$DIR/start_stall_ab.log
PY="python3 -I $D/duplex_ramp.py"
W=$(mktemp -d)
for b in A B; do [ -f "${KO[$b]}" ] || { echo "no module ${KO[$b]}"; exit 1; }; done
case $ID in
C2Pre)        CH=14 PB=4  RAMP=3  DST=13 EXP=13;;
C4Pre)        CH=20 PB=8  RAMP=7  DST=19 EXP=19;;
C8Pre)        CH=20 PB=20 RAMP=7  DST=19 EXP=19;;
C8PreX)       CH=28 PB=28 RAMP=7  DST=27 EXP=27;;
R8Line|R4Pre) CH=60 PB=64 RAMP=31 DST=31 EXP=31;;
*) echo "unknown card id $ID"; exit 1;;
esac
CTL="PCM $DST Capture Enum" SECS=4
cardno() { awk -v id="$ID" '$2 == "["id {print $1; exit}' /proc/asound/cards; }
sudo -v || exit 1
( while sleep 50; do sudo -n true || exit; done ) & KEEP=$!

route() {	# switch off what the ramp channel feeds, route the loopback; restore list in $W/restore
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
	while IFS=$'\t' read -r n v; do amixer -q -c "$C" cset name="$n" "$v" 2>/dev/null; done < "$W/restore"
	: > "$W/restore"
}
trap 'unroute; kill $KEEP 2>/dev/null; rm -rf "$W"' EXIT

stream() {	# stream <build> <rate>
	local r=$2 t0 res k first worst fg fr fp
	t0=$(date +%s.%N)
	arecord -q -D hw:$C,0 -f S32_LE -c $CH -r $r -t raw -d $((SECS + 2)) 2>/dev/null |
		$PY check $CH $EXP $((r * SECS)) > "$W/res" &
	local rp=$!
	sleep 0.5
	$PY gen $((r * SECS)) $PB $RAMP | aplay -q -D hw:$C,0 -t raw -f S32_LE -r $r -c $PB 2>"$W/play.err"
	wait $rp
	sleep 0.3
	res=$(cut -c1-4 "$W/res")
	k=$(sudo -n journalctl -k -o cat --since "@$t0" | grep 'stream-svc:')
	first=$(grep -m1 'stream-svc: periods=' <<<"$k")
	worst=$(grep -m1 'stream-svc: stopped' <<<"$k" | sed -n 's/.*run gapmax=\([0-9]*\)us.*/\1/p')
	fg=$(sed -n 's/.*gapmax=\([0-9]*\)us.*/\1/p' <<<"$first")
	fr=$(sed -n 's/.*readmax=\([0-9]*\)us.*/\1/p' <<<"$first")
	fp=$(sed -n 's/.*posmax=\([0-9]*\)us posblk=\(0x[0-9a-f]*\) posat=\(-*[0-9]*\)ms.*/\1us@\2,\3ms/p' <<<"$first")
	printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$r" "$res" "${fg:-?}" "${worst:-?}" "${fp:--}" >> "$W/all"
	printf -- '-- %s %6s Hz: %s  first window gapmax=%sus readmax=%sus posmax=%s; stream worst gap %sus\n' \
		"$1" "$r" "$res" "${fg:-?}" "${fr:-?}" "${fp:--}" "${worst:-?}" | tee -a "$LOG"
	[ "$res" = PASS ] || sed 's/^/     /' "$W/res" | tee -a "$LOG"
	grep -q underrun "$W/play.err" && echo "     aplay: $(grep -c underrun "$W/play.err") underrun(s)" | tee -a "$LOG"
}

: > "$W/all"
echo "#### $(date) $ID: A = ${KO[A]}, B = ${KO[B]}; $BLOCKS blocks x $N duplex streams of $SECS s" | tee -a "$LOG"
RATES=(48000 96000 192000)
for i in $(seq 0 $((BLOCKS - 1))); do
	b=$([ $((i % 2)) = 0 ] && echo A || echo B)
	[ -n "${C:-}" ] && unroute
	bash "$D/load_build.sh" "${KO[$b]}" "$ID" | tee -a "$LOG" || exit 1
	C=$(cardno)
	route
	echo "- block $((i + 1)): build $b ($(cat /sys/module/snd_clarett/srcversion))" | tee -a "$LOG"
	for j in $(seq 0 $((N - 1))); do stream $b ${RATES[$((j % 3))]}; done
done
echo "-- summary per build: streams / failed / first-window stalls / any-window stalls (gap >= 40 ms)" | tee -a "$LOG"
for b in A B; do
	awk -F'\t' -v b=$b '$1 == b {n++; if ($3 != "PASS") f++; if ($4 >= 40000) s1++; if ($5 >= 40000) s++
		if ($3 != "PASS" && $5 >= 40000) fs++}
		END {printf "   %s: %d streams, %d failed (%d of them with a stall), %d first-window stalls, %d with a stall anywhere\n", b, n, f, fs, s1, s}' "$W/all" | tee -a "$LOG"
done
