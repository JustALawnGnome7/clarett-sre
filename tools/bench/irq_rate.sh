#!/bin/bash
# Per-vector MSI counts, idle against streaming: does a data-plane period event
# raise an interrupt, and if so on which vector?
#   sudo bash irq_rate.sh <card> <bdf> [secs]
# The device raises a period event every irq_descs descriptors (the cadence the
# host picks through the descriptors' IRQ markers), and the servicer catches
# each one by polling 0x300 — it never waits for an interrupt. So the question
# this answers is whether those events also assert MSI: a vector whose count
# rises by the driver's own periods= figure carries them, and if none does, the
# data plane cannot be made interrupt-driven until what enables them is found.
# fcp-server is stopped for the run (KEEP_FCP=1 to leave it), so vec0's mailbox
# traffic does not have to be subtracted from a long baseline.
# Log: ./irq_rate.log
set -u
CARD=${1:?usage: $0 <card> <bdf> [secs]}
BDF=${2:?usage: $0 <card> <bdf> [secs]}
SECS=${3:-20}
# a leading ~ survives `sudo BENCH_LOG_DIR=~/dir` under zsh, and an unwritable
# directory would lose the run to tee errors: expand it, then fall back
DIR=${BENCH_LOG_DIR:-$PWD}
case $DIR in
~/*) DIR=$HOME/${DIR#\~/};;
"~") DIR=$HOME;;
esac
{ [ -d "$DIR" ] && [ -w "$DIR" ]; } || DIR=$PWD
LOG=$DIR/irq_rate.log
DD=/sys/kernel/debug/dynamic_debug/control
ROOT=0
[ "$(id -u)" = 0 ] && ROOT=1

# "<vector index> <interrupts on every CPU summed>" per MSI vector of this unit
snap() {
	awk -v bdf="$BDF" '
		index($0, "PCI-MSI-" bdf) {
			for (i = 2; i <= NF; i++)
				if (index($i, "PCI-MSI-" bdf))
					break
			split($(i + 1), a, "-")
			tot = 0
			for (j = 2; j < i; j++)
				tot += $j
			print a[1], tot
		}' /proc/interrupts
}

delta() {	# delta <before file> <after file>
	join "$1" "$2" | awk '{ print $1, $3 - $2 }'
}

cursor() {
	journalctl -k -n0 --show-cursor | sed -n 's/^-- cursor: //p'
}

restore() {
	[ "$ROOT" = 1 ] && echo 'format "stream-svc:" -p' > $DD
	[ "$ROOT" = 1 ] && [ "${FCP_WAS_UP:-0}" = 1 ] &&
		systemctl start "fcp-server@$CARD" 2>/dev/null
	rm -f /tmp/irq_rate.$$.*
	return 0
}
trap restore EXIT

[ -n "$(snap)" ] || { echo "no MSI vectors found for $BDF"; exit 1; }
CH=$(journalctl -k -q | grep "snd_clarett $BDF: .*FCP hwdep" | tail -1 |
	sed -n 's/.*PCM [0-9]*\/\([0-9]*\)ch.*/\1/p')
[ -n "$CH" ] || { echo "no registration line for $BDF"; exit 1; }

if [ "$ROOT" = 1 ] && [ "${KEEP_FCP:-0}" != 1 ]; then
	systemctl is-active -q "fcp-server@$CARD" && FCP_WAS_UP=1
	[ "${FCP_WAS_UP:-0}" = 1 ] && systemctl stop "fcp-server@$CARD"
fi

echo "== $(date) irq rate card $CARD ($BDF, ${CH}ch), ${SECS}s legs" | tee -a "$LOG"
[ "$ROOT" = 1 ] || echo "   (not root: no periods= figure, fcp-server left running)" | tee -a "$LOG"

# 1. idle: nothing streaming. Whatever still ticks here is not the data plane.
snap > /tmp/irq_rate.$$.a
sleep "$SECS"
snap > /tmp/irq_rate.$$.b
delta /tmp/irq_rate.$$.a /tmp/irq_rate.$$.b > /tmp/irq_rate.$$.idle

# 2. the same window with a capture running, once per ALSA period size: a
# vector carrying the period events tracks the cadence, so its rate must
# change with the period and a fixed-rate source must not.
for P in ${PERIODS:-512 64}; do
	[ "$ROOT" = 1 ] && echo 'format "stream-svc:" +p' > $DD
	cur=$(cursor)
	snap > /tmp/irq_rate.$$.c
	arecord -D "hw:$CARD,0" -c "$CH" -f S32_LE -r 48000 \
		--period-size="$P" --buffer-size=$((P * 4)) -d "$SECS" \
		> /dev/null 2> /tmp/irq_rate.$$.rec
	rc=$?
	snap > /tmp/irq_rate.$$.d
	delta /tmp/irq_rate.$$.c /tmp/irq_rate.$$.d > /tmp/irq_rate.$$.run
	[ "$ROOT" = 1 ] && echo 'format "stream-svc:" -p' > $DD

	if [ $rc != 0 ]; then
		echo "-- period $P: arecord failed ($rc)" | tee -a "$LOG"
		sed -n '1,3p' /tmp/irq_rate.$$.rec | tee -a "$LOG"
		continue
	fi
	PER=$(journalctl -k -q --after-cursor "$cur" |
		sed -n "s/.*snd_clarett $BDF: stream-svc: stopped (periods=\([0-9]*\).*/\1/p" |
		tail -1)
	# with no periods= figure (not root), the cadence is rate/period
	EXP=$((SECS * 48000 / P))

	echo "-- period $P frames: device period events ${PER:-unknown}, expected ~$EXP" |
		tee -a "$LOG"
	join /tmp/irq_rate.$$.idle /tmp/irq_rate.$$.run |
		awk -v secs="$SECS" -v per="${PER:-0}" -v want="$EXP" '
		BEGIN { printf "   %-4s %8s %10s %12s %10s %10s\n",
				"vec", "idle/s", "stream/s", "attributable", "per event",
				"per want" }
		{
			attr = $3 - $2
			n = (per > 0) ? per : want
			printf "   %-4s %8.1f %10.1f %12d %10.3f %10.3f\n",
				$1, $2 / secs, $3 / secs, attr, attr / n, attr / want
		}' | tee -a "$LOG"
done
echo "   A vector carrying the period events reads ~1.000 per event at BOTH" |
	tee -a "$LOG"
echo "   period sizes; a fixed-rate source does not." | tee -a "$LOG"
