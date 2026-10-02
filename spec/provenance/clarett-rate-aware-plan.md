# Rate-aware control plane — remaining work

Everything the Clarett Thunderbolt line does above 48 kHz reshapes the control plane: ADAT S/MUX
removes destinations, the router tables shrink, and the `GET_METER` slot array compacts. The data
plane is finished and hardware-confirmed. What is left is the control plane catching up, plus one
undecoded status word.

Prerequisite that is now DONE, and which unblocks items 1 and 2: **the current sample rate is
available for free**. The driver publishes `rate:` at `/proc/asound/cardN/clarett`, seeded at probe
from `FCP_SYNC_RATE` (0x006005) and updated at every `SET_CLOCK`. The readback is confirmed on all
four models and persists while idle and across reloads. Do not add a mailbox query for the rate.

---

## 1. ~~Hardware-test the fcp-server per-rate meter path~~ — DONE (Oct 2 2026)

**Result:** fcp-server's band switch works on hardware, and the per-rate slots are now generated from
the vendor's own band-1/band-2 `SET_MUX` tables (a destination's slot = its index in that rate's table,
looked up under the pin it carries at that rate; ADAT Output 2.1-2.4 take over port 1's pins on the
8PreX and the Red). Method: a signal on a mixer input whose slot shifts with the rate, reading which
`Level Meter` entry lights at 48/96/192/48 kHz.
- **Red 8Line** (new `peak-index-m/-h`): Mixer Input 20, entry 51, slots 143/127/87 — lit at every rate.
- **8PreX** (indices unchanged by the new rule): Mixer Input 20, entry 47, slots 75/59/51 — lit at every
  rate; ADAT Output 16 (port 2.8) went dark at 96/192k, as a removed destination should.
- **4Pre**, A/B: the OLD map put Mixer Input 20 on entry 29 (Mixer Input 22) at 96/192k — the "2 low"
  error, confirmed — and the regenerated map fixed it (entry 27 at every rate). The old ranking skipped
  the unmetered loopback destinations, which still occupy table positions. 2Pre: same fix, not run.
- **8Pre**: the table rule reproduces its measured indices exactly (map unchanged).
- **Known, accepted:** for up to ~1 s after a rate change fcp-server still applies the previous band
  (it checks `rate:` once per second), so a meter can flash on the wrong entry — seen on the 4Pre at
  96->192k exactly where the maps predict. A shorter poll would shrink it; not done.

The original plan follows for reference.

### Original plan

The map data is validated — `tools/gen_fcp_maps.py` reproduces the 8Pre measurement exactly (Mixer
Input 01 at 40/32/28 for 48/96/192 kHz). The **code that consumes it has only ever been compiled**:
band selection, the proc read, and the meter-map re-push (fcp-support `ae0b31c`, driver `0806a46`).

**Rig:** 8PreX ADAT Out 1 → 8Pre ADAT In, the same one used for the S/MUX work.

**Steps**
1. `make -C ../fcp-support && sudo make -C ../fcp-support install` (PREFIX defaults to
   `/usr/local` on both sides now — do not qualify it), then `sudo systemctl restart 'fcp-server@*'`.
   **Verify the running binary is the one you built:** `systemctl cat 'fcp-server@*'` and check
   `ExecStart`. A leftover install under the other prefix shadows the new one silently — `/usr/local`
   wins in both systemd's and udev's search order, which is exactly how the first attempt at this
   test ran the old binary.
2. Confirm the free rate source: `cat /proc/asound/card*/clarett` shows a `rate:` line per card.
3. Run fcp-server with `LOG_LEVEL=debug` (env var, read at startup) so the band change is visible.
4. Route ADAT 1 to a **mixer input** on the 8Pre — the probe must be ABOVE meter slot 13, because
   everything below the first removed destination does not move and will look fine either way.
   That is exactly what made two earlier runs inconclusive.
5. Stream at 48 kHz, note which slot lights. Stream at 96 kHz, note it again.

**Acceptance:** the log shows `Sample rate now 96000, remapping meters (band 0 -> 1)`, and the same
physical signal lights the SAME named meter at both rates — i.e. the GUI meter follows the channel,
not the slot. Before this change it moved by exactly the number of removed destinations (8 on an
8Pre at double speed).

**Watch for:** the meter control must NOT be recreated or resized (map size is deliberately constant,
absent channels map to -1 → silence). If the control disappears or changes element count, the
constant-size assumption has broken.

**Gotchas that already cost time:** address controls by `amixer cset name='...'`, never numid —
fcp-server renumbers every control on restart. Verify the transmitter's real rate rather than
assuming it followed.

---

## 2. ~~Per-rate router pins for the 8PreX~~ — DONE (Oct 2 2026)

**Result:** it was the Red 8Line too, and the INPUTS (router sources) as well as the outputs: on both
models ADAT port 2 takes over port 1's pins at double/quad speed in both directions [XML <inputs>/
<outputs> pin-m/pin-h, identical]. fcp-server writes every routing change into all three rate tables,
so three bugs: port-2 destinations were unroutable above 48k (base pin absent from those tables);
routing a port-1 channel S/MUX removes (1.5-1.8, or 1.3-1.4 at quad) overwrote the port-2 channel now
holding its pin; and a port-2 SOURCE was written under its base pin. Fix: the maps carry
`router-pin-m`/`router-pin-h` on every renumbered source and destination ("0" = gone), and fcp-server's
mux.c uses the rate's pin for the slot lookup and for the source written (fcp-support, this commit).
**Verified on the Red without an optical cable** by reading the device's own band 0/1/2 tables back
(`tools/fcp_mux_dump.c`) after three routing writes at 48k: ADAT Output 2.1 <- PCM 5 lands on 0x204 at
96k and 0x202 at 192k, ADAT Output 1.5 <- PCM 6 no longer touches either, and capture <- ADAT 2.1 reads
source 0x204 / 0x202. The OLD build's 192k table showed the collision directly: 0x202 carried ADAT
Output 1.3's PCM 19. One old-build reading stays unexplained (96k 0x204 read Off where the old code
should have written PCM 6); it does not affect the new build. **Not done:** the audio-level acceptance
(an optical loop at 96k); the table readback is the direct evidence.

### Original plan

**The bug:** the 8PreX's second ADAT port re-pins under S/MUX — `ADAT Output 2.1` is `0x208` at
single speed, `0x204` at double, `0x202` at quad. fcp-server locates a destination's router slot by
searching each rate's table for the SAME pin (`mux.c`, `write_mux_control` / the slot scan), so at
high speed a GUI routing change lands on the wrong physical output, and port 2 is unreachable.
Models with one ADAT port are unaffected: their pins never move, entries just disappear.

**Work**
- `tools/gen_fcp_maps.py`: emit `router-pin-m` / `router-pin-h` from the [XML] `pin-m`/`pin-h`
  attributes, same shape as the `peak-index-m`/`-h` work (`58c4a74`) — a per-slug table of pin
  substitutions rather than removals.
- fcp-support `server/mux.c`: use the rate-appropriate pin in the per-rate slot search, and in the
  write path. The rate is already available via the same proc read added for meters.

**Acceptance:** on the 8PreX at 96 kHz, routing a PCM source to `ADAT Output 2.1` in
alsa-scarlett-gui comes out ADAT port 2 channel 1 — verified by capturing on the 8Pre with the
optical cable in port 2 and checking capture channels 20–27. At 48 kHz it must still be correct.

**Note:** `mux.c`'s negative-slot guard (`3cc3f09`) is a prerequisite and is already in.

---

## 3. ~~Decode the FCP_SYNC_READ (0x006004) upper bit~~ — DONE (Oct 2 2026)

**Result: bit 0 = locked, bit 1 = sync state changed since the last read** — a latch set by a clock event
(the SET_CLOCK at every stream arm) and CLEARED BY READING IT. Measured with `tools/fcp_sync_read.c` on
the Red 8Line and the 4Pre:
- Red, internal clock: idle 1; every stream (48/96/192k) reads 3 on the FIRST read after start and 1
  thereafter, whenever that first read happens (2 s, 0.3 s, or after 4 s unread: `3 1 1`, then `1 1 1`).
- Red, ADAT 1 or S/PDIF with no signal: 2 then 0. So bit 0 is lock; the latch is independent of it.
- Red treats the "invalid" source 7 as Internal (reads locked, 48k): on this model the real negative
  control is an external source with nothing connected.
- 4Pre: the latch showed once (192k, 0.3 s) and not in the read-to-clear run: the driver's own
  stream-handshake read of 0x006004 follows SET_CLOCK, so if the event lands before it the latch is
  consumed there. That is the whole "varies by model" table below — sampling relative to the latch.
- **Consequence fixed:** fcp-server returned `!!word`, so 2 (unlocked + latch) showed as **Locked** —
  very likely the 8PreX "invalid source 7 read Locked in 2 of 3 trials" result. It now uses bit 0 on the
  Thunderbolt Clarett/Red cards (USB devices unchanged). Checked: Red on S/PDIF with no signal, 75 Sync
  Status reads across 5 stream starts, all Unlocked.
- **Also found:** 0x006002 = CONFIGURED rate; 0x006005 = the rate the clock actually runs at (equal when
  locked, 192000 on an external source with no signal). The driver's probe seeded its published `rate:`
  from 0x006005, so a unit probed while unlocked advertised 192000 to fcp-server; it now seeds from
  0x006002 (checked: Red reloaded while unlocked on S/PDIF reads 48000).
- **Reopens:** the 8PreX ADAT 2 (=1) and Wordclock (=2) clock enums, unverifiable while Sync Status
  could read the latch as lock. Retry on the 8PreX with the new fcp-server, still anchored on a
  negative control (an external source with no signal).

### Original plan

Not a 0/1 lock flag. Observed values so far, all with `tools/` `fcp_clock_read`:

| model | idle | streaming 48k | streaming 96k | streaming 192k |
|---|---|---|---|---|
| 2Pre | 1 | **3** | 3 | 3 |
| 4Pre | 1 | **3** | 3 | — |
| 8Pre | 1 | **1** | 3 | 3 |
| 8PreX | 1 | **1** | 3 | — |

"bit 1 = high speed" fits the 8Pre/8PreX and is refuted by the 2Pre/4Pre at 48 kHz. fcp-server
collapses the value with `!!`, so the exposed `Sync Status` is unaffected either way.

**Why it matters:** this is the likely reason `Sync Status` proved unusable as a clock-source probe on
the 8PreX, which is why `CLARETT_CLOCK_ADAT2` (ADAT 2 = 1) and `CLARETT_CLOCK_WORDCLOCK` remain
unverified. Decoding it may reopen that.

**Method:** tabulate 0x006004 across model × {idle, streaming} × rate × clock source × external
signal present/absent. Vary ONE axis at a time. **Anchor every run on a negative control** (an enum
value in no model's list, e.g. 7) and re-check it *within that run* — the ADAT 2 attempt died because
the control was verified once at the start of a campaign and had stopped holding by the end.

---

## Also open, unrelated to rate

- **EliteBook 640 G11 retest** when it arrives — the ~42 ms Thunderbolt SMI freeze, plus whether the
  TB security level and `pci=` arguments are needed there. See the memory note; three separate
  "Clarett needs X" conclusions currently trace to one ASRock board.
- **8Pre playback untested at any rate**; high-speed playback re-verified only on the 2Pre.
- **Session collapse** (zeroed control plane) trigger still unisolated.
