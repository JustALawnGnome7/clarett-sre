# Clarett — Response-Buffer pmemsave Runbook (wall §7 test 2)

> **RUN July 9 2026 — results (details: wall spec §7 item 2).** S0 + S1 (668 snapshots, 460 distinct
> states; S2 skipped as superseded, S3 impossible — the 2Pre has no front-panel buttons): **no host
> seed, no request mirror** — every state is a device response. **Discovery: resp`+8` is an FCP error
> word — working session = `0x00` + real payloads; our sessions = `0x3` + size=0 = a per-session
> REFUSAL code.** "`0x03` = SUCCESS" is corrected everywhere (transport spec §8, `clarett.h`). New
> facts banked: `0x6005` = sample-rate query (u32, 48000); working `GET_METER` responses are 192 B.
> Next: the **cold-boot error timeline** (§7 of the wall spec) — burst-sample the vendor's own cold
> bring-up and look for the error 3→0 flip; the command where it flips is the arming step.
>
> **Cold sampling pass RUN (July 9, same day):** earliest observable response (seq 61) is already
> `error=0` on a cold device; the bring-up's two ~30 ms command phases are unsampleable at ~10 Hz and
> the GPA is not boot-stable. Any flip is inside seq 0–60 → escalated to the **gdb doorbell ladder**:
> `tools/doorbell_ladder.gdb` (break QEMU's `vfio_region_write` on `0x408<-1`, append the 4 KB buffer
> per hit; GPA learned live from `0x410/0x414`), decoded by `resp_dump.py --ladder` — record N =
> response to command N-1, the complete per-command error record. Also banked: `CONFIG_PUSH` (0x5000)
> is a per-id **name query** (id `0x1e` → `"ADAT 8"`).

**Purpose:** decide whether Focusrite Control's host side ever **writes content into the
`0x410` response buffer** that is not a device-written response — a request mirror, token,
or ack the device DMA-reads. This is the active half of the §7 RAM-contents theory (the
passive half, buffer hygiene, was disproven July 9 by the `resp_prefill=0` A/B). The buffer
is the **only host address the device knows at init**, so if the arming differentiator is
host-RAM content, it is here. Tags per project convention: `[PLAN]` until run.

**Instruments:** `tools/dma_bases.py` (extracts the buffer GPA from a vfio trace),
`tools/resp_burst.sh` (timestamped snapshot bursts), `tools/resp_dump.py` (annotate/diff).

---

## 1. What counts as a hit

Every snapshot state must be explainable as *[16-byte echoed FCP header][`size` payload
bytes] over residue of older/larger responses*. Three outcomes:

| Class | Signature | Meaning |
|---|---|---|
| **(a) persistent seed** | nonzero content in ANY snapshot outside all response extents (especially high offsets, ≥ `0x800`) | host-written token/structure — **the §7 hit**; visible in a single snapshot |
| **(b) request mirror** | a state whose header has `status != 0x03` (the request's zeroed error word) with request-shaped payload (e.g. `GET_DATA`: 8 bytes `{off,len}`) | host writes the request into the buffer before the doorbell — **the §7 hit**, transient |
| **(c) clean** | every state = well-formed device response + residue | negative here; sub-snapshot-rate transients remain possible → escalate to WinDbg `ba w` (§7 test 3) |

`resp_dump.py` flags (b) as `REQUEST-SHAPED` and prints (a) extents automatically.

## 2. Preconditions

- Device bound to `vfio-pci`; FC guest boots with the custom trace-enabled QEMU and
  `x-no-mmap=true` + `-trace enable=vfio_region_*` (recipe: `clarett-mmio-trace-setup`
  memory / CLAUDE.md "Method"). Domain has historically been `Windows10-custom` — confirm
  with `virsh list --all`, and find the live log with `ls -t /var/log/libvirt/qemu/`.
- The buffer GPA is **reallocated every guest boot** — it MUST come from the current
  boot's trace (never reuse an old one; the stale `0x277913000` mistake is on record).
- `pmemsave` takes a guest-physical address; under vfio with no guest vIOMMU, GPA == the
  IOVA the device uses, and the Windows common buffer is pinned for the device's lifetime,
  so one extraction per boot is enough.
- Timestamps: the QEMU log is **UTC**; snapshot filenames are epoch-ms — correlate with
  `date -u`, and decode trace windows with `tools/fcp_decode.py --brief`.

## 3. Capture sequence (one guest boot, four states)

Start the log tail **before** booting the guest, and keep it running throughout:

```sh
sudo tail -F /var/log/libvirt/qemu/<domain>.log > /tmp/respbuf_trace.log &
virsh start <domain>
```

**S0 — driver-only (cleanest state; do NOT start the FC app yet).** The bare
`FocusritePCIe.sys` bring-up issues no `GET_METER` (confirmed by `2pre_cold_boot2.log`),
so once init settles the buffer is quiescent. Extract the GPA and take one snapshot:

```sh
python3 tools/dma_bases.py /tmp/respbuf_trace.log            # prints RESP GPA + command
sudo virsh qemu-monitor-command <domain> '{"execute":"pmemsave","arguments":{"val":<GPA>,"size":4096,"filename":"/tmp/resp_S0.bin"}}'
python3 tools/resp_dump.py /tmp/resp_S0.bin
```

Expected if clean: the last init response's echo + residue, zero tail. **Any high-offset
extent here is already a class-(a) hit** — with no meter poll there is nothing to race.

**S1 — FC running, idle (steady-state sampling).** Start Focusrite Control, let it settle,
then burst ~15 s (~100+ snapshots at the ~24 Hz meter poll = good phase coverage):

```sh
sudo tools/resp_burst.sh <GPA> 15 <domain> /tmp/respburst_S1
```

**S2 — control toggle.** Start a burst, and mid-burst toggle Air (or Mute) in FC — the
`SET_DATA`+`DATA_CMD`+`GET_DATA` cycle passes through the buffer while we sample:

```sh
sudo tools/resp_burst.sh <GPA> 15 <domain> /tmp/respburst_S2   # toggle at ~5 s in
```

**S3 — front-panel press.** Same, pressing the physical Mute/Dim button mid-burst — this
triggers FC's full config re-sync (§5a), the densest command sequence available at idle.

## 4. Analysis

```sh
python3 tools/resp_dump.py /tmp/resp_S0.bin
python3 tools/resp_dump.py --diff /tmp/respburst_S1/resp_*.bin
python3 tools/resp_dump.py --diff /tmp/respburst_S2/resp_*.bin
python3 tools/resp_dump.py --diff /tmp/respburst_S3/resp_*.bin
```

`--diff` prints each **distinct** buffer state once (annotated) and then the transition
extents. Read it against §1's table. For any suspicious state, find the commands in flight
at that timestamp: convert the filename epoch-ms to UTC and read the matching window of
`fcp_decode.py --brief /tmp/respbuf_trace.log`.

**Bonus regardless of outcome:** the S2/S3 states show what a *working* `GET_DATA`
response looks like DMAed into the buffer (the still-open §5b item) — bank the size/status/
payload shape into the transport spec.

## 5. Decision

- **Class (a) or (b) hit** → replicate in `snd-clarett`: write the same bytes into
  `resp_buf` at the same point in the transaction (a one-line addition next to the
  `resp_prefill` hook in `clarett_fcp()`), fresh power-cycle, rerun the §6 A/B. If the
  seed is session-dependent (token-like), capture two boots and diff the seeds first.
- **Class (c) clean** → snapshots cannot rule out sub-50 ms transients; go to §7 test 3
  (WinDbg `ba w` on the buffer during a toggle, module-range attribution, existing serial
  KD rig) as the definitive instrument for CPU-written transients.
- Either way, run the same protocol on the **16 KB descriptor CBs** next (§7 test 4;
  `dma_bases.py` already emits their commands from a stream-window trace) — the data-plane
  wall's version of the same question, plus the undecoded post-terminator structure
  (data-plane §3c).

## 6. Caveats

- The snapshot rate (~5–15 Hz, one virsh round-trip each) undersamples the ~24 Hz meter
  poll: class-(b) detection is statistical, absence here is NOT proof of absence — that is
  what test 3 is for. Class-(a) detection is not rate-sensitive at all.
- `pmemsave` files are written by the QEMU process — keep outputs under a world-writable
  /tmp dir (resp_burst.sh handles this).
- Don't pause/suspend the guest to "freeze" a moment: stopping vCPUs doesn't stop device
  DMA, and it perturbs the very traffic being observed. Sample instead.
