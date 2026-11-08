/*
 * dns_configuration.h -- ipconfigd's side of dnsinfo (#429).
 *
 * On Darwin configd's IPMonitor plugin flattens every service's DNS into
 * the dnsinfo configuration that scutil --dns and libresolv read. NextBSD's
 * configd has no plugin loader and ipconfigd owns the primary election, so
 * the dnsinfo server and that flattening live here.
 */
#ifndef _IPCFG_DNS_CONFIGURATION_H_
#define	_IPCFG_DNS_CONFIGURATION_H_

#include <net/if.h>		/* IF_NAMESIZE */
#include <netinet/in.h>		/* INET_ADDRSTRLEN, INET6_ADDRSTRLEN */

#include "bound_state.h"	/* BOUND_MAX_IF */

struct ipconfig_dns;

/* Bound IPv4 interfaces and interfaces with IPv6 state are tracked apart. */
#define	DNSINFO_MAX_SERVICES	(BOUND_MAX_IF * 2)

/* One service with DNS. v4_addr and v6_addr are empty when not active. */
struct dnsinfo_service {
	char				ifname[IF_NAMESIZE];
	const struct ipconfig_dns	*dns;
	char				v4_addr[INET_ADDRSTRLEN];
	char				v6_addr[INET6_ADDRSTRLEN];
};

/* Check the dnsinfo XPC service in. Call once, before any update. */
void	dns_configuration_init(void);

/*
 * Rebuild the configuration from `n` services in service order; svcs[primary]
 * supplies the default resolver, and -1 means there is none. Stores and
 * notifies only when the result changed. Serialised by the caller.
 */
void	dns_configuration_update(const struct dnsinfo_service *svcs,
	    unsigned n, int primary);

#endif /* _IPCFG_DNS_CONFIGURATION_H_ */
