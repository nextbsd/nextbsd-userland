/*
 * lease_loop.c — post-BOUND lease management. RFC 2131 §4.4.5
 * BOUND/RENEWING/REBINDING state-machine on the daemon's main
 * thread.
 *
 * Sleep is 1s-tick polling on `got_term` so SIGTERM shortens any
 * wait; the longest contiguous block call is sleep(1).
 *
 * iter 5b: lease_cap_secs (from IPCONFIGD_FAST_LEASE in env) caps
 * the effective lease time used to derive T1 / T2. Useful in CI
 * where SLIRP hands out 86400s leases that would otherwise mean
 * T1 = 12 hours; capping to e.g. 10s makes the renewal code run
 * within the boot-test budget. The cap is NOT applied to
 * lease->lease_time (the value sc_publish.c forwards to configd) —
 * downstream consumers see the server's authoritative value. iter
 * 5b also emits IPCFG-RENEW-OK once on the first successful
 * RENEWING/REBINDING ACK so the gate fires deterministically.
 */
#include "lease_loop.h"
#include "bound_state.h"
#include "dhcp_discover.h"
#include "dns_config.h"
#include "sc_publish.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[lease] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/*
 * Sleep for `seconds` in 1-second ticks, returning early if `got_term`
 * (daemon shutdown) or `*stop` (this interface's link went down / it was
 * removed) is asserted. Returns 0 on a full wait, -1 if interrupted.
 *
 * `renew` (may be NULL) asks for a renewal now. When it is set the flag
 * is taken back, `*left` gets the seconds not yet slept and the return is
 * 1. A flag still set at the end of a full wait is dropped: the renewal
 * that wait leads to answers it. `left` must be valid when `renew` is.
 *
 * The per-interface `stop` is what lets a link-down unwind a worker that
 * is parked in a T1 sleep — which on a real lease is hours. Before it
 * existed, an interface that lost its link kept its lease, address and
 * default route until the daemon exited (#39).
 */
static int
sleep_interruptible(uint32_t seconds, volatile sig_atomic_t *stop,
    volatile sig_atomic_t *renew, uint32_t *left)
{
	uint32_t i;

	for (i = 0; i < seconds; i++) {
		if (got_term || (stop != NULL && *stop))
			return (-1);
		if (renew != NULL && *renew) {
			*renew = 0;
			*left = seconds - i;
			return (1);
		}
		(void)sleep(1);
	}
	if (got_term || (stop != NULL && *stop))
		return (-1);
	if (renew != NULL)
		*renew = 0;
	return (0);
}

/*
 * Apply the iter-5b lease cap. Returns min(in, cap) when cap > 0;
 * `in` unchanged otherwise. The cap only shortens timer math —
 * lease->lease_time (the value published to configd) is untouched.
 */
static uint32_t
cap_lease(uint32_t in, uint32_t cap)
{
	if (cap > 0 && in > cap)
		return (cap);
	return (in);
}

/*
 * Compare the fields sc_publish_ipv4() actually writes into the
 * SCDynamicStore: address, netmask, router, server identifier and the
 * DNS server, domain and search lists. lease_time is deliberately excluded — it advances on
 * every renewal by design and is carried by the /DHCP key, not the IPv4
 * key.
 *
 * A renewal that changes none of these must not re-publish: consumers
 * cannot distinguish a rewrite from a real reconfiguration, and
 * mDNSResponder responds to a State:/Network/.../IPv4 write by flushing
 * and re-announcing its whole cache. That is visible to the user as
 * Bonjour service lists clearing and repopulating on every renewal.
 */
static bool
ipv4_publish_equal(const struct dhcp_lease *a, const struct dhcp_lease *b)
{
	unsigned i;

	if (a->addr.s_addr != b->addr.s_addr ||
	    a->netmask.s_addr != b->netmask.s_addr ||
	    a->router.s_addr != b->router.s_addr ||
	    a->server.s_addr != b->server.s_addr ||
	    a->dns_count != b->dns_count ||
	    a->search_count != b->search_count ||
	    strcmp(a->domain_name, b->domain_name) != 0)
		return (false);
	for (i = 0; i < a->dns_count; i++) {
		if (a->dns[i].s_addr != b->dns[i].s_addr)
			return (false);
	}
	for (i = 0; i < a->search_count; i++) {
		if (strcmp(a->search[i], b->search[i]) != 0)
			return (false);
	}
	return (true);
}

/*
 * A renewal changed what the lease publishes: record it and re-merge the
 * service's DNS, since a renewed lease can change the DNS the system uses.
 */
static void
lease_changed(const char *ifname, const struct dhcp_lease *lease)
{
	bound_state_set(ifname, lease);
	dns_config_update(ifname);
}

int
lease_loop_run(const char *ifname, struct dhcp_lease *lease,
    struct sc_publish *pub, uint32_t lease_cap_secs,
    volatile sig_atomic_t *renew, volatile sig_atomic_t *stop)
{
	uint32_t lease_secs = cap_lease(lease->lease_time, lease_cap_secs);
	bool renew_marker_fired = false;
	/* Mirror of what is currently in the store. The caller published
	 * this lease before entering the loop (ipconfigd.c dhcp_run_on_
	 * interface), so the store and this copy start in agreement. */
	struct dhcp_lease published = *lease;
	bool unchanged_logged = false;

	if (lease_secs == 0) {
		xlog("zero lease time — treating as infinite (no renewal)");
		while (!got_term && !(stop != NULL && *stop))
			(void)sleep(1);
		return (0);
	}
	if (lease_cap_secs > 0)
		xlog("lease cap active: server lease=%us → effective=%us",
		    (unsigned)lease->lease_time, lease_secs);

	for (;;) {
		uint32_t t1, t2_after_t1, expiry_after_t2, left;
		struct dhcp_lease renewed;
		int woke;

		/*
		 * RFC 2131 §4.4.5:
		 *   T1 = lease/2
		 *   T2 = 0.875 * lease
		 * Use integer math: T2 - T1 ≈ lease*3/8. Expiry - T2 ≈ lease/8.
		 */
		t1 = lease_secs / 2;
		t2_after_t1 = (lease_secs * 3) / 8;
		expiry_after_t2 = lease_secs - (t1 + t2_after_t1);

		xlog("BOUND: lease=%us T1=+%us T2=+%us expiry=+%us",
		    lease_secs, t1, t1 + t2_after_t1, lease_secs);

		left = t1;
wait_t1:
		woke = sleep_interruptible(left, stop, renew, &left);
		if (woke < 0)
			return (0);

		/* RENEWING. */
		if (woke > 0)
			xlog("RENEWING: renewal requested, sending DHCPREQUEST");
		else
			xlog("RENEWING: T1 fired, sending DHCPREQUEST");
		if (dhcp_renew(ifname, lease, &renewed) == 0) {
			*lease = renewed;
			lease_secs = cap_lease(lease->lease_time,
			    lease_cap_secs);
			if (pub != NULL) {
				if (!ipv4_publish_equal(lease, &published)) {
					if (sc_publish_ipv4(pub, ifname,
					    lease) != 0)
						xlog("RENEWING: re-publish failed (continuing)");
					else
						published = *lease;
					lease_changed(ifname, lease);
				} else if (!unchanged_logged) {
					xlog("RENEWING: IPv4 config unchanged — "
					    "skipping re-publish");
					unchanged_logged = true;
				}
				/* Issue #88: refresh /DHCP on renewal so
				 * LeaseStartTime advances and Option_12
				 * tracks any server-side change. This key is
				 * left unguarded on purpose — no consumer
				 * treats /DHCP as a reconfiguration trigger. */
				if (sc_publish_dhcp(pub, ifname, lease) != 0)
					xlog("RENEWING: /DHCP re-publish failed (continuing)");
			}
			xlog("RENEWING -> BOUND (server lease=%us, "
			    "effective=%us)",
			    (unsigned)lease->lease_time, lease_secs);
			if (!renew_marker_fired) {
				xlog("IPCFG-RENEW-OK");
				renew_marker_fired = true;
			}
			continue;
		}
		if (woke > 0) {
			/*
			 * Nothing answered a renewal T1 did not call for. The
			 * lease is as good as it was, so go back to waiting:
			 * dropping through to T2 here would give up on it
			 * early.
			 */
			xlog("RENEWING: no ACK to the requested renewal; "
			    "still waiting for T1");
			goto wait_t1;
		}
		xlog("RENEWING: no ACK; waiting until T2");

		left = t2_after_t1;
wait_t2:
		woke = sleep_interruptible(left, stop, renew, &left);
		if (woke < 0)
			return (0);

		/* REBINDING. */
		if (woke > 0)
			xlog("REBINDING: renewal requested, sending DHCPREQUEST");
		else
			xlog("REBINDING: T2 fired, sending DHCPREQUEST");
		if (dhcp_renew(ifname, lease, &renewed) == 0) {
			*lease = renewed;
			lease_secs = cap_lease(lease->lease_time,
			    lease_cap_secs);
			if (pub != NULL) {
				if (!ipv4_publish_equal(lease, &published)) {
					if (sc_publish_ipv4(pub, ifname,
					    lease) != 0)
						xlog("REBINDING: re-publish failed (continuing)");
					else
						published = *lease;
					lease_changed(ifname, lease);
				} else if (!unchanged_logged) {
					xlog("REBINDING: IPv4 config unchanged — "
					    "skipping re-publish");
					unchanged_logged = true;
				}
				if (sc_publish_dhcp(pub, ifname, lease) != 0)
					xlog("REBINDING: /DHCP re-publish failed (continuing)");
			}
			xlog("REBINDING -> BOUND (server lease=%us, "
			    "effective=%us)",
			    (unsigned)lease->lease_time, lease_secs);
			if (!renew_marker_fired) {
				xlog("IPCFG-RENEW-OK");
				renew_marker_fired = true;
			}
			continue;
		}
		if (woke > 0) {
			xlog("REBINDING: no ACK to the requested renewal; "
			    "still waiting for T2");
			goto wait_t2;
		}
		xlog("REBINDING: no ACK; waiting until lease expiry");

		if (sleep_interruptible(expiry_after_t2, stop, NULL, NULL) < 0)
			return (0);

		xlog("lease expired without renewal — unpublishing");
		if (pub != NULL)
			(void)sc_publish_remove(pub, ifname);
		return (-1);
	}
}
