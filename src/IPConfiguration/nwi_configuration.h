/*
 * nwi_configuration.h -- ipconfigd's side of network_information (#574).
 *
 * On Darwin configd's IPMonitor plugin writes the nwi state from its
 * election (ElectionResultsCopyPrimary() in ip_plugin.c), and `scutil --nwi`
 * and reachability read it. NextBSD's configd has no plugin loader and
 * ipconfigd owns the election, so the network_information server and the
 * state it publishes live here, alongside dnsinfo.
 */
#ifndef _IPCFG_NWI_CONFIGURATION_H_
#define	_IPCFG_NWI_CONFIGURATION_H_

#include <net/if.h>		/* IF_NAMESIZE */
#include <netinet/in.h>		/* struct in_addr, struct in6_addr */
#include <stdbool.h>

/* One interface. have_v4 / have_v6: it appears in that family's list. */
struct nwi_service {
	char		ifname[IF_NAMESIZE];
	int		rank;		/* iface_rank() */
	bool		have_dns;	/* its service has a DNS entity */
	bool		have_v4;
	struct in_addr	v4_addr;
	bool		have_v6;
	struct in6_addr	v6_addr;
};

/* Check the nwi XPC service in. Call once, before any update. */
void	nwi_configuration_init(void);

/*
 * Rebuild the state from `n` interfaces in service order, best first.
 * Stores and notifies only when the result changed; with nothing in either
 * list the service answers "no state". Serialised by the caller.
 */
void	nwi_configuration_update(const struct nwi_service *svcs, unsigned n);

#endif /* _IPCFG_NWI_CONFIGURATION_H_ */
