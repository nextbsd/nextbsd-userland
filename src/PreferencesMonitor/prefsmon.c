/*
 * Copyright (c) 2000-2008, 2010, 2012-2013 Apple Inc. All rights reserved.
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
 * prefsmon.c — org.nextbsd.PreferencesMonitor (#480).
 *
 * On Darwin these jobs are spread over configd plugins and an agent:
 *
 *   PreferencesMonitor  the first-boot "Automatic" set, and the flatten of
 *                       the current set into Setup: on every Apply
 *                       (configd-596.15 Plugins/PreferencesMonitor/prefsmon.c)
 *   SCMonitor           a default service for a new interface; here always
 *                       its "Configure" action, there being no GUI session
 *                       to prompt (SCMonitor/monitor.c)
 *   InterfaceNamer      NetworkInterfaces.plist, the interfaces this
 *                       machine has seen (Plugins/InterfaceNamer/ifnamer.c)
 *
 * NextBSD's configd has no plugin loader, so they live in this small daemon
 * instead (maintainer decision 2026-11-08).
 *
 * Only the current set's Network/ tree is flattened. Setup:/System and
 * Setup:/Network/HostNames belong to hostnamed (prefs_monitor.c), and this
 * daemon must not remove them, so unlike Darwin's flatten it does not own
 * every Setup: key.
 *
 * Only Ethernet and Wi-Fi interfaces get a service automatically. Bond,
 * Bridge and VLAN interfaces, and every other kind of virtual interface,
 * are configured by hand.
 *
 * A read-only root is normal on live media: a commit that fails with EROFS
 * still applies, as on Darwin, and the session then keeps its uncommitted
 * preferences rather than re-reading the file.
 */

#include <sys/types.h>

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCNetworkConfigurationPrivate.h>
#include <SystemConfiguration/SCSchemaDefinitions.h>
#include <dispatch/dispatch.h>

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The SCSchema names this file uses and the installed headers do not carry. */
#define	kPMPrefCurrentSet		CFSTR("CurrentSet")
#define	kPMResvLink			CFSTR("__LINK__")
#define	kPMResvInactive			CFSTR("__INACTIVE__")
#define	kPMPropSetupCurrentSet		CFSTR("CurrentSet")
#define	kPMPropSetupLastUpdated		CFSTR("LastUpdated")

/* NetworkInterfaces.plist (ifnamer.c's NETWORKINTERFACES_PREFS and keys). */
#define	kPMInterfacesPrefs		CFSTR("NetworkInterfaces.plist")
#define	kPMInterfaces			CFSTR("Interfaces")
#define	kPMInterfaceBSDName		CFSTR("BSD Name")
#define	kPMInterfaceType		CFSTR("SCNetworkInterfaceType")
#define	kPMInterfaceMAC			CFSTR("MACAddress")

#define	PM_ID				CFSTR("org.nextbsd.PreferencesMonitor")
#define	PM_LINK_PATTERN			"State:/Network/Interface/[^/]+/Link"
#define	PM_STORE_RETRIES		60

static SCPreferencesRef		g_prefs;
static SCPreferencesRef		g_ifprefs;
static SCDynamicStoreRef	g_store;
static CFMutableDictionaryRef	g_known;	/* BSD name -> entry */
static bool			g_rofs;		/* keep uncommitted prefs */

/* One Setup: update's working state: prefsmon.c's file-scope globals. */
static CFMutableDictionaryRef	g_newPrefs;

static void
xlog(const char *fmt, ...)
{
	struct timespec ts;
	struct tm tm;
	char tbuf[32];
	va_list ap;

	(void)clock_gettime(CLOCK_REALTIME, &ts);
	(void)gmtime_r(&ts.tv_sec, &tm);
	(void)strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tm);
	(void)fprintf(stderr, "PreferencesMonitor %s ", tbuf);
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/* A CFString for logging: `buf` holds its UTF-8 form, or "?". */
static const char *
cf_cstr(CFStringRef s, char *buf, size_t len)
{
	if (s == NULL || !CFStringGetCString(s, buf, (CFIndex)len,
	    kCFStringEncodingUTF8))
		(void)strlcpy(buf, "?", len);
	return (buf);
}

#pragma mark -
#pragma mark Known interfaces (NetworkInterfaces.plist)

static void
known_load(void)
{
	CFArrayRef list;
	CFIndex i, n;

	g_known = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	list = SCPreferencesGetValue(g_ifprefs, kPMInterfaces);
	if (list == NULL || CFGetTypeID(list) != CFArrayGetTypeID())
		return;
	n = CFArrayGetCount(list);
	for (i = 0; i < n; i++) {
		CFDictionaryRef e = CFArrayGetValueAtIndex(list, i);
		CFStringRef name;

		if (CFGetTypeID(e) != CFDictionaryGetTypeID())
			continue;
		name = CFDictionaryGetValue(e, kPMInterfaceBSDName);
		if (name != NULL && CFGetTypeID(name) == CFStringGetTypeID())
			CFDictionarySetValue(g_known, name, e);
	}
}

/*
 * Has this machine seen this interface? Darwin identifies an interface by
 * its hardware, so a different NIC that happens to take a known name is a
 * new interface. Without a MAC on one side the name alone decides.
 */
static bool
known_contains(CFStringRef name, CFStringRef mac)
{
	CFDictionaryRef e = CFDictionaryGetValue(g_known, name);
	CFStringRef known_mac;

	if (e == NULL)
		return (false);
	known_mac = CFDictionaryGetValue(e, kPMInterfaceMAC);
	return (known_mac == NULL || mac == NULL || CFEqual(known_mac, mac));
}

static void
known_add(CFStringRef name, CFStringRef type, CFStringRef mac)
{
	CFMutableDictionaryRef e;

	e = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(e, kPMInterfaceBSDName, name);
	CFDictionarySetValue(e, kPMInterfaceType, type);
	if (mac != NULL)
		CFDictionarySetValue(e, kPMInterfaceMAC, mac);
	CFDictionarySetValue(g_known, name, e);
	CFRelease(e);
}

/* Write the list out. The in-memory copy stays good on a read-only root. */
static void
known_save(void)
{
	CFIndex n = CFDictionaryGetCount(g_known);
	const void **vals;
	CFArrayRef list;

	vals = calloc((size_t)(n > 0 ? n : 1), sizeof(*vals));
	if (vals == NULL)
		return;
	CFDictionaryGetKeysAndValues(g_known, NULL, vals);
	list = CFArrayCreate(NULL, vals, n, &kCFTypeArrayCallBacks);
	free(vals);
	if (list == NULL)
		return;
	(void)SCPreferencesSetValue(g_ifprefs, kPMInterfaces, list);
	CFRelease(list);
	if (!SCPreferencesCommitChanges(g_ifprefs) && SCError() != EROFS)
		xlog("could not save the known interfaces: %s",
		    SCErrorString(SCError()));
}

#pragma mark -
#pragma mark Default services

/* An interface that gets a service without being asked: wired or Wi-Fi. */
static bool
is_auto_configured(SCNetworkInterfaceRef interface)
{
	CFStringRef type = SCNetworkInterfaceGetInterfaceType(interface);

	return (type != NULL &&
	    (CFEqual(type, kSCNetworkInterfaceTypeEthernet) ||
	    CFEqual(type, kSCNetworkInterfaceTypeIEEE80211)));
}

/*
 * Create or complete the network configuration: establishNewPreferences()
 * for a machine with no current set, updateInterfaceList() for an interface
 * this machine has not seen. An interface whose service was deleted stays
 * known and so stays without one. `first_boot` covers every present
 * interface, known or not, as Darwin's does.
 *
 * The first time this runs with no NetworkInterfaces.plist, every present
 * interface is new and gets a default service if it has none; a service it
 * already has, and everything under /System, is left alone. That is the
 * upgrade path for machines that predate this daemon.
 *
 * A preferences change posts Apply, which is also what reaches
 * prefs_changed() and refreshes Setup:.
 */
static void
establish_configuration(void)
{
	SCNetworkSetRef set;
	CFArrayRef interfaces;
	CFIndex i, n;
	bool first_boot = false, known_dirty = false, updated = false;
	char ifname[64];

	if (!SCPreferencesLock(g_prefs, TRUE)) {
		xlog("could not acquire network configuration lock: %s",
		    SCErrorString(SCError()));
		return;
	}
	if (!g_rofs)
		SCPreferencesSynchronize(g_prefs);

	set = SCNetworkSetCopyCurrent(g_prefs);
	if (set == NULL) {
		first_boot = true;
		set = SCNetworkSetCreate(g_prefs);
		if (set == NULL ||
		    !SCNetworkSetSetName(set, CFSTR("Automatic")) ||
		    !SCNetworkSetSetCurrent(set)) {
			xlog("could not create the Automatic set: %s",
			    SCErrorString(SCError()));
			goto done;
		}
	}

	interfaces = SCNetworkInterfaceCopyAll();
	n = (interfaces != NULL) ? CFArrayGetCount(interfaces) : 0;
	for (i = 0; i < n; i++) {
		SCNetworkInterfaceRef interface;
		CFStringRef name, mac;

		interface = CFArrayGetValueAtIndex(interfaces, i);
		name = SCNetworkInterfaceGetBSDName(interface);
		if (name == NULL || !is_auto_configured(interface))
			continue;
		mac = SCNetworkInterfaceGetHardwareAddressString(interface);
		if (known_contains(name, mac) && !first_boot)
			continue;

		known_add(name, SCNetworkInterfaceGetInterfaceType(interface),
		    mac);
		known_dirty = true;
		if (SCNetworkSetEstablishDefaultInterfaceConfiguration(set,
		    interface)) {
			updated = true;
			xlog("%s: new interface, added a default service",
			    cf_cstr(name, ifname, sizeof(ifname)));
		} else if (SCError() != kSCStatusOK) {
			xlog("%s: could not add a default service: %s",
			    cf_cstr(name, ifname, sizeof(ifname)),
			    SCErrorString(SCError()));
		}
	}
	if (interfaces != NULL)
		CFRelease(interfaces);

	if (first_boot || updated) {
		if (SCPreferencesCommitChanges(g_prefs)) {
			xlog("new network configuration saved");
		} else if (SCError() == EROFS) {
			/* A read-only file system is OK (prefsmon.c). */
			xlog("read-only file system: network configuration "
			    "is for this boot only");
			g_rofs = true;
		} else {
			xlog("could not save the network configuration: %s",
			    SCErrorString(SCError()));
		}
		/* Committed or temporary/read-only changes alike. */
		(void)SCPreferencesApplyChanges(g_prefs);
	}

    done:
	if (known_dirty)
		known_save();
	(void)SCPreferencesUnlock(g_prefs);
	if (set != NULL)
		CFRelease(set);
}

#pragma mark -
#pragma mark Setup: (the current set, flattened)

/* prefsmon.c's flatten(), writing into g_newPrefs. */
static void
flatten(CFStringRef key, CFDictionaryRef base)
{
	CFDictionaryRef subset;
	CFStringRef link;
	CFMutableDictionaryRef myDict;
	CFStringRef myKey;
	CFIndex i, nKeys;
	const void **keys, **vals;

	if (!CFDictionaryGetValueIfPresent(base, kPMResvLink,
	    (const void **)&link)) {
		/* not linked */
		subset = base;
	} else {
		subset = SCPreferencesPathGetValue(g_prefs, link);
		if (subset == NULL)
			return;
	}

	if (CFDictionaryContainsKey(subset, kPMResvInactive))
		return;

	myKey = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@%@"),
	    kSCDynamicStoreDomainSetup, key);

	myDict = (CFMutableDictionaryRef)CFDictionaryGetValue(g_newPrefs,
	    myKey);
	if (myDict != NULL)
		myDict = CFDictionaryCreateMutableCopy(NULL, 0, myDict);
	else
		myDict = CFDictionaryCreateMutable(NULL, 0,
		    &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);

	nKeys = CFDictionaryGetCount(subset);
	if (nKeys > 0) {
		keys = calloc((size_t)nKeys, sizeof(*keys));
		vals = calloc((size_t)nKeys, sizeof(*vals));
		CFDictionaryGetKeysAndValues(subset, keys, vals);
		for (i = 0; i < nKeys; i++) {
			if (CFGetTypeID(vals[i]) != CFDictionaryGetTypeID()) {
				CFDictionarySetValue(myDict, keys[i], vals[i]);
			} else {
				CFStringRef subKey;

				subKey = CFStringCreateWithFormat(NULL, NULL,
				    CFSTR("%@/%@"), key, keys[i]);
				flatten(subKey, vals[i]);
				CFRelease(subKey);
			}
		}
		free(keys);
		free(vals);
	}

	if (CFDictionaryGetCount(myDict) > 0)
		CFDictionarySetValue(g_newPrefs, myKey, myDict);

	CFRelease(myDict);
	CFRelease(myKey);
}

/* The Setup: keys this daemon owns: everything else is hostnamed's. */
static CFDictionaryRef
copy_current_setup(void)
{
	const void *pats[3];
	CFArrayRef patterns;
	CFDictionaryRef current;

	pats[0] = CFSTR("Setup:/Network/Service/.*");
	pats[1] = CFSTR("Setup:/Network/Global/.*");
	pats[2] = CFSTR("Setup:/Network/Interface/.*");
	patterns = CFArrayCreate(NULL, pats, 3, &kCFTypeArrayCallBacks);
	current = SCDynamicStoreCopyMultiple(g_store, NULL, patterns);
	CFRelease(patterns);
	return (current);
}

/*
 * updateSCDynamicStore() for the current set's Network/ tree: flatten it,
 * publish what changed, and remove the keys that are gone.
 */
static void
update_setup(void)
{
	CFDictionaryRef current, set, network;
	CFStringRef currentSet;
	CFMutableArrayRef remove;
	CFMutableDictionaryRef status;
	CFDateRef date;
	CFIndex i, n;
	const void **keys;

	g_newPrefs = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	status = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	currentSet = SCPreferencesGetValue(g_prefs, kPMPrefCurrentSet);
	if (currentSet != NULL &&
	    CFGetTypeID(currentSet) == CFStringGetTypeID()) {
		CFDictionarySetValue(status, kPMPropSetupCurrentSet, currentSet);
		set = SCPreferencesPathGetValue(g_prefs, currentSet);
		network = (set != NULL &&
		    CFGetTypeID(set) == CFDictionaryGetTypeID())
		    ? CFDictionaryGetValue(set, kSCCompNetwork) : NULL;
		if (network != NULL &&
		    CFGetTypeID(network) == CFDictionaryGetTypeID())
			flatten(CFSTR("/Network"), network);
	}

	/*
	 * Setup: says the configuration has been read: a reader can tell
	 * "no services" from "not there yet". Darwin's key, CurrentSet and
	 * LastUpdated.
	 */
	date = CFDateCreate(NULL, CFAbsoluteTimeGetCurrent());
	CFDictionarySetValue(status, kPMPropSetupLastUpdated, date);
	CFRelease(date);
	CFDictionarySetValue(g_newPrefs, kSCDynamicStoreDomainSetup, status);
	CFRelease(status);

	remove = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	current = copy_current_setup();
	n = (current != NULL) ? CFDictionaryGetCount(current) : 0;
	keys = calloc((size_t)(n > 0 ? n : 1), sizeof(*keys));
	if (n > 0)
		CFDictionaryGetKeysAndValues(current, keys, NULL);
	for (i = 0; i < n; i++) {
		CFTypeRef old = CFDictionaryGetValue(current, keys[i]);
		CFTypeRef new = CFDictionaryGetValue(g_newPrefs, keys[i]);

		if (new == NULL)
			CFArrayAppendValue(remove, keys[i]);
		else if (CFEqual(old, new))
			/* Unchanged: do not wake the store's watchers. */
			CFDictionaryRemoveValue(g_newPrefs, keys[i]);
	}
	free(keys);

	if (!SCDynamicStoreSetMultiple(g_store, g_newPrefs, remove, NULL))
		xlog("SCDynamicStoreSetMultiple failed: %s",
		    SCErrorString(SCError()));

	if (current != NULL)
		CFRelease(current);
	CFRelease(remove);
	CFRelease(g_newPrefs);
	g_newPrefs = NULL;
}

/* updateConfiguration(). Only an Apply changes the running configuration. */
static void
prefs_changed(SCPreferencesRef prefs, SCPreferencesNotification type,
    void *info)
{
	(void)prefs;
	(void)info;
	if ((type & kSCPreferencesNotificationApply) == 0)
		return;
	/* An Apply from anyone else carries the committed file. */
	if (!g_rofs)
		SCPreferencesSynchronize(g_prefs);
	update_setup();
}

#pragma mark -
#pragma mark Interface arrival

/*
 * A Link key changed: configd's KernelEventMonitor publishes one for every
 * interface, so a new one means a new interface. Darwin's SCMonitor reacts
 * to the same store change.
 */
static void
link_changed(SCDynamicStoreRef store, CFArrayRef changedKeys, void *info)
{
	CFIndex i, n;

	(void)store;
	(void)info;
	n = CFArrayGetCount(changedKeys);
	for (i = 0; i < n; i++) {
		CFStringRef key = CFArrayGetValueAtIndex(changedKeys, i);
		CFArrayRef parts;
		bool unknown = false;

		parts = CFStringCreateArrayBySeparatingStrings(NULL, key,
		    CFSTR("/"));
		if (parts != NULL && CFArrayGetCount(parts) >= 4) {
			/* State:, Network, Interface, <if>, Link */
			unknown = CFDictionaryGetValue(g_known,
			    CFArrayGetValueAtIndex(parts, 3)) == NULL;
		}
		if (parts != NULL)
			CFRelease(parts);
		if (unknown) {
			establish_configuration();
			return;
		}
	}
}

static void
prime(void *ctx)
{
	(void)ctx;
	establish_configuration();
	update_setup();
}

static void
on_sigterm(void *ctx)
{
	(void)ctx;
	xlog("exiting on signal");
	exit(0);
}

int
main(int argc, char **argv)
{
	dispatch_queue_t queue;
	dispatch_source_t term_src, int_src;
	CFArrayRef patterns;
	const void *pat;
	int tries;

	(void)argc;
	(void)argv;

	xlog("starting");

	/* Before the dispatch sources, so the default action cannot race. */
	(void)signal(SIGTERM, SIG_IGN);
	(void)signal(SIGINT, SIG_IGN);

	queue = dispatch_queue_create("org.nextbsd.PreferencesMonitor", NULL);

	/* configd and this daemon are both RunAtLoad, with no ordering. */
	for (tries = 0; tries < PM_STORE_RETRIES; tries++) {
		g_store = SCDynamicStoreCreate(NULL, PM_ID, link_changed, NULL);
		if (g_store != NULL)
			break;
		(void)sleep(1);
	}
	if (g_store == NULL) {
		xlog("no configd session: %s", SCErrorString(SCError()));
		return (1);
	}

	g_prefs = SCPreferencesCreate(NULL, PM_ID, NULL);
	g_ifprefs = SCPreferencesCreate(NULL, PM_ID, kPMInterfacesPrefs);
	if (g_prefs == NULL || g_ifprefs == NULL) {
		xlog("SCPreferencesCreate failed: %s",
		    SCErrorString(SCError()));
		return (1);
	}
	known_load();

	if (!SCPreferencesSetCallback(g_prefs, prefs_changed, NULL) ||
	    !SCPreferencesSetDispatchQueue(g_prefs, queue)) {
		xlog("cannot watch the network preferences: %s",
		    SCErrorString(SCError()));
		return (1);
	}

	pat = CFSTR(PM_LINK_PATTERN);
	patterns = CFArrayCreate(NULL, &pat, 1, &kCFTypeArrayCallBacks);
	if (!SCDynamicStoreSetNotificationKeys(g_store, NULL, patterns) ||
	    !SCDynamicStoreSetDispatchQueue(g_store, queue)) {
		xlog("cannot watch for new interfaces: %s",
		    SCErrorString(SCError()));
		return (1);
	}
	CFRelease(patterns);

	/* Everything from here on happens on the one serial queue. */
	dispatch_async_f(queue, NULL, prime);

	term_src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM,
	    0, queue);
	int_src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0,
	    queue);
	dispatch_source_set_event_handler_f(term_src, on_sigterm);
	dispatch_source_set_event_handler_f(int_src, on_sigterm);
	dispatch_resume(term_src);
	dispatch_resume(int_src);

	dispatch_main();
}
