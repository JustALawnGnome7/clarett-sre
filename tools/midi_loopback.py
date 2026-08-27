#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Exact-comparison MIDI loopback bench for the Clarett DIN MIDI path.

Drives one ALSA rawmidi port and listens on another, comparing the byte streams
EXACTLY rather than counting bytes — a torn stream and a short one need telling
apart, and only the byte-for-byte diff distinguishes them. Run it against a
known-good partner interface with the DIN in/out cross-connected, so a failure
in one direction attributes to that direction alone, or with a single cable from
one port's OUT to its own IN to take the partner out of the picture.

Listeners run `amidi -d -a -c`. The -a/-c are load-bearing: amidi's default is to
DISCARD Active Sensing (FEh) and Clock (F8h) when printing received data, which
makes those two bytes look like they never arrived.

    ./midi_loopback.py --send hw:2,0 --listen hw:1,0 notes 500
    ./midi_loopback.py --send hw:2,0 --listen hw:1,0 sysex 4096
    ./midi_loopback.py --send hw:2,0 --listen hw:1,0 raw C0 40
    ./midi_loopback.py --a hw:1,0 --b hw:2,0 both 1000
    ./midi_loopback.py --listen hw:2,0 idle 6

Exit status is 0 only if every direction matched exactly.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

MIDI_BYTES_PER_SEC = 3125.0     # DIN wire rate

def note_stream(count, channel=0):
    """count 3-byte note-on messages with cycling data bytes."""
    msgs = [[0x90 | (channel & 0xf), i % 128, (i * 7) % 128 or 1]
            for i in range(count)]
    return [b for m in msgs for b in m]

def sysex_stream(count):
    """One SysEx with `count` data bytes (non-commercial id 0x7d)."""
    return [0xf0, 0x7d] + [i % 128 for i in range(count)] + [0xf7]

def hexs(data):
    return " ".join("%02X" % b for b in data)

def listen(port):
    """Start a dump listener; returns (proc, tempfile).

    -a and -c are MANDATORY, not optional: without them amidi silently discards
    Active Sensing (FEh) and Clock (F8h) from everything it prints, so those two
    bytes read as lost on the wire no matter what the hardware did.

    stderr is kept OUT of the capture file: amidi writes "Device or resource
    busy" there, and folding that into the byte stream turns a failed open into
    corrupt data rather than a reported error.
    """
    f = tempfile.NamedTemporaryFile(mode="r", delete=False)
    p = subprocess.Popen(["amidi", "-p", port, "-d", "-a", "-c"], stdout=f.file,
                         stderr=subprocess.PIPE)
    return p, f

def reap(proc):
    """Stop a listener. Safe to call twice, and called from a finally so a
    failed run cannot leave an amidi holding the rawmidi port: the device has
    one input substream, and an orphan blocks every later open."""
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

def collect(proc, f):
    """Return the bytes a stopped listener captured.

    A listener that died on open captured nothing, which is indistinguishable
    from total loss, so its stderr is raised instead of reported as zero bytes.
    """
    reap(proc)
    err = proc.communicate()[1].decode(errors="replace").strip()
    f.file.close()
    text = open(f.name).read()
    os.unlink(f.name)
    if err:
        raise RuntimeError("listener failed: %s" % err)
    got = []
    for tok in text.split():
        if len(tok) == 2 and all(ch in "0123456789ABCDEFabcdef" for ch in tok):
            got.append(int(tok, 16))
        else:
            raise RuntimeError("unparsable token in capture: %r" % tok)
    return got

def settle(nbytes):
    """Wait out the wire time for nbytes plus a fixed margin."""
    time.sleep(1.5 + nbytes / MIDI_BYTES_PER_SEC)

def report(label, expect, got):
    """Print a verdict; return True on an exact match."""
    if got == expect:
        print("%-28s sent=%-6d recv=%-6d PASS" % (label, len(expect), len(got)))
        return True
    print("%-28s sent=%-6d recv=%-6d FAIL" % (label, len(expect), len(got)))
    n = min(len(got), len(expect))
    at = next((i for i in range(n) if got[i] != expect[i]), n)
    print("    first divergence at byte %d" % at)
    print("    expect: %s" % hexs(expect[at:at + 18]))
    print("    got   : %s" % hexs(got[at:at + 18]))
    if len(got) > len(expect):
        print("    NOTE: received MORE than was sent — look for another writer"
              " on the listening port, not for a transmit fault")
    return False

def one_way(send, lport, data, label):
    proc, f = listen(lport)
    try:
        time.sleep(0.7)
        if proc.poll() is not None:
            raise RuntimeError("listener on %s exited immediately: %s" % (
                lport, proc.communicate()[1].decode(errors="replace").strip()))
        t0 = time.time()
        subprocess.run(["amidi", "-p", send, "-S", hexs(data)], check=True)
        tx = time.time() - t0
        settle(len(data))
    finally:
        reap(proc)
    got = collect(proc, f)
    ok = report(label, data, got)
    floor = len(data) / MIDI_BYTES_PER_SEC
    print("    tx wall %.3fs (wire-rate floor %.3fs)" % (tx, floor))
    return ok

def both_ways(a, b, count):
    """Transmit in both directions at once; compare each independently."""
    exp_a2b, exp_b2a = note_stream(count, 1), note_stream(count, 2)
    pa, fa = listen(a)
    pb, fb = listen(b)
    try:
        time.sleep(0.7)
        sa = subprocess.Popen(["amidi", "-p", a, "-S", hexs(exp_a2b)])
        sb = subprocess.Popen(["amidi", "-p", b, "-S", hexs(exp_b2a)])
        sa.wait()
        sb.wait()
        settle(count * 3)
    finally:
        reap(pa)
        reap(pb)
    # the listener on A hears what B transmitted, and vice versa
    got_b2a, got_a2b = collect(pa, fa), collect(pb, fb)
    ok = report("%s -> %s" % (b, a), exp_b2a, got_b2a)
    ok &= report("%s -> %s" % (a, b), exp_a2b, got_a2b)
    return ok

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--send", help="rawmidi port to transmit from (e.g. hw:2,0)")
    ap.add_argument("--listen", help="rawmidi port to receive on")
    ap.add_argument("--a", help="port A for 'both'")
    ap.add_argument("--b", help="port B for 'both'")
    ap.add_argument("mode", choices=["notes", "sysex", "raw", "both", "idle"])
    ap.add_argument("arg", nargs="*",
                    help="notes/sysex/both: count; raw: hex bytes; idle: seconds")
    args = ap.parse_args()

    if args.mode == "idle":
        secs = float(args.arg[0]) if args.arg else 6.0
        proc, f = listen(args.listen)
        try:
            time.sleep(secs)
        finally:
            reap(proc)
        got = collect(proc, f)
        print("idle listen on %s for %.1fs: %d bytes" % (args.listen, secs, len(got)))
        if got:
            print("    %s" % hexs(got[:32]))
        return 0 if not got else 1

    if args.mode == "both":
        return 0 if both_ways(args.a, args.b, int(args.arg[0])) else 1

    if args.mode == "notes":
        data = note_stream(int(args.arg[0]))
    elif args.mode == "sysex":
        data = sysex_stream(int(args.arg[0]))
    else:
        data = [int(t, 16) for t in args.arg]

    label = "%s -> %s" % (args.send, args.listen)
    return 0 if one_way(args.send, args.listen, data, label) else 1

if __name__ == "__main__":
    sys.exit(main())
