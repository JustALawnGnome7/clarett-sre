# Packaging — DKMS, Fedora akmod, Secure Boot, licensing

Moved verbatim out of `CLAUDE.md` (Build & test) on Oct 8 2026 to keep that file under the session
size limit; `CLAUDE.md` keeps a summary and points here. The full record of how both packaged routes
were built, verified on hardware and across kernel upgrades, and every trap hit on the way.

- **★ PACKAGING (Aug 24 2026) — DKMS + Fedora akmod, both built and verified locally.** `make -C snd-clarett`
  and `modules_install` are dev-only: the module lands under one kernel and vanishes at the next
  update. The two supported routes are `sudo make -C snd-clarett dkms-install` and, on Fedora,
  `make -C snd-clarett rpm-akmod` (or `rpm-kmod KVER=<ver>` for a single kernel). **The RPM targets build
  and stop, printing the `dnf install` line rather than running it** — deliberately, because that
  transaction is the one the partial-kernel trap below lives in, and the read-before-yes cannot be
  delegated to a Makefile. They also enforce two traps that were previously only described: the spec
  is staged into `%{_specdir}` before building (kmodtool re-invokes `rpmbuild` against it there), and
  the spec's `Version:` is checked against `dkms.conf`, which otherwise surfaces as a missing
  `Source0` rather than as version skew. **`snd-clarett/dkms.conf`'s `PACKAGE_VERSION` is the single source of truth** —
  `snd-clarett/Makefile` parses it out, passes it to kbuild, and it is compiled in as `MODULE_VERSION`
  (`-DCLARETT_VERSION`), so `modinfo snd-clarett` names exactly one tree; a build that bypasses the
  Makefile reports `0.0.0-unknown` on purpose. Verified end to end: the module inside the built kmod
  RPM reports `0.1.0`. The specs live under `snd-clarett/` (not a repo-root `packaging/`) so the directory
  is self-packaging as the public submodule.
  - **Three kmodtool traps, each of which cost a failed build** — all fixed in the spec, don't
    re-discover them: (1) kmodtool emits `Requires: %{name}-common` on **every** kmod/akmod subpackage,
    so without a `-common` subpackage the RPMs build and then **fail to install**; (2) an akmod build
    compiles nothing, so the debugsource package is empty and rpmbuild **errors** — hence
    `%global debug_package %{nil}`; (3) `%akmod_install` re-invokes `rpmbuild -bs` against
    `%{_specdir}/%{name}.spec`, so **the spec must be copied to `~/rpmbuild/SPECS/` first**, not built
    in place.
  - **A bare `rpmbuild -bb` on the kmod spec CANNOT work on an ordinary Fedora box** and this is not a
    spec bug: with neither `buildforkernels` nor `kernels` defined, kmodtool takes its
    build-for-current-kernels path, which requires `--repo` **and** the
    `buildsys-build-<repo>-kerneldevpkgs` helper — RPM Fusion build-farm infrastructure. Use
    `--define 'buildforkernels akmod'` (the end-user package) or `--define "kernels $(uname -r)"`.
  - **★ DKMS VERIFIED END TO END (dkms 3.4.1, Fedora 44):** `sudo make -C snd-clarett dkms-install` builds,
    **signs with an auto-generated MOK** (`/var/lib/dkms/mok.{key,pub}`; `modinfo` shows
    `signer: DKMS module signing key`), installs to **`/lib/modules/<kver>/extra/snd-clarett.ko.xz`**
    (Fedora overrides `DEST_MODULE_LOCATION`, as dkms.conf says), runs depmod, and
    `modprobe --show-depends` then resolves the whole chain **including `snd-rawmidi`** — the
    dependency that made a bare `insmod` fail. `dkms status` = installed; installed module = `0.1.0`.
    `dkms-uninstall` backs it all out cleanly.
  - **★★ THE TRAP THAT BROKE THE FIRST DKMS RUN — `KERNELRELEASE` CANNOT TELL YOU KBUILD IS CALLING.**
    `ifneq ($(KERNELRELEASE),)` is *the* conventional out-of-tree idiom and it is **wrong under DKMS**:
    dkms rewrites the leading `make` of `MAKE[0]` into `make -jN KERNELRELEASE=<kver>` and invokes the
    Makefile **directly** (`/usr/sbin/dkms` line ~1603, unconditional), so the test passes, make enters
    the kbuild half, and dies with `make[1]: *** No targets.  Stop.` Measured discriminator:
    | invocation | KERNELRELEASE | obj | src | M |
    |---|---|---|---|---|
    | kbuild include | set | `.` | `./.` | `/…/driver` |
    | DKMS direct | set | *empty* | *empty* | *empty* |
    **Only kbuild sets `obj`** — that is the test the Makefile now uses. Reproduce the dkms invocation
    without dkms: `make -j16 KERNELRELEASE=$(uname -r) KDIR=/lib/modules/$(uname -r)/build`.
  - **`CLEAN` is deprecated in dkms 3.x** (accepts only `true` silently) — dkms builds in a fresh copy
    of the source, so it is simply omitted from dkms.conf.
  - **★ DKMS SURVIVES A REAL KERNEL UPGRADE — VERIFIED (Aug 25 2026, the desktop, 7.1.8 -> 7.1.9).**
    Registered on 7.1.8, rebooted into 7.1.9, and `dkms status` came back with **two lines, both
    `installed`**; the new module sits at `/lib/modules/7.1.9-200.fc44.x86_64/extra/snd-clarett.ko.xz`,
    reports `0.1.0`, and is signed with the host's own DKMS MOK. Note **which** autoinstall path this
    exercised: 7.1.9 was already on disk before the driver was registered, so the in-transaction
    `kernel-install` hook could not fire for it and the rebuild came from **`dkms.service` at boot**.
    The in-dnf-transaction variant is still untested — do that one on the next kernel with the driver
    already registered.
  - **★ THE PACKAGED INSTALL AUTOLOADS AND PROBES ON REAL HARDWARE (Aug 25 2026, the desktop on 7.1.9,
    8Pre).** First end-to-end confirmation of the *packaged* path, as opposed to `insmod`: plugging the
    interface in loaded the driver with **no `modprobe`, no udev rule and no `modules-load.d` entry**,
    then probed and registered the card — `card 4 [C8Pre]`, bound at `0000:1a:00.0`, `initstate: live`,
    module `0.1.0` (the DKMS copy), `refcnt: 2` (PipeWire holding a PCM). The chain, each link verified:
    `MODULE_DEVICE_TABLE(pci,…)` → alias `pci:v00001CB5d00000002sv*sd*bc*sc*i*` in the `.ko` →
    `modules.alias` (written by depmod **during the dkms install**) → the device's own modalias
    `pci:v00001CB5d00000002sv00001CB5sd00000002bc04sc01i00` → udev's kmod builtin. `insmod` never got
    this because the module was not in the module path and depmod had never indexed it.
    Probe timing on that attach: `enabling device` → model line was **3 s**, i.e. `settle_ms` working as
    designed, first attempt.
    **Two taint lines are normal here and neither indicates a fault.** `loading out-of-tree module
    taints kernel` is flag `O`, unavoidable for any out-of-tree module. `module verification failed:
    signature and/or required key missing` does **NOT** mean unsigned — `modinfo` shows
    `signer: DKMS module signing key`; it means the kernel has no *trusted* key to check that signature
    against because the MOK is not enrolled. Secure Boot is off, so it loads and taints with `E`.
    Enrolling the MOK removes the line (and is what would let it load at all under Secure Boot).
  - **★★ `dnf install dkms` INSTALLS A PARTIAL KERNEL THAT BOOTS BROKEN — CHECK THE TRANSACTION
    (Aug 25 2026; cost a broken boot on the desktop).** Fedora's `dkms` carries a rich dep
    `(kernel-devel-matched if kernel-core)`, which resolves to `kernel-devel` + **`kernel-core`** — and
    **nothing in that chain requires `kernel`, `kernel-modules` or `kernel-modules-extra`**. If the
    three-kernel `installonly_limit` also evicts the oldest kernel in the same transaction, the lists
    come out asymmetric: six packages removed at the old version, four installed at the new one.
    `kernel-install` still writes a BLS entry for the half-installed kernel **and makes it the
    default**, so the next reboot lands on it.
    **Symptom:** boots, but 800x600 with no network — every DRM driver (`amdgpu`/`nouveau`/`i915`) and
    `atlantic`/`iwlwifi`/`mac80211` live in `kernel-modules`; only `igb`/`e1000e`/`r8169`/`igc` are in
    `kernel-modules-core`, so a plain gigabit port may still work while 10GbE and Wi-Fi do not.
    **The check, before saying yes to any transaction that touches a kernel:** every package removed at
    the old version must have a counterpart installed at the new one. `rpm -q kernel kernel-core
    kernel-modules kernel-modules-core | sort` confirms it afterwards.
    **Recovery:** do NOT try to fix it from the broken system (no network). Pick the previous kernel from
    the GRUB menu (Esc / hold Shift), then install the missing packages **version-pinned** —
    `sudo dnf install kernel-<ver> kernel-modules-<ver> kernel-modules-extra-<ver>`, because a bare
    `dnf install kernel` resolves to whatever is newest and leaves the broken entry as the default —
    then `sudo dracut -f --kver <ver>` to rebuild its initramfs with the drivers now present.
    **Unrelated, so don't chase them:** disabling kdump, deleting `/boot/*kdump.img`, and
    `grubby --remove-args=crashkernel` had nothing to do with it, nor did the DKMS install, which only
    ever writes to `/usr/src` and `/lib/modules/<kver>/extra`.
  - **A tight `/boot` is a live constraint on this work (the desktop: 974 MB, was 96% full).** The
    transaction above first failed outright with *"needs 33MB more space on the /boot filesystem"* —
    rpm installs before it erases, so it cannot rely on the eviction it is about to perform. Biggest
    win there was **orphaned kdump initramfs images for kernels no longer installed** (~57 MB each;
    three of them), a known Fedora wart where `kdumpctl`'s images outlive their kernel. Disabling
    kdump entirely (`systemctl disable --now kdump.service`) stops new ones: `60-kdump.install` does
    **nothing** on `add` ("kdump initramfs is strictly host only and managed by kdump service"), so the
    service is the only creator, and `92-crashkernel.install` is gated on
    `_should_reset_crashkernel()` = `auto_reset_crashkernel != no` **AND** `systemctl is-enabled kdump`,
    so disabling the service also stops `crashkernel=` being re-added to new kernels. Tradeoff worth
    stating: that gives up vmcore capture on the box where this driver has panicked hosts before.
  - **★ THE AKMOD ROUTE IS VERIFIED ACROSS A REAL KERNEL UPGRADE TOO (Aug 25 2026, the desktop,
    7.1.9 -> 7.1.10, 8Pre attached).** DKMS was uninstalled first (see the path clash below), the akmod
    built from `packaging/snd-clarett-kmod.spec` with `--define 'buildforkernels akmod'`, and the
    timeline out of `rpm -q --qf %{INSTALLTIME:date}` + `journalctl -u akmods` is unambiguous:
    | 22:02:29 | `akmod-snd-clarett` installed (running 7.1.9) |
    | 22:02:32 | `kmod-snd-clarett-7.1.9` built, 3 s later, at install time |
    | 22:06:08 | `kernel-core-7.1.10` installed — **no kmod produced** |
    | 22:08:38 | reboot |
    | 22:08:47-22:09:00 | `akmods.service`: "Building and installing snd-clarett-kmod [OK]" |
    Result: per-kernel `kmod-snd-clarett-7.1.9` **and** `-7.1.10` both installed, and the module
    autoloaded and bound on the PCI modalias exactly as the DKMS install had.
    - **THE REBUILD IS BOOT-TIME BY DESIGN, NOT IN-TRANSACTION** — `akmods.service` is literally
      "Builds and install new kmods from akmod packages" at boot. So the old "in-dnf-transaction
      rebuild" TODO was **mis-framed for akmods**: there is nothing missing to test. (DKMS's own
      in-transaction behaviour stays unobserved rather than disproven — our DKMS run had the new kernel
      on disk *before* the module was registered, so no transaction hook could have fired for it.)
    - **AKMOD MODULES ARE SIGNED as well**, with akmods' own locally generated key
      (`signer: <hostname>_<epoch>_<uuid8>`) — a different mechanism from DKMS's
      `/var/lib/dkms/mok.*` but the same outcome, and the same un-enrolled-MOK taint line.
    - **THE TWO ROUTES MUST NOT BE INSTALLED TOGETHER — different paths, both in depmod's search
      path:** akmod installs to `extra/snd-clarett/snd-clarett.ko.xz` (kmodtool's per-module
      `%{kmodinstdir_postfix}` subdirectory), DKMS to a flat `extra/snd-clarett.ko.xz`. Uninstall one
      before installing the other; `modinfo -n snd-clarett` names which one actually wins.
    - **The partial-kernel check (below) WORKED when applied:** the upgrade was driven as
      `dnf upgrade kernel kernel-devel`, and naming `kernel` is what makes it safe — the metapackage
      requires `kernel-core-uname-r`, `kernel-modules-uname-r`, `kernel-modules-core-uname-r` and
      (matched) `kernel-modules-extra`. All five landed at 7.1.10, 7.1.7 evicted cleanly. **Note
      `akmods` carries the same `(kernel-devel-matched if kernel-core)` rich dep that `dkms` does**, so
      its install transaction needs the same read-before-yes.
  - **★★ SECURE BOOT WORKS VIA THE AKMOD ROUTE — VERIFIED (Aug 27 2026, EliteBook 640 G11).** MOK
    enrolment done and the module loads with Secure Boot enforcing, which is itself the proof the
    signature is trusted: `module.sig_enforce` is set under Secure Boot, so an untrusted module is
    *rejected*, not merely taint-flagged.
    - **akmods signing needs NO configuration and never did.** `kmodgenca` runs at the first
      `akmods.service` run and writes the pair to `/etc/pki/akmods/{private/private_key.priv,
      certs/public_key.der}`; the signer string is literally its
      `KEYNAME="${cert_hostname:0:44}_$(date +%s)_$(uuidgen | awk -F- '{print $1}')"`. Enrol with
      `sudo mokutil --import /etc/pki/akmods/certs/public_key.der`, reboot, MOK Management →
      Enroll MOK (**physical console only, one boot only, QWERTY keyboard regardless of layout**).
    - **`public_key.der` is a SYMLINK to `<KEYNAME>.der`, not the key** (`kmodgenca` `update_key_symlinks`),
      so a broken symlink is a distinct failure from a missing key — `readlink -e` is the discriminator.
      And the certs/private dirs are `0750 root:akmods`, so a non-root `ls` says *Permission denied*,
      which reads like absence. Check both before concluding no key exists.
    - **★ THE BLOCKER WAS NOT MODULE SIGNING AT ALL — HP SHIPS SECURE BOOT WINDOWS-ONLY.** Enabling
      Secure Boot gave `Selected boot image did not authenticate` from the firmware, i.e. **shim
      failing before Linux exists**, which no amount of MOK work can touch. Cause: `db` held only
      Microsoft's *Windows* CAs (`Windows UEFI CA 2023`, `Microsoft Windows Production PCA 2011`) and
      **not the third-party UEFI CA that signs every Linux distro's shim**. HP gates that behind a
      separate BIOS toggle, **"Enable MS UEFI CA key"** (Security → Secure Boot Configuration) —
      turning it on fixed it outright. **The one-line diagnostic:** `sudo mokutil --db | grep -i Microsoft`,
      then read the *Subject* lines — Windows-only CAs mean Windows-only Secure Boot.
    - **Ruled out first, cheaply, and worth doing in this order:** `efibootmgr -v` named
      `\EFI\fedora\shimx64.efi` (not `grubx64.efi` — the classic `grub2-install`-broke-the-chain cause),
      `shim-x64`/`grub2-efi-x64` installed, ESP contents intact. Only after the Fedora side was proven
      clean did it make sense to suspect firmware. Note "restore factory keys" would NOT have fixed
      this: the third-party CA is behind the toggle, not in HP's default key set.
  - **★ LICENSING SETTLED (Aug 24 2026): GPL-2.0-only.** `snd-clarett/LICENSE` is the verbatim FSF GPL v2
    (md5 `b234ee4d69f5fce4486a80fdaf4a4263` — the canonical checksum; **check it**, since several
    copies on a Fedora box carry the obsolete *59 Temple Place* address and are NOT the current text).
    `snd-clarett/LICENSES/Linux-syscall-note.txt` holds the exception that `clarett_fcp_uapi.h`'s
    `GPL-2.0 WITH Linux-syscall-note` tag refers to — taken verbatim from a real `linux-headers`
    tree, and byte-identical (modulo a trailing newline) to alsa-scarlett-gui's copy, whose flat
    REUSE-style `LICENSES/<id>.txt` layout this matches. Both RPMs register both files via `%license`.
    - **`MODULE_LICENSE("GPL")` is CORRECT alongside SPDX `GPL-2.0-only` — do not "fix" it.**
      `include/linux/module.h` documents `"GPL"` as *[GNU Public License v2]* and states outright
      that for module loading the only/or-later distinction "is completely irrelevant and does
      neither replace the proper license identifiers in the corresponding source file nor amends
      them in any way". Its sole job is Proprietary flagging and `EXPORT_SYMBOL_GPL` binding.
      Likewise the uapi header's `GPL-2.0` (rather than `GPL-2.0-only`) is deliberate kernel uapi
      idiom; the kernel's own `LICENSES/preferred/GPL-2.0` lists both spellings as valid.
  - **OPEN before a 0.1.0 tag:** the specs carry **no `%changelog`**, deliberately — entries are
    dated, and `snd-clarett/` is under the no-dates rule. Decide at first release whether a *release*
    date is exempt (it is not an RE observation date) or whether the changelog lives outside
    `snd-clarett/`. rpmbuild only warns (`%source_date_epoch_from_changelog ... no entries`).
