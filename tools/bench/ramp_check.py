#!/usr/bin/python3 -I
# ramp_check.py gen <file> <frames> <channels> <ramp_ch>   write an S32_LE ramp (n*256) on ramp_ch (1-based)
# ramp_check.py check <file> <channels> <expect_ch> <dead_chs, comma list or ->
import sys, array

def gen(path, frames, chans, rch):
	a = array.array('i', bytes(4 * frames * chans))
	for n in range(frames):
		v = ((n + 1) * 256) & 0xffffffff
		a[n * chans + rch - 1] = v - (1 << 32) if v >= 1 << 31 else v
	open(path, 'wb').write(a.tobytes())

def check(path, chans, exp, dead):
	a = array.array('i'); a.frombytes(open(path, 'rb').read())
	frames = len(a) // chans
	cols = [a[c::chans][:frames] for c in range(chans)]
	ramp = []
	for c, col in enumerate(cols):
		good = sum(1 for i in range(1, frames) if (col[i] - col[i - 1]) & 0xffffffff == 256)
		if good > frames // 10:
			ramp.append(c + 1)
	col = cols[exp - 1]
	idx = [i for i in range(1, frames) if (col[i] - col[i - 1]) & 0xffffffff == 256]
	out = [f"ramp on ch {ramp or 'none'} (expected {exp})"]
	breaks = []
	if idx:
		s, e = idx[0] - 1, idx[-1]
		breaks = [i for i in range(s + 1, e + 1) if (col[i] - col[i - 1]) & 0xffffffff != 256]
		out.append(f"ch {exp}: run {e - s} frames, {len(breaks)} breaks"
			   + (f" (first at {breaks[0]}: {col[breaks[0]-1]:#x} -> {col[breaks[0]]:#x})" if breaks else ""))
		tail = [v for v in col[e + 1:] if v]
		out.append(f"after ramp: {len(tail)} non-zero samples")
	nz = {}
	if dead:
		nz = {c: sum(1 for v in cols[c - 1] if v) for c in dead}
		out.append("dead chs " + ",".join(map(str, dead)) + ": "
			   + ("all zero" if not any(nz.values()) else f"NON-ZERO {nz}"))
	ok = ramp == [exp] and idx and not breaks and not any(nz.values())
	print(("PASS  " if ok else "FAIL  ") + "; ".join(out))

if sys.argv[1] == 'gen':
	gen(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]))
else:
	d = sys.argv[5]
	check(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]),
	      [] if d == '-' else [int(x) for x in d.split(',')])
