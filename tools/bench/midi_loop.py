#!/usr/bin/env python3
"""DIN MIDI self-loop test: a 5-pin cable from the unit's MIDI Out to its MIDI In.

Writes a known byte stream to the card's rawmidi device and checks what comes back.

  sysex  one long SysEx whose payload is a 7-bit counter (0..127 repeating), so a lost run
         of bytes shows up as a jump in the counter: each loss is logged with its size and
         arrival time, and the intervals between losses are summarised (the 8Pre's losses
         sat on a ~49.7 ms grid).
  notes  note-on/note-off pairs (note = i % 128), compared byte for byte.

--rate paces the writer in bytes/s (the wire carries 3125 B/s); 0 writes as fast as the
driver accepts. Nothing else may hold the rawmidi open (aconnect -l: no subscriptions).

  ./midi_loop.py --card 2 --mode sysex --bytes 6000
  ./midi_loop.py --card 2 --mode sysex --bytes 6000 --rate 1560
  ./midi_loop.py --card 2 --mode notes --count 1500
"""
import argparse
import os
import select
import sys
import threading
import time

WIRE_BPS = 3125


def build(mode, n):
    if mode == "sysex":
        return bytes([0xF0] + [i % 128 for i in range(n)] + [0xF7])
    out = bytearray()
    for i in range(n):
        out += bytes([0x90, i % 128, 64, 0x80, i % 128, 0])
    return bytes(out)


def reader(fd, total, chunks, stop):
    got = 0
    while not stop.is_set() and got < total:
        r, _, _ = select.select([fd], [], [], 0.05)
        if not r:
            continue
        try:
            b = os.read(fd, 4096)
        except BlockingIOError:
            continue
        if b:
            chunks.append((time.monotonic(), b))
            got += len(b)


def writer(fd, data, rate):
    t0 = time.monotonic()
    pos = 0
    step = 3 if rate else 256
    while pos < len(data):
        if rate:
            due = t0 + pos / rate
            d = due - time.monotonic()
            if d > 0:
                time.sleep(d)
        try:
            pos += os.write(fd, data[pos:pos + step])
        except BlockingIOError:
            select.select([], [fd], [], 0.05)
    return t0


def sysex_losses(chunks, t_start):
    """Walk the counter; return (lost_total, [(t_ms, lost, payload_index)], extra_status)."""
    losses = []
    lost = 0
    prev = None
    idx = 0
    odd = 0
    for t, b in chunks:
        for x in b:
            if x in (0xF0, 0xF7):
                continue
            if x & 0x80:
                odd += 1
                continue
            if prev is not None:
                gap = (x - prev - 1) % 128
                if gap:
                    losses.append(((t - t_start) * 1000, gap, idx))
                    lost += gap
            prev = x
            idx += 1
    return lost, losses, odd


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--card", type=int, required=True)
    ap.add_argument("--device", type=int, default=0)
    ap.add_argument("--mode", choices=("sysex", "notes"), default="sysex")
    ap.add_argument("--bytes", type=int, default=6000, help="sysex payload length")
    ap.add_argument("--count", type=int, default=1500, help="notes: note on/off pairs")
    ap.add_argument("--rate", type=float, default=0, help="bytes/s, 0 = unpaced")
    ap.add_argument("--show", type=int, default=20, help="loss events to list")
    ap.add_argument("--dump", metavar="FILE",
                    help="save what was received: one line per read, '<ms since start> <hex>'")
    a = ap.parse_args()

    data = build(a.mode, a.bytes if a.mode == "sysex" else a.count)
    path = f"/dev/snd/midiC{a.card}D{a.device}"
    fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
    while True:                                 # drop anything stale
        try:
            if not os.read(fd, 4096):
                break
        except BlockingIOError:
            break

    chunks, stop = [], threading.Event()
    th = threading.Thread(target=reader, args=(fd, len(data), chunks, stop), daemon=True)
    th.start()
    t0 = writer(fd, data, a.rate)
    t_wrote = time.monotonic()
    # wire time for the whole stream plus margin, measured from the start
    deadline = t0 + len(data) / WIRE_BPS + 2.0
    while th.is_alive() and time.monotonic() < deadline:
        th.join(0.1)
    stop.set()
    th.join()
    os.close(fd)

    rx = b"".join(b for _, b in chunks)
    if a.dump:
        with open(a.dump, "w") as f:
            for t, b in chunks:
                f.write(f"{(t - t0) * 1000:.1f} {b.hex()}\n")
    t_last = chunks[-1][0] if chunks else t0
    print(f"{path} mode={a.mode} rate={'unpaced' if not a.rate else f'{a.rate:g} B/s'}")
    print(f"sent {len(data)} B (writer done {t_wrote - t0:.3f} s), "
          f"received {len(rx)} B (last at {t_last - t0:.3f} s, "
          f"{len(rx) / max(t_last - t0, 1e-9):.0f} B/s)")
    exact = rx == data
    print("BYTE-EXACT" if exact else "MISMATCH")

    if a.mode == "sysex":
        lost, losses, odd = sysex_losses(chunks, t0)
        if rx[:1] != b"\xf0" or rx[-1:] != b"\xf7":
            print(f"framing: first={rx[:1].hex() or '-'} last={rx[-1:].hex() or '-'}")
        if odd:
            print(f"{odd} unexpected status bytes in the payload")
        print(f"counter gaps: {len(losses)} events, {lost} bytes lost "
              f"(tail short by {max(0, len(data) - len(rx) - lost)})")
        for t, g, i in losses[:a.show]:
            print(f"  t={t:9.1f} ms  lost {g:3d}  at payload byte {i}")
        if len(losses) >= 3:
            iv = sorted(losses[i + 1][0] - losses[i][0] for i in range(len(losses) - 1))
            print(f"  intervals ms: min {iv[0]:.1f} median {iv[len(iv) // 2]:.1f} max {iv[-1]:.1f}")
    elif not exact:
        n = min(len(rx), len(data))
        first = next((i for i in range(n) if rx[i] != data[i]), n)
        print(f"first difference at byte {first}: "
              f"sent {data[first:first + 9].hex(' ')} got {rx[first:first + 9].hex(' ')}")
    return 0 if exact else 1


if __name__ == "__main__":
    sys.exit(main())
