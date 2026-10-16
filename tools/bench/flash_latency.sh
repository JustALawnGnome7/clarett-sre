#!/bin/bash
# Flash read latency. Run as root:  sudo bash flash_latency.sh <card id or number> [N] [segment] [len]
# Stops that card's fcp-server (it holds the hwdep), then times N back-to-back FLASH_READs
# (0x004005 {segment, 0, len}; default 200 reads of 64 bytes from segment 4, App_Env) through the
# driver, and reports min/median/p90/p99/max wall time per read, how many exceeded 100 ms (the normal
# response deadline) and how many failed. Read-only. Needs tools/fcp_cmd.c built at
# $BENCH_LOG_DIR/fcp_cmd. The wall time includes ~1 ms of process start-up per read.
# Log: ./flash_latency.log
set -u
A=${1:?usage: $0 <card id or number> [N] [segment] [len]}; N=${2:-200}; SEG=${3:-4}; LEN=${4:-64}
DIR=${BENCH_LOG_DIR:-$PWD}
LOG=$DIR/flash_latency.log
CMD=$DIR/fcp_cmd
[ -x "$CMD" ] || { echo "build $CMD first: gcc -O2 -o $CMD tools/fcp_cmd.c"; exit 1; }
if [ -e /proc/asound/card$A ]; then CARD=$A; else
	CARD=$(grep -lx "$A" /proc/asound/card*/id 2>/dev/null | head -1 | sed 's#.*/card\([0-9]*\)/id#\1#')
fi
[ -n "${CARD:-}" ] && [ -e /dev/snd/hwC${CARD}D0 ] || { echo "no Clarett card '$A'"; exit 1; }
le32() { printf '%02x %02x %02x %02x ' $(($1 & 255)) $((($1 >> 8) & 255)) $((($1 >> 16) & 255)) $((($1 >> 24) & 255)); }
REQ="$(le32 "$SEG")$(le32 0)$(le32 "$LEN")"
systemctl stop "fcp-server@$CARD"
trap 'systemctl start "fcp-server@$CARD"' EXIT
W=$(mktemp)
fails=0
for i in $(seq 1 "$N"); do
	t0=$(date +%s%N)
	if "$CMD" /dev/snd/hwC${CARD}D0 0x004005 "$LEN" $REQ 2>&1 | grep -q FAILED; then fails=$((fails + 1)); fi
	echo $(( ($(date +%s%N) - t0) / 1000 )) >> "$W"
done
{
	echo "== $(date) flash latency card $CARD $(cat /proc/asound/card$CARD/id): $N x FLASH_READ seg $SEG len $LEN (srcversion $(cat /sys/module/snd_clarett/srcversion))"
	sort -n "$W" | awk -v f="$fails" '{v[NR]=$1} END {
		printf "   min %.1f  median %.1f  p90 %.1f  p99 %.1f  max %.1f ms;  >100 ms: ", v[1]/1000, v[int(NR/2)+1]/1000, v[int(NR*0.9)]/1000, v[int(NR*0.99)]/1000, v[NR]/1000
		n=0; for (i=1;i<=NR;i++) if (v[i]>100000) n++; printf "%d;  failed: %d\n", n, f }'
	journalctl -k -o cat --since "-$(( N / 5 + 30 ))s" | grep -E "snd_clarett .*(never landed|timed out)" | tail -5 | sed 's/^/   /'
} | tee -a "$LOG"
rm -f "$W"
