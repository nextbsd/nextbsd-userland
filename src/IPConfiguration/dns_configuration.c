/*
 * Copyright (c) 2004-2013 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

/*
 * dns_configuration.c -- flatten the services' DNS into the dnsinfo
 * configuration scutil --dns reads (#429).
 *
 * Vendored from configd-596.15 Plugins/IPMonitor/dns-configuration.c. The
 * assembly below is Apple's: a default resolver from the primary service,
 * supplemental resolvers, a scoped resolver per interface and any
 * service-specific ones, sorted and stored only when the signature moves.
 *
 * NextBSD: the input is built from ipconfigd's merged per-service DNS
 * (dns_config.c) rather than read from IPMonitor's service dictionary.
 * Not carried, because NextBSD has nothing that produces them: multicast
 * and private resolvers (mDNSResponder's State:/Network/MulticastDNS and
 * PrivateDNS) and flat-file resolvers (/etc/resolver/, which libc does not
 * read here).
 */

#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <net/if.h>
#include <netinet/in.h>
#include <resolv.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <CommonCrypto/CommonDigest.h>

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>

#include "dnsinfo_shim.h"
#include "dnsinfo.h"
#include "dnsinfo_create.h"
#include "dnsinfo_server.h"

#include "dns_config.h"
#include "dns_configuration.h"

/* Apple's SCSchemaDefinitions names; ipconfig_prefs.c spells them the same. */
#define	kSCPropInterfaceName			CFSTR("InterfaceName")
#define	kSCPropNetIPv4Addresses			CFSTR("Addresses")
#define	kSCPropNetIPv6Addresses			CFSTR("Addresses")
#define	kSCPropNetDNSDomainName			CFSTR("DomainName")
#define	kSCPropNetDNSOptions			CFSTR("Options")
#define	kSCPropNetDNSSearchDomains		CFSTR("SearchDomains")
#define	kSCPropNetDNSSearchOrder		CFSTR("SearchOrder")
#define	kSCPropNetDNSServerAddresses		CFSTR("ServerAddresses")
#define	kSCPropNetDNSServerPort			CFSTR("ServerPort")
#define	kSCPropNetDNSServerTimeout		CFSTR("ServerTimeout")
#define	kSCPropNetDNSServiceIdentifier		CFSTR("ServiceIdentifier")
#define	kSCPropNetDNSSortList			CFSTR("SortList")
#define	kSCPropNetDNSSupplementalMatchDomains	CFSTR("SupplementalMatchDomains")
#define	kSCPropNetDNSSupplementalMatchOrders	CFSTR("SupplementalMatchOrders")
#define	kSCPropNetDNSSupplementalMatchDomainsNoSearch \
						CFSTR("SupplementalMatchDomainsNoSearch")
#ifndef kSCEntNetDNS
#define	kSCEntNetDNS				CFSTR("DNS")
#endif
#ifndef kSCEntNetIPv4
#define	kSCEntNetIPv4				CFSTR("IPv4")
#endif
#ifndef kSCEntNetIPv6
#define	kSCEntNetIPv6				CFSTR("IPv6")
#endif

/* ip_plugin.h and dns-configuration.h */
#define	kIsNULL					CFSTR("IsNULL")
#define	kIPv4DictService			CFSTR("Service")
#define	DNS_CONFIGURATION_SCOPED_QUERY_KEY	CFSTR("__SCOPED_QUERY__")
#define	DNS_CONFIGURATION_FLAGS_KEY		CFSTR("__FLAGS__")
#define	DNS_CONFIGURATION_IF_INDEX_KEY		CFSTR("__IF_INDEX__")
#define	DNS_CONFIGURATION_ORDER_KEY		CFSTR("__ORDER__")

#define	_ISA(BASE, TYPE)						\
	static inline __unused TYPE					\
	isA_ ## BASE(CFTypeRef cf)					\
	{								\
		return ((cf != NULL && CFGetTypeID(cf) == BASE ## GetTypeID()) ? \
		    (TYPE)cf : NULL);					\
	}
_ISA(CFArray, CFArrayRef)
_ISA(CFBoolean, CFBooleanRef)
_ISA(CFDictionary, CFDictionaryRef)
_ISA(CFNumber, CFNumberRef)
_ISA(CFString, CFStringRef)
#undef	_ISA

static void __printflike(1, 2)
dnsinfo_xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[dnsinfo] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/* ip_plugin.c's cfstring_to_ip() and cfstring_to_ip6(). */
static Boolean
cfstring_to_ip(CFStringRef str, struct in_addr *ip_p)
{
	char	buf[128];

	if (_SC_cfstring_to_cstring(str, buf, sizeof(buf),
	    kCFStringEncodingASCII) == NULL ||
	    inet_aton(buf, ip_p) != 1) {
		ip_p->s_addr = 0;
		return FALSE;
	}
	return TRUE;
}

static Boolean
cfstring_to_ip6(CFStringRef str, struct in6_addr *ip6_p)
{
	char	buf[128];

	if (_SC_cfstring_to_cstring(str, buf, sizeof(buf),
	    kCFStringEncodingASCII) == NULL ||
	    inet_pton(AF_INET6, buf, ip6_p) != 1) {
		bzero(ip6_p, sizeof(*ip6_p));
		return FALSE;
	}
	return TRUE;
}


#pragma mark -
#pragma mark DNS resolver flags


static void
add_dns_query_flags(const void *key, const void *value, void *context)
{
	CFDictionaryRef	service		= value;
	uint32_t	*query_flags	= context;


	// check if the service has v4 or v6 configured

	if ((*query_flags & DNS_RESOLVER_FLAGS_REQUEST_A_RECORDS) == 0) {
		CFDictionaryRef	v4_dict;

		v4_dict = CFDictionaryGetValue(service, kSCEntNetIPv4);
		if (v4_dict != NULL) {
			CFDictionaryRef	v4_service;

			v4_service = CFDictionaryGetValue(v4_dict, kIPv4DictService);
			if (isA_CFDictionary(v4_service)) {
				CFArrayRef	if_addrs;
				CFBooleanRef	is_null;

				is_null = CFDictionaryGetValue(v4_service, kIsNULL);
				if_addrs = CFDictionaryGetValue(v4_service, kSCPropNetIPv4Addresses);
				if (isA_CFBoolean(is_null) != NULL && CFBooleanGetValue(is_null)) {
					// ignore this service
				}
				else if (isA_CFArray(if_addrs) != NULL) {
					int	i;
					int	count;

					count = CFArrayGetCount(if_addrs);
					for (i = 0; i < count; i++) {
						CFStringRef	if_addr;
						struct in_addr	v4_addr;

						if_addr = CFArrayGetValueAtIndex(if_addrs, i);
						if (isA_CFString(if_addr) == NULL) {
							continue;
						}

						cfstring_to_ip(if_addr, &v4_addr);
						if (!IN_LINKLOCAL(ntohl(v4_addr.s_addr))) {
							// set the request v4 dns record bit
							*query_flags |= DNS_RESOLVER_FLAGS_REQUEST_A_RECORDS;
							break;
						}
					}
				}
			}
		}
	}

	if ((*query_flags & DNS_RESOLVER_FLAGS_REQUEST_AAAA_RECORDS) == 0) {
		CFDictionaryRef	v6_dict;

		v6_dict = CFDictionaryGetValue(service, kSCEntNetIPv6);
		if (isA_CFDictionary(v6_dict) != NULL) {
			CFArrayRef	if_addrs6;
			CFBooleanRef	is_null;

			is_null = CFDictionaryGetValue(v6_dict, kIsNULL);
			if_addrs6 = CFDictionaryGetValue(v6_dict, kSCPropNetIPv6Addresses);
			if (isA_CFBoolean(is_null) != NULL && CFBooleanGetValue(is_null)) {
				// ignore this service
			}
			else if (isA_CFArray(if_addrs6) != NULL) {
				int	i;
				int	count;

				count = CFArrayGetCount(if_addrs6);
				for (i = 0; i < count; i++) {
					CFStringRef	if_addr6;
					struct in6_addr	v6_addr;

					if_addr6 = CFArrayGetValueAtIndex(if_addrs6, i);
					if (isA_CFString(if_addr6) == NULL) {
						continue;
					}

					cfstring_to_ip6(if_addr6, &v6_addr);
					if (!IN6_IS_ADDR_LINKLOCAL(&v6_addr)
					    && !IN6_IS_ADDR_MC_LINKLOCAL(&v6_addr)) {
						// set the request v6 dns record bit
						*query_flags |= DNS_RESOLVER_FLAGS_REQUEST_AAAA_RECORDS;
						break;
					}
				}
			}
		}
	}


	return;
}


static void
add_resolver(CFMutableArrayRef resolvers, CFMutableDictionaryRef resolver)
{
	CFIndex		i;
	CFStringRef	interface;
	CFIndex		n_resolvers;
	CFNumberRef	order;
	uint32_t	order_val	= 0;

	order = CFDictionaryGetValue(resolver, kSCPropNetDNSSearchOrder);
	if (!isA_CFNumber(order) ||
	    !CFNumberGetValue(order, kCFNumberSInt32Type, &order_val)) {
		order     = NULL;
		order_val = 0;
	}

	n_resolvers = CFArrayGetCount(resolvers);
	for (i = 0; i < n_resolvers; i++) {
		CFDictionaryRef		match_resolver;

		match_resolver = CFArrayGetValueAtIndex(resolvers, i);
		if (CFEqual(resolver, match_resolver)) {
			// a real duplicate
			return;
		}

		if (order != NULL) {
			CFMutableDictionaryRef	compare;
			Boolean			match;

			compare = CFDictionaryCreateMutableCopy(NULL, 0, match_resolver);
			CFDictionarySetValue(compare, kSCPropNetDNSSearchOrder, order);
			match = CFEqual(resolver, compare);
			CFRelease(compare);
			if (match) {
				CFNumberRef	match_order;
				uint32_t	match_order_val	= 0;

				// if only the search order's are different
				match_order = CFDictionaryGetValue(match_resolver, kSCPropNetDNSSearchOrder);
				if (!isA_CFNumber(match_order) ||
				    !CFNumberGetValue(match_order, kCFNumberSInt32Type, &match_order_val)) {
					match_order_val = 0;
				}

				if (order_val < match_order_val ) {
					// if we should prefer this match resolver, else just skip it
					CFArraySetValueAtIndex(resolvers, i, resolver);
				}

				return;
			}
		}
	}

	order = CFNumberCreate(NULL, kCFNumberCFIndexType, &n_resolvers);
	CFDictionarySetValue(resolver, DNS_CONFIGURATION_ORDER_KEY, order);
	CFRelease(order);

	interface = CFDictionaryGetValue(resolver, kSCPropInterfaceName);
	if ((interface != NULL) && !CFEqual(interface, CFSTR("*"))) {
		uint32_t	flags;
		unsigned int	if_index		= 0;
		char		if_name[IF_NAMESIZE];
		CFNumberRef	num;
		CFBooleanRef	val;

		if (_SC_cfstring_to_cstring(interface,
					    if_name,
					    sizeof(if_name),
					    kCFStringEncodingASCII) != NULL) {
			if_index = if_nametoindex(if_name);
		}

		if ((if_index != 0) &&
		    (
		     // check if this is a "scoped" configuration
		     (CFDictionaryGetValueIfPresent(resolver, DNS_CONFIGURATION_FLAGS_KEY, (const void **)&num) &&
		      isA_CFNumber(num) &&
		      CFNumberGetValue(num, kCFNumberSInt32Type, &flags) &&
		      (flags & DNS_RESOLVER_FLAGS_SCOPED) != 0)
		     ||
		     // check if we should scope all queries with this configuration
		     (CFDictionaryGetValueIfPresent(resolver, DNS_CONFIGURATION_SCOPED_QUERY_KEY, (const void **)&val) &&
		      isA_CFBoolean(val) &&
		      CFBooleanGetValue(val))
		    )
		   ) {
			// if interface index available and it should be used
			num = CFNumberCreate(NULL, kCFNumberIntType, &if_index);
			CFDictionarySetValue(resolver, DNS_CONFIGURATION_IF_INDEX_KEY, num);
			CFRelease(num);
		}
	}

	CFArrayAppendValue(resolvers, resolver);
	return;
}


static void
add_supplemental(CFMutableArrayRef resolvers, CFDictionaryRef dns, uint32_t defaultOrder)
{
	CFArrayRef	domains;
	CFIndex		i;
	CFIndex		n_domains;
	CFArrayRef	orders;

	domains = CFDictionaryGetValue(dns, kSCPropNetDNSSupplementalMatchDomains);
	n_domains = isA_CFArray(domains) ? CFArrayGetCount(domains) : 0;
	if (n_domains == 0) {
		return;
	}

	orders = CFDictionaryGetValue(dns, kSCPropNetDNSSupplementalMatchOrders);
	if (orders != NULL) {
		if (!isA_CFArray(orders) || (n_domains != CFArrayGetCount(orders))) {
			return;
		}
	}

	/*
	 * yes, this is a "supplemental" resolver configuration, expand
	 * the match domains and add each to the resolvers list.
	 */
	for (i = 0; i < n_domains; i++) {
		CFStringRef		match_domain;
		CFNumberRef		match_order;
		CFMutableDictionaryRef	match_resolver;

		match_domain = CFArrayGetValueAtIndex(domains, i);
		if (!isA_CFString(match_domain)) {
			continue;
		}

		match_resolver = CFDictionaryCreateMutableCopy(NULL, 0, dns);

		// set supplemental resolver "domain"
		if (CFStringGetLength(match_domain) > 0) {
			CFDictionarySetValue(match_resolver, kSCPropNetDNSDomainName, match_domain);
		} else {
			CFDictionaryRemoveValue(match_resolver, kSCPropNetDNSDomainName);
		}

		// set supplemental resolver "search_order"
		match_order = (orders != NULL) ? CFArrayGetValueAtIndex(orders, i) : NULL;
		if (isA_CFNumber(match_order)) {
			CFDictionarySetValue(match_resolver, kSCPropNetDNSSearchOrder, match_order);
		} else if (!CFDictionaryContainsKey(match_resolver, kSCPropNetDNSSearchOrder)) {
			CFNumberRef     num;

			num = CFNumberCreate(NULL, kCFNumberIntType, &defaultOrder);
			CFDictionarySetValue(match_resolver, kSCPropNetDNSSearchOrder, num);
			CFRelease(num);

			defaultOrder++;		// if multiple domains, maintain ordering
		}

		// remove keys we don't want in a supplemental resolver
		CFDictionaryRemoveValue(match_resolver, kSCPropNetDNSSupplementalMatchDomains);
		CFDictionaryRemoveValue(match_resolver, kSCPropNetDNSSupplementalMatchOrders);
		CFDictionaryRemoveValue(match_resolver, kSCPropNetDNSSearchDomains);
		CFDictionaryRemoveValue(match_resolver, kSCPropNetDNSSortList);

		add_resolver(resolvers, match_resolver);
		CFRelease(match_resolver);
	}

	return;
}


#define	N_QUICK	32


static void
add_supplemental_resolvers(CFMutableArrayRef	resolvers,
			   CFDictionaryRef	services,
			   CFArrayRef		service_order,
			   CFStringRef		scoped_interface,
			   CFDictionaryRef	scoped_service)
{
	const void *		keys_q[N_QUICK];
	const void **		keys	= keys_q;
	CFIndex			i;
	CFIndex			n_order;
	CFIndex			n_services;
	const void *		vals_q[N_QUICK];
	const void **		vals	= vals_q;

	n_services = isA_CFDictionary(services) ? CFDictionaryGetCount(services) : 0;
	if (n_services == 0) {
		return;		// if no services
	}

	if (n_services > (CFIndex)(sizeof(keys_q) / sizeof(CFTypeRef))) {
		keys = CFAllocatorAllocate(NULL, n_services * sizeof(CFTypeRef), 0);
		vals = CFAllocatorAllocate(NULL, n_services * sizeof(CFTypeRef), 0);
	}

	n_order = isA_CFArray(service_order) ? CFArrayGetCount(service_order) : 0;

	CFDictionaryGetKeysAndValues(services, keys, vals);
	for (i = 0; i < n_services; i++) {
		uint32_t		defaultOrder;
		CFDictionaryRef		dns;
		CFStringRef		interface;
		uint32_t		interface_flags;
		CFMutableDictionaryRef	newDNS		= NULL;
		CFDictionaryRef		service		= (CFDictionaryRef)vals[i];

		if (!isA_CFDictionary(service)) {
			continue;
		}

		dns = CFDictionaryGetValue(service, kSCEntNetDNS);
		dns = isA_CFDictionary(dns);
		if (dns == NULL) {
			continue;
		}

		interface = CFDictionaryGetValue(dns, kSCPropInterfaceName);

		if (scoped_interface != NULL) {
			//
			// we only want to add split/supplemental configurations
			// for queries scoped to an interface if they are NOT
			// associated with a "real" service
			//
			if (CFDictionaryContainsKey(service, kSCEntNetIPv4) ||
			    CFDictionaryContainsKey(service, kSCEntNetIPv6)) {
				continue;
			}

			//
			// in addition, we don't want to add split/supplemental
			// configurations for queries scoped to an interface if
			// the configuration does not apply to all interfaces and
			// the configuration is explicitly NOT for this interface
			//
			if (!_SC_CFEqual(interface, CFSTR("*")) &&
			    !_SC_CFEqual(interface, scoped_interface)) {
				continue;
			}

			//
			// lastly, check if A/AAAA queries should be issued (based
			// on the IP[v6] addresses).  If we would not be issuing a
			// query then don't bother adding the configuration.
			//
			interface_flags = 0;
			add_dns_query_flags(NULL, scoped_service, &interface_flags);
			if (interface_flags == 0) {
				continue;
			}
		}

		defaultOrder = DEFAULT_SEARCH_ORDER
			       - (DEFAULT_SEARCH_ORDER / 2)
			       + ((DEFAULT_SEARCH_ORDER / 1000) * i);
		if ((n_order > 0) &&
		    !CFArrayContainsValue(service_order, CFRangeMake(0, n_order), keys[i])) {
			// push out services not specified in service order
			defaultOrder += (DEFAULT_SEARCH_ORDER / 1000) * n_services;
		}

		/*
		 * Ensure that we have the correct InterfaceName in the DNS configuration
		 *
		 * scoped_interface	[supplemental] interface	DNS interface
		 * ================	========================	=================
		 * NULL			NULL				NULL (No change)
		 * NULL			en0				NULL
		 * NULL			*				NULL
		 * en0			NULL				"en0"
		 * en0			en0				"en0" (now mutable)
		 * en0			*				"en0"
		 */
		if ((scoped_interface == NULL) && (interface == NULL)) {
			newDNS = (CFMutableDictionaryRef)CFRetain(dns);
		} else {
			newDNS = CFDictionaryCreateMutableCopy(NULL, 0, dns);
			if (scoped_interface != NULL) {
				CFDictionarySetValue(newDNS, kSCPropInterfaceName, scoped_interface);
			} else {
				CFDictionaryRemoveValue(newDNS, kSCPropInterfaceName);
			}
		}

		if (scoped_interface != NULL) {
			uint32_t	flags;
			CFNumberRef	num;

			// set "scoped" configuration flag(s)
			if (!CFDictionaryGetValueIfPresent(newDNS, DNS_CONFIGURATION_FLAGS_KEY, (const void **)&num) ||
			    !isA_CFNumber(num) ||
			    !CFNumberGetValue(num, kCFNumberSInt32Type, &flags)) {
				flags = 0;
			}
			flags |= DNS_RESOLVER_FLAGS_SCOPED;

			// add A/AAAA query flag(s)
			flags |= interface_flags;

			num = CFNumberCreate(NULL, kCFNumberSInt32Type, &flags);
			CFDictionarySetValue(newDNS, DNS_CONFIGURATION_FLAGS_KEY, num);
			CFRelease(num);
		}

		// add [scoped] resolver entry
		add_supplemental(resolvers, newDNS, defaultOrder);
		CFRelease(newDNS);
	}

	if (keys != keys_q) {
		CFAllocatorDeallocate(NULL, keys);
		CFAllocatorDeallocate(NULL, vals);
	}

	return;
}


static CFComparisonResult
compareBySearchOrder(const void *val1, const void *val2, void *context)
{
	CFDictionaryRef	dns1	= (CFDictionaryRef)val1;
	CFDictionaryRef	dns2	= (CFDictionaryRef)val2;
	CFNumberRef	num1;
	CFNumberRef	num2;
	uint32_t	order1	= DEFAULT_SEARCH_ORDER;
	uint32_t	order2	= DEFAULT_SEARCH_ORDER;

	num1 = CFDictionaryGetValue(dns1, kSCPropNetDNSSearchOrder);
	if (!isA_CFNumber(num1) ||
	    !CFNumberGetValue(num1, kCFNumberSInt32Type, &order1)) {
		order1 = DEFAULT_SEARCH_ORDER;
	}

	num2 = CFDictionaryGetValue(dns2, kSCPropNetDNSSearchOrder);
	if (!isA_CFNumber(num2) ||
	    !CFNumberGetValue(num2, kCFNumberSInt32Type, &order2)) {
		order2 = DEFAULT_SEARCH_ORDER;
	}

	if (order1 == order2) {
		// if same "SearchOrder", retain original orderring for configurations
		if (CFDictionaryGetValueIfPresent(dns1, DNS_CONFIGURATION_ORDER_KEY, (const void **)&num1) &&
		    CFDictionaryGetValueIfPresent(dns2, DNS_CONFIGURATION_ORDER_KEY, (const void **)&num2) &&
		    isA_CFNumber(num1) &&
		    isA_CFNumber(num2) &&
		    CFNumberGetValue(num1, kCFNumberSInt32Type, &order1) &&
		    CFNumberGetValue(num2, kCFNumberSInt32Type, &order2)) {
			if (order1 == order2) {
				return kCFCompareEqualTo;
			} else {
				return (order1 < order2) ? kCFCompareLessThan : kCFCompareGreaterThan;
			}
		}

		return kCFCompareEqualTo;
	}

	return (order1 < order2) ? kCFCompareLessThan : kCFCompareGreaterThan;
}


static CF_RETURNS_RETAINED CFArrayRef
extract_search_domains(CFMutableDictionaryRef defaultDomain, CFArrayRef supplemental)
{
	CFStringRef		defaultDomainName	= NULL;
	uint32_t		defaultOrder		= DEFAULT_SEARCH_ORDER;
	CFArrayRef		defaultSearchDomains	= NULL;
	CFIndex			defaultSearchIndex	= 0;
	CFIndex			i;
	CFMutableArrayRef	mySearchDomains;
	CFMutableArrayRef	mySupplemental		= NULL;
	CFIndex			n_supplemental;

	mySearchDomains = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

	if (defaultDomain != NULL) {
		CFNumberRef	num;

		num = CFDictionaryGetValue(defaultDomain, kSCPropNetDNSSearchOrder);
		if (!isA_CFNumber(num) ||
		    !CFNumberGetValue(num, kCFNumberSInt32Type, &defaultOrder)) {
			defaultOrder = DEFAULT_SEARCH_ORDER;
		}

		defaultDomainName    = CFDictionaryGetValue(defaultDomain, kSCPropNetDNSDomainName);
		defaultSearchDomains = CFDictionaryGetValue(defaultDomain, kSCPropNetDNSSearchDomains);
	}

	// validate the provided "search" domains or move/expand/promote the "domain" name
	if (isA_CFArray(defaultSearchDomains)) {
		CFIndex	n_search;

		n_search = CFArrayGetCount(defaultSearchDomains);
		for (i = 0; i < n_search; i++) {
			CFStringRef	search;

			search = CFArrayGetValueAtIndex(defaultSearchDomains, i);
			search = _SC_trimDomain(search);
			if (search != NULL) {
				CFArrayAppendValue(mySearchDomains, search);
				CFRelease(search);
			}
		}
	} else {
		defaultDomainName = _SC_trimDomain(defaultDomainName);
		if (defaultDomainName != NULL) {
			CFStringRef	defaultOptions;
			char		*domain;
			int		domain_parts	= 1;
			char		*dp;
			int		ndots		= 1;

#define	NDOTS_OPT	"ndots="
#define	NDOTS_OPT_LEN	(sizeof("ndots=") - 1)

			defaultOptions = CFDictionaryGetValue(defaultDomain, kSCPropNetDNSOptions);
			if (defaultOptions != NULL) {
				char	*cp;
				char	*options;

				options = _SC_cfstring_to_cstring(defaultOptions,
								 NULL,
								 0,
								 kCFStringEncodingUTF8);
				cp = strstr(options, NDOTS_OPT);
				if ((cp != NULL) &&
				    ((cp == options) || isspace(cp[-1])) &&
				    ((cp[NDOTS_OPT_LEN] != '\0') && isdigit(cp[NDOTS_OPT_LEN]))) {
					char    *end;
					long    val;

					cp +=  NDOTS_OPT_LEN;
					errno = 0;
					val = strtol(cp, &end, 10);
					if ((*cp != '\0') && (cp != end) && (errno == 0) &&
					    ((*end == '\0') || isspace(*end)) && (val > 0)) {
						ndots = val;
					}
				}
				CFAllocatorDeallocate(NULL, options);
			}

			domain = _SC_cfstring_to_cstring(defaultDomainName,
							 NULL,
							 0,
							 kCFStringEncodingUTF8);
			CFRelease(defaultDomainName);

			// count domain parts
			for (dp = domain; *dp != '\0'; dp++) {
				if (*dp == '.') {
					domain_parts++;
				}
			}

			// move "domain" to "search" list (and expand as needed)
			i = LOCALDOMAINPARTS;
			dp = domain;
			do {
				CFStringRef	search;
				CFStringRef	str;

				str = CFStringCreateWithCString(NULL,
								dp,
								kCFStringEncodingUTF8);
				search = _SC_trimDomain(str);
				CFRelease(str);
				if (search != NULL) {
					CFArrayAppendValue(mySearchDomains, search);
					CFRelease(search);
				}

				dp = strchr(dp, '.') + 1;
			} while (++i <= (domain_parts - ndots));
			CFAllocatorDeallocate(NULL, domain);
		}
	}

	// add any supplemental "domain" names to the search list
	n_supplemental = (supplemental != NULL) ? CFArrayGetCount(supplemental) : 0;
	if (n_supplemental > 1) {
		mySupplemental = CFArrayCreateMutableCopy(NULL, 0, supplemental);
		CFArraySortValues(mySupplemental,
				  CFRangeMake(0, n_supplemental),
				  compareBySearchOrder,
				  NULL);
		supplemental = mySupplemental;
	}
	for (i = 0; i < n_supplemental; i++) {
		CFDictionaryRef dns;
		CFIndex		domainIndex;
		int		noSearch;
		CFNumberRef	num;
		CFStringRef	options;
		CFStringRef	supplementalDomain;
		uint32_t	supplementalOrder;

		dns = CFArrayGetValueAtIndex(supplemental, i);

		options = CFDictionaryGetValue(dns, kSCPropNetDNSOptions);
		if (isA_CFString(options)) {
			CFRange	range;

			if (CFEqual(options, CFSTR("pdns"))) {
				// don't add private resolver domains to the search list
				continue;
			}

			range = CFStringFind(options, CFSTR("interface="), 0);
			if (range.location != kCFNotFound) {
				// don't add scoped resolver domains to the search list
				continue;
			}
		}

		supplementalDomain = CFDictionaryGetValue(dns, kSCPropNetDNSDomainName);
		supplementalDomain = _SC_trimDomain(supplementalDomain);
		if (supplementalDomain == NULL) {
			continue;
		}

		num = CFDictionaryGetValue(dns, kSCPropNetDNSSupplementalMatchDomainsNoSearch);
		if (isA_CFNumber(num) &&
		    CFNumberGetValue(num, kCFNumberIntType, &noSearch) &&
		    (noSearch != 0)) {
			CFRelease(supplementalDomain);
			continue;
		}

		if (CFStringHasSuffix(supplementalDomain, CFSTR(".in-addr.arpa")) ||
		    CFStringHasSuffix(supplementalDomain, CFSTR(".ip6.arpa"    ))) {
			CFRelease(supplementalDomain);
			continue;
		}

		domainIndex = CFArrayGetFirstIndexOfValue(mySearchDomains,
							  CFRangeMake(0, CFArrayGetCount(mySearchDomains)),
							  supplementalDomain);

		num = CFDictionaryGetValue(dns, kSCPropNetDNSSearchOrder);
		if (!isA_CFNumber(num) ||
		    !CFNumberGetValue(num, kCFNumberSInt32Type, &supplementalOrder)) {
			supplementalOrder = DEFAULT_SEARCH_ORDER;
		}

		if (supplementalOrder < defaultOrder) {
			if (domainIndex != kCFNotFound) {
				// if supplemental domain is already in the search list
				CFArrayRemoveValueAtIndex(mySearchDomains, domainIndex);
				if (domainIndex < defaultSearchIndex) {
					defaultSearchIndex--;
				}
			}
			CFArrayInsertValueAtIndex(mySearchDomains,
						  defaultSearchIndex,
						  supplementalDomain);
			defaultSearchIndex++;
		} else {
			if (domainIndex == kCFNotFound) {
				// add to the (end of the) search list
				CFArrayAppendValue(mySearchDomains, supplementalDomain);
			}
		}

		CFRelease(supplementalDomain);
	}
	if (mySupplemental != NULL) CFRelease(mySupplemental);

	// update the "search" domains
	if (CFArrayGetCount(mySearchDomains) == 0) {
		CFRelease(mySearchDomains);
		mySearchDomains = NULL;
	}

	// remove the "domain" name and "search" list
	CFDictionaryRemoveValue(defaultDomain, kSCPropNetDNSDomainName);
	CFDictionaryRemoveValue(defaultDomain, kSCPropNetDNSSearchDomains);

	return mySearchDomains;
}


static void
add_scoped_resolvers(CFMutableArrayRef scoped, CFDictionaryRef services, CFArrayRef service_order)
{
	const void *		keys_q[N_QUICK];
	const void **		keys	= keys_q;
	CFIndex			i;
	CFIndex			n_order;
	CFIndex			n_services;
	CFMutableArrayRef	order;
	CFMutableSetRef		seen;

	n_services = isA_CFDictionary(services) ? CFDictionaryGetCount(services) : 0;
	if (n_services == 0) {
		return;		// if no services
	}

	// ensure that we process all services in order

	n_order = isA_CFArray(service_order) ? CFArrayGetCount(service_order) : 0;
	if (n_order > 0) {
		order = CFArrayCreateMutableCopy(NULL, 0, service_order);
	} else {
		order = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	}

	if (n_services > (CFIndex)(sizeof(keys_q) / sizeof(CFTypeRef))) {
		keys = CFAllocatorAllocate(NULL, n_services * sizeof(CFTypeRef), 0);
	}
	CFDictionaryGetKeysAndValues(services, keys, NULL);
	for (i = 0; i < n_services; i++) {
		CFStringRef	serviceID = (CFStringRef)keys[i];

		if (!CFArrayContainsValue(order, CFRangeMake(0, n_order), serviceID)) {
			CFArrayAppendValue(order, serviceID);
			n_order++;
		}
	}
	if (keys != keys_q) {
		CFAllocatorDeallocate(NULL, keys);
	}

	// iterate over services

	seen = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	for (i = 0; i < n_order; i++) {
		CFDictionaryRef		dns;
		uint32_t		flags;
		char			if_name[IF_NAMESIZE];
		CFStringRef		interface;
		CFMutableDictionaryRef	newDNS;
		CFNumberRef		num;
		CFArrayRef		searchDomains;
		CFDictionaryRef		service;
		CFStringRef		serviceID;
		uint32_t		these_flags;

		serviceID = CFArrayGetValueAtIndex(order, i);
		service = CFDictionaryGetValue(services, serviceID);
		if (!isA_CFDictionary(service)) {
			// if no service
			continue;
		}

		dns = CFDictionaryGetValue(service, kSCEntNetDNS);
		if (!isA_CFDictionary(dns)) {
			// if no DNS
			continue;
		}

		interface = CFDictionaryGetValue(dns, kSCPropInterfaceName);
		if ((interface == NULL) || CFEqual(interface, CFSTR("*"))) {
			// if no [scoped] interface or supplemental configuration w/match-all
			continue;
		}

		if (CFDictionaryContainsKey(dns, kSCPropNetDNSServiceIdentifier)) {
			// if this is a service-specific resolver
			continue;
		}

		if (CFSetContainsValue(seen, interface)) {
			// if we've already processed this [scoped] interface
			continue;
		}
		CFSetSetValue(seen, interface);

		if ((_SC_cfstring_to_cstring(interface,
					     if_name,
					     sizeof(if_name),
					     kCFStringEncodingASCII) == NULL) ||
		    (if_nametoindex(if_name) == 0)) {
			// if interface index not available
			continue;
		}

		// add [scoped] resolver entry
		newDNS = CFDictionaryCreateMutableCopy(NULL, 0, dns);

		// set search list
		searchDomains = extract_search_domains(newDNS, NULL);
		if (searchDomains != NULL) {
			CFDictionarySetValue(newDNS, kSCPropNetDNSSearchDomains, searchDomains);
			CFRelease(searchDomains);
		}

		// set "scoped" configuration flag(s)
		if (!CFDictionaryGetValueIfPresent(newDNS, DNS_CONFIGURATION_FLAGS_KEY, (const void **)&num) ||
		    !isA_CFNumber(num) ||
		    !CFNumberGetValue(num, kCFNumberSInt32Type, &flags)) {
			flags = 0;
		}
		flags |= DNS_RESOLVER_FLAGS_SCOPED;

		these_flags = 0;
		add_dns_query_flags(serviceID, service, &these_flags);
		if (these_flags == 0) {
		    goto skip;
		}
		flags |= these_flags;

		num = CFNumberCreate(NULL, kCFNumberSInt32Type, &flags);
		CFDictionarySetValue(newDNS, DNS_CONFIGURATION_FLAGS_KEY, num);
		CFRelease(num);

		// remove keys we don't want in a [scoped] resolver
		CFDictionaryRemoveValue(newDNS, kSCPropNetDNSSupplementalMatchDomains);
		CFDictionaryRemoveValue(newDNS, kSCPropNetDNSSupplementalMatchOrders);

		// add the [scoped] resolver
		add_resolver(scoped, newDNS);

		// add any supplemental resolver configurations for this interface
		add_supplemental_resolvers(scoped, services, service_order, interface, service);

	skip:
		CFRelease(newDNS);
	}

	CFRelease(seen);
	CFRelease(order);
	return;
}


static void
add_service_specific_resolvers(CFMutableArrayRef resolvers, CFDictionaryRef services)
{
	CFIndex	services_count	= (isA_CFDictionary(services) ? CFDictionaryGetCount(services) : 0);

	if (services_count > 0) {
		CFIndex			key_idx;
		CFStringRef		keys_q[N_QUICK];
		CFStringRef		*keys	= keys_q;
		CFMutableSetRef		seen	= CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);

		if (services_count > (CFIndex)(sizeof(keys_q) / sizeof(keys_q[0]))) {
			keys = CFAllocatorAllocate(kCFAllocatorDefault, services_count * sizeof(keys[0]), 0);
		}

		CFDictionaryGetKeysAndValues(services, (const void **)keys, NULL);

		for (key_idx = 0; key_idx < services_count; key_idx++) {
			CFDictionaryRef	service	= CFDictionaryGetValue(services, keys[key_idx]);
			CFDictionaryRef	dns	= CFDictionaryGetValue(service, kSCEntNetDNS);

			if (isA_CFDictionary(dns)) {
				CFNumberRef	service_identifier	= CFDictionaryGetValue(dns, kSCPropNetDNSServiceIdentifier);

				if (isA_CFNumber(service_identifier)) {
					if (!CFSetContainsValue(seen, service_identifier)) {
						CFMutableDictionaryRef	new_resolver	= CFDictionaryCreateMutableCopy(NULL, 0, dns);
						CFNumberRef		flags_num;
						int32_t			flags		= 0;

						CFSetSetValue(seen, service_identifier);

						if (!CFDictionaryGetValueIfPresent(new_resolver, DNS_CONFIGURATION_FLAGS_KEY, (const void **)&flags_num) ||
						    !isA_CFNumber(flags_num) ||
						    !CFNumberGetValue(flags_num, kCFNumberSInt32Type, &flags)) {
							flags = 0;
						}

						flags |= DNS_RESOLVER_FLAGS_SERVICE_SPECIFIC | DNS_RESOLVER_FLAGS_REQUEST_ALL_RECORDS;

						flags_num = CFNumberCreate(NULL, kCFNumberSInt32Type, &flags);
						CFDictionarySetValue(new_resolver, DNS_CONFIGURATION_FLAGS_KEY, flags_num);
						CFRelease(flags_num);

						if (CFDictionaryContainsKey(new_resolver, kSCPropInterfaceName)) {
							CFDictionarySetValue(new_resolver, DNS_CONFIGURATION_SCOPED_QUERY_KEY, kCFBooleanTrue);
						}

						CFDictionaryRemoveValue(new_resolver, kSCPropNetDNSSupplementalMatchDomains);
						CFDictionaryRemoveValue(new_resolver, kSCPropNetDNSSupplementalMatchOrders);

						add_resolver(resolvers, new_resolver);
						CFRelease(new_resolver);
					} else {
						dnsinfo_xlog("add_service_specific_resolvers: got a resolver with a duplicate service identifier, skipping");
					}
				}
			}
		}

		if (keys != keys_q) {
			CFAllocatorDeallocate(kCFAllocatorDefault, keys);
		}
		CFRelease(seen);
	}
}


static void
add_default_resolver(CFMutableArrayRef	resolvers,
		     CFDictionaryRef	defaultResolver,
		     Boolean		*orderAdded,
		     CFArrayRef		*searchDomains)
{
	CFMutableDictionaryRef	myDefault;
	uint32_t		myOrder	= DEFAULT_SEARCH_ORDER;
	CFNumberRef		order;

	if (defaultResolver == NULL) {
		myDefault = CFDictionaryCreateMutable(NULL,
						      0,
						      &kCFTypeDictionaryKeyCallBacks,
						      &kCFTypeDictionaryValueCallBacks);
	} else {
		myDefault = CFDictionaryCreateMutableCopy(NULL, 0, defaultResolver);
	}
	assert(myDefault != NULL);

	// ensure that the default resolver has a search order

	order = CFDictionaryGetValue(myDefault, kSCPropNetDNSSearchOrder);
	if (!isA_CFNumber(order) ||
	    !CFNumberGetValue(order, kCFNumberSInt32Type, &myOrder)) {
		myOrder = DEFAULT_SEARCH_ORDER;
		order = CFNumberCreate(NULL, kCFNumberIntType, &myOrder);
		CFDictionarySetValue(myDefault, kSCPropNetDNSSearchOrder, order);
		CFRelease(order);
		*orderAdded = TRUE;
	}

	// extract the "search" domain list for the default resolver (and
	// any supplemental resolvers)

	*searchDomains = extract_search_domains(myDefault, resolvers);

	// add the default resolver

	add_resolver(resolvers, myDefault);
	CFRelease(myDefault);
	return;
}


static dns_create_resolver_t
create_resolver(CFDictionaryRef dns)
{
	CFArrayRef		list;
	CFNumberRef		num;
	dns_create_resolver_t	_resolver;
	CFStringRef		str;
	CFStringRef		targetInterface		= NULL;
	unsigned int		targetInterfaceIndex	= 0;

	_resolver = _dns_resolver_create();

	// process domain
	str = CFDictionaryGetValue(dns, kSCPropNetDNSDomainName);
	if (isA_CFString(str) && (CFStringGetLength(str) > 0)) {
		char	domain[NS_MAXDNAME];

		if (_SC_cfstring_to_cstring(str, domain, sizeof(domain), kCFStringEncodingUTF8) != NULL) {
			_dns_resolver_set_domain(&_resolver, domain);
		}
	}

	// process search domains
	list = CFDictionaryGetValue(dns, kSCPropNetDNSSearchDomains);
	if (isA_CFArray(list)) {
		CFIndex	i;
		CFIndex n	= CFArrayGetCount(list);

		// add "search" domains
		for (i = 0; i < n; i++) {
			str = CFArrayGetValueAtIndex(list, i);
			if (isA_CFString(str) && (CFStringGetLength(str) > 0)) {
				char	search[NS_MAXDNAME];

				if (_SC_cfstring_to_cstring(str, search, sizeof(search), kCFStringEncodingUTF8) != NULL) {
					_dns_resolver_add_search(&_resolver, search);
				}
			}
		}
	}

	// process interface index
	num = CFDictionaryGetValue(dns, DNS_CONFIGURATION_IF_INDEX_KEY);
	if (isA_CFNumber(num)) {
		int	if_index;

		if (CFNumberGetValue(num, kCFNumberIntType, &if_index)) {
			char	if_name[IFNAMSIZ];

			_dns_resolver_set_if_index(&_resolver, if_index);

			if ((if_index != 0) &&
			    (if_indextoname(if_index, if_name) != NULL)) {
				targetInterface = CFStringCreateWithCString(NULL,
									    if_name,
									    kCFStringEncodingASCII);
				targetInterfaceIndex = if_index;
			}
		}
	}

	// process flags
	num = CFDictionaryGetValue(dns, DNS_CONFIGURATION_FLAGS_KEY);
	if (isA_CFNumber(num)) {
		uint32_t	flags;

		if (CFNumberGetValue(num, kCFNumberSInt32Type, &flags)) {
			_dns_resolver_set_flags(&_resolver, flags);
		}
	}

	// process nameserver addresses
	// Note: the flags may be overwritten if the resolver has LOOPBACK addresses
	list = CFDictionaryGetValue(dns, kSCPropNetDNSServerAddresses);
	if (isA_CFArray(list)) {
		CFIndex	i;
		CFIndex	n	= CFArrayGetCount(list);

		for (i = 0; i < n; i++) {
			union {
				struct sockaddr         sa;
				struct sockaddr_in      sin;
				struct sockaddr_in6     sin6;
			} addr;
			char				buf[64];

			str = CFArrayGetValueAtIndex(list, i);
			if (!isA_CFString(str)) {
				continue;
			}

			if (_SC_cfstring_to_cstring(str, buf, sizeof(buf), kCFStringEncodingASCII) == NULL) {
				continue;
			}

			if (_SC_string_to_sockaddr(buf, AF_UNSPEC, (void *)&addr, sizeof(addr)) == NULL) {
				continue;
			}

			if ((addr.sa.sa_family == AF_INET6) &&
			    (IN6_IS_ADDR_LINKLOCAL(&addr.sin6.sin6_addr) ||
			     IN6_IS_ADDR_MC_LINKLOCAL(&addr.sin6.sin6_addr)) &&
			    (addr.sin6.sin6_scope_id == 0) &&
			    (targetInterfaceIndex != 0)) {
				// for link local [IPv6] addresses, if the scope id is not
				// set then we should use the interface associated with the
				// resolver configuration
				addr.sin6.sin6_scope_id = targetInterfaceIndex;
			}

			_dns_resolver_add_nameserver(&_resolver, &addr.sa);
		}
	}

	// process search order
	num = CFDictionaryGetValue(dns, kSCPropNetDNSSearchOrder);
	if (isA_CFNumber(num)) {
		uint32_t	order;

		if (CFNumberGetValue(num, kCFNumberSInt32Type, &order)) {
			_dns_resolver_set_order(&_resolver, order);
		}
	}

	// process sortlist
	list = CFDictionaryGetValue(dns, kSCPropNetDNSSortList);
	if (isA_CFArray(list)) {
		CFIndex	i;
		CFIndex n	= CFArrayGetCount(list);

		for (i = 0; i < n; i++) {
			char		buf[128];
			char		*slash;
			dns_sortaddr_t	sortaddr;

			str = CFArrayGetValueAtIndex(list, i);
			if (!isA_CFString(str)) {
				continue;
			}

			if (_SC_cfstring_to_cstring(str, buf, sizeof(buf), kCFStringEncodingASCII) == NULL) {
				continue;
			}

			slash = strchr(buf, '/');
			if (slash != NULL) {
				*slash = '\0';
			}

			bzero(&sortaddr, sizeof(sortaddr));
			if (inet_aton(buf, &sortaddr.address) != 1) {
				/* if address not valid */
				continue;
			}

			if (slash != NULL) {
				if (inet_aton(slash + 1, &sortaddr.mask) != 1) {
					/* if subnet mask not valid */
					continue;
				}
			} else {
				in_addr_t	a;
				in_addr_t	m;

				a = ntohl(sortaddr.address.s_addr);
				if (IN_CLASSA(a)) {
					m = IN_CLASSA_NET;
				} else if (IN_CLASSB(a)) {
					m = IN_CLASSB_NET;
				} else if (IN_CLASSC(a)) {
					m = IN_CLASSC_NET;
				} else {
					continue;
				}

				sortaddr.mask.s_addr = htonl(m);
			}

			_dns_resolver_add_sortaddr(&_resolver, &sortaddr);
		}
	}

	// process port
	num = CFDictionaryGetValue(dns, kSCPropNetDNSServerPort);
	if (isA_CFNumber(num)) {
		int	port;

		if (CFNumberGetValue(num, kCFNumberIntType, &port)) {
			_dns_resolver_set_port(&_resolver, (uint16_t)port);
		}
	}

	// process timeout
	num = CFDictionaryGetValue(dns, kSCPropNetDNSServerTimeout);
	if (isA_CFNumber(num)) {
		int	timeout;

		if (CFNumberGetValue(num, kCFNumberIntType, &timeout)) {
			_dns_resolver_set_timeout(&_resolver, (uint32_t)timeout);
		}
	}

	// process options
	str = CFDictionaryGetValue(dns, kSCPropNetDNSOptions);
	if (isA_CFString(str)) {
		char	*options;

		options = _SC_cfstring_to_cstring(str, NULL, 0, kCFStringEncodingUTF8);
		if (options != NULL) {
			_dns_resolver_set_options(&_resolver, options);
			CFAllocatorDeallocate(NULL, options);
		}
	}

	num = CFDictionaryGetValue(dns, kSCPropNetDNSServiceIdentifier);
	if (isA_CFNumber(num)) {
		int	service_identifier;

		if (CFNumberGetValue(num, kCFNumberIntType, &service_identifier)) {
			_dns_resolver_set_service_identifier(&_resolver, (uint32_t)service_identifier);
		}
	}

	if (targetInterface != NULL) {
		CFRelease(targetInterface);
	}

	return _resolver;
}


static __inline__ Boolean
isScopedConfiguration(CFDictionaryRef dns)
{
	uint32_t	flags;
	CFNumberRef	num;

	if ((dns != NULL) &&
	    CFDictionaryGetValueIfPresent(dns, DNS_CONFIGURATION_FLAGS_KEY, (const void **)&num) &&
	    (num != NULL) &&
	    CFNumberGetValue(num, kCFNumberSInt32Type, &flags) &&
	    ((flags & DNS_RESOLVER_FLAGS_SCOPED) != 0)) {
		return TRUE;
	}

	return FALSE;
}


static __inline__ Boolean
isServiceSpecificConfiguration(CFDictionaryRef dns)
{
	uint32_t	flags;
	CFNumberRef	num;

	if (dns != NULL &&
	    CFDictionaryGetValueIfPresent(dns, DNS_CONFIGURATION_FLAGS_KEY, (const void **)&num) &&
	    num != NULL &&
	    CFNumberGetValue(num, kCFNumberSInt32Type, &flags) &&
	    (flags & DNS_RESOLVER_FLAGS_SERVICE_SPECIFIC))
	{
		return TRUE;
	}

	return FALSE;
}


static CFComparisonResult
compareDomain(const void *val1, const void *val2, void *context)
{
	CFDictionaryRef		dns1	= (CFDictionaryRef)val1;
	CFDictionaryRef		dns2	= (CFDictionaryRef)val2;
	CFStringRef		domain1;
	CFStringRef		domain2;
	CFArrayRef		labels1	= NULL;
	CFArrayRef		labels2	= NULL;
	CFIndex			n1;
	CFIndex			n2;
	CFComparisonResult	result;
	Boolean			rev1;
	Boolean			rev2;
	Boolean			scoped1;
	Boolean			scoped2;

	// "default" domains sort before "supplemental" domains
	domain1 = CFDictionaryGetValue(dns1, kSCPropNetDNSDomainName);
	domain2 = CFDictionaryGetValue(dns2, kSCPropNetDNSDomainName);
	if (domain1 == NULL) {
		return kCFCompareLessThan;
	} else if (domain2 == NULL) {
		return kCFCompareGreaterThan;
	}

	// sort non-scoped before scoped
	scoped1 = isScopedConfiguration(dns1);
	scoped2 = isScopedConfiguration(dns2);
	if (scoped1 != scoped2) {
		if (!scoped1) {
			return kCFCompareLessThan;
		} else {
			return kCFCompareGreaterThan;
		}
	}

	// must have domain names for any further comparisons
	if ((domain1 == NULL) || (domain2 == NULL)) {
		return kCFCompareEqualTo;
	}

	// forward (A, AAAA) domains sort before reverse (PTR) domains
	rev1 = CFStringHasSuffix(domain1, CFSTR(".arpa"));
	rev2 = CFStringHasSuffix(domain2, CFSTR(".arpa"));
	if (rev1 != rev2) {
		if (rev1) {
			return kCFCompareGreaterThan;
		} else {
			return kCFCompareLessThan;
		}
	}

	labels1 = CFStringCreateArrayBySeparatingStrings(NULL, domain1, CFSTR("."));
	n1 = CFArrayGetCount(labels1);

	labels2 = CFStringCreateArrayBySeparatingStrings(NULL, domain2, CFSTR("."));
	n2 = CFArrayGetCount(labels2);

	while ((n1 > 0) && (n2 > 0)) {
		CFStringRef	label1	= CFArrayGetValueAtIndex(labels1, --n1);
		CFStringRef	label2	= CFArrayGetValueAtIndex(labels2, --n2);

		// compare domain labels
		result = CFStringCompare(label1, label2, kCFCompareCaseInsensitive);
		if (result != kCFCompareEqualTo) {
			goto done;
		}
	}

	// longer labels (corp.apple.com) sort before shorter labels (apple.com)
	if (n1 > n2) {
		result = kCFCompareLessThan;
		goto done;
	} else if (n1 < n2) {
		result = kCFCompareGreaterThan;
		goto done;
	}

	// sort by search order
	result = compareBySearchOrder(val1, val2, context);

    done :

	if (labels1 != NULL) CFRelease(labels1);
	if (labels2 != NULL) CFRelease(labels2);
	return result;
}


static Boolean
dns_configuration_set(CFDictionaryRef   defaultResolver,
		      CFDictionaryRef   services,
		      CFArrayRef	serviceOrder)
{
	dns_create_config_t	_config;
	Boolean			changed			= FALSE;
	uint32_t		dns_resolver_flags	= 0;
	CFIndex			i;
	CFMutableDictionaryRef	myDefault;
	Boolean			myOrderAdded		= FALSE;
	CFArrayRef		mySearchDomains		= NULL;
	CFIndex			n_resolvers;
	CFMutableArrayRef	resolvers;
	unsigned char		signature[CC_SHA1_DIGEST_LENGTH];
	static unsigned char	signature_last[CC_SHA1_DIGEST_LENGTH];

	// establish list of resolvers

	resolvers = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	assert(resolvers != NULL);

	// collect (and add) any "supplemental" resolver configurations

	add_supplemental_resolvers(resolvers, services, serviceOrder, NULL, NULL);

	// add the "default" resolver

	add_default_resolver(resolvers, defaultResolver, &myOrderAdded, &mySearchDomains);

	// collect (and add) any "scoped" resolver configurations

	add_scoped_resolvers(resolvers, services, serviceOrder);

	// collect (and add) any "service-specific" resolver configurations

	add_service_specific_resolvers(resolvers, services);

	// sort resolvers

	n_resolvers = CFArrayGetCount(resolvers);
	if (n_resolvers > 1) {
		CFArraySortValues(resolvers, CFRangeMake(0, n_resolvers), compareDomain, NULL);
	}

	// cleanup

	for (i = n_resolvers; --i > 0; ) {
		CFDictionaryRef	resolver;

		resolver = CFArrayGetValueAtIndex(resolvers, i);
		if (!CFDictionaryContainsKey(resolver, kSCPropNetDNSDomainName) &&
		    !CFDictionaryContainsKey(resolver, kSCPropNetDNSSearchDomains) &&
		    !CFDictionaryContainsKey(resolver, kSCPropNetDNSServerAddresses)) {
			// remove empty resolver
			CFArrayRemoveValueAtIndex(resolvers, i);
			n_resolvers--;
		}
	}

	// update the default resolver

	myDefault = CFDictionaryCreateMutableCopy(NULL,
						  0,
						  CFArrayGetValueAtIndex(resolvers, 0));
	if (mySearchDomains != NULL) {
		// add search domains to the default resolver
		CFDictionarySetValue(myDefault, kSCPropNetDNSSearchDomains, mySearchDomains);
		CFRelease(mySearchDomains);
	}
	if (myOrderAdded && (n_resolvers > 1)) {
		CFDictionaryRef	resolver;

		resolver = CFArrayGetValueAtIndex(resolvers, 1);
		if (CFDictionaryContainsKey(resolver, kSCPropNetDNSDomainName) ||
		    isScopedConfiguration(resolver)) {
			// if not a supplemental "default" resolver (a domain name is
			// present) or if it's a scoped configuration
			CFDictionaryRemoveValue(myDefault, kSCPropNetDNSSearchOrder);
		}
	}
	CFArraySetValueAtIndex(resolvers, 0, myDefault);
	CFRelease(myDefault);

	// establish resolver configuration

	if ((defaultResolver == NULL) && (n_resolvers <= 1)) {
		/*
		 * if no default and no supplemental/scoped resolvers
		 */
		_config = NULL;
	} else {
		/*
		 * if default and/or supplemental/scoped resolvers are defined
		 */
		_config = _dns_configuration_create();

		CFDictionaryApplyFunction(services, add_dns_query_flags , &dns_resolver_flags);

		for (i = 0; i < n_resolvers; i++) {
			boolean_t		is_default_resolver;
			CFDictionaryRef		resolver;
			dns_create_resolver_t	_resolver;

			resolver = CFArrayGetValueAtIndex(resolvers, i);

			is_default_resolver = (!isScopedConfiguration(resolver) && !isServiceSpecificConfiguration(resolver));
			if (is_default_resolver) {
				CFMutableDictionaryRef	new_resolver;
				CFNumberRef		num;

				new_resolver = CFDictionaryCreateMutableCopy(NULL, 0, resolver);

				num = CFNumberCreate(NULL, kCFNumberSInt32Type, &dns_resolver_flags);
				CFDictionarySetValue(new_resolver, DNS_CONFIGURATION_FLAGS_KEY, num);
				CFRelease(num);

				resolver = new_resolver;
			}

			_resolver = create_resolver(resolver);
			_dns_configuration_add_resolver(&_config, _resolver);
			_dns_resolver_free(&_resolver);

			if (is_default_resolver) {
				CFRelease(resolver);
			}
		}
	}

	// check if the configuration changed
	_dns_configuration_signature(&_config, signature, sizeof(signature));
	if (bcmp(signature, signature_last, sizeof(signature)) != 0) {
		// save [new] signature
		bcopy(signature, signature_last, sizeof(signature));

		// save [new] configuration
		if (!_dns_configuration_store(&_config)) {
			dnsinfo_xlog("dns_configuration_set: could not store configuration");
		}

		changed = TRUE;
	}

	if (_config != NULL) _dns_configuration_free(&_config);

	CFRelease(resolvers);
	return changed;
}



#pragma mark -
#pragma mark NextBSD: the service dictionary


static void
dict_set_string(CFMutableDictionaryRef dict, CFStringRef key, const char *s)
{
	CFStringRef	str;

	str = CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
	if (str != NULL) {
		CFDictionarySetValue(dict, key, str);
		CFRelease(str);
	}
}

static void
dict_set_int32(CFMutableDictionaryRef dict, CFStringRef key, int32_t v)
{
	CFNumberRef	num;

	num = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
	CFDictionarySetValue(dict, key, num);
	CFRelease(num);
}

/* An array of strings; `count` may be zero, which still sets the key. */
static void
dict_set_list(CFMutableDictionaryRef dict, CFStringRef key, const char *list,
    size_t stride, unsigned count)
{
	CFMutableArrayRef	array;
	unsigned		i;

	array = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	for (i = 0; i < count; i++) {
		CFStringRef	str;

		str = CFStringCreateWithCString(NULL, list + i * stride,
		    kCFStringEncodingUTF8);
		if (str != NULL) {
			CFArrayAppendValue(array, str);
			CFRelease(str);
		}
	}
	CFDictionarySetValue(dict, key, array);
	CFRelease(array);
}

/* get_dns_changes()'s result for this service, InterfaceName included. */
static CFDictionaryRef
dns_dict_create(const struct dnsinfo_service *svc)
{
	const struct ipconfig_dns	*dns	= svc->dns;
	CFMutableDictionaryRef		dict;

	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	dict_set_string(dict, kSCPropInterfaceName, svc->ifname);
	if (dns->server_count > 0)
		dict_set_list(dict, kSCPropNetDNSServerAddresses,
		    dns->servers[0], IPCFG_DNS_ADDR_LEN, dns->server_count);
	if (dns->search_count > 0)
		dict_set_list(dict, kSCPropNetDNSSearchDomains,
		    dns->search[0], IPCFG_DNS_NAME_LEN, dns->search_count);
	if (dns->sort_count > 0)
		dict_set_list(dict, kSCPropNetDNSSortList, dns->sortlist[0],
		    IPCFG_DNS_SORT_LEN, dns->sort_count);
	if (dns->domain[0] != '\0')
		dict_set_string(dict, kSCPropNetDNSDomainName, dns->domain);
	if (dns->options[0] != '\0')
		dict_set_string(dict, kSCPropNetDNSOptions, dns->options);
	if (dns->has_search_order)
		dict_set_int32(dict, kSCPropNetDNSSearchOrder,
		    dns->search_order);
	if (dns->has_port)
		dict_set_int32(dict, kSCPropNetDNSServerPort, dns->port);
	if (dns->has_timeout)
		dict_set_int32(dict, kSCPropNetDNSServerTimeout, dns->timeout);
	if (dns->has_service_id)
		dict_set_int32(dict, kSCPropNetDNSServiceIdentifier,
		    dns->service_id);
	if (dns->has_match_domains)
		dict_set_list(dict, kSCPropNetDNSSupplementalMatchDomains,
		    dns->match_domains[0], IPCFG_DNS_NAME_LEN,
		    dns->match_count);
	if (dns->has_match_orders) {
		CFMutableArrayRef	orders;
		unsigned		i;

		orders = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
		for (i = 0; i < dns->match_order_count; i++) {
			CFNumberRef	num;

			num = CFNumberCreate(NULL, kCFNumberSInt32Type,
			    &dns->match_orders[i]);
			CFArrayAppendValue(orders, num);
			CFRelease(num);
		}
		CFDictionarySetValue(dict,
		    kSCPropNetDNSSupplementalMatchOrders, orders);
		CFRelease(orders);
	}
	if (dns->has_no_search)
		dict_set_int32(dict,
		    kSCPropNetDNSSupplementalMatchDomainsNoSearch,
		    dns->no_search);
	if (dns->scoped_query)
		CFDictionarySetValue(dict, DNS_CONFIGURATION_SCOPED_QUERY_KEY,
		    kCFBooleanTrue);
	return (dict);
}

/* A one-address array under `key`. */
static CFDictionaryRef
addresses_dict_create(CFStringRef key, const char *addr)
{
	CFMutableDictionaryRef	dict;

	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	dict_set_list(dict, key, addr, strlen(addr) + 1, 1);
	return (dict);
}

/* S_service_state_dict's entry for one service, as add_dns_query_flags() reads it. */
static CFDictionaryRef
service_dict_create(const struct dnsinfo_service *svc, CFDictionaryRef dns)
{
	CFMutableDictionaryRef	service;

	service = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(service, kSCEntNetDNS, dns);
	if (svc->v4_addr[0] != '\0') {
		CFMutableDictionaryRef	v4;
		CFDictionaryRef		v4_service;

		v4 = CFDictionaryCreateMutable(NULL, 0,
		    &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		v4_service = addresses_dict_create(kSCPropNetIPv4Addresses,
		    svc->v4_addr);
		CFDictionarySetValue(v4, kIPv4DictService, v4_service);
		CFRelease(v4_service);
		CFDictionarySetValue(service, kSCEntNetIPv4, v4);
		CFRelease(v4);
	}
	if (svc->v6_addr[0] != '\0') {
		CFDictionaryRef	v6;

		v6 = addresses_dict_create(kSCPropNetIPv6Addresses,
		    svc->v6_addr);
		CFDictionarySetValue(service, kSCEntNetIPv6, v6);
		CFRelease(v6);
	}
	return (service);
}

void
dns_configuration_update(const struct dnsinfo_service *svcs, unsigned n,
    int primary)
{
	CFDictionaryRef		defaultResolver = NULL;
	CFMutableDictionaryRef	services;
	CFMutableArrayRef	order;
	unsigned		i;

	services = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	order = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

	for (i = 0; i < n; i++) {
		CFStringRef	name;
		CFDictionaryRef	dns, service;

		name = CFStringCreateWithCString(NULL, svcs[i].ifname,
		    kCFStringEncodingASCII);
		if (name == NULL)
			continue;
		dns = dns_dict_create(&svcs[i]);
		service = service_dict_create(&svcs[i], dns);
		CFDictionarySetValue(services, name, service);
		CFArrayAppendValue(order, name);
		if ((int)i == primary) {
			defaultResolver = dns;
			CFRetain(defaultResolver);
		}
		CFRelease(service);
		CFRelease(dns);
		CFRelease(name);
	}

	(void)dns_configuration_set(defaultResolver, services, order);

	if (defaultResolver != NULL)
		CFRelease(defaultResolver);
	CFRelease(order);
	CFRelease(services);
}

void
dns_configuration_init(void)
{
	/*
	 * Nothing acknowledges a configuration here yet: on Darwin that is
	 * mDNSResponder, whose ack makes IPMonitor post another network
	 * change. So there is nothing for the handler to do.
	 */
	load_DNSConfiguration(^(Boolean inSync __unused) { });
}
