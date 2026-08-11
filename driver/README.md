# snd-clarett — Focusrite Clarett (Thunderbolt) ALSA driver

Out-of-tree ALSA driver for the Focusrite **Clarett** Thunderbolt audio interfaces —
**Clarett 2Pre**, **4Pre**, **8Pre**, and **8PreX** — as a single module, with the model
auto-detected at probe.

**Status: working.** Mixer control plane (through `fcp-server`), PCM capture and playback,
and DIN MIDI, all confirmed on real hardware. Built clean-room; for how it works and the
reverse-engineering history, see **[DEVELOPMENT.md](DEVELOPMENT.md)**.

> Control changes take physical effect: preamp Mode/Air, monitor Mute/Dim, and the SW/HW
> gain selector all move the hardware, and settings persist in the device's own NVRAM.

## Requirements

- Kernel headers for your running kernel (this is an out-of-tree module).
- The interface **visible on the host PCI bus** (`lspci -d 1cb5:0002`). The Clarett line is
  Thunderbolt 2, which needs some firmware/boot setup to reach a modern host — see
  *Thunderbolt 2 setup* below.
- Secure Boot **off**, or the module signed with an enrolled MOK (it ships unsigned).
- For the mixer/routing controls: **`fcp-server`** with the Clarett device maps (see
  `fcp-server-data/`), and **`alsa-scarlett-gui`** for the full mixer/router. **At the moment
  both need patched builds** — the required changes are not yet in Geoffrey Bennett's upstream
  `fcp-server` / `alsa-scarlett-gui`, so use the forks with the Clarett patches applied. The
  kernel module provides only the transport; the controls live in userspace.

## Thunderbolt 2 setup (getting the device onto the PCI bus)

The driver binds a PCI device, so the Clarett must be visible on the host's PCI bus before it
can work at all:

```sh
lspci -d 1cb5:0002        # the interface has to show up here first
```

The whole Clarett line is **Thunderbolt 2**, which takes a little setup to reach a modern
(Thunderbolt 3+) host:

- **Disable Thunderbolt security in the UEFI/BIOS.** TB2 devices are not enumerated by
  `boltctl`, so they cannot be user-approved while Thunderbolt security is enabled — with
  security on, the Clarett never reaches the PCI bus at all. Set the firmware's Thunderbolt
  security level to none/legacy (the exact wording varies by board).

- **Bridge TB2 to the host with an Apple Thunderbolt 2 → Thunderbolt 3 adapter.** It is an
  active cable that appears to the host as a PCI bridge, with the Clarett sitting behind it.
  Many boards' default firmware does not reserve enough PCI bus numbers for a second device
  behind that bridge, so the interface fails to enumerate even though the bridge itself shows
  up. If `lspci` lists the bridge but not `1cb5:0002`, add these kernel boot parameters
  (through GRUB, or whatever bootloader you use) and reboot:

  ```
  pci=assign-busses,realloc,hpbussize=0x10
  ```

  Confirmed on an ASRock X570 Creator; the exact remedy can differ with other board firmware.

## Build & install

```sh
make                        # builds snd-clarett.ko against the running kernel
sudo insmod snd-clarett.ko  # auto-binds PCI 1cb5:0002
```

Build against another tree with `make KDIR=/path/to/kernel`.

## Using it

### Controls — mixer, routing, preamps, metering

The module exposes a minimal FCP *hwdep* transport; the actual controls are created by
**`fcp-server`** in userspace (the same model the mainline 4th-gen Scarlett driver uses):

```sh
sudo fcp-server <card>      # <card> = the ALSA card number or id
```

Then use `alsamixer -c <card>`, or **alsa-scarlett-gui** for the full mixer and router.
`fcp-server` can also be auto-started per interface through its udev/systemd integration.

### Model selection

The whole Clarett Thunderbolt line shares one PCI id, so the model is detected from the
armed device. Override it if detection is ever wrong:

```sh
sudo insmod snd-clarett.ko                # auto-detect
sudo insmod snd-clarett.ko model=2pre     # force: model=2pre / 4pre / 8pre / 8prex
```

Make an override persistent:

```sh
echo 'options snd_clarett model=2pre' | sudo tee /etc/modprobe.d/snd-clarett.conf
```

### PCM (recording and playback)

Capture and playback are available as standard ALSA PCM devices. **There is no default
route** — playback is silent until you wire a PCM source to a physical output in the router
(alsa-scarlett-gui). That is a mixer-config step, not a bug.

### MIDI

The interface's DIN MIDI ports appear as a standard ALSA rawmidi device.

## Settings persistence

The device **owns its settings** — they live in its own NVRAM and survive power cycles,
reboots, and moving to another host. The driver auto-persists your changes (debounced).

Because the device already restores its own state, **disable `alsactl` restore for this
card** so it doesn't overwrite that state on every load:

```sh
sudo systemctl mask alsa-state alsa-restore
sudo systemctl stop alsa-state alsa-restore
sudo rm /var/lib/alsa/asound.state          # only if no other card needs it
```

## Known limitations

- **Low-latency streaming can glitch on some platforms** — periodic audible skips traced to a
  Thunderbolt-triggered firmware SMI, not the driver (the DMA rides through it; the click is
  the audio server re-preparing). A BIOS/firmware issue.
- **Preamp Mode/Air can read wrong right after a cold boot** — the hardware is correct (the
  LEDs are right); only the on-screen value can lag until you touch a control, after which it
  tracks correctly.
- **No per-output mute** — the hardware has none (only master Mute/Dim, reaching the outputs
  that opt in).

## How it works / contributing

The design, the FCP protocol, model detection, the module parameters, and the
reverse-engineering history are documented in **[DEVELOPMENT.md](DEVELOPMENT.md)**.
