# The response-address acknowledgement: the readiness handshake (Sep 9 2026)

The cold-attach refusal recorded in `CLAUDE.md` and `driver/DEVELOPMENT.md` was a
timing race with a signal the device has always sent and the driver never waited for.
This note records how the signal was found in the existing traces, the hardware
experiments that established what it is, and the claims it retires.

## 1. Hypothesis, from the traces alone

Every vendor capture that contains an attach shows the same access order before the
first mailbox command. From `captures/2pre_cold_boot.log`, lines 660-677:

```
write (0x104, 0xf000003f)       IRQ enable
write (0x410, 0x625fe000)       response DMA address, low word
write (0x414, 0x2)              response DMA address, high word
  3.2 ms
read  (0x100) = 0x20000000      five-register interrupt sweep
read  (0x300) = 0x0
read  (0x200) = 0x0
read  (0x400) = 0x1             bit 0, with no command in flight
read  (0x500) = 0xff0000
read  (0x8000..0x801c)          header re-read, all eight words
write (0x8020, 0x80800005)      first command
```

The five-register read is the interrupt service pattern seen at every MSI in every
capture, so an interrupt fired after the address write with no doorbell written, and
`0x400` carried bit 0 in it. Across all twelve captures containing an attach (2Pre x5
including two cold boots, 4Pre, 8Pre, 8PreX x3, Red 8Line) the order is identical: the
`0x414` write is access 13, the `0x400 = 0x1` read is access 17, and the first doorbell
is access 34 (36 on the Red). Nowhere else in the corpus does `0x400` read bit 0
outside a submit-to-ack window.

Hypothesis: the device acknowledges the response-buffer address with `0x400` bit 0,
and the host must see it before sending a command. Our `clarett_hw_init()` slept a
fixed 3.22 ms at that point, copied from the trace gap, and discarded the sweep.

## 2. Experiments: leah (ThinkPad T480), 4Pre, sysfs rebind cycles, no settle

A module lever waited on the driver's existing bit 0 completion and logged the
latency; the response buffer was pre-zeroed and checked afterwards.

| mode | what was varied | result |
|---|---|---|
| write low + high, wait | baseline | bit 0 arrived 7/7, 2.0-3.0 ms after the high write |
| wait 300 ms first with nothing written, then write | is it time? | absent 3/3, then arrived 3/3 |
| write low word only, wait; then high, wait | which write? | absent 3/3 after low; arrived 3/3 after high |
| write both, wait, write both again, wait | does a rewrite re-raise it? | arrived, then arrived again 3/3 (~4 ms) |

The response buffer stayed all-zero in every run: this is a status interrupt, not a
DMA. The bit is caused by the high-word write completing the address, not by elapsed
time and not by the IRQ enable.

Then the command was sent inside the window:

| first command issued | result |
|---|---|
| immediately after the high write | wedged 3/3 |
| 1 ms after the high write | wedged 3/3 |

The log matched the recorded cold-attach signature line for line: `accepted but
unanswered after 500 ms`, then `response never landed`, then `STREAM_INFO{0} transport
failed (-110)`. The "accepted" was the address acknowledgement, arriving during the
command's wait and misread as the command's accept. Two rebinds with the wait in
place immediately afterwards came up clean, so the wedge does not persist across a
re-init that waits.

## 3. Cold power cycles, both models, first touch at PCI enable

Probe loaded with a 5 s wait before the address write (a spontaneous bit 0 at
power-on would have shown there), then the address write and a 5 s bounded wait, then
`STREAM_INFO` at once. Off time measured host-side from Thunderbolt disconnect to PCI
re-enable.

| # | model | off for | bit 0 in the 5 s before the write | bit 0 after the high write | detected |
|---|---|---|---|---|---|
| 1 | 4Pre | 17 s | absent | 10.56 ms | first command |
| 2 | 4Pre | 3.7 s | absent | 4.04 ms | first command |
| 3 | 4Pre | 7.0 s | absent | 10.61 ms | first command |
| 4 | 2Pre | 15.5 s | absent | 10.08 ms | first command |
| 5 | 2Pre | 5.3 s | absent | 3.30 ms | first command |
| 6 | 2Pre | 8.1 s | absent | 9.48 ms | first command |
| 7 | 2Pre | 4.2 s | absent | 3.33 ms | first command |

The device never raises bit 0 on its own. Off for about 7 s or longer the reply
takes about 10 ms; off for under about 5 s, 3-4 ms; warm, 2-3 ms. The old fixed
3.22 ms sleep sat at the warm figure, which is why a quick replug usually survived
and a real power cycle wedged. Two of the 2Pre attaches had the Thunderbolt link flap
(found, disconnected, found again) during the wait and were unaffected.

## 4. What changed

`clarett_hw_init()` now writes the address and waits for bit 0 with a 500 ms bound
(`CLARETT_ADDR_ACK_MS`, about 50x the worst case seen), failing the probe on timeout.
Removed: `settle_ms`, `wait_ready_ms`, `CLARETT_READY_RETRY_MS` and the 30 s re-init
loop. The probe asks `STREAM_INFO` once. Verified on leah: three warm rebinds of the
2Pre with the production code acknowledged at 2.1-2.2 ms and registered.

## 5. Claims retired

- "A cold device cannot answer its first command and nothing recovers it." It answers
  its first command once the address is acknowledged; every recorded failed recovery
  re-sent the command inside the same 2-11 ms window, and the recorded successes were
  the ones whose re-init happened to be followed by a slower first command.
- "Manual sysfs bind never fails, automatic probe does: not a timing question." It
  was exactly a timing question; whatever put the bind's first command past 3 ms saved
  it.
- "`0x400` bit 0 is only ever set inside a submit-to-ack window." It is also the
  address acknowledgement, once per address write.
- The 3.22 ms gap in `clarett_hw_init()` was "vendor inter-access timing". It was the
  vendor host's interrupt latency under MMIO trapping, and the vendor host waited for
  the interrupt rather than sleeping.
- `spec/clarett-interface.md` §6.1's "~2 s budget" and "only waiting rescues it".

## 6. Method

The trace had shown this from the first capture. It was missed because the sweep
after the address write was read as noise and its gap as a pause, and because the
cold-attach experiments all varied how to retry rather than what the device had said.
The productive step was to ask, for each write in the attach sequence, what the
device did in response to it, and to test that with one variable changed per run and
a negative control on the same device. The whole set of experiments took an afternoon
against a device on the bench; the failed diagnosis had taken weeks.
