#!/usr/bin/python3 -I
# Streaming digital-loopback ramp, for runs too long to hold in a file.
#   duplex_ramp.py gen <frames> <channels> <ramp_ch>   S32_LE ramp ((n+1)*256) on ramp_ch (1-based) -> stdout
#   duplex_ramp.py check <channels> <ch> [expect_frames] S32_LE capture on stdin -> one result line
# The check follows channel ch: silence, then the ramp, then silence. It reports the first ramp value
# (256 = no start loss), the run length, every step that is not +256 inside the run, and any non-zero
# sample after the run ended. Steps wrap mod 2^32, so a ramp longer than 2^24 frames is fine.
import sys, array

M = 0xffffffff
CHUNK = 4800

def gen(frames, chans, rch):
	out = sys.stdout.buffer
	n = 0
	while n < frames:
		k = min(CHUNK, frames - n)
		a = array.array('I', bytes(4 * k * chans))
		a[rch - 1::chans] = array.array('I', (((n + i + 1) * 256) & M for i in range(k)))
		out.write(a.tobytes())
		n += k

def check(chans, ch, expect):
	inp = sys.stdin.buffer
	fb = 4 * chans
	pend = b''
	idx = 0			# frame index of the next sample
	state = 0		# 0 before the ramp, 1 in it, 2 after it
	first = first_at = last_at = prev = None
	breaks = []
	tail = 0
	while True:
		buf = inp.read(fb * CHUNK)
		if not buf:
			break
		buf = pend + buf
		n = len(buf) // fb
		pend = buf[n * fb:]
		col = array.array('I', buf[:n * fb])[ch - 1::chans]
		for v in col:
			if state == 0:
				if v:
					state, first, first_at, last_at = 1, v, idx, idx
			elif state == 1:
				if (v - prev) & M == 256:
					last_at = idx
				elif v == 0:
					state = 2
				else:
					if len(breaks) < 5:
						breaks.append(f"{idx}:{prev:#x}->{v:#x}")
					else:
						breaks.append(None)
					last_at = idx
			elif v:
				tail += 1
			prev = v
			idx += 1
	if first is None:
		print(f"FAIL  {idx} frames captured, no ramp on ch {ch}")
		return
	run = last_at - first_at + 1
	nb = len(breaks)
	ok = nb == 0 and tail == 0 and (expect is None or (first == 256 and run == expect))
	s = (f"{'PASS' if ok else 'FAIL'}  captured {idx}; ramp from {first // 256} (start loss {first // 256 - 1}),"
	     f" run {run}" + (f" of {expect}" if expect else "") + f", {nb} breaks"
	     + (" (" + " ".join(b for b in breaks if b) + ")" if nb else "") + f", {tail} non-zero after")
	print(s)

if sys.argv[1] == 'gen':
	gen(int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]))
else:
	check(int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]) if len(sys.argv) > 4 else None)
