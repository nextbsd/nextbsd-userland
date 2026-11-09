/*
 * ipconfig_method.h — per-interface IPv4 configuration methods (#58).
 *
 * Apple's IPConfiguration runs one service per interface, and the
 * service's config method (DHCP, Manual, INFORM, BOOTP, ...) decides
 * what it does. The method comes from the network service's IPv4 entity
 * in SCPreferences (ConfigMethod, Addresses, SubnetMasks, Router), or
 * from `ipconfig set`, which builds the same dictionary. Apple carries
 * the parsed form as ipconfig_method_info_t (bootp/bootplib/
 * ipconfig_types.h); this is the subset ipconfigd serves.
 *
 * ipconfigd serves DHCP, Manual, LinkLocal and NONE. An interface no service
 * names stays on DHCP, so a machine with no network preferences behaves
 * exactly as it did before methods existed. The other methods parse but
 * are not served: an interface configured with one is left alone rather
 * than DHCP'd behind the administrator's back.
 */
#ifndef _IPCFG_IPCONFIG_METHOD_H_
#define _IPCFG_IPCONFIG_METHOD_H_

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#include "dhcp_packet.h"		/* DHCP_LEASE_MAX_DNS */

/* Apple's ipconfig_method_t values (bootp/bootplib/ipconfig_types.h). */
typedef enum {
	ipconfig_method_none_e		= 0,
	ipconfig_method_manual_e	= 1,
	ipconfig_method_bootp_e		= 2,
	ipconfig_method_dhcp_e		= 3,
	ipconfig_method_inform_e	= 4,
	ipconfig_method_linklocal_e	= 5,
	ipconfig_method_failover_e	= 6,

	/* IPv6 (#423): Apple's values. */
	ipconfig_method_none_v6_e	= 0x200,
	ipconfig_method_manual_v6_e	= 0x201,
	ipconfig_method_automatic_v6_e	= 0x202,
	ipconfig_method_rtadv_e		= 0x203,
	ipconfig_method_stf_e		= 0x204,
	ipconfig_method_linklocal_v6_e	= 0x205,

	/*
	 * NextBSD only: ipconfigd does not touch this interface's IPv6 at
	 * all (a disabled service, or a 6to4 entity). Off, by contrast, is
	 * ipconfig_method_none_v6_e and takes IPv6 off the interface.
	 */
	ipconfig_method_unmanaged_v6_e	= 0x2ff
} ipconfig_method_t;

/*
 * A parsed IPv4 method. addr/mask/router are used by Manual (and would
 * be by INFORM). Zero-filled fields are absent.
 *
 * The service's DNS entity is deliberately not here (#425): on Darwin it
 * sits beside IPv4, and the method is compared whole to decide whether a
 * running service must restart. A DNS-only change must not restart DHCP;
 * see dns_config.h and ipconfig_prefs_lookup_dns().
 */
struct ipconfig_method_info {
	ipconfig_method_t	method;
	struct in_addr		addr;
	struct in_addr		mask;
	struct in_addr		router;
};

/*
 * A parsed IPv6 method (#423), the other half of Apple's
 * ipconfig_method_info_t. addr/prefix_len are used by Manual only. Zero the
 * struct before filling it: it is compared whole to decide whether the
 * running IPv6 service must restart.
 */
struct ipconfig_v6_method_info {
	ipconfig_method_t	method;
	struct in6_addr		addr;
	uint8_t			prefix_len;
};

/* Apple's ConfigMethod spelling for `m` ("DHCP", "Manual", ...). */
const char	*ipconfig_method_string(ipconfig_method_t m);

/* Does ipconfigd run a service for `m`? (DHCP, Manual and LinkLocal.) */
bool		ipconfig_method_is_served(ipconfig_method_t m);

/*
 * Parse an IPv4 entity into `out` — Apple's
 * ipconfig_method_info_from_dict(). The argument is a CFDictionaryRef,
 * spelled as its underlying type so plain-C callers need not pull
 * CoreFoundation. Returns false when the dictionary is not a valid method
 * (unknown ConfigMethod, Manual with no address).
 */
struct __CFDictionary;
bool		ipconfig_method_info_from_dict(
		    const struct __CFDictionary *ipv4,
		    struct ipconfig_method_info *out);

/*
 * Apple's ipconfig_method_string() spelling for an IPv6 method
 * ("AUTOMATIC-V6", "MANUAL-V6", ...), used to name an `ipconfig set`
 * service and in the log.
 */
const char	*ipconfig_v6_method_string(ipconfig_method_t m);

/*
 * Parse an IPv6 entity into `out` — Apple's method_info_from_ipv6_dict().
 * Returns false when the dictionary is not a valid method: an unknown
 * ConfigMethod, Manual with no address, or a Manual fe80::/10 address.
 */
bool		ipconfig_v6_method_info_from_dict(
		    const struct __CFDictionary *ipv6,
		    struct ipconfig_v6_method_info *out);

#endif /* _IPCFG_IPCONFIG_METHOD_H_ */
