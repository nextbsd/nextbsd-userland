/*
 * nwi_configuration.c -- build and publish the nwi state (#574).
 *
 * What IPMonitor's PrimaryServiceChanged path does with nwi_state, in
 * ip_plugin.c at configd-596.15: copy the old state, clear and refill the
 * per-family lists, diff, stamp generations, store it if the signature
 * moved and post the change. Here the lists come from dns_config.c, which
 * already walks every service in election order.
 *
 * Apple's candidates carry a NetworkSignature and VPN attributes. NextBSD
 * publishes neither, so `scutil --nwi` prints an empty signature, and a
 * candidate's reachability is just Reachable, which is also what
 * add_reachability_flags_to_candidate() starts from: every interface in
 * these lists has an address and so a direct route at least.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <mach/mach_time.h>
#include <notify.h>

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SCNetworkReachability.h>
#include <SystemConfiguration/SCPrivate.h>	/* _SC_NOTIFY_NETWORK_CHANGE_NWI */

#include "bound_state.h"	/* IFRANK_* */
#include "nwi_configuration.h"

#include "network_information_priv.h"
#include "network_information_server.h"

static nwi_state_t	S_nwi_state = NULL;

/* update_nwi(): store only when something other than the generation moved. */
static Boolean
update_nwi(nwi_state_t state)
{
	unsigned char		signature[CC_SHA1_DIGEST_LENGTH];
	static unsigned char	signature_last[CC_SHA1_DIGEST_LENGTH];

	_nwi_state_signature(state, signature, sizeof(signature));
	if (bcmp(signature, signature_last, sizeof(signature)) == 0)
		return (FALSE);
	bcopy(signature, signature_last, sizeof(signature));

	if (!_nwi_state_store(state))
		(void)fprintf(stderr,
		    "ipconfigd[nwi] Notifying nwi_state_store failed\n");
	return (TRUE);
}

/* Apple's post-network-change step, for the NWI change it found. */
static void
post_nwi_change(void)
{
	uint32_t status;

	status = notify_post(_SC_NOTIFY_NETWORK_CHANGE_NWI);
	if (status != NOTIFY_STATUS_OK)
		(void)fprintf(stderr, "ipconfigd[nwi] notify_post("
		    _SC_NOTIFY_NETWORK_CHANGE_NWI ") failed: %u\n", status);
}

/*
 * Darwin's Rank sorts ascending, best first (RankCompare), with the top
 * byte a rank assertion; iface_rank() is the reverse. Equal interfaces
 * stay in the order given, as the election breaks ties.
 */
static Rank
nwi_rank(int rank)
{
	return ((Rank)(IFRANK_WIRED - rank));
}

static void
insert(nwi_state_t state, int af, const struct nwi_service *s)
{
	uint64_t flags = 0;
	void *addr;

	if (s->rank == IFRANK_NEVER)
		flags |= NWI_IFSTATE_FLAGS_NOT_IN_LIST;
	if (s->have_dns)
		flags |= NWI_IFSTATE_FLAGS_HAS_DNS;
	addr = (af == AF_INET) ? (void *)(uintptr_t)&s->v4_addr :
	    (void *)(uintptr_t)&s->v6_addr;
	(void)nwi_insert_ifstate(state, s->ifname, af, flags, nwi_rank(s->rank),
	    addr, NULL, kSCNetworkReachabilityFlagsReachable);
}

void
nwi_configuration_init(void)
{
	/*
	 * Nothing acknowledges the state here yet, as with dnsinfo, so there
	 * is nothing for the handler to do.
	 */
	load_NetworkInformation(^(Boolean inSync __unused) { });
}

void
nwi_configuration_update(const struct nwi_service *svcs, unsigned n)
{
	nwi_state_t old_state, changes;
	unsigned i, v4 = 0, v6 = 0;

	for (i = 0; i < n; i++) {
		if (svcs[i].have_v4)
			v4++;
		if (svcs[i].have_v6)
			v6++;
	}

	if (v4 == 0 && v6 == 0) {
		/*
		 * Darwin keeps an empty state once it has had one, and scutil
		 * prints "No IPv4 states found". Here "nothing is bound" reads
		 * the same as before the first bind: no state at all, so
		 * `scutil --nwi` says "No network information" and exits 1.
		 */
		if (S_nwi_state != NULL) {
			nwi_state_release(S_nwi_state);
			S_nwi_state = NULL;
		}
		if (update_nwi(NULL))
			post_nwi_change();
		return;
	}

	old_state = nwi_state_copy_priv(S_nwi_state);
	S_nwi_state = nwi_state_new(S_nwi_state, (int)n);
	if (S_nwi_state == NULL) {
		if (old_state != NULL)
			nwi_state_release(old_state);
		return;
	}

	nwi_state_clear(S_nwi_state, AF_INET);
	nwi_state_clear(S_nwi_state, AF_INET6);
	for (i = 0; i < n; i++) {
		if (svcs[i].have_v4)
			insert(S_nwi_state, AF_INET, &svcs[i]);
		if (svcs[i].have_v6)
			insert(S_nwi_state, AF_INET6, &svcs[i]);
	}
	nwi_state_set_last(S_nwi_state, AF_INET);
	nwi_state_set_last(S_nwi_state, AF_INET6);

	changes = nwi_state_diff(old_state, S_nwi_state);
	S_nwi_state->generation_count = mach_absolute_time();
	/* Apple's global flags come from transient (VPN, PPP) services. */
	_nwi_state_set_reachability_flags(S_nwi_state, 0, 0);
	_nwi_state_update_interface_generations(old_state, S_nwi_state, changes);

	if (changes != NULL)
		nwi_state_release(changes);
	if (old_state != NULL)
		nwi_state_release(old_state);

	if (update_nwi(S_nwi_state))
		post_nwi_change();
}
