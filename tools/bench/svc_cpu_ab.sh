#!/bin/bash
# A/B the stream servicer's CPU cost between two builds of the module, on one
# host and one unit, which is the only basis for comparing them:
#   sudo bash svc_cpu_ab.sh <card id> <a.ko> <b.ko> [secs] [periods...]
# e.g. sudo bash svc_cpu_ab.sh R8Line /tmp/poll/snd-clarett.ko snd-clarett/snd-clarett.ko
# For each module in turn it reloads it, then times the clarett-svc thread
# (/proc/<tid>/stat, utime+stime) across a capture at each ALSA period size, and
# reports %CPU of one core with the device period rate beside it. The card is
# looked up by its id, since a reload can hand it another number, and the
# capture width comes from the card itself.
# This reloads the module with the unit powered on and nothing holding the card
# (as the rebind tests do); it does not power-cycle the unit, so the
# Thunderbolt reconnect race is not in play. Whichever module is named last is
# the one left loaded. Log: ./svc_cpu_ab.log
set -u
ID=${1:?usage: $0 <card id> <a.ko> <b.ko> [secs] [periods...]}
KO_A=${2:?usage: $0 <card id> <a.ko> <b.ko> [secs] [periods...]}
KO_B=${3:?usage: $0 <card id> <a.ko> <b.ko> [secs] [periods...]}
SECS=${4:-15}
shift 4 2>/dev/null || shift $#
PERIODS=${*:-"512 64"}
# A leading ~ survives `sudo BENCH_LOG_DIR=~/dir` under zsh (it tilde-expands
# assignments, not arguments), and an unwritable directory would otherwise lose
# the whole run to tee errors: expand it, then fall back to the cwd.
DIR=${BENCH_LOG_DIR:-$PWD}
case $DIR in
~/*) DIR=$HOME/${DIR#\~/};;
"~") DIR=$HOME;;
esac
if ! { [ -d "$DIR" ] && [ -w "$DIR" ]; }; then
	echo "log directory $DIR is not writable; logging to $PWD instead" >&2
	DIR=$PWD
fi
LOG=$DIR/svc_cpu_ab.log
[ "$(id -u)" = 0 ] || { echo "needs root (module reloads)"; exit 1; }
for k in "$KO_A" "$KO_B"; do
	[ -f "$k" ] || { echo "no such module: $k"; exit 1; }
done

card_of() {	# the card number currently holding this id, or empty
	local n
	for n in /proc/asound/card[0-9]*; do
		[ -r "$n/id" ] && [ "$(cat "$n/id")" = "$ID" ] && {
			basename "$n" | tr -dc 0-9
			return
		}
	done
}

# Stop the services that hold the card. Anything ELSE still holding it is
# reported and the run stops: killing it blind could take out the user's
# PipeWire, which keeps control devices open in some configurations.
holders_stop() {
	local c=$1 who
	systemctl stop "fcp-server@$c" 2>/dev/null
	systemctl stop alsa-state.service 2>/dev/null
	sleep 0.5
	who=$(fuser -v /dev/snd/controlC"$c" /dev/snd/pcmC"$c"D0* \
		/dev/snd/hwC"$c"D0 2>&1 | tail -n +2)
	[ -z "$who" ] || {
		echo "still holding card $c, stop it first:" >&2
		echo "$who" >&2
		return 1
	}
}

reload() {	# reload <module path>; echoes the new card number
	local ko=$1 c i
	c=$(card_of)
	[ -n "$c" ] && { holders_stop "$c" || return 1; }
	rmmod snd_clarett 2>/dev/null || { echo "rmmod failed" >&2; return 1; }
	insmod "$ko" || { echo "insmod $ko failed" >&2; return 1; }
	for i in $(seq 1 50); do
		c=$(card_of)
		[ -n "$c" ] && { echo "$c"; return; }
		sleep 0.2
	done
	echo "card $ID did not come back after loading $ko" >&2
	return 1
}

# %CPU of one core for the servicer thread across one capture
measure() {	# measure <card> <channels> <period>
	local c=$1 ch=$2 per=$3 tid t0 t1 rc
	arecord -D "hw:$c,0" -c "$ch" -f S32_LE -r 48000 \
		--period-size="$per" --buffer-size=$((per * 4)) \
		-d $((SECS + 4)) > /dev/null 2>&1 &
	rc=$!
	sleep 2
	tid=$(pgrep -x clarett-svc | head -1)
	if [ -z "$tid" ]; then
		wait $rc
		echo "no servicer thread"
		return
	fi
	t0=$(awk '{print $14+$15}' "/proc/$tid/stat")
	sleep "$SECS"
	t1=$(awk '{print $14+$15}' "/proc/$tid/stat" 2>/dev/null || echo "$t0")
	wait $rc
	awk -v a="$t0" -v b="$t1" -v s="$SECS" \
		'BEGIN { printf "%.2f", (b - a) / s }'
}

say() { echo "$*" | tee -a "$LOG"; }

say "== $(date) servicer CPU A/B on $ID, ${SECS}s per cell, periods: $PERIODS"
: > /tmp/svc_cpu_ab.$$
for ko in "$KO_A" "$KO_B"; do
	CARD=$(reload "$ko") || exit 1
	SRC=$(cat /sys/module/snd_clarett/srcversion)
	CH=$(arecord -D "hw:$CARD,0" --dump-hw-params -d 1 /dev/null 2>&1 |
		sed -n 's/^CHANNELS: \([0-9]*\)$/\1/p' | head -1)
	say "-- $(basename "$(dirname "$ko")")/$(basename "$ko") srcversion $SRC, card $CARD, ${CH}ch"
	for per in $PERIODS; do
		cpu=$(measure "$CARD" "$CH" "$per")
		say "$(printf '   period %4s frames: clarett-svc %6s %% of a core' "$per" "$cpu")"
		printf '%s\t%s\t%s\n' "$SRC" "$per" "$cpu" >> /tmp/svc_cpu_ab.$$
	done
done

say "-- summary (% of one core)"
awk -F'\t' '{ cpu[$1" "$2]=$3; if(!($1 in sv)){sv[$1]=++n; sn[n]=$1}
	if(!($2 in pp)){pp[$2]=++m; pn[m]=$2} }
	END {
		printf "   %-26s", "period"
		for (j = 1; j <= m; j++) printf "%10s", pn[j]
		printf "\n"
		for (i = 1; i <= n; i++) {
			printf "   %-26s", sn[i]
			for (j = 1; j <= m; j++) printf "%10s", cpu[sn[i]" "pn[j]]
			printf "\n"
		}
	}' /tmp/svc_cpu_ab.$$ | tee -a "$LOG"
rm -f /tmp/svc_cpu_ab.$$
# alsa-state was stopped to free the card; fcp-server is relaunched by udev
systemctl start alsa-state.service 2>/dev/null
say "   (left loaded: $(cat /sys/module/snd_clarett/srcversion))"
