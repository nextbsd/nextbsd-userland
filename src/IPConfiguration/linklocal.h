/*
 * linklocal.h — 169.254/16 address selection (RFC 3927, #426).
 *
 * The address half of bootp's IPConfiguration.bproj/linklocal.c: pick a
 * candidate in 169.254.1.0 - 169.254.254.255, probe it, and keep the one
 * this interface had before if it can. The service half (installing and
 * publishing the address, and giving it up) is in ipconfigd.c, because a
 * LinkLocal service and a DHCP service's fallback share it there.
 */
#ifndef _IPCFG_LINKLOCAL_H_
#define _IPCFG_LINKLOCAL_H_

#include <netinet/in.h>
#include <time.h>

#include "dhcp_packet.h"

/* linklocal.c: IN_CLASSB_NET, i.e. 255.255.0.0. */
#define	LINKLOCAL_MASK		0xffff0000U

/* State of one allocation: linklocal.c's current and retry timer. */
struct linklocal_probe {
	struct in_addr	candidate;	/* the next address, 0: pick one */
	int		tries;		/* addresses found in use so far */
	time_t		retry_at;	/* monotonic secs; backing off until */
};

/* Start an allocation, trying `ifname`'s previous address first. */
void	linklocal_probe_init(struct linklocal_probe *lp, const char *ifname);

/*
 * Probe one candidate. Blocks for the length of arp_probe_linklocal().
 *
 *   1  *out is free, and is remembered as `ifname`'s address
 *   0  in use, or backing off: call again
 *  -1  could not probe; backing off
 */
int	linklocal_probe_next(struct linklocal_probe *lp, const char *ifname,
	    struct in_addr *out);

/* `ifname` lost its address to another host: do not try it again. */
void	linklocal_forget(const char *ifname);

/* Fill `lease` for `addr`: the mask and nothing else, so no router. */
void	linklocal_lease(struct dhcp_lease *lease, struct in_addr addr);

/*
 * Take every 169.254/16 address off `ifname`. A DHCP lease replaces the
 * fallback address, including one an earlier ipconfigd left installed.
 * `keep` is left alone: a server may hand out a 169.254 address.
 */
void	linklocal_remove_addresses(const char *ifname, struct in_addr keep);

#endif /* _IPCFG_LINKLOCAL_H_ */
