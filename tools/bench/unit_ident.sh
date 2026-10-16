#!/bin/bash
# Read-only identity snapshot of one unit. Run as root:  sudo bash unit_ident.sh <card id or number>
# e.g.  sudo bash unit_ident.sh C8PreX      (the id from /proc/asound/cards, or the card number)
# Stops that card's fcp-server (it holds the hwdep), then reads:
#   appspace bytes 0-127 (GET_DATA): App version @8, FPGA word @12 (== BAR 0x000), the FPGA `type` @16,
#     and the text name from @80;
#   0x800005 {0, 8}, the vendor's first init command;
#   the first 32 bytes of flash segment 4 (App_Env, "SERIAL_NO = <digits>") and of segment 3
#     (FPGA_Upgrade, whose bitstream header names the FPGA design);
# then restarts the server. Nothing is written to the device. Needs ../fcp_cmd.c built at
# $BENCH_LOG_DIR/fcp_cmd (gcc -O2 -o fcp_cmd tools/fcp_cmd.c). Log: ./unit_ident.log
set -u
A=${1:?usage: $0 <card id or number>}
DIR=${BENCH_LOG_DIR:-$PWD}
LOG=$DIR/unit_ident.log
CMD=$DIR/fcp_cmd
[ -x "$CMD" ] || { echo "build $CMD first: gcc -O2 -o $CMD tools/fcp_cmd.c"; exit 1; }
if [ -e /proc/asound/card$A ]; then CARD=$A; else
	CARD=$(grep -lx "$A" /proc/asound/card*/id 2>/dev/null | head -1 | sed 's#.*/card\([0-9]*\)/id#\1#')
fi
[ -n "${CARD:-}" ] && [ -e /dev/snd/hwC${CARD}D0 ] || { echo "no Clarett card '$A'"; exit 1; }
ID=$(cat /proc/asound/card$CARD/id)
HW=/dev/snd/hwC${CARD}D0
le32() { printf '%02x %02x %02x %02x ' $(($1 & 255)) $((($1 >> 8) & 255)) $((($1 >> 16) & 255)) $((($1 >> 24) & 255)); }
run() {	# label, opcode, resp_len, request bytes...
	local l=$1; shift
	echo "## $l"
	"$CMD" "$HW" "$@" 2>&1 | sed -n '/hex:/,/u32:/p' | grep -v -e 'hex:' -e 'u32:'
	"$CMD" "$HW" "$@" 2>&1 | sed -n 's/^  ascii: /   ascii: /p'
}
systemctl stop "fcp-server@$CARD"
trap 'systemctl start "fcp-server@$CARD"' EXIT
{
	echo "== $(date) unit ident card $CARD $ID ($(basename "$(readlink -f /sys/class/sound/card$CARD/device)"))"
	run "appspace 0-127" 0x800000 128 00 00 00 00 80 00 00 00
	run "0x800005 {0, 8}" 0x800005 32 00 00 00 00 08 00 00 00
	run "flash seg 4 (App_Env) +0" 0x004005 32 $(le32 4) 00 00 00 00 20 00 00 00
	run "flash seg 3 (FPGA_Upgrade) +0" 0x004005 32 $(le32 3) 00 00 00 00 20 00 00 00
} | tee -a "$LOG"
