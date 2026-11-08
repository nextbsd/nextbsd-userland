/*
 * dns_config.h — per-service merged DNS and the global DNS it drives (#425).
 *
 * On Darwin the merge is IPMonitor's: get_dns_changes() combines a
 * service's State:/Network/Service/<id>/DNS (from IPConfiguration) with
 * its Setup:/Network/Service/<id>/DNS (the user's), and update_dns() /
 * set_dns() publish the primary's result as State:/Network/Global/DNS
 * and resolv.conf. NextBSD's configd has no plugin loader, so there is
 * no IPMonitor and no Setup:/Network/Service keys; the same rules live
 * here, reading "Setup" from the SCPreferences table ipconfig_prefs.c
 * already keeps.
 *
 * Because ipconfigd is its own IPMonitor, the key it publishes as
 * State:/Network/Service/<id>/DNS holds the merged result. On Darwin
 * that key holds only what DHCP supplied and the merge stays inside
 * IPMonitor; here there is no other process to read it from.
 *
 * Everything is plain C and fixed-size, with no pointers, so the
 * preferences table can compare entries with memcmp. Fill a struct only
 * after zeroing it.
 */
#ifndef _IPCFG_DNS_CONFIG_H_
#define _IPCFG_DNS_CONFIG_H_

#include <net/if.h>		/* IF_NAMESIZE */
#include <netinet/in.h>		/* INET6_ADDRSTRLEN */
#include <stdbool.h>
#include <stdint.h>

#include "dhcp_packet.h"	/* DHCP_LEASE_MAX_*, struct dhcp_lease */

#define	IPCFG_DNS_MAX_SERVERS	DHCP_LEASE_MAX_DNS
#define	IPCFG_DNS_MAX_SEARCH	DHCP_LEASE_MAX_SEARCH
#define	IPCFG_DNS_MAX_SORT	16
/* An IPv6 address, '%' and an interface name. */
#define	IPCFG_DNS_ADDR_LEN	(INET6_ADDRSTRLEN + IF_NAMESIZE + 1)
#define	IPCFG_DNS_NAME_LEN	DHCP_LEASE_MAX_DOMAIN
#define	IPCFG_DNS_SORT_LEN	64
#define	IPCFG_DNS_OPTIONS_LEN	256

/*
 * One DNS dictionary in Apple's shape (kSCEntNetDNS). Used for a Setup
 * entity, for a service's dynamic state and for the merged result.
 * has_search and has_sortlist record that the array exists at all: an
 * empty Setup array still replaces State's, as merge_array_prop() has it.
 */
struct ipconfig_dns {
	char		servers[IPCFG_DNS_MAX_SERVERS][IPCFG_DNS_ADDR_LEN];
	unsigned	server_count;
	char		search[IPCFG_DNS_MAX_SEARCH][IPCFG_DNS_NAME_LEN];
	unsigned	search_count;
	bool		has_search;
	char		sortlist[IPCFG_DNS_MAX_SORT][IPCFG_DNS_SORT_LEN];
	unsigned	sort_count;
	bool		has_sortlist;
	char		domain[IPCFG_DNS_NAME_LEN];
	char		options[IPCFG_DNS_OPTIONS_LEN];
	bool		has_search_order;
	int32_t		search_order;
	bool		has_port;
	int32_t		port;
	bool		has_timeout;
	int32_t		timeout;
	bool		has_service_id;
	int32_t		service_id;
};

/*
 * Re-merge `ifname`'s DNS, publish or remove its
 * State:/Network/Service/<UUID>/DNS, then bring State:/Network/Global/DNS
 * and /etc/resolv.conf in line with the primary service. Call once
 * bound_state reflects the change. Serialised internally; the caller must
 * not hold g_workers_lock.
 */
void	dns_config_update(const char *ifname);

/* `ifname`'s service ended: forget its IPv6 state, then update as above. */
void	dns_config_service_down(const char *ifname);

/* `ifname` gained or lost its published IPv6 configuration. */
void	dns_config_set_v6_active(const char *ifname, bool active);

/*
 * Set `ifname`'s IPv6 DNS source (DHCPv6, RDNSS), or clear it with NULL.
 * Merged after DHCPv4's, de-duplicated, as DNSEntityCreateWithDHCPv4And
 * DHCPv6Info() does. Addresses carry no scope.
 */
void	dns_config_set_v6_source(const char *ifname,
	    const struct ipconfig_dns *v6);

#endif /* _IPCFG_DNS_CONFIG_H_ */
