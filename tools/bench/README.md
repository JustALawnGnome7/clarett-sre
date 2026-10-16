# Bench scripts

Hardware test scripts for snd-clarett, run against a live unit. Each appends to a log named after
itself in the current directory (or in `$BENCH_LOG_DIR`), so run them from a scratch directory.
Most need root (dynamic debug, sysfs binds, fcp-server control); the user is prompted wherever a
hand on the unit is needed. `<card>` is the ALSA card number, `<bdf>` the unit's PCI address
(`cat /proc/asound/cards`, `lspci -d 1cb5:`).

| script | what it checks |
|---|---|
| `attach_test.sh [N]` | Cold attach over N power cycles: response-address acknowledgement time, which attempt answered, enable -> registered, Thunderbolt appearances. |
| `rebind_test.sh <bdf> [N]` | The same over N warm sysfs unbind/bind cycles. |
| `phase_check.sh <card> <bdf>` | Mailbox phase bits per command (3 s trace), then a 25 s capture while the monitor knob is turned (events relayed mid-stream) and one hands-off (none relayed). |
| `idle_write.sh <card> <bdf> [idle_s] [control]` | No host traffic for minutes (GUI closed), then a switch write through fcp-server; asks whether the front panel followed. |
| `arm_cost.sh <card> <bdf> [label] [N]` | Mailbox commands and time from stream open to engine armed, plus simultaneous capture + playback starts. Logs the module srcversion, for before/after comparisons. |
| `stream_info.sh <card>` | Raw STREAM_INFO replies for bands 0-3 via `tools/fcp_cmd.c` (stops fcp-server meanwhile). |
| `loopback_order.sh <card>` | Digital-loopback ramp at 48/96/192 kHz on any Clarett or a Red 8Line/4Pre (model from the card id; the Red gets per-rate legs): capture channel order (loopback and ADAT 1), sample-exactness, S/MUX-removed channels silent. Whatever the ramp channel feeds is switched off for the run and restored; `RAMP=<n>` picks another playback channel. Run as your user. |
| `duplex_stress.sh <card>` | Simultaneous capture + playback, checked with a streamed loopback ramp: long legs at 48/96/192 kHz and a 64/256 leg, then N cycles started and stopped in the same instant at mixed rates (no servicer thread or stuck client may survive), then a playback opened at another rate mid-capture. PipeWire's profile and the routes it touches are restored at exit. `SHORT=1` for a ~3 min run. Run as your user, and do not start alsa-scarlett-gui during it (it clears mono routes from linked pairs at startup). |
| `duplex_ramp.py` | Streaming ramp generator and live checker used by the duplex script. |
| `clock_live.sh <card> [external]` | Clock Source changed while streaming: a 20 s loopback ramp with the source switched to an absent external one and back, Sync Status after each switch, the ramp's breaks; then the same switches with PipeWire holding the card. Run as your user. |
| `midi_loop.py --card <card> [--mode sysex\|notes] [--rate B/s]` | DIN MIDI self-loop (5-pin cable Out -> In): byte-exact check; in `sysex` mode the payload is a 7-bit counter, so each lost run is logged with its size and time, and the intervals between losses summarised. No root needed; nothing else may hold the rawmidi open. |
| `tb_manual_auth.sh <bolt uuid> [N] [hold_s]` | Thunderbolt reconnect race: sets the unit's boltd policy to `manual` (restored at exit), and per power cycle logs the unit's power-on link bounce, then authorizes once it has been present unbroken for hold_s and checks PCIe comes up. |
| `ramp_check.py` | Ramp generator and checker used by the loopback script. |
| `kmsg_times.py` | Attach intervals from kernel timestamps (`journalctl -k -o json \| kmsg_times.py [bdf]`), used by the attach and rebind scripts. journald's receive times batch kernel lines and are not usable for millisecond intervals. |

The digital-loopback ramp method and what each result means are described in `CLAUDE.md`.
