# nextbsd-installer

A dialog-driven, text-mode installer for headless / console NextBSD
deployments — a **C++ FTXUI front-end** over a **`/bin/sh` engine**.

- **UI (this binary):** FTXUI (MIT), statically linked from the vendored
  `../ftxui`. Amber-on-black, truecolor, keyboard-driven. Five screens:
  Mode → Disk → Account → Install → Finish.
- **Engine (`engine/*.sh`):** the dangerous, auditable work — partition, `newfs`,
  `cpdup` clone, bootcode/ESP, account, hostname. The binary forks these and
  parses their `PROGRESS` / `STATUS` lines. cpdup is vendored at `../cpdup`.

The installed root is laid down by cloning the running system with **cpdup** onto
**ZFS** (E31, nextbsd/nextbsd#600; the install media stay UFS): pool `NextBSD`
(`ashift=12`, `compression=lz4`, `atime=off`, `autotrim=on`), boot environment
`NextBSD/ROOT/default` (the pool's `bootfs`), and the shared datasets `Local`,
`root`, `private/tmp`, `private/var/log` and `private/var/tmp`. PCs get GPT with
`gptzfsboot` (BIOS), the ESP with `loader.efi` and `freebsd-zfs`; the loader boots
the `bootfs`, so `loader.conf` names no root (a USB target gets
`vfs.mountroot.timeout`). A Raspberry Pi 5-family board keeps MBR + FAT
(`config.txt`, DTB, `kernel8.img`) with the pool in the second slice, and
`cmdline.txt` names `vfs.root.mountfrom=zfs:NextBSD/ROOT/default
vfs.mountroot.timeout=30`. launchctl's boot pass mounts the datasets
(nextbsd-userland#200). No swap, no `/etc/fstab` (nextbsd-overlays#5).

Copy mode (`-c`) makes ZFS too, in a pool named `NextBSD-xxxx` so a copy never
shares the running pool's name: from a ZFS system by `zfs send` of a recursive
snapshot (the running boot environment and the shared datasets, without
`/tmp`, `/var/tmp` or the other BEs), from a UFS system by `cpdup` into the new
datasets (the way an existing UFS install moves to ZFS). The copy gets its own
identity as before, and also leaves out the display arrangement
(`/Local/Library/Preferences/org.nextbsd.windowserver.displays.plist`) and
`zpool.cache`. Install-over (`-U`) on a ZFS disk writes a new boot environment
(named after the build, e.g. `NextBSD-27.0.0-186`), makes it the `bootfs` (and
the Pi's `cmdline.txt` root), carries over the host's identity from the old one,
and leaves `Local`, `root` and the `private` datasets alone; the old BE stays
bootable. `-U` on a UFS disk works as before. `NEXTBSD_FS=ufs` makes UFS (an
escape hatch, like `NEXTBSD_LAYOUT`).

## Shared with Installer.app

Installer.app (nextbsd/apps-installer) drives the same two scripts, through
SettingsHelper's `InstallStart` (`src/SettingsHelper/sh_install.c`), which
runs them as root without a shell:

- `probe-disks.sh` also prints `SYSTEM` (`image` when running from the install
  image, `installed` otherwise), `NEED` (the smallest usable disk, in bytes:
  30 GB, or more for a copy of a fuller system) and each disk's size in bytes
  (DISK's sixth field). It reads GEOM's configuration, so it works without
  root. Memory disks are listed only while `/var/run/nextbsd-installer.test-disks`
  exists (root creates it; it's gone after a reboot), for trying an install on
  a file-backed `md` disk.
- `do-install.sh -c` copies the running system instead of installing the
  image: from a UFS snapshot of `/`, leaving out `/tmp`, `/var/tmp`, `/var/run`,
  `/var/vm` and `/Volumes`' contents, and giving the copy its own root label
  (`NEXTBSD-xxxx`), new SSH host keys at first boot and no host ID, random seed
  or DHCP identity of this machine's. It doesn't add an NVRAM boot entry (nor
  for an `md` disk, ever). `PHASE` lines (`prepare`, `copy`, `setup`,
  `finish`) mark the stages, and `PROGRESS` climbs through the copy.

## Build & test locally (no hardware, no root)

```sh
cmake -S src/nextbsd-installer -B build -G Ninja
cmake --build build -j

./build/nextbsd-installer --demo      # synthetic disks, simulated install
```

`--demo` is implied automatically on any non-FreeBSD host, so on macOS/Linux you
get the full UI with fake disks and a simulated cpdup clone — drive every screen,
watch the gauge, with nothing touched. (`--dry-run` makes the engine print each
command instead of running it on a real target.)

Requires CMake ≥ 3.16 and a C++17 compiler. FTXUI builds from the vendored
sibling — no system packages needed.

## Cross-build (CI, amd64 + arm64)

Same as the rest of userland: pass the cross toolchain file and the staged
compat sysroot —

```sh
SYSROOT=/path/to/sysroot CROSS_BINDIR=/path/to/toolchain/bin \
cmake -S src/nextbsd-installer -B build-amd64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/cross-amd64.cmake"
cmake --build build-amd64 -j
DESTDIR=/stage cmake --install build-amd64   # -> /stage/usr/sbin + /usr/libexec
```

## Status

Front-end: all five screens implemented (amber theme, live gauge). Engine:
`do-install.sh` / `probe-disks.sh` carry the real command sequence (dry-run
aware); the C++↔shell wiring for the on-target path (parsing engine output,
hostname via the launchd-native `hostnamed` store) is the next pass — the
off-target `--demo` path is fully functional today.
