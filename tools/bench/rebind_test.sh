#!/bin/bash
# Warm rebind timing (no power cycle). Run as root:  sudo bash rebind_test.sh <bdf> [N]
# Per rebind: response-address acknowledgement, attempt answered, ack -> card registered.
# Log: ./rebind_test.log
set -u
BDF=${1:?usage: $0 <bdf> [N]}; N=${2:-3}
LOG=${BENCH_LOG_DIR:-$PWD}/rebind_test.log
DRV=/sys/bus/pci/drivers/snd_clarett
DD=/sys/kernel/debug/dynamic_debug/control
cursor() { journalctl -k -n0 --show-cursor | sed -n 's/^-- cursor: //p'; }
trap 'echo "format \"readiness:\" -p" > $DD' EXIT
echo 'format "readiness:" +p' > $DD
D=$(dirname "$(readlink -f "$0")")

echo "== $(date) warm rebind $BDF x$N" | tee -a "$LOG"
for i in $(seq 1 "$N"); do
	[ -e "$DRV/$BDF" ] && echo "$BDF" > "$DRV/unbind"
	sleep 2
	cur=$(cursor)
	echo "$BDF" > "$DRV/bind" || { echo "-- rebind $i: bind failed" | tee -a "$LOG"; continue; }
	reg=""
	for _ in $(seq 1 40); do
		sleep 0.25
		reg=$(journalctl -k -q -o short-unix --after-cursor "$cur" | grep -E "snd_clarett $BDF: [^:]*: .*(FCP hwdep|no interfaces registered)" | tail -1)
		[ -n "$reg" ] && break
	done
	k=$(journalctl -k -q -o short-unix --after-cursor "$cur" | grep "snd_clarett $BDF")
	a=$(sed -n 's/.*answered on attempt \([0-9]*\).*/\1/p' <<<"$k" | tail -1)
	{
		if [ -n "$reg" ]; then
			echo "-- rebind $i: attempt ${a:-1}  $(journalctl -k -q -o json --after-cursor "$cur" | python3 -I "$D/kmsg_times.py" "$BDF" | tail -1)"
		else
			echo "-- rebind $i: NOT REGISTERED"
		fi
		grep -E "never landed|not acknowledged|refus|-110|error" <<<"$k" | sed 's/^/   /'
	} | tee -a "$LOG"
	sleep 3
done
