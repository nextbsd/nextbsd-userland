/*
 * hostnamedhcpset — CI fixture helper for hostnamed iter 3a / 3b.
 *
 * Usage:  hostnamedhcpset <option-12-value>
 *         hostnamedhcpset --hold <option-12-value>
 *         hostnamedhcpset --clear
 *
 * With a value: finds the State:/Network/Service/<UUID>/DHCP dict that
 * ipconfigd already published (issue #88), adds Option_12=<argv[1]> to
 * it, and writes the augmented dict back to SCDynamicStore. The next
 * hostnamed run will read that value via its DHCP tier (try_dhcp) and
 * use it, proving Tier-3a fires.
 *
 * With --hold: writes the value the same way, then stays running and
 * puts it back whenever ipconfigd republishes the dict without it, until
 * killed. ipconfigd rewrites /DHCP from the lease on every renewal, and
 * CI caps the lease at 10 s, so a one-shot write is gone within about
 * 5 s. A real DHCP server that assigns a name sends it in every ACK;
 * holding the fixture models that. Prints DHCPSET-HOLDING once the first
 * write is in (nextbsd-userland#621).
 *
 * With --clear: enumerates all matching /DHCP dicts and removes
 * Option_12 from each, so a previously-injected fixture does not
 * short-circuit later tiers (iter 3b's ROUND 4 needs this to exercise
 * the mDNS path with the DHCP tier guaranteed empty).
 *
 * Why we modify ipconfigd's existing /DHCP key rather than minting our
 * own: the live key is already shaped correctly (InterfaceName +
 * LeaseStartTime) and has a real UUID derived from the active NIC MAC.
 * Synthesizing one would duplicate sc_publish.c's UUID logic; piggy-
 * backing keeps the fixture small and the test against a real surface.
 *
 * Not shipped to real images (CI-only tool); lives next to
 * hostnametest + hostnameprefset under /usr/tests/freebsd-launchd-mach/.
 *
 * Issue: #90
 */

#include <SystemConfiguration/SCDynamicStore.h>
#include <CoreFoundation/CoreFoundation.h>

#include <dispatch/dispatch.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Clear Option_12 from every matching /DHCP dict. Returns 0 on success
 * (even when no dicts contained Option_12 — the empty-key case is fine
 * for run.sh which calls --clear unconditionally). Returns 1 on SCDS
 * error. */
static int
clear_all_option12(SCDynamicStoreRef store, CFStringRef pattern,
    CFStringRef k_opt12)
{
	CFArrayRef keys;
	CFIndex i, n;
	int cleared = 0;

	keys = SCDynamicStoreCopyKeyList(store, pattern);
	if (keys == NULL)
		return (0);	/* no /DHCP keys at all — nothing to clear */
	n = CFArrayGetCount(keys);
	for (i = 0; i < n; i++) {
		CFStringRef key;
		CFPropertyListRef plist;
		CFMutableDictionaryRef dict;

		key = (CFStringRef)CFArrayGetValueAtIndex(keys, i);
		if (key == NULL)
			continue;
		plist = SCDynamicStoreCopyValue(store, key);
		if (plist == NULL)
			continue;
		if (CFGetTypeID(plist) != CFDictionaryGetTypeID() ||
		    !CFDictionaryContainsKey((CFDictionaryRef)plist, k_opt12)) {
			CFRelease(plist);
			continue;
		}
		dict = CFDictionaryCreateMutableCopy(NULL, 0,
		    (CFDictionaryRef)plist);
		CFRelease(plist);
		if (dict == NULL)
			continue;
		CFDictionaryRemoveValue(dict, k_opt12);
		if (SCDynamicStoreSetValue(store, key, dict)) {
			char keybuf[256];
			if (CFStringGetCString(key, keybuf, sizeof(keybuf),
			    kCFStringEncodingUTF8))
				(void)printf("hostnamedhcpset: cleared "
				    "Option_12 from %s\n", keybuf);
			cleared++;
		}
		CFRelease(dict);
	}
	CFRelease(keys);
	if (cleared == 0)
		(void)printf("hostnamedhcpset: no /DHCP dict carried "
		    "Option_12 (already clear)\n");
	return (0);
}

/* --hold state: the key and value to keep in place. */
static CFStringRef	g_hold_key;
static CFStringRef	g_hold_value;
static CFStringRef	g_hold_opt12;

static void
hold_changed(SCDynamicStoreRef store, CFArrayRef changed, void *info)
{
	CFPropertyListRef plist;
	CFMutableDictionaryRef dict;
	CFTypeRef cur;

	(void)changed;
	(void)info;
	plist = SCDynamicStoreCopyValue(store, g_hold_key);
	if (plist == NULL)
		return;	/* key removed (interface down): nothing to hold */
	if (CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		CFRelease(plist);
		return;
	}
	cur = CFDictionaryGetValue((CFDictionaryRef)plist, g_hold_opt12);
	if (cur != NULL && CFEqual(cur, g_hold_value)) {
		CFRelease(plist);
		return;	/* still in place (or our own write echoing back) */
	}
	dict = CFDictionaryCreateMutableCopy(NULL, 0, (CFDictionaryRef)plist);
	CFRelease(plist);
	if (dict == NULL)
		return;
	CFDictionarySetValue(dict, g_hold_opt12, g_hold_value);
	if (SCDynamicStoreSetValue(store, g_hold_key, dict))
		(void)printf("hostnamedhcpset: re-applied Option_12 after a "
		    "/DHCP republish\n");
	else
		(void)fprintf(stderr, "hostnamedhcpset: re-apply failed: %s\n",
		    SCErrorString(SCError()));
	(void)fflush(stdout);
	CFRelease(dict);
}

static void
hold_check_now(void *ctx)
{
	hold_changed((SCDynamicStoreRef)ctx, NULL, NULL);
}

/* Keep Option_12 in g_hold_key until killed. Does not return on success. */
static int
hold_option12(CFStringRef key, CFStringRef value, CFStringRef k_opt12)
{
	SCDynamicStoreRef store;
	CFArrayRef keys;
	dispatch_queue_t q;

	g_hold_key = CFRetain(key);
	g_hold_value = CFRetain(value);
	g_hold_opt12 = CFRetain(k_opt12);
	store = SCDynamicStoreCreate(NULL, CFSTR("hostnamedhcpset-hold"),
	    hold_changed, NULL);
	if (store == NULL) {
		(void)fprintf(stderr, "hold: SCDynamicStoreCreate failed: %s\n",
		    SCErrorString(SCError()));
		return (1);
	}
	keys = CFArrayCreate(NULL, (const void **)&g_hold_key, 1,
	    &kCFTypeArrayCallBacks);
	q = dispatch_queue_create("hostnamedhcpset.hold", NULL);
	if (keys == NULL || q == NULL ||
	    !SCDynamicStoreSetNotificationKeys(store, keys, NULL) ||
	    !SCDynamicStoreSetDispatchQueue(store, q)) {
		(void)fprintf(stderr, "hold: cannot watch the /DHCP key: %s\n",
		    SCErrorString(SCError()));
		return (1);
	}
	CFRelease(keys);
	/* A republish between the first write and the watch going live
	 * would otherwise go unnoticed. */
	dispatch_sync_f(q, (void *)(uintptr_t)store, hold_check_now);
	(void)printf("DHCPSET-HOLDING\n");
	(void)fflush(stdout);
	dispatch_main();
	/* NOTREACHED */
}

int
main(int argc, char **argv)
{
	SCDynamicStoreRef store = NULL;
	CFStringRef session = NULL, pattern = NULL, k_opt12 = NULL;
	CFStringRef value = NULL, key = NULL;
	CFArrayRef keys = NULL;
	CFPropertyListRef plist = NULL;
	CFMutableDictionaryRef dict = NULL;
	CFIndex n;
	const char *arg;
	int clear_mode = 0;
	int hold_mode = 0;
	int rc = 1;

	if (argc == 3 && strcmp(argv[1], "--hold") == 0) {
		hold_mode = 1;
		arg = argv[2];
	} else if (argc == 2) {
		arg = argv[1];
	} else {
		arg = "";
	}
	if (arg[0] == '\0' || (hold_mode && strcmp(arg, "--clear") == 0)) {
		(void)fprintf(stderr,
		    "usage: %s <option-12-value>\n"
		    "       %s --hold <option-12-value>\n"
		    "       %s --clear\n", argv[0], argv[0], argv[0]);
		return (2);
	}
	if (strcmp(arg, "--clear") == 0)
		clear_mode = 1;

	session = CFStringCreateWithCString(NULL, "hostnamedhcpset",
	    kCFStringEncodingUTF8);
	pattern = CFStringCreateWithCString(NULL,
	    "State:/Network/Service/[^/]+/DHCP", kCFStringEncodingUTF8);
	k_opt12 = CFStringCreateWithCString(NULL, "Option_12",
	    kCFStringEncodingUTF8);
	if (!clear_mode)
		value = CFStringCreateWithCString(NULL, arg,
		    kCFStringEncodingUTF8);
	if (session == NULL || pattern == NULL || k_opt12 == NULL ||
	    (!clear_mode && value == NULL)) {
		(void)fprintf(stderr, "CFString allocation failed\n");
		goto out;
	}

	store = SCDynamicStoreCreate(NULL, session, NULL, NULL);
	if (store == NULL) {
		(void)fprintf(stderr, "SCDynamicStoreCreate failed: %s\n",
		    SCErrorString(SCError()));
		goto out;
	}

	if (clear_mode) {
		rc = clear_all_option12(store, pattern, k_opt12);
		goto out;
	}

	keys = SCDynamicStoreCopyKeyList(store, pattern);
	if (keys == NULL || (n = CFArrayGetCount(keys)) == 0) {
		(void)fprintf(stderr,
		    "no State:/Network/Service/<UUID>/DHCP key found "
		    "(is ipconfigd running and BOUND?)\n");
		goto out;
	}

	/* Use the first matching key. iter 3a doesn't yet pick the
	 * primary service — fine for a CI environment with a single
	 * em0 / virtio NIC; iter 3b will do State:/Network/Global/IPv4
	 * lookup to pick the active service. */
	key = (CFStringRef)CFArrayGetValueAtIndex(keys, 0);
	if (key == NULL) {
		(void)fprintf(stderr, "key list returned NULL entry\n");
		goto out;
	}
	CFRetain(key);

	plist = SCDynamicStoreCopyValue(store, key);
	if (plist == NULL ||
	    CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		(void)fprintf(stderr, "existing /DHCP key has no dict value\n");
		goto out;
	}

	dict = CFDictionaryCreateMutableCopy(NULL, 0,
	    (CFDictionaryRef)plist);
	if (dict == NULL) {
		(void)fprintf(stderr,
		    "CFDictionaryCreateMutableCopy failed\n");
		goto out;
	}
	CFDictionarySetValue(dict, k_opt12, value);

	if (!SCDynamicStoreSetValue(store, key, dict)) {
		(void)fprintf(stderr,
		    "SCDynamicStoreSetValue failed: %s\n",
		    SCErrorString(SCError()));
		goto out;
	}

	{
		char keybuf[256];
		if (CFStringGetCString(key, keybuf, sizeof(keybuf),
		    kCFStringEncodingUTF8)) {
			(void)printf("hostnamedhcpset: wrote Option_12='%s' "
			    "into %s\n", arg, keybuf);
		} else {
			(void)printf("hostnamedhcpset: wrote Option_12='%s'\n",
			    arg);
		}
	}
	(void)fflush(stdout);
	rc = 0;
	if (hold_mode)
		rc = hold_option12(key, value, k_opt12);
out:
	if (dict != NULL) CFRelease(dict);
	if (plist != NULL) CFRelease(plist);
	if (key != NULL) CFRelease(key);
	if (keys != NULL) CFRelease(keys);
	if (store != NULL) CFRelease(store);
	if (value != NULL) CFRelease(value);
	if (k_opt12 != NULL) CFRelease(k_opt12);
	if (pattern != NULL) CFRelease(pattern);
	if (session != NULL) CFRelease(session);
	return (rc);
}
