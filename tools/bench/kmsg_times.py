#!/usr/bin/python3 -I
# Attach intervals from KERNEL timestamps. Reads `journalctl -k -o json` on stdin and prints, per
# snd-clarett registration line, the response-address ack time the driver logged and the intervals
# from the PCI enable (or, on a rebind, which logs no enable, from the ack) to the ack and to the
# registration. journald's own receive times are not usable for this: kernel lines are read in
# batches, so a receive-time interval can even come out shorter than the ack inside it.
#   journalctl -k -o json --after-cursor ... | kmsg_times.py [bdf]
import sys, json

bdf = sys.argv[1] if len(sys.argv) > 1 else ""
en = ack = ack_us = None
for line in sys.stdin:
	j = json.loads(line)
	m = j.get("MESSAGE", "")
	if not isinstance(m, str) or "snd_clarett" not in m or bdf not in m:
		continue
	t = int(j.get("_SOURCE_MONOTONIC_TIMESTAMP") or j["__MONOTONIC_TIMESTAMP"]) / 1000.0
	if "enabling device" in m:
		en, ack, ack_us = t, None, None
	elif "acknowledged after" in m:
		ack, ack_us = t, m.split("acknowledged after ")[1].split()[0]
	elif ": serial" in m:
		parts = [f"ack {ack_us or 'none'} us"]
		if en is not None and ack is not None:
			parts.append(f"enable->ack {ack - en:.1f} ms")
		if en is not None:
			parts.append(f"enable->registered {t - en:.1f} ms")
		elif ack is not None:
			parts.append(f"ack->registered {t - ack:.1f} ms")
		print("  ".join(parts))
		en = ack = ack_us = None
