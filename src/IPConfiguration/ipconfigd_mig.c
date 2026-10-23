/*
 * ipconfigd_mig.c — server-side bodies for ipconfig.defs.
 *
 * The MIG-generated demux _ipconfig_server() (in ipconfigServer.c)
 * calls these per routine. The read-only routines pull live data from
 * bound_state.{c,h}; ipconfig_set drives the DHCP workers through the
 * hooks in ipconfigd.h.
 *
 * Apple's per-routine bodies live inline in IPConfiguration.bproj/
 * ipconfigd.c (10k+ LOC of plugin shell); we keep ours in a small
 * dedicated TU so the daemon main and the lease-loop stay clean.
 */

#include "bound_state.h"
#include "ipconfig_mig_types.h"
#include "ipconfigd.h"
#include "sc_link_watch.h"		/* link_active_in_store() */

#include <mach/mach.h>
#include "ipconfigServer.h"		/* MIG-emitted prototypes */

#include <CoreFoundation/CoreFoundation.h>

#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdbool.h>
#include <string.h>

/*
 * The SCSchema strings ipconfig_set's method dictionary and the summary
 * use. Apple takes them from SCSchemaDefinitions.h; ours carries only
 * the subset other daemons needed so far, so spell them here.
 */
#define	kIPCfgEntIPv4		CFSTR("IPv4")
#define	kIPCfgEntIPv6		CFSTR("IPv6")
#define	kIPCfgConfigMethod	CFSTR("ConfigMethod")

/* Apple's <mach/vm_statistics.h> value; libmach's headers lack it. */
#ifndef VM_FLAGS_ANYWHERE
#define	VM_FLAGS_ANYWHERE	0x0001
#endif

/*
 * Copy `len` bytes into a fresh vm_allocate()d buffer for an
 * out-of-line reply argument. ipconfig.defs marks those `dealloc`, so
 * the kernel unmaps the buffer from ipconfigd as the reply is sent
 * (configd's ool_reply()). Returns false if it could not be allocated.
 */
static bool
ool_reply(const void *bytes, size_t len, uint8_t **out,
    mach_msg_type_number_t *outCnt)
{
	vm_address_t addr = 0;

	*out = NULL;
	*outCnt = 0;
	if (len == 0)
		return (true);
	if (vm_allocate(mach_task_self(), &addr, len, VM_FLAGS_ANYWHERE) !=
	    KERN_SUCCESS)
		return (false);
	(void)memcpy((void *)addr, bytes, len);
	*out = (uint8_t *)addr;
	*outCnt = (mach_msg_type_number_t)len;
	return (true);
}

/* MIG's if_name may fill all 16 bytes: copy out with a forced NUL. */
static void
ifname_from_mig(char ifname[IF_NAMESIZE], const InterfaceName name)
{
	(void)memset(ifname, 0, IF_NAMESIZE);
	(void)memcpy(ifname, name, IF_NAMESIZE - 1);
}

/*
 * Scalar out, no input: how many interfaces are BOUND.
 */
kern_return_t
_ipconfig_if_count(mach_port_t server, int *count)
{
	(void)server;
	*count = bound_state_count();
	return (KERN_SUCCESS);
}

/*
 * Input if_name + scalar out + status. Returns the BOUND
 * IPv4 address as a network-order packed uint32 (matches Apple's
 * ip_address_t wire encoding). Unknown / unbound interfaces return
 * ipconfig_status_no_server_e.
 *
 * MIG passes `name` as an array[16] of char — may not be NUL
 * terminated if the client filled all 16 bytes. Force a NUL at the
 * last slot before strcmp.
 */
kern_return_t
_ipconfig_if_addr(mach_port_t server, InterfaceName name,
    ip_address_t *addr, ipconfig_status_t *status)
{
	char ifname[IF_NAMESIZE];
	uint32_t a = 0;

	(void)server;
	ifname_from_mig(ifname, name);

	if (bound_state_get_addr(ifname, &a)) {
		*addr = a;
		*status = ipconfig_status_success_e;
	} else {
		*addr = 0;
		*status = ipconfig_status_no_server_e;
	}
	return (KERN_SUCCESS);
}

/*
 * Re-encode option `code` from `lease` in DHCP wire form (RFC 2132):
 * what Apple's service_get_option() copies out of the saved ACK. We keep
 * the parsed lease rather than the packet, so only the options
 * struct dhcp_lease carries can be answered. Returns the byte count, or
 * 0 if the lease has no such option.
 */
static mach_msg_type_number_t
lease_option(const struct dhcp_lease *lease, int code, uint8_t *out,
    size_t outsz)
{
	uint32_t be;
	unsigned i;

	switch (code) {
	case DHCP_OPT_SUBNET_MASK:
		(void)memcpy(out, &lease->netmask, 4);
		return (4);
	case DHCP_OPT_ROUTER:
		if (lease->router.s_addr == INADDR_ANY)
			return (0);
		(void)memcpy(out, &lease->router, 4);
		return (4);
	case DHCP_OPT_DNS_SERVER:
		for (i = 0; i < lease->dns_count && (i + 1) * 4 <= outsz; i++)
			(void)memcpy(out + i * 4, &lease->dns[i], 4);
		return (i * 4);
	case DHCP_OPT_HOST_NAME:
		if (lease->host_name_len == 0 || lease->host_name_len > outsz)
			return (0);
		(void)memcpy(out, lease->host_name, lease->host_name_len);
		return (lease->host_name_len);
	case DHCP_OPT_LEASE_TIME:
		be = htonl(lease->lease_time);
		(void)memcpy(out, &be, 4);
		return (4);
	case DHCP_OPT_SERVER_ID:
		if (lease->server.s_addr == INADDR_ANY)
			return (0);
		(void)memcpy(out, &lease->server, 4);
		return (4);
	default:
		return (0);
	}
}

/*
 * One option from one interface's lease (Apple's get_if_option()). An
 * empty name means "whichever service is in charge": Apple walks every
 * interface; we answer from the primary, the only one whose options
 * (router, DNS) are actually in effect.
 */
kern_return_t
_ipconfig_get_option(mach_port_t server, InterfaceName name, int option_code,
    dataOut_t *option_data, mach_msg_type_number_t *option_dataCnt,
    ipconfig_status_t *status)
{
	char ifname[IF_NAMESIZE];
	struct dhcp_lease lease;
	uint8_t buf[DHCP_LEASE_MAX_HOSTNAME];
	mach_msg_type_number_t n;

	(void)server;
	*option_data = NULL;
	*option_dataCnt = 0;
	ifname_from_mig(ifname, name);

	if (ifname[0] != '\0') {
		if (!bound_state_get_lease(ifname, &lease)) {
			*status = ipconfig_status_interface_does_not_exist_e;
			return (KERN_SUCCESS);
		}
		n = lease_option(&lease, option_code, buf,
		    sizeof(buf));
	} else {
		n = 0;
		if (bound_state_primary(ifname, sizeof(ifname)) &&
		    bound_state_get_lease(ifname, &lease))
			n = lease_option(&lease, option_code, buf,
			    sizeof(buf));
		if (n == 0 && bound_state_count() == 0) {
			*status = ipconfig_status_interface_does_not_exist_e;
			return (KERN_SUCCESS);
		}
	}
	if (n == 0)
		*status = ipconfig_status_not_found_e;
	else if (!ool_reply(buf, n, option_data, option_dataCnt))
		*status = ipconfig_status_allocation_failed_e;
	else
		*status = ipconfig_status_success_e;
	return (KERN_SUCCESS);
}

/*
 * `ipconfig set <if> <method>`. Root only, as on Apple. The method
 * dictionary is Apple's: DHCP, MANUAL and NONE are served (#58);
 * INFORM, BOOTP, LinkLocal, Failover and the IPv6 methods have no
 * service behind them yet, so they answer operation_not_supported.
 */
kern_return_t
_ipconfig_set(mach_port_t server, InterfaceName name, xmlData_t method_data,
    mach_msg_type_number_t method_dataCnt, ipconfig_status_t *status,
    audit_token_t audit_token)
{
	char ifname[IF_NAMESIZE];
	CFDataRef data;
	CFPropertyListRef plist = NULL;
	CFDictionaryRef dict;
	CFTypeRef v4, v6;
	struct ipconfig_method_info info;
	uint32_t euid = (uint32_t)-1;

	(void)server;
	ifname_from_mig(ifname, name);

	audit_token_to_au32(audit_token, NULL, &euid, NULL, NULL, NULL, NULL,
	    NULL, NULL);
	if (euid != 0) {
		*status = ipconfig_status_permission_denied_e;
		goto done;
	}

	/* No method data is NONE (Apple's ipconfig_method_none_e). */
	if (method_dataCnt == 0) {
		(void)memset(&info, 0, sizeof(info));
		info.method = ipconfig_method_none_e;
		*status = ipconfigd_set_method(ifname, &info);
		goto done;
	}

	data = CFDataCreateWithBytesNoCopy(NULL, method_data, method_dataCnt,
	    kCFAllocatorNull);
	if (data != NULL) {
		plist = CFPropertyListCreateWithData(NULL, data,
		    kCFPropertyListImmutable, NULL, NULL);
		CFRelease(data);
	}
	if (plist == NULL || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		*status = ipconfig_status_invalid_parameter_e;
		goto done;
	}

	/*
	 * Apple's method_info_from_xml_data(): an IPv4 sub-dictionary wins,
	 * then IPv6; an empty one is NONE-V4 / NONE-V6. Otherwise the
	 * dictionary itself is the IPv4 method.
	 */
	dict = plist;
	v4 = CFDictionaryGetValue(dict, kIPCfgEntIPv4);
	v6 = CFDictionaryGetValue(dict, kIPCfgEntIPv6);
	if (v4 != NULL || v6 != NULL) {
		/*
		 * NONE-V4, NONE-V6 and the IPv6 methods. One worker runs
		 * DHCPv4 and SLAAC together here, so neither family can be
		 * switched on its own yet.
		 */
		*status = ipconfig_status_operation_not_supported_e;
		goto done;
	}
	/*
	 * Apple's ipconfig_method_info_from_dict(), then set_if() (#58).
	 * An unknown ConfigMethod, or Manual without an address, is an
	 * invalid parameter; INFORM, BOOTP, LinkLocal and Failover parse
	 * but have no service behind them, and ipconfigd_set_method()
	 * answers operation_not_supported for those.
	 */
	if (!ipconfig_method_info_from_dict(
	    (const struct __CFDictionary *)dict, NULL, &info))
		*status = ipconfig_status_invalid_parameter_e;
	else
		*status = ipconfigd_set_method(ifname, &info);

done:
	if (plist != NULL)
		CFRelease(plist);
	/* The out-of-line argument is ours once we return KERN_SUCCESS. */
	if (method_data != NULL && method_dataCnt != 0)
		(void)vm_deallocate(mach_task_self(),
		    (vm_address_t)(uintptr_t)method_data, method_dataCnt);
	return (KERN_SUCCESS);
}

static void
dict_set_cstr(CFMutableDictionaryRef d, CFStringRef key, const char *s)
{
	CFStringRef v;

	v = CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
	if (v != NULL) {
		CFDictionarySetValue(d, key, v);
		CFRelease(v);
	}
}

static void
dict_set_addr(CFMutableDictionaryRef d, CFStringRef key, struct in_addr a,
    bool as_array)
{
	char buf[INET_ADDRSTRLEN];
	CFStringRef v;
	CFArrayRef arr;

	(void)inet_ntop(AF_INET, &a, buf, sizeof(buf));
	v = CFStringCreateWithCString(NULL, buf, kCFStringEncodingASCII);
	if (v == NULL)
		return;
	if (as_array) {
		arr = CFArrayCreate(NULL, (const void **)&v, 1,
		    &kCFTypeArrayCallBacks);
		if (arr != NULL) {
			CFDictionarySetValue(d, key, arr);
			CFRelease(arr);
		}
	} else {
		CFDictionarySetValue(d, key, v);
	}
	CFRelease(v);
}

/*
 * The IPv4 service summary — the subset of Apple's ServiceCopySummary()
 * keys we have state for, for a service running method `m`. Only DHCP
 * has the DHCP sub-dictionary.
 */
static CFDictionaryRef
service_summary(const char *ifname, ipconfig_method_t m)
{
	CFMutableDictionaryRef svc, dhcp;
	struct dhcp_lease lease;
	bool bound;

	svc = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (svc == NULL)
		return (NULL);
	dict_set_cstr(svc, kIPCfgConfigMethod, ipconfig_method_string(m));
	bound = bound_state_get_lease(ifname, &lease);
	CFDictionarySetValue(svc, CFSTR("IsPublished"),
	    bound ? kCFBooleanTrue : kCFBooleanFalse);
	if (bound) {
		dict_set_addr(svc, CFSTR("Addresses"), lease.addr, true);
		dict_set_addr(svc, CFSTR("SubnetMasks"), lease.netmask, true);
		if (lease.router.s_addr != INADDR_ANY)
			dict_set_addr(svc, CFSTR("Router"), lease.router,
			    false);
		dhcp = (m != ipconfig_method_dhcp_e) ? NULL :
		    CFDictionaryCreateMutable(NULL, 0,
		    &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		if (dhcp != NULL) {
			dict_set_cstr(dhcp, CFSTR("State"), "BOUND");
			CFDictionarySetValue(svc, CFSTR("DHCP"), dhcp);
			CFRelease(dhcp);
		}
	}
	return (svc);
}

/*
 * `ipconfig getsummary <if>` — Apple's IFStateCopySummary() shape:
 * an IPv4 array of service summaries, link status and interface type.
 * Unknown interfaces (and loopback, which ipconfigd never manages)
 * answer interface_does_not_exist, as Apple's do.
 */
kern_return_t
_ipconfig_get_summary(mach_port_t server, InterfaceName name,
    xmlDataOut_t *state, mach_msg_type_number_t *stateCnt,
    ipconfig_status_t *status, audit_token_t audit_token)
{
	char ifname[IF_NAMESIZE];
	CFMutableDictionaryRef summary;
	CFDictionaryRef svc;
	CFArrayRef services;
	CFDataRef data;
	struct ipconfig_method_info info;
	bool running;

	(void)server;
	(void)audit_token;	/* Apple gates only the private Wi-Fi keys */
	*state = NULL;
	*stateCnt = 0;
	ifname_from_mig(ifname, name);

	if (ifname[0] == '\0' || if_nametoindex(ifname) == 0 ||
	    iface_rank(ifname) == IFRANK_NEVER) {
		*status = ipconfig_status_interface_does_not_exist_e;
		return (KERN_SUCCESS);
	}

	summary = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (summary == NULL) {
		*status = ipconfig_status_allocation_failed_e;
		return (KERN_SUCCESS);
	}
	running = ipconfigd_get_method(ifname, &info);
	if (running || bound_state_is_bound(ifname)) {
		svc = service_summary(ifname,
		    running ? info.method : ipconfig_method_dhcp_e);
		if (svc != NULL) {
			services = CFArrayCreate(NULL, (const void **)&svc, 1,
			    &kCFTypeArrayCallBacks);
			if (services != NULL) {
				CFDictionarySetValue(summary, kIPCfgEntIPv4,
				    services);
				CFRelease(services);
			}
			CFRelease(svc);
		}
	}
	/* Apple: LinkStatusActive when up; the Link entity's Active if not. */
	if (link_active_in_store(ifname))
		CFDictionarySetValue(summary, CFSTR("LinkStatusActive"),
		    kCFBooleanTrue);
	else
		CFDictionarySetValue(summary, CFSTR("Active"), kCFBooleanFalse);
	dict_set_cstr(summary, CFSTR("InterfaceType"),
	    iface_rank(ifname) == IFRANK_WIRELESS ? "WiFi" : "Ethernet");

	data = CFPropertyListCreateData(NULL, summary,
	    kCFPropertyListBinaryFormat_v1_0, 0, NULL);
	CFRelease(summary);
	if (data == NULL || !ool_reply(CFDataGetBytePtr(data),
	    (size_t)CFDataGetLength(data), state, stateCnt))
		*status = ipconfig_status_allocation_failed_e;
	else
		*status = ipconfig_status_success_e;
	if (data != NULL)
		CFRelease(data);
	return (KERN_SUCCESS);
}
