#!/bin/sh
#
# wedge-check.sh -- prove each Mach service actually ANSWERS, and dump why not.
#
# Why this exists
# ---------------
# A wedged daemon is invisible to everything we had. It has not crashed: it
# sits healthy and idle in ipc_mqueue_receive waiting for a request that never
# lands in its queue, so `launchctl list` reports status 0 for it indefinitely
# while every client blocks forever. On 2026-08-30 the box showed a completely
# clean `launchctl list` -- 13 jobs, all status 0 -- with syslog 100% dead.
#
# So liveness (the process exists) and health (status 0) are both worthless as
# evidence here. The only thing that proves a Mach service works is a BOUNDED
# round trip against it.
#
# Two rules this script exists to enforce
# ---------------------------------------
# 1. Every probe is bounded. An unbounded probe against a wedged service does
#    not fail, it hangs -- which is how this class of bug hid for so long.
# 2. A service with no probe reports UNPROBED, never PASS. Absence of evidence
#    must not read as success; that is the same mistake in a different place.
#
# On any WEDGED result the diagnostic bundle below is dumped automatically.
# It is exactly the set of facts that took a full day to assemble by hand:
# who is blocked, where in the kernel, and what the daemon last did.
#
# usage: wedge-check.sh [timeout_seconds]
# exit:  0 all probed services answered; 1 one or more wedged
#
TMO=${1:-15}
FAILED=0
UNPROBED=""

log()  { echo "$@"; }
have() { command -v "$1" >/dev/null 2>&1; }

# --------------------------------------------------------------- diagnostics
# Dumped on failure only. Keep this in one place so every probe gets the same
# evidence and nobody has to remember what to collect at 3am.
dump_wedge() {
	svc=$1
	log ""
	log "=== WEDGE-DUMP: $svc ======================================"

	pid=$(launchctl list 2>/dev/null | awk -v s="$svc" '$3 == s { print $1 }')
	log "--- launchctl view ---"
	launchctl list 2>/dev/null | awk -v s="$svc" 'NR==1 || $3 == s'

	if [ -n "$pid" ] && [ "$pid" != "-" ]; then
		log "--- $svc (pid $pid) kernel stacks ---"
		log "    (ipc_mqueue_receive here means it is WAITING, not hung in work)"
		procstat -kk "$pid" 2>/dev/null | head -20
	else
		log "--- no live pid for $svc (on-demand, or failed to start) ---"
	fi

	log "--- every process currently blocked in Mach ---"
	log "    (a client in ipc_mqueue_receive has SENT and is awaiting a reply)"
	ps -axo pid,ppid,state,wchan,command 2>/dev/null \
	    | awk 'NR==1 || $4 ~ /thread_b|ipc_/' | cut -c1-120 | head -15

	for f in /var/log/"${svc##*.}".stderr /var/log/"${svc##*.}"d.stderr; do
		[ -f "$f" ] || continue
		log "--- tail $f ---"
		tail -15 "$f" 2>/dev/null
	done

	log "--- all jobs (for context: status 0 does NOT mean serving) ---"
	launchctl list 2>/dev/null | head -20
	log "=== END WEDGE-DUMP: $svc =================================="
	log ""
}

# ------------------------------------------------------------------- probes
# Each probe must (a) be bounded and (b) exercise the service's MACH path,
# not merely observe that the process exists.

probe() {
	svc=$1; desc=$2; shift 2
	printf "  %-28s %-34s " "$svc" "$desc"
	if timeout "$TMO" "$@" >/dev/null 2>&1; then
		echo "OK"
		return 0
	fi
	echo "WEDGED"
	FAILED=$((FAILED + 1))
	dump_wedge "$svc"
	return 1
}

probe_syslogd() {
	tag="WEDGECHK-$$-$(date +%s 2>/dev/null)"
	timeout "$TMO" syslog -s -l Notice "$tag" >/dev/null 2>&1 || return 1
	# a send that returns is necessary but not sufficient: read it back, which
	# proves the store path answered too.
	i=0
	while [ $i -lt 5 ]; do
		timeout "$TMO" syslog -k Message S "$tag" 2>/dev/null | grep -q "$tag" && return 0
		i=$((i + 1))
		sleep 1
	done
	return 1
}

log "=== Mach service wedge check (timeout ${TMO}s per probe) ==="
log ""
printf "  %-28s %-34s %s\n" SERVICE PROBE RESULT

# launchd itself: launchctl list is an RPC into PID 1.
probe "com.apple.launchd" "launchctl list (RPC to PID 1)" launchctl list

# syslogd: the ASL Mach service. Send AND read back.
printf "  %-28s %-34s " "com.apple.syslogd" "syslog round-trip (send+read)"
if probe_syslogd; then
	echo "OK"
else
	echo "WEDGED"
	FAILED=$((FAILED + 1))
	dump_wedge "com.apple.syslogd"
fi

# mDNSResponder: a bounded browse. -t is honoured by our dns-sd; timeout backs
# it up in case it is not.
if have dns-sd; then
	probe "com.apple.mDNSResponder" "dns-sd bounded query" dns-sd -t 5 -B _services._dns-sd._udp
else
	UNPROBED="$UNPROBED com.apple.mDNSResponder(no-dns-sd)"
fi

# IPConfiguration: ipconfig talks to ipconfigd over its Mach service.
if have ipconfig; then
	probe "com.apple.IPConfiguration" "ipconfig ifcount" ipconfig ifcount
else
	UNPROBED="$UNPROBED com.apple.IPConfiguration(no-ipconfig)"
fi

# --- deliberately UNPROBED, with the reason recorded ------------------------
# These have MachServices and can therefore wedge, but nothing on the image
# can currently exercise them. They are listed so the gap is visible rather
# than silently counted as passing.
#
#   com.apple.notifyd          needs notifyutil / notifypoke (not installed).
#                              This one matters most: syslogd's startup blocks
#                              on notify RPCs, so notifyd is the prime suspect
#                              for the whole wedge class and we cannot probe it
#                              directly. Installing notifyutil would close the
#                              single biggest hole here.
#   com.apple.configd          needs scutil, not yet ported (see #64).
#   com.apple.DiskArbitration  (service com.apple.DiskArbitration.diskarbitrationd)
#                              exercised by run.sh's DA-BOOT check, whose
#                              datest is a DiskArbitration client (#197);
#                              not probed here.
#   com.apple.hostnamed        no Mach client tool; hostname(1) reads the
#                              kernel value, which does not exercise its port.
#   org.nextbsd.wland          no client tool.
#   com.apple.aslmanager       on-demand; exercised indirectly by syslogd.
UNPROBED="$UNPROBED com.apple.notifyd(no-notifyutil) com.apple.configd(no-scutil)"
UNPROBED="$UNPROBED com.apple.DiskArbitration com.apple.hostnamed org.nextbsd.wland"

log ""
if [ -n "$UNPROBED" ]; then
	log "  UNPROBED (can wedge, cannot currently be tested -- NOT a pass):"
	for u in $UNPROBED; do log "    - $u"; done
fi

log ""
if [ "$FAILED" -gt 0 ]; then
	log "WEDGE-CHECK-FAIL: $FAILED service(s) did not answer within ${TMO}s"
	exit 1
fi
log "WEDGE-CHECK-OK: every probed service answered within ${TMO}s"
exit 0
