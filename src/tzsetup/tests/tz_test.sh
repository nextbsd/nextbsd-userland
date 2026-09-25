#!/bin/sh
# Host-side tests for tzsetup's non-terminal halves.
#
# The dialog is FTXUI and is not driven here. What is driven is every line the
# dialog calls once somebody presses Enter -- the table parser and the apply
# layer -- which is where a mistake would cost something. Each path is a
# parameter, so nothing writes to /etc and no privilege is needed, and
# launchctl is a stub that records its arguments.
#
#   sh tz_test.sh
set -u

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
fix=${TMPDIR:-/tmp}/tz_test.$$
trap 'rm -rf "$fix"' EXIT
mkdir -p "$fix/root" "$fix/root/zi"

# The zone table has to come from somewhere real: parsing is half of what is
# under test, and a hand-written table would only prove the parser reads what I
# wrote. Use the host's, whichever name it ships under.
zi=""
for d in /usr/share/zoneinfo; do
    if [ -f "$d/zone1970.tab" ] || [ -f "$d/zone.tab" ]; then zi=$d; break; fi
done
if [ -z "$zi" ]; then
    echo "TZ-SKIP: no zoneinfo table on this host"
    exit 0
fi

cat > "$fix/root/launchctl" <<'STUB'
#!/bin/sh
printf '%s\n' "$*" >> "$(dirname "$0")/launchctl.log"
exit 0
STUB
chmod +x "$fix/root/launchctl"

${CXX:-c++} -std=c++17 -O1 -g -Wall -Wextra \
    -I"$top/src/tzsetup" \
    -o "$fix/tz_test" \
    "$here/tz_test.cpp" "$top/src/tzsetup/zones.cpp" "$top/src/tzsetup/apply.cpp" || exit 2

"$fix/tz_test" "$zi" "$fix/root"
rc=$?

# The stub must have been asked to load the job, not something else.
if [ "$rc" -eq 0 ]; then
    if ! grep -q "load -w" "$fix/root/launchctl.log" 2>/dev/null; then
        echo "FAIL: launchctl was not asked to load -w"
        exit 1
    fi
fi
exit $rc
