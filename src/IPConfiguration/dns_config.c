/*
 * dns_config.c — per-service merged DNS and the global DNS it drives (#425).
 *
 * See dns_config.h. The merge follows Apple's rules:
 *
 *   configd Plugins/IPMonitor/ip_plugin.c
 *     accumulate_dns_servers(), merge_dns_servers(), merge_array_prop(),
 *     pick_prop(), get_dns_changes(), update_dns()
 *   bootp IPConfiguration.bproj/sysconfig.c
 *     DNSEntityCreateWithDHCPv4AndDHCPv6Info()
 */
#include "dns_config.h"
#include "apply_lease.h"
#include "bound_state.h"
#include "dns_configuration.h"
#include "ipconfig_prefs.h"
#include "ipconfigd.h"
#include "sc_publish.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <sys/socket.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[dns] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

struct dns_service {
	char			ifname[IF_NAMESIZE];
	bool			v6_active;
	bool			have_v6;
	struct ipconfig_dns	v6;
};

/*
 * g_dns_lock orders whole updates, so two workers cannot interleave the
 * store and resolv.conf writes. It is held across configd RPCs and the
 * file write, which only ever wait on configd and the disk.
 */
static pthread_mutex_t		g_dns_lock = PTHREAD_MUTEX_INITIALIZER;
static struct dns_service	g_services[BOUND_MAX_IF];

/*
 * What update_primary_locked() last applied: Apple's S_dns_dict. `valid`
 * starts false so the first update always acts: /etc/resolv.conf outlives
 * a reboot here, where Darwin's /var/run copy does not.
 */
static struct {
	bool			valid;
	bool			have;
	char			ifname[IF_NAMESIZE];
	struct ipconfig_dns	dns;
} g_applied;

static struct dns_service *
service_find_locked(const char *ifname, bool create)
{
	struct dns_service *free_slot = NULL;
	int i;

	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (g_services[i].ifname[0] == '\0') {
			if (free_slot == NULL)
				free_slot = &g_services[i];
		} else if (strcmp(g_services[i].ifname, ifname) == 0) {
			return (&g_services[i]);
		}
	}
	if (!create || free_slot == NULL)
		return (NULL);
	(void)memset(free_slot, 0, sizeof(*free_slot));
	(void)strlcpy(free_slot->ifname, ifname, sizeof(free_slot->ifname));
	return (free_slot);
}

static bool
list_contains(const char *list, size_t stride, unsigned count, const char *s)
{
	unsigned i;

	for (i = 0; i < count; i++) {
		if (strcmp(list + i * stride, s) == 0)
			return (true);
	}
	return (false);
}

/*
 * DNSEntityCreateWithDHCPInfo(): a lease with no server contributes
 * nothing, even if it named a domain.
 */
static bool
state_from_lease(const struct dhcp_lease *lease, struct ipconfig_dns *out)
{
	unsigned i;

	if (lease->dns_count == 0)
		return (false);
	for (i = 0; i < lease->dns_count; i++) {
		if (inet_ntop(AF_INET, &lease->dns[i],
		    out->servers[out->server_count], IPCFG_DNS_ADDR_LEN) !=
		    NULL)
			out->server_count++;
	}
	(void)strlcpy(out->domain, lease->domain_name, sizeof(out->domain));
	for (i = 0; i < lease->search_count; i++)
		(void)strlcpy(out->search[out->search_count++],
		    lease->search[i], IPCFG_DNS_NAME_LEN);
	out->has_search = out->search_count > 0;
	return (true);
}

/* merge_dict_arrays(): DHCPv4's first, then IPv6's not already there. */
static void
state_merge_v6(struct ipconfig_dns *state, const struct ipconfig_dns *v6)
{
	unsigned i, servers = state->server_count, search = state->search_count;

	for (i = 0; i < v6->server_count &&
	    state->server_count < IPCFG_DNS_MAX_SERVERS; i++) {
		if (!list_contains(state->servers[0], IPCFG_DNS_ADDR_LEN,
		    servers, v6->servers[i]))
			(void)strlcpy(state->servers[state->server_count++],
			    v6->servers[i], IPCFG_DNS_ADDR_LEN);
	}
	for (i = 0; i < v6->search_count &&
	    state->search_count < IPCFG_DNS_MAX_SEARCH; i++) {
		if (!list_contains(state->search[0], IPCFG_DNS_NAME_LEN,
		    search, v6->search[i]))
			(void)strlcpy(state->search[state->search_count++],
			    v6->search[i], IPCFG_DNS_NAME_LEN);
	}
	if (state->search_count > 0)
		state->has_search = true;
}

/*
 * accumulate_dns_servers(): keep the servers this service can reach. An
 * IPv4 server needs active IPv4 and an IPv6 server active IPv6, loopback
 * excepted. `ifname` is non-NULL for Setup's list, whose link-local
 * addresses get their scope; State's are published without one.
 */
static void
accumulate_servers(const struct ipconfig_dns *in, bool v4_active,
    bool v6_active, const char *ifname, struct ipconfig_dns *out)
{
	unsigned i;

	for (i = 0; i < in->server_count &&
	    out->server_count < IPCFG_DNS_MAX_SERVERS; i++) {
		const char *addr = in->servers[i];
		struct in_addr v4;
		struct in6_addr v6;
		char *dst = out->servers[out->server_count];

		if (inet_pton(AF_INET, addr, &v4) == 1) {
			if (!v4_active && ntohl(v4.s_addr) != INADDR_LOOPBACK)
				continue;
			(void)strlcpy(dst, addr, IPCFG_DNS_ADDR_LEN);
		} else if (inet_pton(AF_INET6, addr, &v6) == 1) {
			if (!v6_active && !IN6_IS_ADDR_LOOPBACK(&v6))
				continue;
			if ((IN6_IS_ADDR_LINKLOCAL(&v6) ||
			    IN6_IS_ADDR_MC_LINKLOCAL(&v6)) && ifname != NULL)
				(void)snprintf(dst, IPCFG_DNS_ADDR_LEN,
				    "%s%%%s", addr, ifname);
			else
				(void)strlcpy(dst, addr, IPCFG_DNS_ADDR_LEN);
		} else {
			xlog("ignoring bad DNS server address '%s'", addr);
			continue;
		}
		out->server_count++;
	}
}

/*
 * get_dns_changes(), with S_append_state's default of FALSE: Setup
 * replaces State when it has a usable server list, SearchDomains and
 * SortList replace, and the single-valued properties pick Setup first.
 * `have_setup` is merge_dns_servers()'s: the service has a Setup IP entity.
 */
static void
merge_dns(const struct ipconfig_dns *state, const struct ipconfig_dns *setup,
    bool v4_active, bool v6_active, bool have_setup, const char *ifname,
    struct ipconfig_dns *out)
{
	const struct ipconfig_dns *src;
	bool have_dns_setup = false;

	(void)memset(out, 0, sizeof(*out));

	if (setup != NULL) {
		accumulate_servers(setup, v4_active, v6_active, ifname, out);
		have_dns_setup = out->server_count > 0;
	}
	if (out->server_count == 0 && state != NULL)
		accumulate_servers(state, v4_active, v6_active, NULL, out);
	/*
	 * Bind queries to the interface when the network supplied the
	 * servers, but not when the user chose them: they may only be
	 * reachable another way.
	 */
	out->scoped_query = out->server_count != 0 && have_setup &&
	    !have_dns_setup;

	src = (setup != NULL && setup->has_search) ? setup : state;
	if (src != NULL && src->search_count > 0) {
		(void)memcpy(out->search, src->search, sizeof(out->search));
		out->search_count = src->search_count;
		out->has_search = true;
	}
	src = (setup != NULL && setup->has_sortlist) ? setup : state;
	if (src != NULL && src->sort_count > 0) {
		(void)memcpy(out->sortlist, src->sortlist,
		    sizeof(out->sortlist));
		out->sort_count = src->sort_count;
		out->has_sortlist = true;
	}

	src = (setup != NULL && setup->domain[0] != '\0') ? setup : state;
	if (src != NULL)
		(void)strlcpy(out->domain, src->domain, sizeof(out->domain));
	src = (setup != NULL && setup->options[0] != '\0') ? setup : state;
	if (src != NULL)
		(void)strlcpy(out->options, src->options,
		    sizeof(out->options));
	src = (setup != NULL && setup->has_search_order) ? setup : state;
	if (src != NULL && src->has_search_order) {
		out->has_search_order = true;
		out->search_order = src->search_order;
	}
	src = (setup != NULL && setup->has_port) ? setup : state;
	if (src != NULL && src->has_port) {
		out->has_port = true;
		out->port = src->port;
	}
	src = (setup != NULL && setup->has_timeout) ? setup : state;
	if (src != NULL && src->has_timeout) {
		out->has_timeout = true;
		out->timeout = src->timeout;
	}
	src = (setup != NULL && setup->has_service_id) ? setup : state;
	if (src != NULL && src->has_service_id) {
		out->has_service_id = true;
		out->service_id = src->service_id;
	}

	/* merge_array_prop() and pick_prop(); only Setup can name these. */
	if (setup != NULL && setup->has_match_domains) {
		(void)memcpy(out->match_domains, setup->match_domains,
		    sizeof(out->match_domains));
		out->match_count = setup->match_count;
		out->has_match_domains = true;
	}
	if (setup != NULL && setup->has_match_orders) {
		(void)memcpy(out->match_orders, setup->match_orders,
		    sizeof(out->match_orders));
		out->match_order_count = setup->match_order_count;
		out->has_match_orders = true;
	}
	if (setup != NULL && setup->has_no_search) {
		out->has_no_search = true;
		out->no_search = setup->no_search;
	}
}

static bool
dns_is_empty(const struct ipconfig_dns *d)
{
	return (d->server_count == 0 && d->search_count == 0 &&
	    d->sort_count == 0 && d->domain[0] == '\0' &&
	    d->options[0] == '\0' && !d->has_search_order && !d->has_port &&
	    !d->has_timeout && !d->has_service_id && !d->has_match_domains &&
	    !d->has_match_orders && !d->has_no_search);
}

/*
 * `ifname`'s merged DNS. False when it has none: a service with neither
 * IPv4 nor IPv6 active, or nothing to merge. A service under an
 * `ipconfig set` method has no Setup, like Darwin's serviceless `set`.
 */
static bool
service_merged_locked(const char *ifname, struct ipconfig_dns *out)
{
	static struct ipconfig_dns setup, state;
	struct dhcp_lease lease;
	const struct dns_service *s = service_find_locked(ifname, false);
	struct ipconfig_method_info method;
	bool v4_active, v6_active, have_state = false, have_setup = false;
	bool have_setup_ip = false;

	v4_active = bound_state_get_lease(ifname, &lease);
	v6_active = s != NULL && s->v6_active;
	if (!v4_active && !v6_active)
		return (false);

	(void)memset(&state, 0, sizeof(state));
	if (v4_active)
		have_state = state_from_lease(&lease, &state);
	if (s != NULL && s->have_v6 && s->v6.server_count > 0) {
		state_merge_v6(&state, &s->v6);
		have_state = true;
	}
	if (!ipconfigd_set_method_active(ifname)) {
		have_setup = ipconfig_prefs_lookup_dns(ifname, &setup);
		have_setup_ip = v4_active &&
		    ipconfig_prefs_lookup(ifname, &method) &&
		    method.method != ipconfig_method_none_e;
	}
	if (!have_state && !have_setup)
		return (false);

	merge_dns(have_state ? &state : NULL, have_setup ? &setup : NULL,
	    v4_active, v6_active, have_setup_ip, ifname, out);
	return (!dns_is_empty(out));
}

/* update_dns(): act only when the primary's DNS actually changed. */
static void
update_primary_locked(struct sc_publish *pub)
{
	static struct ipconfig_dns dns;
	char primary[IF_NAMESIZE] = "";
	bool have = false;

	if (bound_state_primary(primary, sizeof(primary)))
		have = service_merged_locked(primary, &dns);
	if (!have) {
		(void)memset(&dns, 0, sizeof(dns));
		primary[0] = '\0';
	}
	if (g_applied.valid && g_applied.have == have &&
	    strcmp(g_applied.ifname, primary) == 0 &&
	    memcmp(&g_applied.dns, &dns, sizeof(dns)) == 0)
		return;

	if (pub != NULL) {
		if (have)
			(void)sc_publish_dns_global(pub, &dns);
		else
			(void)sc_publish_remove_dns_global(pub);
	}
	resolv_conf_sync(have ? primary : NULL, have ? &dns : NULL);

	g_applied.valid = true;
	g_applied.have = have;
	(void)strlcpy(g_applied.ifname, primary, sizeof(g_applied.ifname));
	g_applied.dns = dns;
}

/* The first global IPv6 address on `ifname`; false when it has none. */
static bool
global_v6_addr(const char *ifname, char *out, size_t outsz)
{
	struct ifaddrs *ifa, *p;
	bool found = false;

	if (getifaddrs(&ifa) != 0)
		return (false);
	for (p = ifa; p != NULL && !found; p = p->ifa_next) {
		const struct sockaddr_in6 *sin6;

		if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_INET6 ||
		    strcmp(p->ifa_name, ifname) != 0)
			continue;
		sin6 = (const struct sockaddr_in6 *)(const void *)p->ifa_addr;
		if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr) ||
		    IN6_IS_ADDR_MC_LINKLOCAL(&sin6->sin6_addr))
			continue;
		found = inet_ntop(AF_INET6, &sin6->sin6_addr, out,
		    (socklen_t)outsz) != NULL;
	}
	freeifaddrs(ifa);
	return (found);
}

/*
 * dns_configuration_set()'s input. Every service, not only the primary:
 * each gets a scoped resolver, so a secondary's change matters too.
 * Interfaces go in election order, standing in for Apple's service order.
 */
static void
dnsinfo_update_locked(void)
{
	static struct ipconfig_dns merged[DNSINFO_MAX_SERVICES];
	static struct dnsinfo_service svcs[DNSINFO_MAX_SERVICES];
	static char names[DNSINFO_MAX_SERVICES][IF_NAMESIZE];
	char primary[IF_NAMESIZE] = "";
	int count, i, j, n = 0, primary_idx = -1;

	count = bound_state_list(names, BOUND_MAX_IF);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (g_services[i].ifname[0] == '\0')
			continue;
		for (j = 0; j < count; j++) {
			if (strcmp(names[j], g_services[i].ifname) == 0)
				break;
		}
		if (j == count)
			(void)strlcpy(names[count++], g_services[i].ifname,
			    IF_NAMESIZE);
	}
	/* Insertion sort, so equal ranks keep table order as the election does. */
	for (i = 1; i < count; i++) {
		char tmp[IF_NAMESIZE];

		(void)strlcpy(tmp, names[i], sizeof(tmp));
		for (j = i; j > 0 && iface_rank(tmp) > iface_rank(names[j - 1]);
		    j--)
			(void)strlcpy(names[j], names[j - 1], IF_NAMESIZE);
		(void)strlcpy(names[j], tmp, IF_NAMESIZE);
	}

	(void)bound_state_primary(primary, sizeof(primary));
	for (i = 0; i < count; i++) {
		const struct dns_service *s;
		uint32_t addr;

		if (!service_merged_locked(names[i], &merged[n]))
			continue;
		(void)memset(&svcs[n], 0, sizeof(svcs[n]));
		(void)strlcpy(svcs[n].ifname, names[i], sizeof(svcs[n].ifname));
		svcs[n].dns = &merged[n];
		if (bound_state_get_addr(names[i], &addr))
			(void)inet_ntop(AF_INET, &addr, svcs[n].v4_addr,
			    sizeof(svcs[n].v4_addr));
		s = service_find_locked(names[i], false);
		if (s != NULL && s->v6_active)
			(void)global_v6_addr(names[i], svcs[n].v6_addr,
			    sizeof(svcs[n].v6_addr));
		if (strcmp(names[i], primary) == 0)
			primary_idx = n;
		n++;
	}
	dns_configuration_update(svcs, (unsigned)n, primary_idx);
}

void
dns_config_update(const char *ifname)
{
	static struct ipconfig_dns merged;
	struct sc_publish *pub;

	(void)pthread_mutex_lock(&g_dns_lock);
	pub = sc_publish_open("ipconfigd-dns");
	if (pub != NULL) {
		if (service_merged_locked(ifname, &merged))
			(void)sc_publish_dns(pub, ifname, &merged);
		else
			(void)sc_publish_remove_dns(pub, ifname);
	}
	update_primary_locked(pub);
	dnsinfo_update_locked();
	sc_publish_close(pub);
	(void)pthread_mutex_unlock(&g_dns_lock);
}

void
dns_config_service_down(const char *ifname)
{
	struct dns_service *s;

	(void)pthread_mutex_lock(&g_dns_lock);
	s = service_find_locked(ifname, false);
	if (s != NULL)
		(void)memset(s, 0, sizeof(*s));
	(void)pthread_mutex_unlock(&g_dns_lock);
	dns_config_update(ifname);
}

void
dns_config_set_v6_active(const char *ifname, bool active)
{
	struct dns_service *s;

	(void)pthread_mutex_lock(&g_dns_lock);
	s = service_find_locked(ifname, active);
	if (s != NULL)
		s->v6_active = active;
	(void)pthread_mutex_unlock(&g_dns_lock);
	dns_config_update(ifname);
}

void
dns_config_set_v6_source(const char *ifname, const struct ipconfig_dns *v6)
{
	struct dns_service *s;

	(void)pthread_mutex_lock(&g_dns_lock);
	s = service_find_locked(ifname, v6 != NULL);
	if (s != NULL) {
		s->have_v6 = v6 != NULL;
		if (v6 != NULL)
			s->v6 = *v6;
		else
			(void)memset(&s->v6, 0, sizeof(s->v6));
	}
	(void)pthread_mutex_unlock(&g_dns_lock);
	dns_config_update(ifname);
}
