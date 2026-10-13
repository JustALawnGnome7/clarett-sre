#!/bin/bash
# Read STREAM_INFO (0x007001) bands 0-3. Run as root:  sudo FCP_CMD=<path> bash stream_info.sh <card>
# Needs tools/fcp_cmd.c built (gcc -O2 -o fcp_cmd tools/fcp_cmd.c); FCP_CMD defaults to ./fcp_cmd.
# Stops fcp-server@<card> for the duration (it holds the hwdep) and restarts it. Log: ./stream_info.log
set -u
CARD=${1:?usage: $0 <card>}
FCP_CMD=${FCP_CMD:-./fcp_cmd}
[ -x "$FCP_CMD" ] || { echo "no fcp_cmd at $FCP_CMD (set FCP_CMD)"; exit 1; }
LOG=${BENCH_LOG_DIR:-$PWD}/stream_info.log
trap 'systemctl start fcp-server@$CARD' EXIT
systemctl stop fcp-server@$CARD
sleep 0.5
echo "== $(date) STREAM_INFO card $CARD ($(cat /proc/asound/card$CARD/id))" | tee -a "$LOG"
for b in 0 1 2 3; do
	echo "-- band $b" | tee -a "$LOG"
	"$FCP_CMD" /dev/snd/hwC${CARD}D0 0x007001 20 0$b 2>&1 | sed 's/^/   /' | tee -a "$LOG"
done
