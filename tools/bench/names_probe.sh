#!/bin/bash
# names_probe.sh <card> — read-only probe of FCP 0x005000 (the name table).
# Sends ONLY 0x005000 (the vendor's own read). Stops fcp-server for the card,
# turns on the driver's FCPr trace so the device's true reply size is logged,
# then restores both. Run as root. Needs tools/fcp_cmd.c built (FCP_CMD, default
# $BENCH_LOG_DIR/fcp_cmd). Output: $BENCH_LOG_DIR/names_<cardid>.log
set -u
card=${1:?usage: names_probe.sh <card number>}
hw=/dev/snd/hwC${card}D0
dir=${BENCH_LOG_DIR:-$PWD}
cmd=${FCP_CMD:-$dir/fcp_cmd}
[ -x "$cmd" ] || { echo "build $cmd first: gcc -O2 -o $cmd tools/fcp_cmd.c (or set FCP_CMD)"; exit 1; }
id=$(cat /proc/asound/card${card}/id)
bdf=$(basename "$(readlink -f /sys/class/sound/card${card}/device)")
out=$dir/names_${id}.log
dd=/sys/kernel/debug/dynamic_debug/control

systemctl stop "fcp-server@${card}" 2>/dev/null
sleep 0.5
echo 'format "FCPr" +p' > $dd
start=$(date '+%Y-%m-%d %H:%M:%S')
sleep 1

{
echo "# card $card id $id bdf $bdf"
echo "## A: ids 0x000-0x100, {u16 id}, 64-byte reply"
for i in $(seq 0 256); do
	$cmd $hw 0x005000 64 $(printf '%02x %02x' $((i & 255)) $((i >> 8)))
done
echo "## B: request shapes, ids 0x01 and 0x0d (then 0x12 for a Red's Input 1)"
for x in 01 0d 12; do
	$cmd $hw 0x005000 64
	$cmd $hw 0x005000 64 $x
	$cmd $hw 0x005000 64 $x 00 00 00
	$cmd $hw 0x005000 64 $x 00 01 00
	$cmd $hw 0x005000 64 $x 00 02 00
	$cmd $hw 0x005000 64 $x 00 ff ff
	$cmd $hw 0x005000 64 $x 00 00 00 00 00 00 00
	$cmd $hw 0x005000 64 $x 00 00 00 01 00 00 00
done
echo "## C: high ids"
for v in "ff 00" "00 01" "ff 7f" "00 80" "ff ff"; do
	$cmd $hw 0x005000 64 $v
done
echo "## D: reply size asked 4 / 8 / 256"
for n in 4 8 256; do $cmd $hw 0x005000 $n 01 00; done
} > "$out" 2>&1

sleep 1
echo 'format "FCPr" -p' > $dd
echo "## FCPr trace (op 0x005000)" >> "$out"
journalctl -k --since "$start" -o cat | grep "$bdf" \
	| grep -E "FCPr (op=0x005000|  payload)" >> "$out"
systemctl start "fcp-server@${card}" 2>/dev/null
echo "wrote $out ($(grep -c '^op=' "$out") commands, $(grep -c 'FCPr op=0x005000' "$out") traced)"
