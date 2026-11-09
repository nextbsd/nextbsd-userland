/*
 * dhcp_duid_iaid.h — the DHCPv6 client identifier and per-interface IAIDs
 * (#424). Apple's bootp IPConfiguration.bproj/DHCPDUIDIAID.c.
 */
#ifndef _IPCFG_DHCP_DUID_IAID_H_
#define _IPCFG_DHCP_DUID_IAID_H_

#include <CoreFoundation/CoreFoundation.h>

#include <stdint.h>

typedef uint32_t	DHCPIAID;

/*
 * This host's DUID, loaded from /var/db/dhcpclient/DUID_IA.plist or made and
 * stored there. Don't release it. NULL if none can be determined.
 */
CFDataRef	DHCPDUIDGet(void);

/* `ifname`'s IAID: its position in the stored interface list, added if new. */
DHCPIAID	DHCPIAIDGet(const char *ifname);

#endif /* _IPCFG_DHCP_DUID_IAID_H_ */
