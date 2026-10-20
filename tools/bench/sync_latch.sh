#!/bin/bash
# Is SYNC_READ (0x006004) bit 1 a read-to-clear latch, or a ~25 ms transition window?
# Run as root:  sudo bash sync_latch.sh <card> [external source]
#
# The two readings agree on 1 -> 2 -> 0 when an absent external source is selected and the word is
# polled every ~23 ms, because each poll is also a read. They disagree on what happens when nobody
# reads: a latch keeps bit 1 until the first read, a timed window has dropped it after ~25 ms. So each
# change is followed once by a 2 s wait before the first read ("late") and once by a 20 ms poll
# ("poll", the original measurement). A no-change leg is the negative control.
#   latch:  late reads 2 0 0 (to an absent source) / 3 1 1 if acquiring lock also sets it
#   window: late reads 0 0 0 / 1 1 1
# The external source should have nothing connected. Stops fcp-server for the card (the hwdep is
# exclusive) and restarts it at exit; the Clock Source is restored. Leave the card's PCMs alone while it
# runs: a stream arm reads 0x006004 itself and would consume a latch.
# Log: ./sync_latch.log
set -u
CARD=${1:?usage: $0 <card> [external source]}
EXT=${2:-S/PDIF}
REPS=${REPS:-3}
D=$(dirname "$(readlink -f "$0")")
LOG=${BENCH_LOG_DIR:-$PWD}/sync_latch.log
DEV=/dev/snd/hwC${CARD}D0
TOOL=$(mktemp)
gcc -O2 -o "$TOOL" "$D/../fcp_sync_read.c" || exit 1

amixer -c "$CARD" cget name='Clock Source' | grep -q "Item #[0-9]* '$EXT'$" || {
	echo "'$EXT' is not a Clock Source item on card $CARD:"; amixer -c "$CARD" cget name='Clock Source' | grep Item; exit 1; }
CLK0=$(amixer -c "$CARD" cget name='Clock Source' | sed -n 's/.*: values=//p')
UNIT=fcp-server@$CARD.service
WAS_ACTIVE=$(systemctl is-active "$UNIT")
cleanup() {
	amixer -q -c "$CARD" cset name='Clock Source' "$CLK0"
	[ "$WAS_ACTIVE" = active ] && systemctl start "$UNIT"
	rm -f "$TOOL"
}
trap cleanup EXIT
[ "$WAS_ACTIVE" = active ] && systemctl stop "$UNIT"

say() { echo "$*" | tee -a "$LOG"; }
word() { "$TOOL" "$DEV" | sed -n 's/.*006004=0x\([0-9a-f]*\).*/\1/p' | sed 's/^0*\([0-9a-f]\)/\1/'; }
setclk() { amixer -q -c "$CARD" cset name='Clock Source' "$1"; }
settle() { sleep 2; word >/dev/null; word >/dev/null; }	# any latch from earlier is read away
late() { sleep 2; echo "$(word) $(word) $(word)"; }
poll() {
	local t0 t out=""
	t0=$(date +%s%N)
	while :; do
		t=$(( ($(date +%s%N) - t0) / 1000000 ))
		[ $t -gt 300 ] && break
		out+="$t:$(word) "
		sleep 0.02
	done
	echo "$out"
}

: > "$LOG"
say "card $CARD $(cat /proc/asound/card$CARD/id), external '$EXT', $(date -Is)"
say "streams: $(grep -h '^state' /proc/asound/card$CARD/pcm*/sub*/status 2>/dev/null | tr '\n' ' ')"
for r in $(seq "$REPS"); do
	say "--- rep $r"
	setclk Internal; settle
	say "control (no change)    late: $(late)"
	setclk Internal; settle; setclk "$EXT"
	say "Internal -> $EXT  late: $(late)"
	setclk "$EXT"; settle; setclk Internal
	say "$EXT -> Internal  late: $(late)"
	setclk Internal; settle; setclk "$EXT"
	say "Internal -> $EXT  poll ms:word  $(poll)"
	setclk "$EXT"; settle; setclk Internal
	say "$EXT -> Internal  poll ms:word  $(poll)"
done
say "streams: $(grep -h '^state' /proc/asound/card$CARD/pcm*/sub*/status 2>/dev/null | tr '\n' ' ')"
