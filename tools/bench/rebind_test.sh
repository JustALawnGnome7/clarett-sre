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
ts() { awk '{print $1}' <<<"$1"; }
ms() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.1f", (b-a)*1000}'; }

echo "== $(date) warm rebind $BDF x$N" | tee -a "$LOG"
for i in $(seq 1 "$N"); do
	[ -e "$DRV/$BDF" ] && echo "$BDF" > "$DRV/unbind"
	sleep 2
	cur=$(cursor)
	echo "$BDF" > "$DRV/bind" || { echo "-- rebind $i: bind failed" | tee -a "$LOG"; continue; }
	reg=""
	for _ in $(seq 1 40); do
		sleep 0.25
		reg=$(journalctl -k -q -o short-unix --after-cursor "$cur" | grep "snd_clarett $BDF: [^:]*: serial" | tail -1)
		[ -n "$reg" ] && break
	done
	k=$(journalctl -k -q -o short-unix --after-cursor "$cur" | grep "snd_clarett $BDF")
	# a rebind logs no "enabling device"; time from the address ack instead
	en=$(grep "readiness: response address acknowledged" <<<"$k" | head -1)
	ack=$(sed -n 's/.*acknowledged after \([0-9]*\) us.*/\1 us/p' <<<"$k" | tail -1)
	a=$(sed -n 's/.*answered on attempt \([0-9]*\).*/\1/p' <<<"$k" | tail -1)
	{
		echo "-- rebind $i: ack ${ack:-none}  attempt ${a:-1}  $( [ -n "$reg" ] && [ -n "$en" ] && echo "ack -> registered $(ms "$(ts "$en")" "$(ts "$reg")") ms" || echo "NOT REGISTERED")"
		grep -E "never landed|not acknowledged|refus|-110|error" <<<"$k" | sed 's/^/   /'
	} | tee -a "$LOG"
	sleep 3
done
