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

## 1. Hardware-test the fcp-server per-rate meter path (highest value, cheapest)

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

## 2. Per-rate router pins for the 8PreX

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

## 3. Decode the FCP_SYNC_READ (0x006004) upper bit

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
