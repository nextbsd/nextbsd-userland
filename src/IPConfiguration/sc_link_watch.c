/*
 * sc_link_watch.c — ipconfigd SCDynamicStore interface-state watcher.
 *
 * Watches State:/Network/Interface/<if>/Link (published by configd's
 * KernelEventMonitor from PF_ROUTE link-state changes) and, since the
 * WLAN work, State:/Network/Interface/<if>/AirPort (published by wland).
 * DHCP starts only when BOTH say go.
 *
 * ---------------------------------------------------------------------
 * The DHCP gate (WLAN plan §1.4)
 *
 * ipconfigd's contract used to be "Link{Active} means L2 is usable for
 * IP". For Ethernet that is true. For 802.11 it is a lie, and the lie is
 * expensive:
 *
 *   net80211 reaches RUN state as soon as the station has ASSOCIATED —
 *   which is BEFORE the WPA 4-way handshake. The interface reports
 *   LINK_STATE_UP at that instant, KernelEventMonitor publishes
 *   Link{Active:true}, and ipconfigd fires DHCPDISCOVER onto a port that
 *   is not yet keyed. Every packet is dropped. The {4,8,16}s retransmit
 *   ladder burns ~28 seconds and gives up — and because Link is ALREADY
 *   Active, no new link event ever arrives, so nothing retries. Silence,
 *   forever.
 *
 * The repair is a second predicate: require Link{Active} AND
 * AirPort{Authenticated}. wland publishes AirPort{Authenticated:false}
 * on association and {Authenticated:true} on CTRL-EVENT-CONNECTED.
 *
 * KEY-ABSENT-MEANS-READY IS THE LOAD-BEARING DEFAULT. No WLAN daemon has
 * claimed em0, so em0 has no AirPort key, so wlan_ready() returns 1 and
 * the wired path behaves bit-for-bit as it always has. That matters more
 * than it sounds: tests/boot-test.sh gates the whole CI on em0 getting a
 * lease, and this gate must be invisible to it.
 *
 * The re-trigger falls out for free. wland's second configset (false ->
 * true) is itself a change on the AirPort pattern, so configd wakes this
 * same session, evaluate_interface() re-runs, /Link is still Active, the
 * gate is now open, and DHCP starts. Event-driven; no polling, no
 * debounce, no new IPC, no configd change, no MIG regeneration.
 *
 * It also incidentally fixes the "one failed attempt then silence" hole
 * for WLAN: a re-association generates a fresh AirPort transition and
 * therefore a fresh trigger, even though /Link never moved.
 * ---------------------------------------------------------------------
 *
 * Threading: the SC change callout runs on a dedicated serial delivery
 * queue (under SCDynamicStoreSetDispatchQueue). To keep that queue
 * free, the callout never runs DHCP itself — it dispatches each
 * link-up to the global concurrent queue, where the callback may block
 * in the DHCP + lease loop. The same scheduling path serves the
 * one-shot initial scan, so a link already Active before we subscribed
 * still fires. Concurrent/duplicate fires are the callback's problem
 * to dedupe (ipconfigd guards per-interface on its worker table).
 *
 * Address conflicts (#349). The same session also watches configd's
 * notify-only conflict keys, as Darwin's ipconfigd watches
 * IPv4ARPCollision (ipconfigd.c handle_change / arp_collision):
 *
 *   State:/Network/Interface/<if>/IPv4ARPCollision/<ip>/<hw>
 *   State:/Network/Interface/<if>/IPv6DuplicatedAddress/<ip>/<hw>
 *
 * configd posts them with SCDynamicStoreNotifyValue and never stores a
 * value, so there is nothing to read back and nothing for the initial
 * scan to find: the key IS the event, and is parsed here and handed to
 * the conflict callback on the global queue. Two identical posts that
 * land before we drain coalesce into one change, which is harmless — the
 * second would describe the same conflict. The keys must never reach
 * handle_iface_key(): its entity-agnostic parse would read them as a
 * Link/AirPort change for <if>.
 *
 * Single CF translation unit, like sc_publish.c — the rest of the
 * daemon stays plain C.
 */
#include "sc_link_watch.h"

#include <SystemConfiguration/SCDynamicStore.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>

#include <sys/socket.h>		/* AF_INET, AF_INET6 */

#include <net/if.h>		/* IFNAMSIZ */
#include <netinet/in.h>
#include <arpa/inet.h>		/* inet_pton */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sc_link_watch_cb	g_cb;
static sc_addr_conflict_cb	g_conflict_cb;
static uint32_t		g_lease_cap_secs;

/*
 * The watch patterns: every interface's Link entity, and every interface's
 * AirPort entity. POSIX ERE — configd anchors them ^...$. [^/]+ matches one
 * path component (the ifname).
 *
 * Watching AirPort as a *pattern* rather than a fixed key is what makes the
 * gate work for a VAP that does not exist yet: wland clones wlan0 at runtime,
 * and its first AirPort publish matches the pattern and wakes us with no
 * re-subscription.
 */
#define LINK_PATTERN	"State:/Network/Interface/[^/]+/Link"
#define AIRPORT_PATTERN	"State:/Network/Interface/[^/]+/AirPort"

/*
 * The notify-only conflict keys, as Darwin's ipconfigd builds its
 * IPv4ARPCollision pattern: the entity key followed by ".*", since the
 * address and hardware address follow it as further path components.
 */
#define ARP_COLLISION_ENTITY	"IPv4ARPCollision"
#define DUP_ADDRESS_ENTITY	"IPv6DuplicatedAddress"
#define ARP_COLLISION_PATTERN \
	"State:/Network/Interface/[^/]+/" ARP_COLLISION_ENTITY "/.*"
#define DUP_ADDRESS_PATTERN \
	"State:/Network/Interface/[^/]+/" DUP_ADDRESS_ENTITY "/.*"

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[link] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static CFStringRef
mkstr(const char *s)
{
	return (CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8));
}

/*
 * Read State:/Network/Interface/<if>/Link {Active} straight from configd via a
 * synchronous SCDynamicStoreCopyValue. Authoritative even when the change
 * *wakeup* was lost (#250 recovery): configd sends a watcher notification only
 * on the empty->non-empty edge, so for a late NIC's rapid Active:0 -> Active:1
 * the second wakeup can be raced away — but the *value* is always committed to
 * the store. ipconfigd polls this after admin-up so a dropped wakeup still
 * recovers. Uses a private throwaway session (no notification registration) so
 * it cannot perturb the linkwatch session's notify state. Returns 1 if Active.
 */
int
link_active_in_store(const char *ifname)
{
	SCDynamicStoreRef store;
	CFStringRef key;
	CFDictionaryRef dict;
	CFBooleanRef active;
	int is_active = 0;

	store = SCDynamicStoreCreate(NULL, CFSTR("ipconfigd-linkpoll"),
	    NULL, NULL);
	if (store == NULL)
		return (0);
	key = CFStringCreateWithFormat(NULL, NULL,
	    CFSTR("State:/Network/Interface/%s/Link"), ifname);
	if (key != NULL) {
		dict = SCDynamicStoreCopyValue(store, key);
		if (dict != NULL) {
			if (CFGetTypeID(dict) == CFDictionaryGetTypeID()) {
				active = CFDictionaryGetValue(dict,
				    CFSTR("Active"));
				is_active = (active != NULL &&
				    CFGetTypeID(active) == CFBooleanGetTypeID() &&
				    CFBooleanGetValue(active));
			}
			CFRelease(dict);
		}
		CFRelease(key);
	}
	CFRelease(store);
	return (is_active);
}

/* Trampoline payload — heap-allocated, freed by the trampoline. */
struct link_event {
	char		ifname[IFNAMSIZ];
	int		active;
	uint32_t	lease_cap_secs;
};

static void
run_link_trampoline(void *arg)
{
	struct link_event *ev = arg;

	if (g_cb != NULL)
		g_cb(ev->ifname, ev->active, ev->lease_cap_secs);
	free(ev);
}

/* Schedule the caller's link-event callback for `ifname` on the global queue. */
static void
schedule_link_event(const char *ifname, int active)
{
	struct link_event *ev;

	ev = calloc(1, sizeof(*ev));
	if (ev == NULL)
		return;
	(void)strlcpy(ev->ifname, ifname, sizeof(ev->ifname));
	ev->active = active;
	ev->lease_cap_secs = g_lease_cap_secs;
	dispatch_async_f(dispatch_get_global_queue(
	    DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ev, run_link_trampoline);
}

/* Trampoline payload for a conflict notification — freed by the trampoline. */
struct conflict_event {
	char		ifname[IFNAMSIZ];
	int		af;
	union {
		struct in_addr	v4;
		struct in6_addr	v6;
	}		addr;
	uint8_t		hw[16];
	int		hw_len;
};

static void
run_conflict_trampoline(void *arg)
{
	struct conflict_event *ev = arg;

	if (g_conflict_cb != NULL)
		g_conflict_cb(ev->ifname, ev->af, &ev->addr, ev->hw,
		    ev->hw_len);
	free(ev);
}

/*
 * Decode a colon-hex hardware address ("00:11:22:33:44:55") into `hw`.
 * Returns the byte count, or -1 if `s` is not one (bootp's
 * bytesFromColonHexString).
 */
static int
parse_hwaddr(const char *s, uint8_t *hw, size_t cap)
{
	size_t n = 0;
	unsigned long v;
	char *end;

	while (*s != '\0') {
		if (n == cap)
			return (-1);
		v = strtoul(s, &end, 16);
		if (end == s || end - s > 2 || v > 0xff)
			return (-1);
		hw[n++] = (uint8_t)v;
		s = end;
		if (*s == ':')
			s++;
		else if (*s != '\0')
			return (-1);
	}
	return ((int)n);
}

/*
 * If `keybuf` is one of the conflict keys, parse it, schedule the conflict
 * callback and return 1; otherwise return 0 and leave it to
 * handle_iface_key(). Turns
 *   State:/Network/Interface/<if>/<entity>/<ip>/<hw>
 * into its parts the way bootp's IPv4ARPCollisionKeyParse does. A
 * malformed conflict key is still claimed (1), so it is never mistaken
 * for a Link change.
 */
static int
handle_conflict_key(const char *keybuf)
{
	const char *prefix = "State:/Network/Interface/";
	char buf[256];
	char *ifname, *entity, *ip, *hw, *p;
	struct conflict_event *ev;
	int af, hw_len;

	if (strstr(keybuf, "/" ARP_COLLISION_ENTITY "/") == NULL &&
	    strstr(keybuf, "/" DUP_ADDRESS_ENTITY "/") == NULL)
		return (0);
	if (strncmp(keybuf, prefix, strlen(prefix)) != 0 ||
	    strlcpy(buf, keybuf + strlen(prefix), sizeof(buf)) >= sizeof(buf))
		return (1);

	ifname = buf;
	if ((entity = strchr(ifname, '/')) == NULL)
		return (1);
	*entity++ = '\0';
	if ((ip = strchr(entity, '/')) == NULL)
		return (1);
	*ip++ = '\0';
	hw = NULL;
	if ((p = strchr(ip, '/')) != NULL) {
		*p = '\0';
		hw = p + 1;
	}
	if (strcmp(entity, ARP_COLLISION_ENTITY) == 0)
		af = AF_INET;
	else if (strcmp(entity, DUP_ADDRESS_ENTITY) == 0)
		af = AF_INET6;
	else
		return (1);
	if (ifname[0] == '\0' || strlen(ifname) >= IFNAMSIZ ||
	    strncmp(ifname, "lo", 2) == 0 || g_conflict_cb == NULL)
		return (1);

	ev = calloc(1, sizeof(*ev));
	if (ev == NULL)
		return (1);
	(void)strlcpy(ev->ifname, ifname, sizeof(ev->ifname));
	ev->af = af;
	if (inet_pton(af, ip, &ev->addr) != 1) {
		xlog("IPCFG-CONFLICT-BADKEY: %s", keybuf);
		free(ev);
		return (1);
	}
	hw_len = 0;
	if (hw != NULL &&
	    (hw_len = parse_hwaddr(hw, ev->hw, sizeof(ev->hw))) < 0) {
		xlog("IPCFG-CONFLICT-BADKEY: %s", keybuf);
		free(ev);
		return (1);
	}
	ev->hw_len = hw_len;
	dispatch_async_f(dispatch_get_global_queue(
	    DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ev, run_conflict_trampoline);
	return (1);
}

/*
 * Read a boolean out of State:/Network/Interface/<ifname>/<entity>.
 *
 * Returns 1 / 0 for a present boolean, and `dflt` when the entity key or the
 * field is absent (or is not a boolean). The default is the whole point — see
 * wlan_ready().
 */
static int
iface_entity_bool(SCDynamicStoreRef store, const char *ifname,
    const char *entity, const char *field, int dflt)
{
	CFStringRef key, cf_field;
	CFDictionaryRef dict;
	CFBooleanRef val;
	int result = dflt;

	key = CFStringCreateWithFormat(NULL, NULL,
	    CFSTR("State:/Network/Interface/%s/%s"), ifname, entity);
	if (key == NULL)
		return (dflt);
	dict = SCDynamicStoreCopyValue(store, key);
	CFRelease(key);
	if (dict == NULL)
		return (dflt);			/* entity absent */
	if (CFGetTypeID(dict) != CFDictionaryGetTypeID()) {
		CFRelease(dict);
		return (dflt);
	}
	cf_field = CFStringCreateWithCString(NULL, field, kCFStringEncodingUTF8);
	if (cf_field != NULL) {
		val = CFDictionaryGetValue(dict, cf_field);
		if (val != NULL && CFGetTypeID(val) == CFBooleanGetTypeID())
			result = CFBooleanGetValue(val) ? 1 : 0;
		CFRelease(cf_field);
	}
	CFRelease(dict);
	return (result);
}

/*
 * Is `ifname` cleared for IP traffic as far as 802.11 is concerned?
 *
 * Reads State:/Network/Interface/<if>/AirPort {Authenticated}, which wland
 * publishes: false on association (net80211 RUN, but not yet keyed) and true
 * on CTRL-EVENT-CONNECTED (4-way handshake complete).
 *
 * KEY ABSENT => 1. This is deliberate and load-bearing. An interface no WLAN
 * daemon has claimed — em0, or any wired NIC — has no AirPort key at all, so it
 * is unconditionally ready and the wired path is completely unchanged. Getting
 * this backwards would deadlock DHCP on every Ethernet machine in existence,
 * and tests/boot-test.sh would be the first casualty.
 */
static int
wlan_ready(SCDynamicStoreRef store, const char *ifname)
{
	return (iface_entity_bool(store, ifname, "AirPort", "Authenticated", 1));
}

/*
 * Decide whether `ifname` should be DHCP'd, admin-upped, or left alone, and
 * schedule the caller's callback accordingly.
 *
 * This is called for a change to EITHER of the interface's watched entities
 * (/Link or /AirPort), and it re-reads both from the store rather than trusting
 * whichever key happened to wake us — so the two predicates can arrive in
 * either order, any number of times, and the answer is always the current
 * truth.
 *
 * The gate: DHCP requires Link{Active} AND wlan_ready(). The admin-up path
 * (link down) is untouched by the gate — a WLAN VAP still needs IFF_UP before
 * it can associate at all, and gating that on association would be circular.
 */
static void
evaluate_interface(SCDynamicStoreRef store, const char *ifname)
{
	int link_up, ready;

	/*
	 * Link absent entirely. Either the interface departed — configd removes
	 * the Link key when it goes, and on Darwin removing the key ends the
	 * service — or an AirPort key showed up for a VAP whose Link key configd
	 * has not published yet. The callback cannot tell those apart and does
	 * not need to: tearing down an interface with no worker and no binding
	 * is a no-op, and in the VAP case the Link key's own arrival calls us
	 * straight back.
	 */
	if (iface_entity_bool(store, ifname, "Link", "Active", -1) == -1) {
		schedule_link_event(ifname, SC_LINK_GONE);
		return;
	}

	link_up = iface_entity_bool(store, ifname, "Link", "Active", 0);
	if (!link_up) {
		xlog("IPCFG-LINK-SEEN: %s present, link down — scheduling "
		    "admin-up", ifname);
		schedule_link_event(ifname, 0);
		return;
	}

	ready = wlan_ready(store, ifname);
	if (!ready) {
		/*
		 * Associated but not yet keyed. Do NOT fire DHCP — this is the
		 * exact instant net80211 hits RUN and the old code blasted a
		 * DHCPDISCOVER into an unauthenticated port. wland's
		 * Authenticated:true will wake us again.
		 */
		xlog("IPCFG-LINK-GATED: %s link Active but AirPort not "
		    "Authenticated — holding DHCP until the 4-way handshake "
		    "completes", ifname);
		return;
	}

	xlog("IPCFG-LINK-UP: %s link Active — scheduling DHCP", ifname);
	schedule_link_event(ifname, 1);
}

/*
 * Extract the interface name from a State:/Network/Interface/<if>/<entity> key
 * and hand off to evaluate_interface(). The parse is entity-agnostic — it reads
 * the path component after the prefix — so one function serves the /Link
 * pattern, the /AirPort pattern, the change callout, and the initial scan.
 * lo0 is filtered here so the callback never sees loopback.
 */
static void
handle_iface_key(SCDynamicStoreRef store, CFStringRef key)
{
	char keybuf[256];
	char ifname[IFNAMSIZ];
	const char *prefix = "State:/Network/Interface/";
	const char *p, *slash;
	size_t n;

	if (!CFStringGetCString(key, keybuf, sizeof(keybuf),
	    kCFStringEncodingUTF8))
		return;
	if (strncmp(keybuf, prefix, strlen(prefix)) != 0)
		return;
	if (handle_conflict_key(keybuf))
		return;
	p = keybuf + strlen(prefix);
	slash = strchr(p, '/');
	if (slash == NULL)
		return;
	n = (size_t)(slash - p);
	if (n == 0 || n >= sizeof(ifname))
		return;
	(void)memcpy(ifname, p, n);
	ifname[n] = '\0';

	if (strncmp(ifname, "lo", 2) == 0)	/* loopback never DHCPs */
		return;

	evaluate_interface(store, ifname);
}

/* SCDynamicStore change callout — one or more watched keys changed. */
static void
link_changed(SCDynamicStoreRef store, CFArrayRef changedKeys, void *info)
{
	CFIndex i, count;

	(void)info;
	count = CFArrayGetCount(changedKeys);
	for (i = 0; i < count; i++)
		handle_iface_key(store, CFArrayGetValueAtIndex(changedKeys, i));
}

int
sc_link_watch_start(sc_link_watch_cb cb, sc_addr_conflict_cb conflict_cb,
    uint32_t lease_cap_secs)
{
	SCDynamicStoreContext ctx;
	SCDynamicStoreRef store;
	dispatch_queue_t queue;
	CFStringRef name, link_pat, air_pat, arp_pat, dup_pat;
	CFArrayRef patterns, existing;
	const void *pvals[4];

	g_cb = cb;
	g_conflict_cb = conflict_cb;
	g_lease_cap_secs = lease_cap_secs;

	(void)memset(&ctx, 0, sizeof(ctx));
	name = mkstr("ipconfigd-linkwatch");
	/*
	 * Retry the session open: ipconfigd and configd are both RunAtLoad
	 * with no ordering guarantee, so the first create can race configd's
	 * bootstrap check-in. Without the retry a lost race would silently
	 * leave the daemon with no DHCP trigger at all.
	 */
	{
		int tries;

		store = NULL;
		for (tries = 0; tries < 60; tries++) {
			store = SCDynamicStoreCreate(NULL, name, link_changed,
			    &ctx);
			if (store != NULL)
				break;
			(void)sleep(1);
		}
	}
	if (name != NULL)
		CFRelease(name);
	if (store == NULL) {
		xlog("IPCFG-LINK-FAIL: SCDynamicStoreCreate: %s",
		    SCErrorString(SCError()));
		return (-1);
	}

	/*
	 * Subscribe to BOTH entities. A change to either one re-runs
	 * evaluate_interface(), which re-reads both — so Link and AirPort can
	 * arrive in any order, any number of times, and the gate always sees
	 * current truth. The two conflict patterns ride the same session.
	 */
	link_pat = mkstr(LINK_PATTERN);
	air_pat = mkstr(AIRPORT_PATTERN);
	arp_pat = mkstr(ARP_COLLISION_PATTERN);
	dup_pat = mkstr(DUP_ADDRESS_PATTERN);
	pvals[0] = link_pat;
	pvals[1] = air_pat;
	pvals[2] = arp_pat;
	pvals[3] = dup_pat;
	patterns = CFArrayCreate(NULL, pvals, 4, &kCFTypeArrayCallBacks);
	CFRelease(arp_pat);
	CFRelease(dup_pat);
	if (!SCDynamicStoreSetNotificationKeys(store, NULL, patterns)) {
		xlog("IPCFG-LINK-FAIL: SetNotificationKeys: %s",
		    SCErrorString(SCError()));
		CFRelease(patterns);
		CFRelease(link_pat);
		CFRelease(air_pat);
		CFRelease(store);
		return (-1);
	}

	queue = dispatch_queue_create("com.apple.ipconfigd.linkwatch", NULL);
	if (!SCDynamicStoreSetDispatchQueue(store, queue)) {
		xlog("IPCFG-LINK-FAIL: SetDispatchQueue: %s",
		    SCErrorString(SCError()));
		dispatch_release(queue);
		CFRelease(patterns);
		CFRelease(link_pat);
		CFRelease(air_pat);
		CFRelease(store);
		return (-1);
	}
	xlog("IPCFG-LINK-WATCH-OK: watching %s and %s (DHCP requires "
	    "Link{Active} AND AirPort{Authenticated}; AirPort absent means "
	    "ready, so wired NICs are unaffected), plus %s and %s",
	    LINK_PATTERN, AIRPORT_PATTERN, ARP_COLLISION_PATTERN,
	    DUP_ADDRESS_PATTERN);

	/*
	 * Initial scan: a link that was already Active before the watch
	 * registered won't generate a change event, so sweep matching keys
	 * once and evaluate any interface we find. KernelEventMonitor's own
	 * startup snapshot makes this the common path in CI (SLIRP link is up
	 * immediately).
	 *
	 * Only the Link pattern is swept. An AirPort key cannot exist without a
	 * Link key for the same interface (the VAP has to attach before wland
	 * can publish anything about it), so sweeping Link alone reaches every
	 * interface — and evaluate_interface() reads the AirPort side anyway.
	 */
	existing = SCDynamicStoreCopyKeyList(store, link_pat);
	if (existing != NULL) {
		CFIndex i, count = CFArrayGetCount(existing);

		for (i = 0; i < count; i++)
			handle_iface_key(store,
			    CFArrayGetValueAtIndex(existing, i));
		CFRelease(existing);
	}

	CFRelease(patterns);
	CFRelease(link_pat);
	CFRelease(air_pat);
	/*
	 * Intentionally leak `store` + `queue` for the daemon's lifetime —
	 * the watch must stay live until exit; there is no stop path.
	 */
	return (0);
}
