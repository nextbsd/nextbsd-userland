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
