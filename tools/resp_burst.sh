#!/bin/sh
# resp_burst.sh — rapid-fire pmemsave snapshots of the FCP response buffer (wall §7 test 2).
#
# Dumps GPA..GPA+4K every loop iteration (~5-15 Hz; each virsh round-trip is ~50-100 ms)
# into OUTDIR with millisecond-timestamped names, so the sequence sorts chronologically and
# can be cross-correlated with the UTC-stamped vfio trace. Files are written BY THE QEMU
# PROCESS, so OUTDIR must be writable by it (use a fresh dir under /tmp; the dev box runs
# security_driver=none so SELinux won't block it).
#
# Usage: sudo ./resp_burst.sh GPA SECONDS [DOMAIN] [OUTDIR]
#   GPA     response-buffer guest-physical address (from tools/dma_bases.py), e.g. 0x27f440000
#   SECONDS how long to keep snapshotting
#   DOMAIN  libvirt domain (default Windows10)
#   OUTDIR  output directory (default /tmp/respburst)
#
# Analyse with: python3 tools/resp_dump.py --diff OUTDIR/resp_*.bin

set -eu

GPA=${1:?usage: resp_burst.sh GPA SECONDS [DOMAIN] [OUTDIR]}
SECS=${2:?usage: resp_burst.sh GPA SECONDS [DOMAIN] [OUTDIR]}
DOMAIN=${3:-Windows10}
OUTDIR=${4:-/tmp/respburst}

mkdir -p "$OUTDIR"
chmod 777 "$OUTDIR"    # the dump is written by the qemu process, not by us

GPADEC=$(printf '%d' "$GPA")
END=$(( $(date +%s) + SECS ))
N=0
while [ "$(date +%s)" -lt "$END" ]; do
	F="$OUTDIR/resp_$(date +%s%3N).bin"
	virsh qemu-monitor-command "$DOMAIN" \
		"{\"execute\":\"pmemsave\",\"arguments\":{\"val\":$GPADEC,\"size\":4096,\"filename\":\"$F\"}}" \
		>/dev/null
	N=$((N + 1))
done
echo "captured $N snapshots in ${SECS}s -> $OUTDIR (analyse: tools/resp_dump.py --diff $OUTDIR/resp_*.bin)"
