/*
 * v6_service.c — the IPv6 service on each interface (#423).
 *
 * See v6_service.h. One slot per interface, from its first IPv4 result until
 * the link goes down. A slot runs one thread for the method in force; the
 * thread configures the address, parks, and tears down what it installed.
 * What the thread does per method is Darwin's rtadv.c, manual_v6.c and
 * linklocal_v6.c; off is inet6_detach_interface(). DHCPv6 (#424) runs in a
 * client of its own, started from the first RA of a service start.
 *
 * Like the IPv4 side, a stop on SIGTERM leaves the address and route in
 * place, so the machine stays on the network across an ipconfigd restart.
 */
#include "v6_service.h"

#include <sys/types.h>

#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "apply_lease_v6.h"
#include "bound_state.h"	/* BOUND_MAX_IF */
#include "dhcpv6_client.h"
#include "dns_config.h"
#include "ipconfig_method.h"
#include "ipconfigd.h"
#include "ra_listen.h"
#include "ra_router.h"
#include "sc_link_watch.h"
#include "sc_publish.h"

/* ipconfigd.c's signal flag. */
extern volatile sig_atomic_t got_term;

/* How long ra_acquire() listens for a Router Advertisement. */
#define	V6_RA_BUDGET_MS		15000

/*
 * The most v6_service_wait_first_result() waits: the RA budget, plus DAD and
 * slack. It only has to outlast one pass of the service's start-up.
 */
#define	V6_FIRST_RESULT_MAX_MS	(V6_RA_BUDGET_MS + 10000)

/*
 * The address the service installed, so a duplicated-address notification
 * can find it. PENDING from just before it is installed until it is
 * published and tracked — a duplicate reported in that window is only
 * recorded (dup), and v6_commit() acts on it — then ACTIVE, when the
 * notification withdraws it directly.
 */
#define	V6_NONE		0
#define	V6_PENDING	1
#define	V6_ACTIVE	2

struct v6_service {
	char			ifname[IFNAMSIZ];	/* empty: slot free */
	bool			v4_result;	/* first IPv4 result seen */
	bool			running;	/* thread exists */
	bool			first_result;	/* start-up has finished */
	pthread_t		tid;
	volatile sig_atomic_t	stop;
	bool			restart;	/* stop, then re-evaluate */
	struct ipconfig_v6_method_info method;
	char			service_id[SC_PUBLISH_SERVICE_ID_LEN];

	int			state;
	bool			dup;
	struct in6_addr		dup_addr;
	bool			have_addr;	/* addr holds a SLAAC address */
	struct in6_addr		addr;
	bool			have_router;	/* ::/0 via router */
	struct in6_addr		router;
	unsigned		ifindex;
	struct ra_router	*rtr;

	bool			ready;		/* IPv6 is published */
	struct dhcpv6_client	*dhcp_client;	/* Automatic only */
	/* The service's IPv6 DNS: RDNSS, and the DHCPv6 reply's. */
	struct in6_addr		rdnss[RA_MAX_RDNSS];
	unsigned		rdnss_count;
	bool			have_dhcp_dns;
	struct ipconfig_dns	dhcp_dns;
};

static pthread_mutex_t		g_v6_services_lock = PTHREAD_MUTEX_INITIALIZER;
static struct v6_service	g_v6_services[BOUND_MAX_IF];

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[v6] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/* Caller holds g_v6_services_lock. */
static struct v6_service *
v6_find_locked(const char *ifname)
{
	int i;

	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (g_v6_services[i].ifname[0] != '\0' &&
		    strcmp(g_v6_services[i].ifname, ifname) == 0)
			return (&g_v6_services[i]);
	}
	return (NULL);
}

/* A slot for `ifname`, new if need be. Caller holds g_v6_services_lock. */
static struct v6_service *
v6_find_or_add_locked(const char *ifname)
{
	struct v6_service *s;
	int i;

	if ((s = v6_find_locked(ifname)) != NULL)
		return (s);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		s = &g_v6_services[i];
		if (s->ifname[0] == '\0' && !s->running) {
			(void)memset(s, 0, sizeof(*s));
			(void)strlcpy(s->ifname, ifname, sizeof(s->ifname));
			return (s);
		}
	}
	return (NULL);
}

static void
v6_slot_free_locked(struct v6_service *s)
{
	s->ifname[0] = '\0';
	s->v4_result = false;
}

/*
 * Take a duplicated address back: stop the router tracker with its ::/0 (or,
 * without one, the ::/0 Automatic installed), remove the address and withdraw
 * State:/Network/Service/<id>/IPv6. rtadv.c amounts to this by never
 * publishing a duplicated address; manual_v6.c removes it and publishes the
 * failure. IPv4 is untouched.
 */
static void
v6_withdraw(const char *ifname, const struct in6_addr *addr,
    const struct in6_addr *router, unsigned ifindex, struct ra_router *rtr)
{
	struct sc_publish *pub;
	char a[INET6_ADDRSTRLEN];

	(void)inet_ntop(AF_INET6, addr, a, sizeof(a));
	xlog("%s: DAD failed: %s is a duplicate — removing it and the IPv6 "
	    "publish", ifname, a);
	if (rtr != NULL)
		ra_router_stop(rtr, 1);
	else if (router != NULL)
		(void)remove_v6_default_route(router, ifindex);
	(void)remove_v6_address(ifname, addr);
	pub = sc_publish_open("ipconfigd");
	if (pub != NULL) {
		(void)sc_publish_remove_ipv6(pub, ifname);
		sc_publish_close(pub);
	}
	dns_config_set_v6_active(ifname, false);
}

/* `s` is about to install an address: hold duplicates for it. */
static void
v6_pending(struct v6_service *s)
{
	pthread_mutex_lock(&g_v6_services_lock);
	s->state = V6_PENDING;
	s->dup = false;
	pthread_mutex_unlock(&g_v6_services_lock);
}

/*
 * The address `addr` (NULL: none was installed) is published, with router
 * tracker `rtr` if it has one. Record them so a later duplicated-address
 * notification can withdraw them — or, if DAD already reported `addr`
 * duplicated while it was pending, withdraw them now. `router` is NULL for a
 * service with no default route.
 */
static void
v6_commit(struct v6_service *s, const struct in6_addr *addr,
    const struct in6_addr *router, unsigned ifindex, struct ra_router *rtr)
{
	bool dup = false;

	pthread_mutex_lock(&g_v6_services_lock);
	if (addr == NULL) {
		s->state = V6_NONE;
	} else if (s->dup && IN6_ARE_ADDR_EQUAL(&s->dup_addr, addr)) {
		s->state = V6_NONE;
		s->ready = false;
		dup = true;
	} else {
		s->state = V6_ACTIVE;
		s->have_addr = true;
		s->addr = *addr;
		s->have_router = router != NULL;
		if (router != NULL)
			s->router = *router;
		s->ifindex = ifindex;
		s->rtr = rtr;
	}
	s->dup = false;
	pthread_mutex_unlock(&g_v6_services_lock);
	if (dup)
		v6_withdraw(s->ifname, addr, router, ifindex, rtr);
}

/*
 * The service's IPv6 DNS for the merge: the DHCPv6 reply's servers, then
 * RDNSS, de-duplicated, as DNSEntityCreateWithDHCPv6Info() orders them.
 * dns_config.c merges it after DHCPv4's, and needs active IPv6 to use it.
 */
static void
v6_dns_refresh(struct v6_service *s, const char *ifname)
{
	struct ipconfig_dns dns;
	unsigned i, j;

	(void)memset(&dns, 0, sizeof(dns));
	pthread_mutex_lock(&g_v6_services_lock);
	if (s->have_dhcp_dns)
		dns = s->dhcp_dns;
	for (i = 0; i < s->rdnss_count &&
	    dns.server_count < IPCFG_DNS_MAX_SERVERS; i++) {
		char a[INET6_ADDRSTRLEN];

		if (inet_ntop(AF_INET6, &s->rdnss[i], a, sizeof(a)) == NULL)
			continue;
		for (j = 0; j < dns.server_count; j++) {
			if (strcmp(dns.servers[j], a) == 0)
				break;
		}
		if (j == dns.server_count)
			(void)strlcpy(dns.servers[dns.server_count++], a,
			    IPCFG_DNS_ADDR_LEN);
	}
	pthread_mutex_unlock(&g_v6_services_lock);
	dns_config_set_v6_source(ifname, dns.server_count > 0 ? &dns : NULL);
}

/* The DHCPv6 client saved a reply: take its DNS. On the client's thread. */
static void
v6_dhcp_notify(void *arg, struct dhcpv6_client *client)
{
	struct v6_service *s = arg;
	struct ipconfig_dns dns;
	char ifname[IFNAMSIZ];
	bool have;

	have = dhcpv6_client_copy_dns(client, &dns);
	pthread_mutex_lock(&g_v6_services_lock);
	(void)strlcpy(ifname, s->ifname, sizeof(ifname));
	s->have_dhcp_dns = have;
	if (have)
		s->dhcp_dns = dns;
	pthread_mutex_unlock(&g_v6_services_lock);
	v6_dns_refresh(s, ifname);
}

/*
 * rtadv.c's IFEventID_data_e: the sampled RA's flags choose DHCPv6, and its
 * RDNSS replaces what was held. O without M is stateless; a restart here
 * re-asks. M alone gets no client yet (#576).
 */
static void
v6_sample_ra(struct v6_service *s, const char *ifname, const struct ra_info *ra)
{
	struct dhcpv6_client *client;
	bool stateless = ra->other && !ra->managed;
	unsigned i;

	pthread_mutex_lock(&g_v6_services_lock);
	client = s->dhcp_client;
	s->dhcp_client = NULL;
	s->rdnss_count = ra->rdnss_count;
	for (i = 0; i < ra->rdnss_count; i++)
		s->rdnss[i] = ra->rdnss[i];
	if (!stateless)
		s->have_dhcp_dns = false;
	pthread_mutex_unlock(&g_v6_services_lock);

	dhcpv6_client_stop(&client);
	v6_dns_refresh(s, ifname);
	if (!stateless || got_term || s->stop)
		return;
	client = dhcpv6_client_start(ifname, ra->router_scope_id,
	    v6_dhcp_notify, s);
	pthread_mutex_lock(&g_v6_services_lock);
	s->dhcp_client = client;
	pthread_mutex_unlock(&g_v6_services_lock);
}

/* ra_router's sample callback: the RA that answered a renew. */
static void
v6_on_ra_sample(void *arg, const struct ra_info *ra)
{
	struct v6_service *s = arg;
	char ifname[IFNAMSIZ];

	pthread_mutex_lock(&g_v6_services_lock);
	(void)strlcpy(ifname, s->ifname, sizeof(ifname));
	pthread_mutex_unlock(&g_v6_services_lock);
	xlog("%s: renew: resampling RA", ifname);
	v6_sample_ra(s, ifname, ra);
}

/*
 * The RA gave no address. rtadv.c takes the router regardless, so ::/0 and
 * lifetime tracking don't wait for a prefix; there is nothing to publish
 * until DHCPv6 (or a later RA) supplies an address.
 */
static void
v6_track_router_only(struct v6_service *s, const char *ifname,
    const struct ra_info *ra)
{
	struct ra_router *rtr;

	if (install_v6_default_route(&ra->router_lladdr,
	    ra->router_scope_id) != 0)
		xlog("default ::/0 route install failed (non-fatal)");
	rtr = ra_router_start(ifname, ra, NULL, v6_on_ra_sample, s);

	pthread_mutex_lock(&g_v6_services_lock);
	s->state = V6_ACTIVE;
	s->have_addr = false;
	s->have_router = true;
	s->router = ra->router_lladdr;
	s->ifindex = ra->router_scope_id;
	s->rtr = rtr;
	pthread_mutex_unlock(&g_v6_services_lock);
}

/* A Darwin rtadv service, as ipconfigd has always run it after IPv4 binds. */
static void
v6_run_automatic(struct v6_service *s, const char *ifname,
    struct sc_publish *pub)
{
	struct ra_info ra;
	int rar;

	/*
	 * Round-1 CI showed em0 has ND6_IFF_IFDISABLED set (this image's
	 * net.inet6.ip6.auto_linklocal is 0, so the kernel never auto-added a
	 * link-local). bring_v6_up clears IFDISABLED and installs fe80::EUI-64
	 * so the kernel can source-select the link-local-scoped RS.
	 */
	(void)bring_v6_up(ifname, true, NULL);

	/*
	 * Solicit and listen for one RA, derive a SLAAC address, install it
	 * and the v6 default route, and publish State:/.../IPv6. 15s budget —
	 * QEMU SLIRP answers within ms, so a miss means RA isn't configured;
	 * we log IPCFG-RA-MISS and continue with IPv4 only.
	 */
	rar = ra_acquire(ifname, V6_RA_BUDGET_MS, &ra);
	if (rar == 0) {
		struct in6_addr v6;
		char a6[INET6_ADDRSTRLEN];

		/*
		 * M, O and RDNSS come from this first RA (rtadv.c), whatever
		 * else it holds. DHCPv6 runs on its own thread and never
		 * holds up the result the DHCP worker is waiting for.
		 */
		v6_sample_ra(s, ifname, &ra);

		/*
		 * DAD runs in the kernel after the install. Hold ::/0 and the
		 * publish until it has passed (#407), as rtadv.c does; a
		 * duplicate also comes back as the IPv6DuplicatedAddress
		 * notification (#349), which v6_commit() honours if it beat
		 * us here — after a timed-out wait, say.
		 */
		v6_pending(s);
		if (ra.prefix_len == 0) {
			v6_commit(s, NULL, NULL, 0, NULL);
			v6_track_router_only(s, ifname, &ra);
			xlog("IPCFG-RA-MISS: RA has no usable prefix");
		} else if (apply_ra_lease(ifname, &ra, &v6) != 0) {
			v6_commit(s, NULL, NULL, 0, NULL);
			v6_track_router_only(s, ifname, &ra);
			xlog("IPCFG-RA-MISS: apply_ra_lease failed");
		} else if (sc_v6_dad_wait(ifname, &v6,
		    v6_dad_budget_ms(ifname)) == SC_V6_DAD_DUPLICATED) {
			v6_commit(s, NULL, NULL, 0, NULL);
			(void)inet_ntop(AF_INET6, &v6, a6, sizeof(a6));
			xlog("%s: DAD failed: %s is a duplicate — removing it",
			    ifname, a6);
			(void)remove_v6_address(ifname, &v6);
			xlog("IPCFG-RA-MISS: SLAAC address is a duplicate");
		} else {
			struct ra_router *rtr;

			/*
			 * Route install is best-effort. If it fails we still
			 * publish — the address is up and reachable on-link.
			 */
			if (install_v6_default_route(&ra.router_lladdr,
			    ra.router_scope_id) != 0)
				xlog("default ::/0 route install failed "
				    "(non-fatal)");
			/* IPv6 servers now count as reachable (#425). */
			if (pub != NULL && sc_publish_ipv6(pub, ifname, &v6,
			    ra.prefix_len, &ra.router_lladdr) == 0) {
				pthread_mutex_lock(&g_v6_services_lock);
				s->ready = true;
				pthread_mutex_unlock(&g_v6_services_lock);
				dns_config_set_v6_active(ifname, true);
			}
			xlog("IPCFG-RA-OK");
			/*
			 * Keep hearing RAs so ::/0 follows the router's
			 * lifetime instead of outliving it (#350).
			 */
			rtr = ra_router_start(ifname, &ra, &v6,
			    v6_on_ra_sample, s);
			v6_commit(s, &v6, &ra.router_lladdr,
			    ra.router_scope_id, rtr);
		}
	} else if (rar == 1) {
		xlog("IPCFG-RA-MISS: no RA in 15s "
		    "(SLIRP may not advertise IPv6)");
	} else {
		xlog("IPCFG-RA-MISS: ra_acquire fatal");
	}
}

/*
 * manual_v6.c: install the address with infinite lifetimes, publish once DAD
 * has passed. A duplicate has the address removed and is left that way until
 * the link or the method changes, as manual_v6_address_changed() does.
 */
static void
v6_run_manual(struct v6_service *s, const char *ifname,
    const struct ipconfig_v6_method_info *m, struct sc_publish *pub)
{
	char a[INET6_ADDRSTRLEN];

	(void)inet_ntop(AF_INET6, &m->addr, a, sizeof(a));
	(void)bring_v6_up(ifname, false, NULL);
	v6_pending(s);
	if (install_v6_manual_address(ifname, &m->addr, m->prefix_len) != 0) {
		v6_commit(s, NULL, NULL, 0, NULL);
		xlog("%s: could not install Manual %s", ifname, a);
		return;
	}
	if (sc_v6_dad_wait(ifname, &m->addr, v6_dad_budget_ms(ifname)) ==
	    SC_V6_DAD_DUPLICATED) {
		v6_commit(s, NULL, NULL, 0, NULL);
		xlog("%s: IPv6 address %s is in use by another host", ifname,
		    a);
		(void)remove_v6_address(ifname, &m->addr);
		return;
	}
	if (pub != NULL && sc_publish_ipv6(pub, ifname, &m->addr,
	    m->prefix_len, NULL) == 0)
		dns_config_set_v6_active(ifname, true);
	xlog("%s: Manual %s/%u configured", ifname, a,
	    (unsigned)m->prefix_len);
	v6_commit(s, &m->addr, NULL, 0, NULL);
}

/* linklocal_v6.c: publish the fe80:: address, no router, no global. */
static void
v6_run_linklocal(const char *ifname, struct sc_publish *pub)
{
	struct in6_addr ll;
	char a[INET6_ADDRSTRLEN];

	if (bring_v6_up(ifname, false, &ll) != 0) {
		xlog("%s: could not bring up link-local IPv6", ifname);
		return;
	}
	(void)inet_ntop(AF_INET6, &ll, a, sizeof(a));
	if (sc_v6_dad_wait(ifname, &ll, v6_dad_budget_ms(ifname)) ==
	    SC_V6_DAD_DUPLICATED) {
		xlog("%s: link-local %s is in use by another host", ifname, a);
		return;
	}
	if (pub != NULL)
		(void)sc_publish_ipv6(pub, ifname, &ll, 64, NULL);
	xlog("%s: link-local %s configured", ifname, a);
}

/*
 * A service moved here from another interface (#568): wait for the old one to
 * finish tearing down, since its teardown removes the same key.
 */
static void
v6_id_wait_free(const struct v6_service *self)
{
	if (self->service_id[0] == '\0')
		return;
	while (!got_term && !self->stop) {
		bool busy = false;
		int i;

		pthread_mutex_lock(&g_v6_services_lock);
		for (i = 0; i < BOUND_MAX_IF; i++) {
			if (&g_v6_services[i] != self &&
			    g_v6_services[i].running &&
			    strcmp(g_v6_services[i].service_id,
			    self->service_id) == 0)
				busy = true;
		}
		pthread_mutex_unlock(&g_v6_services_lock);
		if (!busy)
			return;
		(void)sleep(1);
	}
}

static void *
v6_service_main(void *arg)
{
	struct v6_service *s = arg;
	struct ipconfig_v6_method_info method;
	struct sc_publish *pub;
	struct ra_router *rtr;
	struct dhcpv6_client *client;
	struct in6_addr addr, router;
	char ifname[IFNAMSIZ];
	unsigned ifindex;
	bool had_addr, have_router, restart;

	pthread_mutex_lock(&g_v6_services_lock);
	(void)strlcpy(ifname, s->ifname, sizeof(ifname));
	method = s->method;
	pthread_mutex_unlock(&g_v6_services_lock);

	v6_id_wait_free(s);
	/* Fix the service ID while the MAC is still readable (#409). */
	sc_publish_service_begin_v6(ifname, s->service_id);
	pub = sc_publish_open("ipconfigd");
	if (pub == NULL)
		xlog("%s: no configd session; IPv6 will not be published",
		    ifname);

	if (!got_term && !s->stop) {
		switch (method.method) {
		case ipconfig_method_manual_v6_e:
			v6_run_manual(s, ifname, &method, pub);
			break;
		case ipconfig_method_linklocal_v6_e:
			v6_run_linklocal(ifname, pub);
			break;
		default:
			v6_run_automatic(s, ifname, pub);
			break;
		}
	}

	pthread_mutex_lock(&g_v6_services_lock);
	s->first_result = true;
	pthread_mutex_unlock(&g_v6_services_lock);

	while (!got_term && !s->stop)
		(void)sleep(1);

	/*
	 * Stop the router tracker first, so it cannot republish behind the
	 * teardown or restart the DHCPv6 client after a renew. A duplicate
	 * may already have taken all of this back.
	 */
	pthread_mutex_lock(&g_v6_services_lock);
	had_addr = s->state == V6_ACTIVE && s->have_addr;
	rtr = s->state == V6_ACTIVE ? s->rtr : NULL;
	addr = s->addr;
	router = s->router;
	have_router = s->state == V6_ACTIVE && s->have_router;
	ifindex = s->ifindex;
	s->rtr = NULL;
	s->state = V6_NONE;
	s->ready = false;
	pthread_mutex_unlock(&g_v6_services_lock);

	ra_router_stop(rtr, !got_term);

	pthread_mutex_lock(&g_v6_services_lock);
	client = s->dhcp_client;
	s->dhcp_client = NULL;
	pthread_mutex_unlock(&g_v6_services_lock);
	dhcpv6_client_stop(&client);

	if (got_term) {
		xlog("%s: daemon shutting down — leaving IPv6 installed",
		    ifname);
	} else {
		xlog("%s: stopping IPv6 %s", ifname,
		    ipconfig_v6_method_string(method.method));
		if (rtr == NULL && have_router)
			(void)remove_v6_default_route(&router, ifindex);
		/*
		 * rtadv.c and manual_v6.c flush what they configured. The
		 * link-local stays until IPv6 itself is turned off.
		 */
		if (had_addr)
			(void)remove_v6_address(ifname, &addr);
		if (pub != NULL)
			(void)sc_publish_remove_ipv6(pub, ifname);
		dns_config_service_down(ifname);
	}
	if (pub != NULL)
		sc_publish_close(pub);

	pthread_mutex_lock(&g_v6_services_lock);
	restart = s->restart;
	s->running = false;
	s->stop = 0;
	s->restart = false;
	if (!restart && !s->v4_result)
		v6_slot_free_locked(s);
	pthread_mutex_unlock(&g_v6_services_lock);

	if (restart && !got_term)
		v6_service_reconfigure(ifname);
	return (NULL);
}

/* Start `s`'s thread for `want`. Caller holds g_v6_services_lock. */
static void
v6_spawn_locked(struct v6_service *s,
    const struct ipconfig_v6_method_info *want, const char *service_id)
{
	pthread_attr_t attr;
	int rc;

	s->method = *want;
	(void)strlcpy(s->service_id, service_id, sizeof(s->service_id));
	s->stop = 0;
	s->restart = false;
	s->first_result = false;
	s->state = V6_NONE;
	s->dup = false;
	s->have_addr = false;
	s->rtr = NULL;
	s->ready = false;
	s->dhcp_client = NULL;
	s->rdnss_count = 0;
	s->have_dhcp_dns = false;
	s->running = true;

	(void)pthread_attr_init(&attr);
	(void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	rc = pthread_create(&s->tid, &attr, v6_service_main, s);
	(void)pthread_attr_destroy(&attr);
	if (rc != 0) {
		s->running = false;
		xlog("%s: pthread_create failed: %s", s->ifname, strerror(rc));
		return;
	}
	xlog("%s: started IPv6 %s", s->ifname,
	    ipconfig_v6_method_string(want->method));
}

void
v6_service_reconfigure(const char *ifname)
{
	struct ipconfig_v6_method_info want;
	char service_id[SC_PUBLISH_SERVICE_ID_LEN];
	struct v6_service *s;
	bool off;

	if (got_term)
		return;
	ipconfigd_v6_lookup(ifname, &want, service_id);
	off = want.method == ipconfig_method_none_v6_e;

	pthread_mutex_lock(&g_v6_services_lock);
	s = v6_find_locked(ifname);
	if (s != NULL && s->running) {
		if (s->stop || memcmp(&s->method, &want, sizeof(want)) != 0 ||
		    strcmp(s->service_id, service_id) != 0) {
			/* Its exit re-evaluates, so old and new never overlap. */
			s->stop = 1;
			s->restart = true;
		}
		pthread_mutex_unlock(&g_v6_services_lock);
		return;
	}
	if (s != NULL && s->v4_result && !off &&
	    want.method != ipconfig_method_unmanaged_v6_e)
		v6_spawn_locked(s, &want, service_id);
	else if (s != NULL && !s->v4_result)
		v6_slot_free_locked(s);
	pthread_mutex_unlock(&g_v6_services_lock);

	/* Off has no thread and needs no IPv4 result. */
	if (off)
		(void)v6_turn_off(ifname);
}

void
v6_service_v4_result(const char *ifname)
{
	struct v6_service *s;

	pthread_mutex_lock(&g_v6_services_lock);
	s = v6_find_or_add_locked(ifname);
	if (s != NULL)
		s->v4_result = true;
	pthread_mutex_unlock(&g_v6_services_lock);
	if (s == NULL) {
		xlog("%s: no free IPv6 service slot — ignoring", ifname);
		return;
	}
	v6_service_reconfigure(ifname);
}

void
v6_service_link_down(const char *ifname)
{
	struct v6_service *s;

	pthread_mutex_lock(&g_v6_services_lock);
	s = v6_find_locked(ifname);
	if (s != NULL) {
		s->v4_result = false;
		if (s->running) {
			s->stop = 1;
			s->restart = false;
		} else {
			v6_slot_free_locked(s);
		}
	}
	pthread_mutex_unlock(&g_v6_services_lock);
}

bool
v6_service_wants_link(const char *ifname)
{
	struct ipconfig_v6_method_info want;
	char service_id[SC_PUBLISH_SERVICE_ID_LEN];

	ipconfigd_v6_lookup(ifname, &want, service_id);
	return (want.method != ipconfig_method_none_v6_e &&
	    want.method != ipconfig_method_unmanaged_v6_e);
}

void
v6_service_solicit(const char *ifname)
{
	struct v6_service *s;
	bool automatic = false;

	pthread_mutex_lock(&g_v6_services_lock);
	s = v6_find_locked(ifname);
	if (s != NULL && s->running && !s->stop)
		automatic = s->method.method == ipconfig_method_automatic_v6_e ||
		    s->method.method == ipconfig_method_rtadv_e;
	/* Under the lock, so the tracker can't be stopped in between. */
	if (automatic && s->rtr != NULL)
		ra_router_request_sample(s->rtr);
	pthread_mutex_unlock(&g_v6_services_lock);
	if (automatic)
		(void)ra_solicit(ifname);
}

void
v6_service_on_duplicated(const char *ifname, const struct in6_addr *addr)
{
	struct v6_service *s;
	struct ra_router *rtr;
	struct in6_addr router;
	unsigned ifindex;
	bool have_router;
	char a[INET6_ADDRSTRLEN];

	(void)inet_ntop(AF_INET6, addr, a, sizeof(a));
	pthread_mutex_lock(&g_v6_services_lock);
	s = v6_find_locked(ifname);
	if (s != NULL && s->state == V6_PENDING) {
		/* Not published yet: v6_commit() checks this. */
		s->dup = true;
		s->dup_addr = *addr;
		pthread_mutex_unlock(&g_v6_services_lock);
		xlog("%s: %s reported duplicated while being installed",
		    ifname, a);
		return;
	}
	if (s == NULL || s->state != V6_ACTIVE || !s->have_addr ||
	    !IN6_ARE_ADDR_EQUAL(&s->addr, addr)) {
		pthread_mutex_unlock(&g_v6_services_lock);
		xlog("%s: %s reported duplicated — not an address we "
		    "configured; ignoring", ifname, a);
		return;
	}
	rtr = s->rtr;
	router = s->router;
	have_router = s->have_router;
	ifindex = s->ifindex;
	s->rtr = NULL;
	s->state = V6_NONE;
	s->ready = false;
	pthread_mutex_unlock(&g_v6_services_lock);
	v6_withdraw(ifname, addr, have_router ? &router : NULL, ifindex, rtr);
}

bool
v6_service_get_packet(const char *ifname, void *buf, size_t size, size_t *len)
{
	struct v6_service *s;
	bool ok = false;

	pthread_mutex_lock(&g_v6_services_lock);
	s = v6_find_locked(ifname);
	if (s != NULL && s->running && s->ready && s->dhcp_client != NULL)
		ok = dhcpv6_client_copy_packet(s->dhcp_client, buf, size, len);
	pthread_mutex_unlock(&g_v6_services_lock);
	return (ok);
}

void
v6_service_wait_first_result(const char *ifname, volatile sig_atomic_t *stop)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
	int waited_ms;

	for (waited_ms = 0; waited_ms < V6_FIRST_RESULT_MAX_MS;
	    waited_ms += 100) {
		struct v6_service *s;
		bool pending;

		if (got_term || (stop != NULL && *stop))
			return;
		pthread_mutex_lock(&g_v6_services_lock);
		s = v6_find_locked(ifname);
		pending = s != NULL && s->running && !s->first_result;
		pthread_mutex_unlock(&g_v6_services_lock);
		if (!pending)
			return;
		(void)nanosleep(&ts, NULL);
	}
}
