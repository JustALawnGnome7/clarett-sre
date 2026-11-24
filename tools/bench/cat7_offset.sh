#!/bin/bash
# cat7_offset.sh <card> — is the channel-list request {u16 band, u16 first, u16 count}?
# Sends only 0x007002/0x007003 (the vendor's reads) with bytes 2-3 (a "first channel"
# field, if there is one) and the count varied. Stops fcp-server for the card, turns
# on the driver's FCPr trace so each reply's true size is logged, restores both.
# Run as root. Needs tools/fcp_cmd.c built (FCP_CMD, default $BENCH_LOG_DIR/fcp_cmd).
# Output: $BENCH_LOG_DIR/cat7off_<cardid>.log (summarise with cat7_sum.py)
set -u
card=${1:?usage: cat7_offset.sh <card number>}
hw=/dev/snd/hwC${card}D0
dir=${BENCH_LOG_DIR:-$PWD}
cmd=${FCP_CMD:-$dir/fcp_cmd}
[ -x "$cmd" ] || { echo "build $cmd first: gcc -O2 -o $cmd tools/fcp_cmd.c (or set FCP_CMD)"; exit 1; }
id=$(cat /proc/asound/card${card}/id)
bdf=$(basename "$(readlink -f /sys/class/sound/card${card}/device)")
out=$dir/cat7off_${id}.log
dd=/sys/kernel/debug/dynamic_debug/control

systemctl stop "fcp-server@${card}" 2>/dev/null
sleep 0.5
echo 'format "FCPr" +p' > $dd
start=$(date '+%Y-%m-%d %H:%M:%S')
sleep 1

q() { echo "# $*"; $cmd $hw "$@"; }
{
echo "# card $card id $id bdf $bdf"
echo "## A: request shapes (idle)"
for op in 0x007002 0x007003; do
	q $op 1024 00 00 00 00 40 00	# baseline: band 0, first 0, count 64
	q $op 1024 00 00 08 00 40 00	# first 8, count 64
	q $op 1024 00 00 01 00 40 00	# first 1
	q $op 1024 00 00 08 00 04 00	# first 8, count 4
	q $op 1024 00 00 00 00 04 00	# first 0, count 4
	q $op 1024 00 00 02 00 01 00	# first 2, count 1
	q $op 1024 00 00 0c 00 40 00	# first 12
	q $op 1024 00 00 3c 00 40 00	# first 60
	q $op 1024 00 00 40 00 40 00	# first 64 (past a 64-channel list)
	q $op 1024 00 00 00 01 40 00	# bytes 2-3 = 0x0100
	q $op 1024 01 00 08 00 40 00	# band 1, first 8
	q $op 1024 02 00 04 00 40 00	# band 2, first 4
done
} > "$out" 2>&1

sleep 1
echo 'format "FCPr" -p' > $dd
echo "## FCPr trace (category 7)" >> "$out"
journalctl -k --since "$start" -o cat | grep "$bdf" \
	| awk '/FCPr op=0x00700/ {print; k=1; next} /FCPr   payload/ && k {print} {k=0}' >> "$out"
systemctl start "fcp-server@${card}" 2>/dev/null
echo "wrote $out ($(grep -c '^op=' "$out") commands, $(grep -c 'FCPr op=0x00700' "$out") traced)"
