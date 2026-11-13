#!/bin/bash
# What the 0x100 summary register shows, and whether its two enabled-but-never-
# observed bits (4 and 5) are stream error indicators:
#   sudo bash cause_probe.sh <card> <bdf> [secs]
# Vendor traces of all five models show only bits 0-3 (together, once per stream
# stop), 28 (MIDI), 29 (mailbox) and 31 (the period event) -- but not one of
# those captures contains a single period overrun, so an error bit never had the
# chance to fire. The driver accumulates every value it reads (`cause=` in the
# stream-svc summary) and says so at info the first time an unknown bit appears
# (`stream-cause:`).
# Legs: a plain capture; a 16-frame period, the cadence that makes the DEVICE
# flag period overruns (0x300 bit30); and the same in duplex, which is the only
# way to make the playback block (0x200) do anything, since bit30 of 0x100 is
# the candidate summary bit for it and nothing has ever raised it.
# Log: ./cause_probe.log
set -u
CARD=${1:?usage: $0 <card> <bdf> [secs]}
BDF=${2:?usage: $0 <card> <bdf> [secs]}
SECS=${3:-20}
DIR=${BENCH_LOG_DIR:-$PWD}
case $DIR in
~/*) DIR=$HOME/${DIR#\~/};;
"~") DIR=$HOME;;
esac
{ [ -d "$DIR" ] && [ -w "$DIR" ]; } || DIR=$PWD
LOG=$DIR/cause_probe.log
DD=/sys/kernel/debug/dynamic_debug/control
[ "$(id -u)" = 0 ] || { echo "needs root (dynamic debug)"; exit 1; }

CH=$(journalctl -k -q | grep "snd_clarett $BDF: .*FCP hwdep" | tail -1 |
	sed -n 's/.*PCM [0-9]*\/\([0-9]*\)ch.*/\1/p')
PB=$(journalctl -k -q | grep "snd_clarett $BDF: .*FCP hwdep" | tail -1 |
	sed -n 's/.*PCM \([0-9]*\)\/[0-9]*ch.*/\1/p')
[ -n "$CH" ] || { echo "no registration line for $BDF"; exit 1; }

cursor() { journalctl -k -n0 --show-cursor | sed -n 's/^-- cursor: //p'; }
trap 'echo '\''format "stream-svc:" -p'\'' > $DD' EXIT
echo 'format "stream-svc:" +p' > $DD

say() { echo "$*" | tee -a "$LOG"; }
say "== $(date) 0x100 cause probe card $CARD ($BDF, ${CH}ch), ${SECS}s legs"

leg() {	# leg <label> <period> <duplex 0|1> [rate]
	local label=$1 per=$2 dx=$3 rate=${4:-48000} cur buf=$(( $2 * 4 ))
	[ "$buf" -lt 128 ] && buf=128
	cur=$(cursor)
	if [ "$dx" = 1 ]; then
		aplay -D "hw:$CARD,0" -c "$PB" -f S32_LE -r "$rate" -t raw \
			--period-size="$per" --buffer-size="$buf" \
			-d "$SECS" /dev/zero > /dev/null 2>&1 &
	fi
	arecord -D "hw:$CARD,0" -c "$CH" -f S32_LE -r "$rate" \
		--period-size="$per" --buffer-size="$buf" -d "$SECS" \
		> /dev/null 2>&1
	wait
	sleep 1
	local k ovr cause unknown
	k=$(journalctl -k -q --after-cursor "$cur" | grep "snd_clarett $BDF: ")
	cause=$(sed -n 's/.*stream-svc: stopped (.*cause=\(0x[0-9a-f]*\)).*/\1/p' \
		<<<"$k" | tail -1)
	ovr=$(sed -n 's/.*stream-svc: stopped (.*overrun=\([0-9]*\).*/\1/p' \
		<<<"$k" | tail -1)
	unknown=$(grep -o "stream-cause:.*" <<<"$k" | head -1)
	say "$(printf -- '-- %-30s %6s Hz period %4s: device overruns %-4s cause=%s' \
		"$label" "$rate" "$per" "${ovr:-?}" "${cause:-none}")"
	[ -n "$unknown" ] && say "   $unknown"
	return 0
}

leg "capture" 512 0
leg "capture, floor cadence" 16 0
leg "duplex, floor cadence" 16 1
# The floor at 48 kHz no longer makes the device flag anything, now that the
# period is acked from the interrupt rather than up to a poll later. Raising the
# rate raises the event rate instead: 16 frames at 192 kHz is one event every
# 83 us, which is where the servicer must start coalescing.
leg "capture, floor at 2x" 16 0 96000
leg "capture, floor at 4x" 16 0 192000
leg "duplex, floor at 4x" 16 1 192000
say "   Known bits: 0x80000000 period event, 0x20000000 mailbox, 0xf stop class"
say "   (the stop class is raised after the servicer has gone, so cause= never"
say "   shows it). An error indicator would appear as an unknown bit in a leg"
say "   whose overrun count is non-zero; a leg with no overruns proves nothing."
