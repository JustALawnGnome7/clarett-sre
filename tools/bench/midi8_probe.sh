#!/bin/bash
# midi8_probe.sh <card> [opcode] [all|empty] — probe of a category-8 opcode
# (default 0x008001 all; never sent by the vendor). "empty" sends only the empty request.
# 0x008001 is a read (MIDI In port list, measured); 0x008002 is unproven.
# Sends only that opcode, with 0x008000 (MIDI_INFO, a known read) before and after
# as a sanity check. Stops fcp-server for the card, turns on the driver's FCPr
# trace so the true reply size and status are logged, then restores both.
# Run as root. Needs tools/fcp_cmd.c built (FCP_CMD, default $BENCH_LOG_DIR/fcp_cmd).
# Output: $BENCH_LOG_DIR/midi8_<cardid>_<op>_<mode>.log
set -u
card=${1:?usage: midi8_probe.sh <card number> [opcode] [all|empty]}
op=${2:-0x008001}
mode=${3:-all}
case $op in 0x00800[12]) ;; *) echo "only 0x008001/0x008002"; exit 2;; esac
case $mode in all|empty) ;; *) echo "mode: all or empty"; exit 2;; esac
hw=/dev/snd/hwC${card}D0
dir=${BENCH_LOG_DIR:-$PWD}
cmd=${FCP_CMD:-$dir/fcp_cmd}
[ -x "$cmd" ] || { echo "build $cmd first: gcc -O2 -o $cmd tools/fcp_cmd.c (or set FCP_CMD)"; exit 1; }
id=$(cat /proc/asound/card${card}/id)
bdf=$(basename "$(readlink -f /sys/class/sound/card${card}/device)")
out=$dir/midi8_${id}_${op#0x00}_${mode}.log
dd=/sys/kernel/debug/dynamic_debug/control

systemctl stop "fcp-server@${card}" 2>/dev/null
sleep 0.5
echo 'format "FCPr" +p' > $dd
start=$(date '+%Y-%m-%d %H:%M:%S')
sleep 1

q() { echo "# $*"; $cmd $hw "$@"; }
{
echo "# card $card id $id bdf $bdf"
echo "## before: 0x008000"
q 0x008000 64
echo "## $op request shapes ($mode)"
q $op 256
if [ "$mode" = all ]; then
q $op 256 00
q $op 256 01
q $op 256 00 00
q $op 256 01 00
q $op 256 00 00 00 00
q $op 256 01 00 00 00
q $op 256 00 00 00 00 01 00
q $op 256 00 00 00 00 08 00
q $op 256 01 00 00 00 08 00
q $op 256 00 00 00 00 00 00 00 00
fi
echo "## after: 0x008000"
q 0x008000 64
} > "$out" 2>&1

sleep 1
echo 'format "FCPr" -p' > $dd
echo "## FCPr trace (category 8)" >> "$out"
journalctl -k --since "$start" -o cat | grep "$bdf" \
	| awk '/FCPr op=0x00800/ {print; k=1; next} /FCPr   payload/ && k {print} {k=0}' >> "$out"
echo "## other kernel lines from the device during the run" >> "$out"
journalctl -k --since "$start" -o cat | grep "$bdf" | grep -v "FCPr" >> "$out"
systemctl start "fcp-server@${card}" 2>/dev/null
echo "wrote $out ($(grep -c '^op=' "$out") commands, $(grep -c 'FCPr op=0x00800' "$out") traced)"
