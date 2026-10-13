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
| `loopback_order_2pre.sh <card>` | 2Pre digital-loopback ramp at 48/96/192 kHz: capture channel order (loopback and ADAT), sample-exactness, S/MUX-removed channels silent. Edit its three model constants for another model. |
| `ramp_check.py` | Ramp generator and checker used by the loopback script. |

The digital-loopback ramp method and what each result means are described in `CLAUDE.md`.
