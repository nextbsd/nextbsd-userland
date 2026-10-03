#!/bin/sh
# /usr/tests/nb-tally.sh — sentinel + exit-code machinery for on-image test
# suites (the nextbsd-ci sentinel contract; epic nextbsd/nextbsd#443, T2).
#
# A suite sources this, calls nb_tally_setup once (right after `set -u`,
# before its first output) and installs nbsd_finish as its EXIT trap body.
# Everything the suite and its children write to stdout is mirrored,
# unchanged, to the console through a counting reader: lines shaped like a
# capability marker (<NAME>-<OK|FAIL|SKIP>) are appended to a count file, so
# markers emitted by helper binaries and client tests are tallied exactly
# like markers echoed by the suite itself. nbsd_finish restores the console,
# drains the reader, prints the done marker and the NEXTBSD-TEST-SUMMARY
# aggregate as the LAST line, so:
#   - the serial harness gates on the suite's own verdict — a "green run with
#     failed tests" (the old 259 markers, exit 0 hole) is impossible;
#   - a direct (non-serial) consumer of the suite sees the verdict in the
#     exit code: 0 = no failures, N = N failures (capped at 255), 2 = the
#     sentinel could not be produced, which the harness treats as a hang,
#     never as a pass.
#
# Nothing here may abort a suite: every stage degrades to pass-through, and
# marker emission keeps its `|| true` semantics. The only hard failure mode
# (no /tmp for the count file or fifo) yields rc 2, which gates.

NB_TALLY_FIFO=/tmp/nb-tally-$$
NB_TALLY_COUNT=/tmp/nb-tally-count-$$
NB_TALLY_READER=
NB_TALLY_STARTED=0
NB_TALLY_SETTLED=0
NB_OK=0
NB_FAIL=0
NB_SKIP=0

nb_tally_setup() {
    [ "$NB_TALLY_STARTED" = 1 ] && return
    rm -f "$NB_TALLY_FIFO" "$NB_TALLY_COUNT"
    : > "$NB_TALLY_COUNT" || return
    mkfifo "$NB_TALLY_FIFO" 2>/dev/null || { rm -f "$NB_TALLY_COUNT"; return; }
    exec 3>&1
    # LC_ALL=C inside the reader only: the [A-Z] case class must be
    # byte-strict (a UTF-8 collation can make a range class match
    # lowercase), and the byte-for-byte relay is locale-transparent.
    {
        LC_ALL=C
        cat "$NB_TALLY_FIFO" | while IFS= read -r line || [ -n "$line" ]; do
            printf '%s\n' "$line" >&3
            # printf '%s\n', not printf '...\\n': dash's printf does not
            # process backslash escapes in the format string.
            case $line in
                [A-Z]*-OK*)   printf '%s\n' ok   >> "$NB_TALLY_COUNT" ;;
                [A-Z]*-FAIL*) printf '%s\n' fail >> "$NB_TALLY_COUNT" ;;
                [A-Z]*-SKIP*) printf '%s\n' skip >> "$NB_TALLY_COUNT" ;;
            esac
        done
    } 2>/dev/null &
    NB_TALLY_READER=$!
    exec > "$NB_TALLY_FIFO" || return
    NB_TALLY_STARTED=1
}

# Wait for the reader to drain the fifo and finish its last appends, then
# restore the console. The reader is asynchronous: ANY count read must happen
# after this, or it can miss the trailing lines.
#
# Runs at most ONCE (NB_TALLY_SETTLED): the console fd (3) is closed on the
# first settle, and a failed `exec >&3` is fatal in dash — a non-interactive
# shell exits (status 2) and no `|| true` can catch it — so the second call
# (nb_tally_finish after nb_tally_exit) must not touch the fds again.
nb_tally_settle() {
    [ "$NB_TALLY_SETTLED" = 1 ] && return
    NB_TALLY_SETTLED=1
    if [ "$NB_TALLY_STARTED" = 1 ] && [ -n "$NB_TALLY_READER" ]; then
        exec >&3 2>/dev/null || true
        exec 3>&- 2>/dev/null || true
        wait "$NB_TALLY_READER" 2>/dev/null || true
    fi
}

# Reads the count file into NB_OK / NB_FAIL / NB_SKIP (all default to 0).
# Call nb_tally_settle first, so the count file is final.
nb_tally_read() {
    NB_OK=$(grep -c '^ok$'   "$NB_TALLY_COUNT" 2>/dev/null || true)
    NB_FAIL=$(grep -c '^fail$' "$NB_TALLY_COUNT" 2>/dev/null || true)
    NB_SKIP=$(grep -c '^skip$' "$NB_TALLY_COUNT" 2>/dev/null || true)
    NB_OK=${NB_OK:-0}
    NB_FAIL=${NB_FAIL:-0}
    NB_SKIP=${NB_SKIP:-0}
}

# $1 = the done marker (e.g. LAUNCHD-MACH-RUN-DONE). The caller guards
# against double runs with its own done flag. Drains the reader, prints the
# done marker, then the sentinel (last line) — in that order, so the
# sentinel is last.
nb_tally_finish() {
    nb_tally_settle
    [ -n "${1:-}" ] && echo "$1"
    if [ "$NB_TALLY_STARTED" = 1 ]; then
        nb_tally_read
        echo "NEXTBSD-TEST-SUMMARY ok=$NB_OK fail=$NB_FAIL skip=$NB_SKIP"
    fi
    rm -f "$NB_TALLY_FIFO" "$NB_TALLY_COUNT"
    return 0
}

# The suite's normal exit: rc = capped fail count, or 2 when the tally never
# started (no sentinel — the harness reads that as a hang, never a pass). The
# cap applies AFTER nb_tally_finish, which re-reads the (uncapped) aggregate
# for the sentinel line; the sentinel prints the truth, the exit code is
# shell-safe.
nb_tally_exit() {
    if [ "$NB_TALLY_STARTED" != 1 ]; then
        nbsd_finish
        exit 2
    fi
    nbsd_finish
    [ "$NB_FAIL" -gt 255 ] && NB_FAIL=255
    exit "$NB_FAIL"
}
