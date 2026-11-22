# nextbsd-installer

A dialog-driven, text-mode installer for headless / console NextBSD
deployments — a **C++ FTXUI front-end** over a **`/bin/sh` engine**.

- **UI (this binary):** FTXUI (MIT), statically linked from the vendored
  `../ftxui`. Amber-on-black, truecolor, keyboard-driven. Five screens:
  Mode → Disk → Account → Install → Finish.
- **Engine (`engine/*.sh`):** the dangerous, auditable work — partition, `newfs`,
  `cpdup` clone, bootcode/ESP, account, hostname. The binary forks these and
  parses their `PROGRESS` / `STATUS` lines. cpdup is vendored at `../cpdup`.

The installed root is laid down by cloning the running system with **cpdup** and
labeling the target UFS `NEXTBSD` (not the medium's `ROOTFS`). Root is found by
`vfs.root.mountfrom` — in `loader.conf` on EFI, on the `cmdline.txt` line on a Pi —
never by an fstab line, so the install is disk-path agnostic (ada0/nvd0/vtbd0 all
just work). No `/etc/fstab` is shipped (nextbsd-overlays#5).

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
