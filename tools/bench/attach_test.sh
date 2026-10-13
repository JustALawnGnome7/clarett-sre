#!/bin/bash
# Cold-attach timing over N power cycles. Run as root:  sudo bash attach_test.sh [N]
# Per cycle: enable -> response-address acknowledgement -> first answer (attempt n) -> card
# registered, plus how many times the unit appeared on Thunderbolt. Log: ./attach_test.log
set -u
N=${1:-3}
LOG=${BENCH_LOG_DIR:-$PWD}/attach_test.log
DD=/sys/kernel/debug/dynamic_debug/control
cursor() { journalctl -k -n0 --show-cursor | sed -n 's/^-- cursor: //p'; }
trap 'echo "format \"readiness:\" -p" > $DD' EXIT
echo 'format "readiness:" +p' > $DD

D=$(dirname "$(readlink -f "$0")")

echo "== $(date) cold attach x$N" | tee -a "$LOG"
for i in $(seq 1 "$N"); do
	cur=$(cursor)
	echo ">>> Cycle $i/$N: power the unit OFF, wait ~5 s, power it back ON."
	reg=""
	for _ in $(seq 1 240); do
		sleep 0.5
		reg=$(journalctl -k -q -o short-unix --after-cursor "$cur" | grep -E "snd_clarett .*: Clarett [^:]*: serial|snd_clarett .*: Red [^:]*: serial" | tail -1)
		[ -n "$reg" ] && break
	done
	sleep 1
	k=$(journalctl -k -q -o short-unix --after-cursor "$cur")
	if [ -z "$reg" ]; then
		echo "-- cycle $i: no registration within 120 s" | tee -a "$LOG"
		grep -E "snd_clarett|thunderbolt" <<<"$k" | tail -10 | sed 's/^/   /' | tee -a "$LOG"
		continue
	fi
	bdf=$(sed -n 's/.*snd_clarett \([0-9a-f:.]*\): .*/\1/p' <<<"$reg")
	att=$(grep "snd_clarett $bdf: readiness: answered on attempt" <<<"$k" | tail -1)
	tb=$(grep -c "thunderbolt.*new device found" <<<"$k")
	{
		echo "-- cycle $i ($bdf): TB appearances $tb"
		echo "   $(journalctl -k -q -o json --after-cursor "$cur" | python3 -I "$D/kmsg_times.py" "$bdf" | tail -1)"
		a=$(sed -n 's/.*attempt \([0-9]*\).*/\1/p' <<<"$att")
		echo "   attempt:  ${a:-1}"	# only a retry is logged
		grep "$bdf" <<<"$k" | grep -E "never landed|not acknowledged|refus|-110" | sed 's/^/   /'
	} | tee -a "$LOG"
done
