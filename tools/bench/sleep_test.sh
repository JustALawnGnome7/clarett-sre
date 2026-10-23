#!/bin/bash
# System sleep and resume, with or without a card. Run as root:  sudo bash sleep_test.sh <card|none> [mode] [secs]
#   mode: s2idle (suspend to idle) or deep (S3), set in /sys/power/mem_sleep for the run and restored after;
#   default deep. Or hibernate (`systemctl hibernate`, so the image goes to the disk swap and the host powers
#   off): needs a disk swap and resume= on the kernel command line; if the firmware ignores the RTC alarm in
#   that state, wake it with the power button. secs: how long to sleep (default 20). Sleep goes through `systemctl suspend`, so the hooks
#   systemd runs (nvidia-suspend/-resume on a host with NVIDIA's driver) run too; writing the kernel's sleep
#   state directly, as `rtcwake -m mem` does, skips them, and on such a host the desktop comes back frozen.
#   An RTC alarm (`rtcwake -m no`) wakes the host. "none" runs the same sleep with no card checks: the
#   baseline for a host that has not been shown to resume.
# Before and after: the card's stream states, Sync Status, whether fcp-server is up. After: a Level Meter
# read (a GET_METER through the hwdep, so it proves the mailbox answers again), whether the PCI device was
# re-enumerated (removed and probed again rather than resumed), and every snd_clarett, thunderbolt and
# fcp-server line logged across the sleep.
# Leave PipeWire running to test a held stream: it should go back to RUNNING once it re-prepares.
# Log: ./sleep_test.log
set -u
CARD=${1:?usage: $0 <card|none> [s2idle|deep] [secs]}
MODE=${2:-deep}
SECS=${3:-20}
LOG=${BENCH_LOG_DIR:-$PWD}/sleep_test.log
say() { echo "$*" | tee -a "$LOG"; }
streams() { grep -h '^state' /proc/asound/card$CARD/pcm*/sub*/status 2>/dev/null | tr '\n' ' '; }
sync_status() {
	local out v
	out=$(amixer -c "$CARD" cget name='Sync Status' 2>/dev/null) || { echo "unreadable"; return; }
	v=$(sed -n 's/.*: values=//p' <<<"$out")
	sed -n "s/.*Item #$v '\(.*\)'$/\1/p" <<<"$out"
}
state() {
	say "  card:     $(cat /proc/asound/card$CARD/id 2>/dev/null || echo GONE)"
	say "  streams:  $(streams)"
	say "  sync:     $(sync_status)"
	say "  fcp-srv:  $(systemctl is-active fcp-server@$CARD.service)"
}

if [ "$MODE" = hibernate ]; then
	grep -qw disk /sys/power/state || { echo "hibernation not offered: $(cat /sys/power/state)"; exit 1; }
	EXIT_MARK='PM: hibernation: hibernation exit'
else
	grep -qw "$MODE" /sys/power/mem_sleep || { echo "mode '$MODE' not offered: $(cat /sys/power/mem_sleep)"; exit 1; }
	EXIT_MARK='PM: suspend exit'
fi
: > "$LOG"
if [ "$CARD" = none ]; then
	say "no card (baseline), mode $MODE, ${SECS}s, $(date -Is)"
else
	[ -e /proc/asound/card$CARD ] || { echo "no card $CARD"; exit 1; }
	BDF=$(basename "$(readlink -f /sys/class/sound/card$CARD/device)")
	say "card $CARD $(cat /proc/asound/card$CARD/id) at $BDF, mode $MODE, ${SECS}s, $(date -Is)"
	say "module srcversion $(cat /sys/module/snd_clarett/srcversion)"
	say "before:"
	state
fi
sync
OLD_SLEEP=$(sed -n 's/.*\[\(.*\)\].*/\1/p' /sys/power/mem_sleep)
[ "$MODE" = hibernate ] || echo "$MODE" > /sys/power/mem_sleep
T0=$(date '+%Y-%m-%d %H:%M:%S')
rtcwake -m no -s "$SECS" >/dev/null || { echo "$OLD_SLEEP" > /sys/power/mem_sleep; say "rtcwake failed"; exit 1; }
if [ "$MODE" = hibernate ]; then systemctl hibernate; else systemctl suspend; fi
# systemctl returns once the job is queued; wait for the kernel to report the resume.
for _ in $(seq $((SECS + 120))); do
	journalctl -k --since "$T0" | grep -q "$EXIT_MARK" && break
	sleep 1
done
echo "$OLD_SLEEP" > /sys/power/mem_sleep
journalctl -k --since "$T0" | grep -q "$EXIT_MARK" || say "NO RESUME logged within $((SECS + 120)) s"
sleep 5
if [ "$CARD" = none ]; then
	say "resumed"
	journalctl --since "$T0" -o short-precise --no-pager | grep -E "PM: (suspend|resume|hibernation|Image)|ACPI: PM|nvidia" | tee -a "$LOG"
	exit 0
fi
say "after (5 s):"
state
if amixer -c "$CARD" cget iface=PCM,name='Level Meter' >/dev/null 2>&1; then
	say "  meter:    read OK"
else
	say "  meter:    READ FAILED"
fi
if journalctl -k --since "$T0" | grep -q "$BDF: enabling device"; then
	say "  device:   RE-ENUMERATED (probed again, not resumed)"
else
	say "  device:   resumed in place"
fi
say "kernel and fcp-server log across the sleep:"
journalctl --since "$T0" -o short-precise --no-pager | grep -E "snd_clarett|thunderbolt|fcp-server|PM: (suspend|resume|hibernation|Image)|ACPI: PM" \
	| grep -v 'GET_METER' | tee -a "$LOG"
