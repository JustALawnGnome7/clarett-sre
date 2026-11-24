#!/bin/bash
# notify_reads.sh <card> <bdf> [seconds] — mailbox commands per relayed notification.
# Run as root while turning a front-panel control (the monitor knob). Over the window it
# counts the notifications the driver relays to fcp-server, by event word, and every FCP
# command the driver sends, by opcode. The relays are counted before the driver's 50 ms
# debounce, so fcp-server sees at most 20 notifications a second however many relays there
# are: compare GET_DATA (0x800000) per second between runs, not per relay. On a Clarett it
# includes the driver's own monitor-region read, also at most 20 a second. Records which
# fcp-server binary was running, so runs of two builds can be told apart.
# Log: $BENCH_LOG_DIR/notify_reads.log (default ./)
set -u
CARD=${1:?usage: $0 <card> <bdf> [seconds]}; BDF=${2:?usage: $0 <card> <bdf> [seconds]}
SECS=${3:-15}
LOG=${BENCH_LOG_DIR:-$PWD}/notify_reads.log
DD=/sys/kernel/debug/dynamic_debug/control
dbg() {
	echo "format \"FCP op=\" $1" > $DD
	echo "format \"async notification handled\" $1" > $DD
}
trap 'dbg -p' EXIT

pid=$(systemctl show -p MainPID --value "fcp-server@$CARD")
[ "${pid:-0}" != 0 ] || { echo "fcp-server@$CARD is not running"; exit 1; }
exe=$(readlink -f /proc/$pid/exe)
sum=$(md5sum < /proc/$pid/exe | cut -c1-12)

cur=$(journalctl -k -n0 --show-cursor | sed -n 's/^-- cursor: //p')
dbg +p
echo "turn the control now ($SECS s)"
sleep "$SECS"
dbg -p
k=$(journalctl -k -q --after-cursor "$cur" | grep "snd_clarett $BDF: ")

relays=$(grep -c "async notification handled" <<<"$k")
gets=$(grep -c "FCP op=0x800000 " <<<"$k")
{
	echo "== $(date) card $CARD ($BDF), $SECS s, fcp-server $exe md5 $sum"
	echo "   relays: $relays"
	grep -o "handled: 0x[0-9a-f]*" <<<"$k" | sort | uniq -c | sed 's/^/     /'
	echo "   commands by opcode:"
	grep -o "FCP op=0x[0-9a-f]*" <<<"$k" | sort | uniq -c | sed 's/^/     /'
	awk -v g="$gets" -v r="$relays" -v t="$SECS" 'BEGIN {
		printf "   GET_DATA per second: %.1f", g / t
		if (r) printf " (per relay %.2f)", g / r
		printf "\n" }'
} | tee -a "$LOG"
