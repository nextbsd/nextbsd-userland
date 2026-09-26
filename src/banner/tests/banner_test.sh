#!/bin/sh
# Host-side tests for nextbsd-fetch(1).
#
# The gates and the colour decision are what matter here, and all of them
# are decidable without a real machine: the loginwindow path and os-release
# are build-time overrides, ~/.hushlogin is just $HOME, and the colour depth
# comes from the environment. The task list's gate is the root filesystem's
# type, and no host here has a unionfs root, so the type that means "live"
# is a build-time override too: name this host's own and the same statfs
# says live media. What is NOT covered is the readings themselves -- memory,
# uptime and the root filesystem are whatever this host happens to have, so
# the tests assert their shape rather than their values.
#
#   sh banner_test.sh
#
# Run under sh, not zsh.
set -u

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
fix=${TMPDIR:-/tmp}/banner_test.$$
trap 'rm -rf "$fix"' EXIT
mkdir -p "$fix/home" "$fix/lw"

cat > "$fix/os-release" <<'OSR'
NAME="NextBSD"
PRETTY_NAME="NextBSD 20260924-211500"
ID=nextbsd
VERSION="20260924-211500"
OSR

build() {   # build <binary> <loginwindow path> [filesystem type that is live]
	${CC:-cc} -O1 -g -Wall -Wextra -Wshadow -Wstrict-prototypes \
	    -Wmissing-prototypes \
	    -DBANNER_LOGINWINDOW="\"$2\"" \
	    -DBANNER_OS_RELEASE="\"$fix/os-release\"" \
	    ${3:+-DBANNER_LIVE_FSTYPE="\"$3\""} \
	    -I"$top/src/banner" -o "$1" "$top/src/banner/banner.c" || exit 2
}
build "$fix/fetch"   "$fix/lw/absent.plist"
build "$fix/fetch-d" "$fix/lw/present.plist"
: > "$fix/lw/present.plist"

# What this host's root actually is, so one build can be told that is live.
cat > "$fix/rootfs.c" <<'PROBE'
#include <sys/param.h>
#include <sys/mount.h>
#include <stdio.h>
int main(void) { struct statfs f; if (statfs("/", &f) == -1) return 1;
	puts(f.f_fstypename); return 0; }
PROBE
${CC:-cc} -o "$fix/rootfs" "$fix/rootfs.c" || exit 2
rootfs=$("$fix/rootfs") || exit 2
build "$fix/fetch-l" "$fix/lw/absent.plist" "$rootfs"

pass=0; fail=0
ck()    { if [ "$2" = "$3" ]; then pass=$((pass+1)); else fail=$((fail+1))
          printf 'FAIL %-44s got [%s] want [%s]\n' "$1" "$2" "$3"; fi; }
ckhas() { if printf '%s' "$2" | grep -q -- "$3"; then pass=$((pass+1)); else fail=$((fail+1))
          printf 'FAIL %-44s no /%s/ in output\n' "$1" "$3"; fi; }
cknot() { if printf '%s' "$2" | grep -q -- "$3"; then fail=$((fail+1))
          printf 'FAIL %-44s /%s/ should not be there\n' "$1" "$3"; else pass=$((pass+1)); fi; }

HOME=$fix/home; export HOME
out=$("$fix/fetch" -f 2>&1)

# ---- what it prints
ckhas "the heading"                 "$out" "Welcome to NextBSD!"
ckhas "the version from os-release" "$out" "NextBSD 20260924-211500"
ckhas "a kernel line"               "$out" "kernel"
ckhas "a shell line"                "$out" "shell"
ckhas "a memory line"               "$out" "memory"
ckhas "an uptime line"              "$out" "uptime"
ckhas "a storage line"              "$out" "storage"
cknot "no userland line"            "$out" "userland"
cknot "no desktop line"             "$out" "desktop"
cknot "and no user@host heading"    "$out" "@"

# The cube is drawn with block characters, and there are a lot of them: a
# handful would mean the art collapsed rather than rendered.
nblocks=$(printf '%s' "$out" | tr -cd '█' | wc -c | tr -d ' ')
if [ "$nblocks" -gt 100 ]; then pass=$((pass+1)); else fail=$((fail+1))
   printf 'FAIL %-44s only %s block characters\n' "the cube is drawn" "$nblocks"; fi

# ---- gate: the task list, from the same statfs as the storage line
cknot "no task list on an installed machine"  "$out" "Running from live media"
cknot "and no installer to run"               "$out" "nextbsd-installer"
cknot "storage names a device then"           "$out" "live media"
out9=$("$fix/fetch-l" -f 2>&1)
ckhas "storage reads live media on unionfs"   "$out9" "storage  live media"
ckhas "and the task list follows"             "$out9" "Running from live media. Nothing is written to disk."
ckhas "01 passwd, without sudo"               "$out9" "01   set a password for admin    passwd"
ckhas "02 tzsetup"                            "$out9" "02   set the time zone           sudo tzsetup"
ckhas "03 wlan"                               "$out9" "03   connect to wireless         sudo wlan"
ckhas "04 the installer"                      "$out9" "04   install NextBSD to disk     sudo nextbsd-installer"
cknot "passwd is never under sudo"            "$out9" "sudo passwd"
ck    "the list comes after the banner" \
      "$(printf '%s\n' "$out9" | grep -n 'Welcome to NextBSD!\|^  01 ' | cut -d: -f1 | tr '\n' ' ')" \
      "$(printf '%s\n' "$out9" | grep -n 'Welcome to NextBSD!\|^  01 ' | cut -d: -f1 | sort -n | tr '\n' ' ')"
cknot "no escapes off a terminal, list included" "$out9" "$(printf '\033')"

# ---- gate: the login window
out2=$("$fix/fetch-d" 2>&1)
ck    "a login window silences it"      "$(printf '%s' "$out2" | wc -c | tr -d ' ')" "0"
ck    "and it still succeeds"           "$("$fix/fetch-d" >/dev/null 2>&1; echo $?)" "0"
out3=$("$fix/fetch-d" -f 2>&1)
ckhas "-f overrides the login window"   "$out3" "Welcome to NextBSD!"

# ---- gate: ~/.hushlogin
: > "$fix/home/.hushlogin"
out4=$("$fix/fetch" 2>&1)
ck    "~/.hushlogin silences it"        "$(printf '%s' "$out4" | wc -c | tr -d ' ')" "0"
out5=$("$fix/fetch" -f 2>&1)
ckhas "-f overrides ~/.hushlogin"       "$out5" "Welcome to NextBSD!"
rm -f "$fix/home/.hushlogin"

# ---- colour: never escapes when stdout is not a terminal
cknot "no escapes off a terminal"       "$out" "$(printf '\033')"

# ---- a missing os-release is not fatal
${CC:-cc} -O1 -Wall -DBANNER_OS_RELEASE="\"$fix/nope\"" \
    -DBANNER_LOGINWINDOW="\"$fix/lw/absent.plist\"" \
    -I"$top/src/banner" -o "$fix/fetch-n" "$top/src/banner/banner.c" || exit 2
out6=$("$fix/fetch-n" -f 2>&1)
ckhas "a missing os-release still prints" "$out6" "Welcome to NextBSD!"
ckhas "and falls back to the name"        "$out6" "NextBSD"

# ---- the shell line
out7=$(SHELL=/usr/local/bin/zsh ZSH_VERSION=5.9.2 "$fix/fetch" -f 2>&1)
ckhas "the shell's version when passed"   "$out7" "zsh 5.9.2"
out8=$(SHELL=/usr/local/bin/zsh "$fix/fetch" -f 2>&1)
cknot "and no guess when it is not"       "$out8" "zsh 5"
ckhas "just the name then"                "$out8" "zsh"

# ---- usage
ck "an unknown flag is a usage error"  "$("$fix/fetch" -Z >/dev/null 2>&1; echo $?)" "2"
ck "so is a stray argument"            "$("$fix/fetch" wat >/dev/null 2>&1; echo $?)" "2"

printf '\n%d checks, %d failures\n' "$((pass+fail))" "$fail"
if [ "$fail" -eq 0 ]; then
	echo "BANNER-OK: the gates, the fields, the task list, the fallbacks and the colour rule"
	exit 0
fi
echo "BANNER-FAIL"
exit 1
