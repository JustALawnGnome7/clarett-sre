#!/bin/bash
# Thunderbolt reconnect race test: authorize by hand once the unit's power-on link bounce is over.
# Run as root:  sudo bash tb_manual_auth.sh <bolt uuid> [N] [hold_s]
# Sets the unit's boltd policy to "manual" (restored at exit), then per power cycle: waits for the
# unit to appear, logs every disappearance (the bounce), and once it has been present without a
# break for hold_s (default 2.5) runs `boltctl authorize`; then checks the PCIe endpoint appears and
# snd_clarett registers. Get the uuid from `boltctl list`. Log: ./tb_manual_auth.log
set -u
UUID=${1:?usage: $0 <bolt uuid> [N] [hold_s]}; N=${2:-5}; HOLD=${3:-2.5}
LOG=${BENCH_LOG_DIR:-$PWD}/tb_manual_auth.log
OLD=$(boltctl config device.policy "$UUID") || { echo "no bolt device $UUID"; exit 1; }
trap 'boltctl config device.policy "$UUID" "$OLD"; echo "policy restored to $OLD"' EXIT
boltctl config device.policy "$UUID" manual

node() { grep -lx "$UUID" /sys/bus/thunderbolt/devices/*/unique_id 2>/dev/null | head -1 | xargs -r dirname; }
now() { date +%s.%N; }
ms() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.0f", (b-a)*1000}'; }
pci_up() { lspci -d 1cb5: | grep -q .; }

echo "== $(date) manual-auth power cycles x$N, hold ${HOLD}s, $UUID (was policy $OLD)" | tee -a "$LOG"
for i in $(seq 1 "$N"); do
	echo ">>> Cycle $i/$N: power the unit OFF."
	while [ -n "$(node)" ]; do sleep 0.1; done
	sleep 2
	echo ">>> Now power it back ON."
	while [ -z "$(node)" ]; do sleep 0.02; done
	t0=$(now); since=$t0; ev=""
	while :; do
		sleep 0.02
		if [ -z "$(node)" ]; then
			ev="$ev gone@$(ms "$t0" "$(now)")"
			while [ -z "$(node)" ]; do
				sleep 0.02
				[ "$(ms "$t0" "$(now)")" -gt 20000 ] && break 2
			done
			since=$(now); ev="$ev back@$(ms "$t0" "$since")"
		fi
		[ "$(ms "$since" "$(now)")" -ge "$(awk -v h="$HOLD" 'BEGIN{print h*1000}')" ] && break
	done
	d=$(node)
	if [ -z "$d" ]; then
		echo "-- cycle $i: unit gone and not back within 20 s ($ev )" | tee -a "$LOG"
		echo ">>> Replug the TB2 cable at the unit, then press Enter."; read -r
		sleep 1; boltctl authorize "$UUID" >/dev/null 2>&1	# manual policy: nothing else will
		continue
	fi
	pre=$(cat "$d/authorized")
	ta=$(now); boltctl authorize "$UUID" >/dev/null 2>&1; arc=$?
	up=no
	for _ in $(seq 1 50); do pci_up && { up=yes; break; }; sleep 0.1; done
	sleep 1
	reg=$(journalctl -k -q --since "-15s" | grep -c "snd_clarett .*: serial")
	echo "-- cycle $i: found, then:${ev:- no bounce}; authorized=$pre before; authorize @$(ms "$t0" "$ta") ms rc $arc; PCIe $up; registered $reg" | tee -a "$LOG"
	if [ $up = no ]; then
		journalctl -k -q --since "-10s" | grep -E "thunderbolt|tunnel" | tail -5 | sed 's/^/   /' | tee -a "$LOG"
		echo ">>> Not up. Replug the TB2 cable at the unit, then press Enter."; read -r
		sleep 1; boltctl authorize "$UUID" >/dev/null 2>&1
	fi
done
