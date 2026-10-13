#!/bin/bash
# Idle, then a write. Run as root, alsa-scarlett-gui CLOSED:
#   sudo bash idle_write.sh <card> <bdf> [idle_s] [control]
# Traces the mailbox while nothing touches the card (expect ~0 commands), then flips a switch
# control through fcp-server (default 'Line In 1 Air Capture Switch'), checks the write's commands,
# asks whether the front panel followed, and restores the control. Log: ./idle_write.log
set -u
CARD=${1:?usage: $0 <card> <bdf> [idle_s] [control]}; BDF=${2:?usage}; IDLE=${3:-180}
CTL=${4:-Line In 1 Air Capture Switch}
LOG=${BENCH_LOG_DIR:-$PWD}/idle_write.log
DD=/sys/kernel/debug/dynamic_debug/control
cursor() { journalctl -k -n0 --show-cursor | sed -n 's/^-- cursor: //p'; }
trap 'echo "format \"FCP op=\" -p" > $DD' EXIT
summ() {	# $1 = journal text
	l=$(grep "FCP op=.*phase=" <<<"$1")
	echo "   $(grep -c . <<<"$l") commands; by opcode / done / phase:"
	grep -oE "op=0x[0-9a-f]+|done=[01]|phase=0x[0-9a-f]+" <<<"$l" | paste - - - | sort | uniq -c | sed 's/^/     /'
	echo "   never-landed: $(grep -c "never landed" <<<"$1")   timeouts/errors: $(grep -cE -- "-110|timed out" <<<"$1")"
}

# process names are cut to 15 characters, so match the command line
pgrep -f '^[^ ]*alsa-scarlett-gui' >/dev/null && { echo "close alsa-scarlett-gui first"; exit 1; }
echo "== $(date) idle-then-write card $CARD ($BDF), idle ${IDLE}s, '$CTL'" | tee -a "$LOG"

cur=$(cursor)
echo 'format "FCP op=" +p' > $DD
echo ">>> Idle for ${IDLE} s: do not touch the unit or open any mixer."
sleep "$IDLE"
k=$(journalctl -k -q --after-cursor "$cur" | grep "snd_clarett $BDF: ")
{ echo "-- idle ${IDLE} s:"; summ "$k"; } | tee -a "$LOG"

was=$(amixer -c "$CARD" cget name="$CTL" | sed -n 's/.*: values=//p')
new=$([ "$was" = on ] && echo off || echo on)
cur=$(cursor)
amixer -q -c "$CARD" cset name="$CTL" "$new"; rc=$?
sleep 1
k=$(journalctl -k -q --after-cursor "$cur" | grep "snd_clarett $BDF: ")
now=$(amixer -c "$CARD" cget name="$CTL" | sed -n 's/.*: values=//p')
{ echo "-- write: $was -> $new (amixer rc $rc, reads back $now):"; summ "$k"; } | tee -a "$LOG"

read -r -p ">>> Did the front panel show it turn ${new}? [y/n] " ans
echo "   user: turned $new on the front panel: $ans" >> "$LOG"
amixer -q -c "$CARD" cset name="$CTL" "$was"
echo "   restored to $was" | tee -a "$LOG"
