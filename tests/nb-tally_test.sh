#!/bin/sh
# tests/nb-tally_test.sh — host-native test of overlay/usr/tests/nb-tally.sh
# (the sentinel + exit-code machinery for on-image test suites, epic
# nextbsd/nextbsd#443 T2).
#
# Runs on the runner (no booted image needed): it exercises the same
# primitives the image's /bin/sh will run — fifo, background reader, fd
# dance, grep counts — under dash, the strictest common POSIX shell.
#
# Scenarios: a clean pass (rc 0), a failure (rc = fail count, sentinel last,
# relay byte-for-byte), the 255 cap (sentinel prints the true count, the exit
# code is capped), a signal death (partial sentinel, rc 143), and the
# missing-helper fallback (done marker, no sentinel, rc 2 = hang, never pass).
#
# Run: sh tests/nb-tally_test.sh   (exit 0 = all scenarios pass)

set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
HELPER=$ROOT/overlay/usr/tests/nb-tally.sh

if [ ! -r "$HELPER" ]; then
    echo "FATAL: $HELPER not found" >&2
    exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fails=0
ok() { echo "OK   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

# check <name> <expected-rc> <actual-rc>
check_rc() {
    if [ "$2" = "$3" ]; then ok "$1 (rc=$3)"; else bad "$1: expected rc $2, got $3"; fi
}
# check_out <name> <file> <fixed substring that must be present>
check_out() {
    if grep -qF "$3" "$2" 2>/dev/null; then ok "$1"; else bad "$1: '$3' not in output"; fi
}
# check_absent <name> <file> <fixed substring that must NOT be present>
check_absent() {
    if grep -qF "$3" "$2" 2>/dev/null; then bad "$1: '$3' present where it must not be"; else ok "$1"; fi
}
# check_last <name> <file> <expected final line>
check_last() {
    last=$(tail -1 "$2" 2>/dev/null)
    if [ "$last" = "$3" ]; then ok "$1"; else bad "$1: last line was '$last', expected '$3'"; fi
}
# check_count <name> <file> <n> — the file must have exactly n lines
check_count() {
    n=$(wc -l < "$2" 2>/dev/null || echo -1)
    if [ "$n" = "$3" ]; then ok "$1"; else bad "$1: $n lines, expected $3"; fi
}

# write_suite <file> <helper-path-to-source> — a suite shaped like the real
# ones: source (or fall back), guard, trap, setup, then $BODY.
write_suite() {
    cat > "$1" <<EOF
#!/bin/sh
set -u
nbsd_done=0
if [ -r "$2" ]; then
    . "$2"
else
    NB_TALLY_STARTED=0
    nb_tally_setup() { :; }
    nb_tally_read() { NB_OK=0; NB_FAIL=0; NB_SKIP=0; }
    nb_tally_finish() { [ -n "\${1:-}" ] && echo "\$1"; return 0; }
    nb_tally_exit() { nbsd_finish; exit 2; }
fi
nbsd_finish() {
    [ "\$nbsd_done" = 1 ] && return
    nbsd_done=1
    nb_tally_finish "SUITE-RUN-DONE"
}
trap 'nbsd_finish' EXIT
trap 'nbsd_finish; exit 129' HUP
trap 'nbsd_finish; exit 130' INT
trap 'nbsd_finish; exit 143' TERM
nb_tally_setup
EOF
}

# --- 1. a clean pass ---------------------------------------------------------
write_suite "$TMP/suite-pass.sh" "$HELPER"
cat >> "$TMP/suite-pass.sh" <<'EOF'
echo "not a marker: plain chatter"
echo "ACCT-HELPERS-OK: helpers roundtrip"
echo "ACCT-PW-OK: password changed"
/usr/bin/printf 'CHILD-OK: marker from a child process\nchild chatter line\n'
echo "WEDGE-CHECK-SKIP: filter unavailable"
echo "lowercase note: contains -OK mid-line but does not start with a marker"
nb_tally_exit
EOF
sh "$TMP/suite-pass.sh" > "$TMP/out-pass" 2> "$TMP/err-pass"
check_rc "pass: exit code" 0 $?
check_count "pass: relay is complete and ordered" "$TMP/out-pass" 9
check_out "pass: child line relayed" "$TMP/out-pass" "child chatter line"
check_out "pass: done marker" "$TMP/out-pass" "SUITE-RUN-DONE"
check_last "pass: sentinel is the last line" "$TMP/out-pass" \
    "NEXTBSD-TEST-SUMMARY ok=3 fail=0 skip=1"
check_absent "pass: non-markers are not tallied" "$TMP/out-pass" "ok=4"
[ -s "$TMP/err-pass" ] && bad "pass: stderr noise: $(head -1 "$TMP/err-pass")" || ok "pass: quiet stderr"

# --- 2. a failure: rc = fail count, sentinel carries the truth ---------------
write_suite "$TMP/suite-fail.sh" "$HELPER"
cat >> "$TMP/suite-fail.sh" <<'EOF'
echo "ACCT-HELPERS-OK: helpers roundtrip"
echo "ACCT-PW-FAIL: mismatched confirmation"
echo "ACCT-CHPASS-FAIL: chpass refused"
nb_tally_exit
EOF
sh "$TMP/suite-fail.sh" > "$TMP/out-fail" 2> "$TMP/err-fail"
check_rc "fail: exit code" 2 $?
check_last "fail: sentinel is the last line" "$TMP/out-fail" \
    "NEXTBSD-TEST-SUMMARY ok=1 fail=2 skip=0"
[ -s "$TMP/err-fail" ] && bad "fail: stderr noise: $(head -1 "$TMP/err-fail")" || ok "fail: quiet stderr"

# --- 3. the cap: sentinel prints the truth, the exit code is capped ----------
write_suite "$TMP/suite-cap.sh" "$HELPER"
{
    echo "i=0"
    echo "while [ \$i -lt 300 ]; do"
    echo "    echo \"CAP-TEST-\$i-FAIL: failure \$i\""
    echo "    i=\$((i + 1))"
    echo "done"
    echo "nb_tally_exit"
} >> "$TMP/suite-cap.sh"
sh "$TMP/suite-cap.sh" > "$TMP/out-cap" 2> "$TMP/err-cap"
check_rc "cap: exit code capped at 255" 255 $?
check_last "cap: sentinel prints the uncapped truth" "$TMP/out-cap" \
    "NEXTBSD-TEST-SUMMARY ok=0 fail=300 skip=0"
check_count "cap: every line relayed" "$TMP/out-cap" 302

# --- 4. a signal death: partial sentinel, not a silent death ------------------
write_suite "$TMP/suite-sig.sh" "$HELPER"
cat >> "$TMP/suite-sig.sh" <<'EOF'
echo "ACCT-HELPERS-OK: survived"
kill -TERM $$
sleep 30   # the trap must exit before this completes
echo "UNREACHABLE"
EOF
sh "$TMP/suite-sig.sh" > "$TMP/out-sig" 2> "$TMP/err-sig"
check_rc "signal: rc 143" 143 $?
check_out "signal: done marker" "$TMP/out-sig" "SUITE-RUN-DONE"
check_last "signal: partial sentinel is last" "$TMP/out-sig" \
    "NEXTBSD-TEST-SUMMARY ok=1 fail=0 skip=0"
check_absent "signal: unreachable line did not print" "$TMP/out-sig" "UNREACHABLE"

# --- 5. the missing-helper fallback: done marker, no sentinel, rc 2 ----------
write_suite "$TMP/suite-fallback.sh" "$TMP/no-such-helper.sh"
cat >> "$TMP/suite-fallback.sh" <<'EOF'
echo "output still flows without the helper"
nb_tally_exit
EOF
sh "$TMP/suite-fallback.sh" > "$TMP/out-fb" 2> "$TMP/err-fb"
check_rc "fallback: rc 2 (no sentinel = hang, never pass)" 2 $?
check_out "fallback: output still flows" "$TMP/out-fb" "output still flows"
check_out "fallback: done marker for the old pull model" "$TMP/out-fb" "SUITE-RUN-DONE"
check_absent "fallback: no sentinel" "$TMP/out-fb" "NEXTBSD-TEST-SUMMARY"

echo
if [ "$fails" -gt 0 ]; then
    echo "nb-tally_test: $fails assertion(s) FAILED"
    exit 1
fi
echo "nb-tally_test: all scenarios passed"
exit 0
