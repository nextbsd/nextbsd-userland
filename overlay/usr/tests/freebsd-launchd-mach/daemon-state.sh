#!/bin/sh
# daemon-state.sh — hold every LaunchDaemon to the contract its own plist declares.
#
#   DAEMON-STATE-OK    every daemon is in the state its plist promises
#   DAEMON-STATE-FAIL  at least one is not; each failure names the label and why
#
# WHY THIS EXISTS
#
# The LAUNCHCTL-LIST check earlier in run.sh asserts a hardcoded list of labels
# is *loaded*. Loaded is not running. `launchctl list` reports PID "-" for a job
# launchd knows about but has never started, and #81 showed the weaker version
# of this -- passing on `grep "^PID"`, which matches the HEADER LINE -- let a
# boot with an entirely empty job table report success for months.
#
# Nor is exit status usable as a proxy. job_export() (core.c:1095) ALWAYS
# inserts LastExitStatus, so the Status column reads 0 both for a job that
# exited cleanly and for one that has never run in its life. That is exactly
# how the box on 2026-08-30 showed "13 jobs all at status 0 with syslog
# completely dead" (#78).
#
# So liveness has to come from launchd's PID column, which job_export() emits
# only `if (j->p)` -- i.e. only when a process actually exists -- cross-checked
# against ps(1) so that a stale j->p cannot pass either.
#
# WHY IT IS DRIVEN FROM THE PLISTS
#
# Reading the directory rather than a hardcoded list means a daemon added later
# is covered the day its plist lands, instead of the day someone remembers to
# edit this file. Each job is then judged by what its OWN plist declares, since
# "should be running" is not one rule:
#
#   KeepAlive=true / RunAtLoad=true  resident -- must have a live process
#   MachServices, in RESIDENT_DEMAND demand-launched but resident once started
#                                    -- must be LOADED and RUNNING
#   MachServices, otherwise          demand-launched -- must be loaded; running
#                                    or not is not something we can judge
#   StartInterval                    periodic -- must be loaded; may or may not
#                                    be mid-run when we look
#   Disabled=true                    skipped, deliberately off
#
# Run it AFTER the Mach stress rounds. Before them it only proves boot worked;
# after them it proves the stress did not quietly kill anything.

set -u

DAEMONS_DIR=${1:-/System/Library/LaunchDaemons}
LIST=/tmp/daemon_state_list.out
BUDGET=30

# ---------------------------------------------------------------- table --
# One bounded `launchctl list`, reused for every job. Background + kill budget,
# not timeout(1): a wedged PID 1 leaves this in an uninterruptible Mach receive
# that SIGTERM cannot reap, and `$(...)` would then block this script forever.
/bin/launchctl list > "$LIST" 2>&1 &
list_pid=$!
i=0
while [ "$i" -lt "$BUDGET" ] && kill -0 "$list_pid" 2>/dev/null; do
    sleep 1
    i=$((i + 1))
done
if kill -0 "$list_pid" 2>/dev/null; then
    kill -9 "$list_pid" 2>/dev/null || true
    echo "DAEMON-STATE-FAIL: launchctl list did not return in ${BUDGET}s (wedged PID 1?)"
    exit 1
fi
wait "$list_pid" 2>/dev/null || true

if ! [ -s "$LIST" ]; then
    echo "DAEMON-STATE-FAIL: launchctl list produced no output"
    exit 1
fi

echo "--- launchd job table ---"
cat "$LIST"
echo "--- end job table ---"

# ------------------------------------------------------------ plist read --
# Emit `key=value` for the TOP-LEVEL keys of a plist, with XML comments
# stripped first. Comment stripping is not optional: com.apple.syslogd.plist's
# header discusses "KeepAlive=true" in prose, and com.apple.aslmanager.plist's
# discusses StartInterval -- a naive grep reads both as declarations and
# classifies the job wrongly. Depth tracking matters for the same reason:
# MachServices and EnvironmentVariables contain nested <key> elements
# (com.apple.aslmanager, ASL_DISABLE) that are values, not job keys.
plist_keys() {
    awk '
    function flush(k, v) { if (k != "") printf "%s=%s\n", k, v }
    BEGIN { incomment = 0; depth = 0; adepth = 0; pending = "" }
    {
        line = $0; out = ""
        while (length(line) > 0) {
            if (incomment) {
                p = index(line, "-->")
                if (p == 0) { line = ""; break }
                line = substr(line, p + 3); incomment = 0
            } else {
                p = index(line, "<!--")
                if (p == 0) { out = out line; line = ""; break }
                out = out substr(line, 1, p - 1)
                line = substr(line, p + 4)
                incomment = 1
            }
        }

        # Value first: <dict>/<array> are both a value for the pending key AND
        # a depth change, and reading them in the other order loses the key.
        if (pending != "") {
            if (out ~ /<true\/>/)              { flush(pending, "true");    pending = "" }
            else if (out ~ /<false\/>/)        { flush(pending, "false");   pending = "" }
            else if (match(out, /<string>[^<]*<\/string>/)) {
                v = substr(out, RSTART + 8, RLENGTH - 17); flush(pending, v); pending = ""
            }
            else if (match(out, /<integer>[^<]*<\/integer>/)) {
                v = substr(out, RSTART + 9, RLENGTH - 19); flush(pending, v); pending = ""
            }
            else if (out ~ /<dict>/)           { flush(pending, "dict");    pending = "" }
            else if (out ~ /<array>/)          { flush(pending, "array");   pending = "" }
        }

        if (out ~ /<dict>/)    depth++
        if (out ~ /<\/dict>/)  depth--
        if (out ~ /<array>/)   adepth++
        if (out ~ /<\/array>/) adepth--

        if (depth == 1 && adepth == 0 && match(out, /<key>[^<]*<\/key>/)) {
            pending = substr(out, RSTART + 5, RLENGTH - 11)
        }
    }' "$1"
}

# ------------------------------------------------------ residency table --
# Whether a demand-launched job STAYS UP after servicing a request is a
# property of the program, not of its plist -- there is no key that says it --
# so the expectation has to be written down. This is that list, and it should
# stay short, with a reason per entry.
#
#   com.apple.aslmanager  launchd demand-launches it once (syslogd's trigger
#                         from db_asl_open at first store open) and it then
#                         stays resident servicing later triggers. Apple's
#                         managed path ran dispatch_main() and never returned;
#                         ours blocks in mach_msg (aslmanager.c:run_managed).
#                         macOS shows the same thing: aslmanager is up from
#                         boot from a plist with no RunAtLoad/KeepAlive.
RESIDENT_DEMAND="com.apple.aslmanager"

# ------------------------------------------------------------- the check --
fails=""
checked=0

for plist in "$DAEMONS_DIR"/*.plist; do
    [ -f "$plist" ] || continue

    keys=$(plist_keys "$plist")
    label=$(printf '%s\n' "$keys" | awk -F= '$1 == "Label" { print $2; exit }')
    # Fall back to the filename. Every plist in the tree names itself, but a
    # malformed one should be reported against something, not skipped silently.
    if [ -z "$label" ]; then
        label=$(basename "$plist" .plist)
        echo "    NOTE $label: no top-level Label key; using the filename"
    fi

    getkey() { printf '%s\n' "$keys" | awk -F= -v k="$1" '$1 == k { print $2; exit }'; }

    disabled=$(getkey Disabled)
    keepalive=$(getkey KeepAlive)
    runatload=$(getkey RunAtLoad)
    machsvc=$(getkey MachServices)
    interval=$(getkey StartInterval)

    checked=$((checked + 1))

    # Plain glob membership test — expr(1) would treat the label as a BRE, and
    # every label here contains dots.
    resident_demand=0
    case " $RESIDENT_DEMAND " in
    *" $label "*) resident_demand=1 ;;
    esac

    if [ "$disabled" = "true" ]; then
        echo "    SKIP $label: Disabled=true in its plist"
        continue
    fi

    # Loaded? Column 3 of the job table.
    if ! awk -v want="$label" '$3 == want { f = 1 } END { exit !f }' "$LIST"; then
        fails="$fails\n    $label: declared in $(basename "$plist") but NOT loaded by launchd"
        continue
    fi
    pid=$(awk -v want="$label" '$3 == want { print $1; exit }' "$LIST")

    # The framebuffer getty is a deliberate no-op on a guest with no GPU: its
    # ProgramArguments are `test -c /dev/ttyv0 || exit 0`, so on a headless
    # qemu -machine virt (no GOP -> no efifb -> vt(4) never attaches) there is
    # no /dev/ttyv0 and the job correctly exits 0 without running getty.
    if [ "$label" = "org.nextbsd.getty.ttyv0" ] && [ ! -c /dev/ttyv0 ]; then
        echo "    SKIP $label: no /dev/ttyv0 on this guest; the plist's test -c guard makes it a no-op by design"
        continue
    fi

    if [ "$keepalive" = "true" ] || [ "$runatload" = "true" ]; then
        # Resident. Must have a live process, and ps must agree with launchd.
        if [ "$pid" = "-" ] || [ -z "$pid" ]; then
            fails="$fails\n    $label: resident (KeepAlive/RunAtLoad) but launchd holds no PID — it is not running"
        elif ! ps -p "$pid" >/dev/null 2>&1; then
            fails="$fails\n    $label: launchd reports pid $pid but ps cannot see it — stale j->p"
        else
            echo "    OK   $label: resident, running as pid $pid"
        fi
    elif [ "$keepalive" = "dict" ]; then
        # Conditional KeepAlive: whether it should be up right now depends on
        # runtime state we are not modelling. Report, do not judge.
        echo "    INFO $label: conditional KeepAlive (dict); loaded, pid=$pid — liveness not asserted"
    elif [ -n "$machsvc" ] && [ "$resident_demand" -eq 1 ]; then
        # Demand-launched, but expected resident once started. Something has to
        # have triggered it -- for aslmanager that is syslogd at first store
        # open -- so "never started" and "started and died" are both failures,
        # and both are invisible to `launchctl list` on its own.
        if [ "$pid" = "-" ] || [ -z "$pid" ]; then
            fails="$fails\n    $label: demand-launched and expected resident, but launchd holds no PID — it either was never triggered or died"
        elif ! ps -p "$pid" >/dev/null 2>&1; then
            fails="$fails\n    $label: launchd reports pid $pid but ps cannot see it — stale j->p"
        else
            echo "    OK   $label: demand-launched and resident, running as pid $pid"
        fi
    elif [ -n "$machsvc" ] || [ -n "$interval" ]; then
        # Demand-launched or periodic with no residency expectation recorded.
        # Loaded is all we can fairly assert.
        if [ "$pid" = "-" ] || [ -z "$pid" ]; then
            echo "    OK   $label: demand/periodic, loaded and idle"
        else
            echo "    OK   $label: demand/periodic, loaded and currently running as pid $pid"
        fi
    else
        echo "    INFO $label: loaded, but its plist declares no start policy (no KeepAlive/RunAtLoad/MachServices/StartInterval)"
    fi
done

if [ "$checked" -eq 0 ]; then
    echo "DAEMON-STATE-FAIL: no plists found in $DAEMONS_DIR"
    exit 1
fi

if [ -n "$fails" ]; then
    echo "=== DAEMON-STATE diagnostics ==="
    echo "--- ps auxww ---"
    ps auxww || true
    echo "--- $DAEMONS_DIR ---"
    ls -la "$DAEMONS_DIR" || true
    echo "=== end diagnostics ==="
    # printf, not echo: \n is not portable through echo(1) across shells.
    printf 'DAEMON-STATE-FAIL: %d daemon(s) not in the state their plist declares:%b\n' \
        "$(printf '%b' "$fails" | grep -c '^    ')" "$fails"
    exit 1
fi

echo "DAEMON-STATE-OK: $checked plist(s) checked, every daemon matches its declared contract"
exit 0
