/*
 * Copyright (c) 2000-2025 Apple Inc. All rights reserved.
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
 * Modification History
 *
 * July 9, 2001			Allan Nathanson <ajn@apple.com>
 * - added "-r" option for checking network reachability
 * - added "-w" option to check/wait for the presence of a
 *   dynamic store key.
 *
 * June 1, 2001			Allan Nathanson <ajn@apple.com>
 * - public API conversion
 *
 * November 9, 2000		Allan Nathanson <ajn@apple.com>
 * - initial revision
 */

/*
 * NextBSD: of Apple's tests.c the "-w" key wait, "--dns", "--proxy" and
 * "--renew" are carried. The rest backs options whose APIs our
 * libSystemConfiguration does not have yet -- "-r"
 * (SCNetworkReachabilityCreateWithName/WithAddress), "--nwi"
 * (network_information), "--snapshot", "--advisory" and "--rank".
 *
 * "--dns" is configd-596.15's, not this file's later tag: its printer
 * (dnsinfo_internal.h) is the 10.9.5 one, matching the dnsinfo the server
 * stores. "--dns <hostname>" needs reachability and is not carried yet.
 */

#include <signal.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <net/if.h>

#include <SystemConfiguration/SCDynamicStoreCopySpecificPrivate.h>

#include "scutil.h"
#include "prefs.h"
#include "tests.h"

/* NextBSD: "-d" is not carried, and dnsinfo_internal.h prints under it. */
#define	_sc_debug	FALSE
#include <dnsinfo.h>
#include "dnsinfo_internal.h"

__private_extern__
void
do_showDNSConfiguration(int argc __unused, char **argv __unused)
{
	dns_config_t	*dns_config;

	dns_config = dns_configuration_copy();

	if (dns_config == NULL) {
		SCPrint(TRUE, stdout, CFSTR("No DNS configuration available\n"));
		exit(1);
	}

	_dns_configuration_print(dns_config);

	dns_configuration_free(dns_config);
	exit(0);
}


static void
showProxy(CFDictionaryRef proxy)
{
	CFMutableDictionaryRef	cleaned;

	/*
	 * NextBSD: Apple shows these internal keys under "-d", which this
	 * port does not carry, so they are always removed.
	 */
	cleaned = CFDictionaryCreateMutableCopy(NULL, 0, proxy);
	CFDictionaryRemoveValue(cleaned, kSCPropNetProxiesScoped);
	CFDictionaryRemoveValue(cleaned, kSCPropNetProxiesServices);
	CFDictionaryRemoveValue(cleaned, kSCPropNetProxiesSupplemental);

	SCPrint(TRUE, stdout, CFSTR("%@\n"), cleaned);
	CFRelease(cleaned);
	return;
}


__private_extern__
void
do_showProxyConfiguration(int argc, char **argv)
{
	CFMutableDictionaryRef	options = NULL;
	CFDictionaryRef		proxies;

	if (getenv("BYPASS_GLOBAL_PROXY") != NULL) {
		options = CFDictionaryCreateMutable(NULL, 0,
						    &kCFTypeDictionaryKeyCallBacks,
						    &kCFTypeDictionaryValueCallBacks);
		CFDictionaryAddValue(options, kSCProxiesNoGlobal, kCFBooleanTrue);
	}

	proxies = SCDynamicStoreCopyProxiesWithOptions(NULL, options);

	if (options != NULL) {
		CFRelease(options);
	}

	if (proxies != NULL) {
		CFStringRef	interface	= NULL;
		CFStringRef	server		= NULL;

		while (argc > 0) {
			if (strcasecmp(argv[0], "interface") == 0) {
				argv++;
				argc--;

				if (argc < 1) {
					SCPrint(TRUE, stderr, CFSTR("No interface\n"));
					exit(1);
				}

				if (if_nametoindex(argv[0]) == 0) {
					SCPrint(TRUE, stderr, CFSTR("No interface: %s\n"), argv[0]);
					exit(1);
				}

				interface = CFStringCreateWithCString(NULL, argv[0], kCFStringEncodingUTF8);
				argv++;
				argc--;
			} else {
				if (server != NULL) {
					CFRelease(server);
				}

				server = CFStringCreateWithCString(NULL, argv[0], kCFStringEncodingUTF8);
				argv++;
				argc--;
			}
		}

		if ((server != NULL) || (interface != NULL)) {
			CFArrayRef	matching;

			matching = SCNetworkProxiesCopyMatching(proxies, server, interface);
			if (matching != NULL) {
				CFIndex	i;
				CFIndex	n;

				if (server != NULL) {
					if (interface != NULL) {
						SCPrint(TRUE, stdout,
							CFSTR("server = %@, interface = %@\n"),
							server,
							interface);
					} else {
						SCPrint(TRUE, stdout,
							CFSTR("server = %@\n"),
							server);
					}
				} else {
					SCPrint(TRUE, stdout,
						CFSTR("interface = %@\n"),
						interface);
				}

				n = CFArrayGetCount(matching);
				for (i = 0; i < n; i++) {
					CFDictionaryRef	proxy;

					proxy = CFArrayGetValueAtIndex(matching, i);
					SCPrint(TRUE, stdout, CFSTR("\nproxy #%d\n"), (int)(i + 1));
					showProxy(proxy);
				}

				CFRelease(matching);
			} else {
				SCPrint(TRUE, stdout, CFSTR("No matching proxy configurations\n"));
			}
		} else {
			showProxy(proxies);
		}

		if (interface != NULL) CFRelease(interface);
		if (server != NULL) CFRelease(server);
		CFRelease(proxies);
	} else {
		SCPrint(TRUE, stdout, CFSTR("No proxy configuration available\n"));
	}

	exit(0);
}


__private_extern__
void
do_renew(char *if_name)
{
	CFArrayRef	interfaces;
	CFStringRef	match_name;
	CFArrayRef	services;
	Boolean		ok	= FALSE;

	if ((if_name == NULL) || (strlen(if_name) == 0)) {
		SCPrint(TRUE, stderr, CFSTR("No interface name\n"));
		exit(1);
	}

	match_name = CFStringCreateWithCString(NULL, if_name, kCFStringEncodingUTF8);
	if (match_name == NULL) {
		SCPrint(TRUE, stderr, CFSTR("No interface\n"));
		exit(1);
	}

	/*
	 * NextBSD: Apple keeps this SCNetworkInterfaceCopyAll path behind
	 * ATTEMPT_DHCP_RENEW_WITH_SCDYNAMICSTORE. Here it always runs: an
	 * interface DHCP'd from the link watch need not have a network
	 * service, and the prefs path below would report "No interface".
	 */
	interfaces = SCNetworkInterfaceCopyAll();
	if (interfaces != NULL) {
		CFIndex		i;
		CFIndex		n;

		n = CFArrayGetCount(interfaces);
		for (i = 0; i < n; i++) {
			CFStringRef		bsd_name;
			SCNetworkInterfaceRef	interface;

			interface = CFArrayGetValueAtIndex(interfaces, i);
			bsd_name = SCNetworkInterfaceGetBSDName(interface);
			if ((bsd_name != NULL) && CFEqual(bsd_name, match_name)) {
				// if match
				ok = SCNetworkInterfaceForceConfigurationRefresh(interface);
				if (!ok) {
					int	status;

					status = SCError();
					if (status != kSCStatusAccessError) {
						SCPrint(TRUE, stderr, CFSTR("%s\n"), SCErrorString(status));
						exit(1);
					}

					// ... and if can't write the SCDynamicStore, try w/prefs
				}

				break;
			}
		}

		CFRelease(interfaces);
	}

	if (ok) {
		exit(0);
	}

	do_prefs_init();	/* initialization */
	do_prefs_open(0, NULL);	/* open default prefs */

	services = SCNetworkServiceCopyAll(prefs);
	if (services != NULL) {
		CFIndex		i;
		CFIndex		n;

		n = CFArrayGetCount(services);
		for (i = 0; i < n; i++) {
			CFStringRef		bsd_name;
			SCNetworkInterfaceRef	interface;
			SCNetworkServiceRef	service;

			service = CFArrayGetValueAtIndex(services, i);
			interface = SCNetworkServiceGetInterface(service);
			if (interface == NULL) {
				// if no interface
				continue;
			}

			bsd_name = SCNetworkInterfaceGetBSDName(interface);
			if ((bsd_name != NULL) && CFEqual(bsd_name, match_name)) {
				// if match
				ok = SCNetworkInterfaceForceConfigurationRefresh(interface);
				if (!ok) {
					SCPrint(TRUE, stderr, CFSTR("%s\n"), SCErrorString(SCError()));
					exit(1);
				}

				break;
			}
		}

		CFRelease(services);
	}

	CFRelease(match_name);

	if (!ok) {
		SCPrint(TRUE, stderr, CFSTR("No interface\n"));
		exit(1);
	}

	_prefs_close();
	exit(0);
}


static void __attribute__((noreturn))
waitKeyFound(SCDynamicStoreRef session, CFArrayRef changes,
	     void * info)
{
#pragma unused(session, changes, info)
	/*
	 * NextBSD: exit as soon as the watched key changes, as configd-1109
	 * and earlier did. configd-1405's bare "return" leaves CFRunLoopRun
	 * spinning until the -t timeout, so a key that appears late would
	 * report failure.
	 */
	exit(0);
}

static void __attribute__((noreturn))
waitTimeout(int sigraised)
{
#pragma unused(sigraised)
	exit(1);
}

__private_extern__
void
do_wait(char *waitKey, int timeout)
{
	struct itimerval	itv;
	CFStringRef		key;
	CFMutableArrayRef	keys;
	Boolean			ok;

	store = SCDynamicStoreCreate(NULL, CFSTR("scutil (wait)"), (SCDynamicStoreCallBack)waitKeyFound, NULL);
	if (store == NULL) {
		SCPrint(TRUE, stderr,
			CFSTR("SCDynamicStoreCreate() failed: %s\n"), SCErrorString(SCError()));
		exit(1);
	}

	key  = CFStringCreateWithCString(NULL, waitKey, kCFStringEncodingUTF8);

	keys = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	CFArrayAppendValue(keys, key);
	ok = SCDynamicStoreSetNotificationKeys(store, keys, NULL);
	CFRelease(keys);
	if (!ok) {
		SCPrint(TRUE, stderr,
			CFSTR("SCDynamicStoreSetNotificationKeys() failed: %s\n"), SCErrorString(SCError()));
		exit(1);
	}

	notifyRls = SCDynamicStoreCreateRunLoopSource(NULL, store, 0);
	if (!notifyRls) {
		SCPrint(TRUE, stderr,
			CFSTR("SCDynamicStoreCreateRunLoopSource() failed: %s\n"), SCErrorString(SCError()));
		exit(1);
	}

	CFRunLoopAddSource(CFRunLoopGetCurrent(), notifyRls, kCFRunLoopDefaultMode);

	value = SCDynamicStoreCopyValue(store, key);
	CFRelease(key);
	if (value) {
		/* if the key is already present */
		return;
	}

	if (timeout > 0) {
		signal(SIGALRM, waitTimeout);
		memset(&itv, 0, sizeof(itv));
		itv.it_value.tv_sec = timeout;
		if (setitimer(ITIMER_REAL, &itv, NULL) == -1) {
			SCPrint(TRUE, stderr,
				CFSTR("setitimer() failed: %s\n"), strerror(errno));
			exit(1);
		}
	}

	CFRunLoopRun();
}
