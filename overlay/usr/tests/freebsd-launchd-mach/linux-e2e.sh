#!/bin/sh
# linux-e2e.sh — Linux compatibility end to end (nextbsd-userland#190).
#
# Proves the #190 method: launchctl bootstrap pre-creates and mounts the Linux
# ABI filesystems under compat.linux.emul_path BEFORE any Linux userland
# exists, and a Linux userland installed afterwards works on top of them.
#
#   1. the mounts are present on an EMPTY root (nothing but mount points)
#   2. `pkg install claude-code` populates that already-mounted root (it pulls
#      linux_base-rl9) without tripping over the existing dirs or mounts
#   3. the mounts are all still present afterwards
#   4. a Linux binary from the root runs (uname -s says Linux)
#   5. `claude --help` runs. The port's wrapper itself checks linprocfs,
#      fdescfs, and linrdlnk (dev/fd/2 must be a symlink), so this is also
#      an independent check of the mount options.
#
# Uses FreeBSD's ports repo only as the quickest way to get a Linux userland
# and a real Linux app into CI; NextBSD's own path is debootstrap (#241).
#
# Emits exactly one LINUX-E2E-OK / -FAIL / -SKIP line (SKIP only when
# pkg.FreeBSD.org is unreachable from the VM), then returns.

set -u
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin
export PATH

fail() { echo "LINUX-E2E-FAIL: $*"; exit 0; }
skip() { echo "LINUX-E2E-SKIP: $*"; exit 0; }

tool=/usr/libexec/nextbsd-linux
[ -x "$tool" ] || fail "$tool missing"
emul=$(sysctl -n compat.linux.emul_path 2>/dev/null)
emul=${emul:-/compat/linux}

echo "==> linux-e2e: 1. mounts on an empty root"
"$tool" --status || fail "Linux ABI mounts not all present before any userland was installed"
echo "--- contents of $emul before install:"
ls -la "$emul"
df -h /

echo "==> linux-e2e: 2. pkg install claude-code into the mounted root"
case "$(uname -p)" in
amd64)   abi=FreeBSD:15:amd64 ;;
aarch64) abi=FreeBSD:15:aarch64 ;;
*)       skip "no FreeBSD package set for $(uname -p)" ;;
esac
fetch -q -T 30 -o /dev/null "http://pkg.FreeBSD.org/$abi/quarterly/meta.conf" 2>/dev/null ||
    skip "pkg.FreeBSD.org unreachable from the VM"
mkdir -p /etc/pkg
cat > /etc/pkg/FreeBSD.conf <<EOF
FreeBSD: {
  url: "pkg+http://pkg.FreeBSD.org/\${ABI}/quarterly",
  mirror_type: "srv",
  signature_type: "fingerprints",
  fingerprints: "/usr/share/keys/pkg",
  enabled: yes
}
EOF
# Keep the package cache off the small CI root.
mkdir -p /var/cache/pkg
mount -t tmpfs -o size=1g tmpfs /var/cache/pkg 2>/dev/null || true
ASSUME_ALWAYS_YES=yes
IGNORE_OSVERSION=yes
export ASSUME_ALWAYS_YES IGNORE_OSVERSION
pkg bootstrap -f >/tmp/linux-e2e-pkg.log 2>&1 || { cat /tmp/linux-e2e-pkg.log; fail "pkg bootstrap failed"; }
pkg install -y claude-code >>/tmp/linux-e2e-pkg.log 2>&1
prc=$?
cat /tmp/linux-e2e-pkg.log
[ "$prc" -eq 0 ] || fail "pkg install claude-code exited $prc"
if grep -iE "(error|cannot|failed|busy|exists).*${emul}" /tmp/linux-e2e-pkg.log; then
    fail "pkg reported a problem under $emul (see above)"
fi
df -h /

echo "==> linux-e2e: 3. mounts still intact after install"
"$tool" --status || fail "Linux ABI mounts changed during pkg install"

echo "==> linux-e2e: 4. a Linux binary from the root"
lu=$(ls "$emul"/usr/bin/uname "$emul"/bin/uname 2>/dev/null | head -1)
[ -n "$lu" ] || fail "no uname in $emul after installing linux_base"
os=$("$lu" -s 2>&1)
echo "$lu -s -> $os"
[ "$os" = Linux ] || fail "$lu -s printed '$os', expected Linux"

echo "==> linux-e2e: 5. claude --help"
out=$(timeout 120 claude --help </dev/null 2>&1)
crc=$?
echo "$out" | head -25
[ "$crc" -eq 0 ] || fail "claude --help exited $crc"
echo "$out" | grep -q "Usage" || fail "claude --help printed no usage text"

echo "LINUX-E2E-OK: pkg installed claude-code into the pre-mounted $emul; mounts intact; Linux uname and claude --help ran"
