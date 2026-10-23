/*
 * prefs_monitor.c — PreferencesMonitor-equivalent for hostnamed.
 *
 * Real-macOS analog: PreferencesMonitor is the configd plugin that
 * watches the system network-preferences file
 * (/Local/Library/Preferences/SystemConfiguration/preferences.plist) and,
 * on every committed change, republishes the relevant subset into
 * SCDynamicStore — including Setup:/System/ComputerName and
 * Setup:/Network/HostNames. Apple's set-hostname.c (vendored under
 * src/hostnamed/vendored/) then reads those SCDS keys via
 * SCDynamicStoreKeyCreateComputerName / KeyCreateHostNames.
 *
 * Our build has no PreferencesMonitor plugin — configd is a thin
 * SCDynamicStore server, not a plugin host. We provide the same
 * SCPrefs → SCDS bridge inside hostnamed itself: ~100 LOC. On
 * startup we publish initial values (synthesized fallback when the
 * prefs file is absent / has no ComputerName); on every
 * SCPreferencesCommitChanges from another process (scutil --set, or
 * the hostnameprefset CI fixture) the SCPrefs callback re-reads the
 * prefs and re-publishes.
 *
 * The prefs file carries three names, at Apple's paths:
 *
 *   /System/System             ComputerName, HostName
 *   /System/Network/HostNames  LocalHostName
 *
 * They are read with the SCDHostName.c readers that pair with the
 * SCPreferencesSet* setters scutil writes through, so a LocalHostName
 * set on its own is honoured instead of being derived from
 * ComputerName.
 *
 * Boot ordering: prefs_monitor's initial publish runs FIRST in
 * hostnamed's main(), before load_hostname() — so by the time
 * set-hostname.c's decision engine wakes up, Setup:/System and
 * Setup:/Network/HostNames are already populated.
 */

#include "freebsd-shim/ip_plugin.h"

#include <SystemConfiguration/SCDynamicStore.h>
#include <SystemConfiguration/SCPreferences.h>
#include <SystemConfiguration/SCPreferencesGetSpecificPrivate.h>
#include <SystemConfiguration/SCSchemaDefinitions.h>

#include <CoreFoundation/CoreFoundation.h>

#include <dispatch/dispatch.h>

#include <stdio.h>
#include <string.h>

/* hostnamed.c's logger. */
extern void	xlog(const char *fmt, ...);

/* Internal state — singleton per process. */
struct prefs_monitor_state {
	SCPreferencesRef	prefs;
	SCDynamicStoreRef	store;
	dispatch_queue_t	queue;
};
static struct prefs_monitor_state g_state;

/* The names the SCPrefs file carries; each NULL when unset. All
 * retained — release with release_prefs_names(). */
struct prefs_names {
	CFStringRef	computer;	/* /System/System ComputerName */
	CFStringRef	host;		/* /System/System HostName */
	CFStringRef	local;		/* /System/Network/HostNames LocalHostName */
};

static void
read_prefs_names(SCPreferencesRef prefs, struct prefs_names *names)
{
	names->computer = _SCPreferencesCopyComputerName(prefs, NULL);
	names->host = SCPreferencesGetHostName(prefs);
	if (names->host != NULL)
		CFRetain(names->host);
	names->local = _SCPreferencesCopyLocalHostName(prefs);
}

static void
release_prefs_names(struct prefs_names *names)
{
	if (names->computer != NULL) CFRelease(names->computer);
	if (names->host != NULL) CFRelease(names->host);
	if (names->local != NULL) CFRelease(names->local);
	memset(names, 0, sizeof(*names));
}

/* The name set-hostname.c's prefs tier sees in Setup:/System HostName:
 * the HostName preference when set, else ComputerName. Not retained. */
static CFStringRef
prefs_system_hostname(const struct prefs_names *names)
{
	return (names->host != NULL ? names->host : names->computer);
}

/* Read the configured ComputerName straight from SCPrefs, without
 * requiring prefs_monitor_start() to have run yet.
 *
 * hostnamed's boot-time sethostname() used to call
 * freebsd_synthesize_hostname() unconditionally, so a machine with a
 * configured name was called by its synthesised name for the first few
 * seconds of every boot -- including in getty's banner, which is the very
 * thing that boot-time set exists to get right (#66). Reading SCPrefs here
 * costs one plist open and removes the wrong-name window entirely.
 *
 * Caller releases. NULL when unset or unreadable, in which case the caller
 * falls back to synthesis exactly as before. */
CFStringRef
prefs_monitor_copy_configured_name(void)
{
	SCPreferencesRef prefs;
	struct prefs_names names;
	CFStringRef name;

	prefs = SCPreferencesCreate(NULL,
	    CFSTR("com.apple.hostnamed.bootread"), CFSTR("preferences.plist"));
	if (prefs == NULL)
		return (NULL);
	read_prefs_names(prefs, &names);
	name = prefs_system_hostname(&names);
	if (name != NULL)
		CFRetain(name);
	release_prefs_names(&names);
	CFRelease(prefs);
	return (name);
}

/* Publish or unpublish (both names NULL) Setup:/System. Unpublish
 * lets set-hostname.c's copy_prefs_hostname return NULL so the engine
 * falls through to DHCP / PTR / mDNS / synth carry -- rather than the
 * SCPrefs tier always winning and masking those tiers.
 *
 * kSCPropSystemComputerName (user-visible) mirrors the ComputerName
 * preference; kSCPropSystemHostName mirrors the HostName preference,
 * or carries ComputerName when only that is set (the synthesised
 * slug+suffix is already DNS-safe, so no second derivation is
 * needed). ComputerNameEncoding is informational (UTF-8). */
static void
publish_setup_system(SCDynamicStoreRef store, CFStringRef computer,
    CFStringRef host)
{
	CFStringRef key;
	CFMutableDictionaryRef dict;
	CFNumberRef enc_num;
	SInt32 enc_val = (SInt32)kCFStringEncodingUTF8;

	if (store == NULL)
		return;
	key = SCDynamicStoreKeyCreateComputerName(NULL);
	if (key == NULL)
		return;
	if (host == NULL)
		host = computer;
	if (host == NULL) {
		(void)SCDynamicStoreRemoveValue(store, key);
		CFRelease(key);
		return;
	}
	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	enc_num = CFNumberCreate(NULL, kCFNumberSInt32Type, &enc_val);
	if (dict != NULL && enc_num != NULL) {
		if (computer != NULL) {
			CFDictionarySetValue(dict, kSCPropSystemComputerName,
			    computer);
			CFDictionarySetValue(dict,
			    kSCPropSystemComputerNameEncoding, enc_num);
		}
		CFDictionarySetValue(dict, kSCPropSystemHostName, host);
		if (!SCDynamicStoreSetValue(store, key, dict))
			xlog("prefs_monitor: SetValue(Setup:/System) failed");
	}
	if (enc_num != NULL) CFRelease(enc_num);
	if (dict != NULL) CFRelease(dict);
	CFRelease(key);
}

/* Publish (name non-NULL) or unpublish (name NULL) Setup:/Network/HostNames.
 * The HostNames key carries kSCPropNetHostName + kSCPropNetLocalHostName —
 * mDNSResponder reads LocalHostName for the .local broadcast (see
 * apple-oss-distributions/mDNSResponder mDNSMacOSX.c:3404
 * GetUserSpecifiedLocalHostName). When SCPrefs is empty we want this
 * key POPULATED (with the synthesised name) even though Setup:/System
 * is empty — that's the freebsd-launchd-mach split:
 *
 *   - Setup:/System mirrors SCPrefs ComputerName, so absent = empty
 *     (lets set-hostname.c's decision engine fall through to DHCP /
 *     PTR / mDNS tiers in the test rounds and on real first boots).
 *   - Setup:/Network/HostNames carries a usable .local name regardless,
 *     either the SCPrefs value when set, or the synthesised slug+suffix
 *     otherwise. mDNSResponder gets a real name to announce on a fresh
 *     machine instead of "Amnesiac".
 */
static void
publish_hostnames(SCDynamicStoreRef store, CFStringRef name)
{
	CFStringRef key;
	CFMutableDictionaryRef dict;

	if (store == NULL)
		return;
	key = SCDynamicStoreKeyCreateHostNames(NULL);
	if (key == NULL)
		return;
	if (name == NULL) {
		(void)SCDynamicStoreRemoveValue(store, key);
		CFRelease(key);
		return;
	}
	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	if (dict != NULL) {
		CFDictionarySetValue(dict, kSCPropNetHostName, name);
		CFDictionarySetValue(dict, kSCPropNetLocalHostName, name);
		if (!SCDynamicStoreSetValue(store, key, dict))
			xlog("prefs_monitor: SetValue("
			    "Setup:/Network/HostNames) failed");
		CFRelease(dict);
	}
	CFRelease(key);
}

/* Apply the current state. Setup:/System mirrors ComputerName and
 * HostName (unpublished when neither is set, so the engine falls
 * through). Setup:/Network/HostNames carries LocalHostName when set,
 * else ComputerName, else the synthesised name, so mDNSResponder has
 * a real .local broadcast name on a fresh machine. */
static void
apply(SCDynamicStoreRef store, const struct prefs_names *names)
{
	CFStringRef local;

	if (store == NULL)
		return;
	publish_setup_system(store, names->computer, names->host);
	local = (names->local != NULL) ? names->local : names->computer;
	if (local != NULL) {
		publish_hostnames(store, local);
		return;
	}
	{
		CFStringRef synth = freebsd_synthesize_hostname();
		if (synth != NULL) {
			publish_hostnames(store, synth);
			CFRelease(synth);
		} else {
			publish_hostnames(store, NULL);
		}
	}
}

/* Log what apply() is about to publish. `when` is "re-publishing" or
 * "initial publish". Only called when Setup:/System has a name. */
static void
log_publish(const char *when, const struct prefs_names *names)
{
	char buf[256], lbuf[256];
	CFStringRef name = prefs_system_hostname(names);
	CFStringRef local;

	if (name == NULL || !CFStringGetCString(name, buf, sizeof(buf),
	    kCFStringEncodingUTF8))
		return;
	local = (names->local != NULL) ? names->local : names->computer;
	if (local != NULL && CFEqual(local, name)) {
		xlog("prefs_monitor: %s '%s' "
		    "(both Setup:/System and Setup:/Network/HostNames)",
		    when, buf);
		return;
	}
	if (local == NULL)
		(void)strlcpy(lbuf, "<synthesised>", sizeof(lbuf));
	else if (!CFStringGetCString(local, lbuf, sizeof(lbuf),
	    kCFStringEncodingUTF8))
		lbuf[0] = '\0';
	xlog("prefs_monitor: %s '%s' (Setup:/System), "
	    "'%s' (Setup:/Network/HostNames)", when, buf, lbuf);
}

/* SCPrefs commit callback: re-read SCPrefs (which may have just been
 * mutated by scutil --set or hostnameprefset), re-publish to SCDS.
 * set-hostname.c's SCDS callback then fires for the Setup:/System
 * change and re-runs the decision engine. */
static void
prefs_cb(SCPreferencesRef prefs, SCPreferencesNotification type,
    void *info)
{
	struct prefs_names names;
	struct prefs_monitor_state *st = info;

	(void)type;
	if (st == NULL || st->store == NULL)
		return;
	/* Drop the cached in-memory plist so the next PathGetValue picks
	 * up the value just committed by another process (e.g. the
	 * hostnameprefset CI fixture). Without this, our SCPrefs ref
	 * stays at the snapshot loaded at SCPreferencesCreate time. */
	SCPreferencesSynchronize(prefs);
	read_prefs_names(prefs, &names);
	if (prefs_system_hostname(&names) == NULL) {
		xlog("prefs_monitor: SCPrefs ComputerName absent — "
		    "Setup:/System empty (engine falls through); "
		    "Setup:/Network/HostNames = %s",
		    names.local != NULL ? "LocalHostName" : "synthesised");
	} else
		log_publish("re-publishing", &names);
	apply(st->store, &names);
	release_prefs_names(&names);
}

/* Entry: open SCPrefs + SCDS, register the SCPrefs callback on the
 * supplied dispatch queue, do the initial publish. Returns 0 on
 * success; non-zero on a failure that prevents publishing. The
 * caller (hostnamed's main) keeps `queue` alive for the daemon's
 * lifetime. */
int
prefs_monitor_start(dispatch_queue_t queue)
{
	SCPreferencesContext pctx;
	struct prefs_names names;

	memset(&g_state, 0, sizeof(g_state));
	g_state.queue = queue;
	g_state.prefs = SCPreferencesCreate(NULL,
	    CFSTR("com.apple.hostnamed.prefs"), CFSTR("preferences.plist"));
	if (g_state.prefs == NULL) {
		xlog("prefs_monitor: SCPreferencesCreate failed");
		return (-1);
	}
	g_state.store = SCDynamicStoreCreate(NULL,
	    CFSTR("com.apple.hostnamed.publish"), NULL, NULL);
	if (g_state.store == NULL) {
		xlog("prefs_monitor: SCDynamicStoreCreate failed");
		return (-1);
	}

	/* Initial publish. SCPrefs absent → Setup:/System empty (engine
	 * falls through to DHCP / PTR / mDNS), Setup:/Network/HostNames
	 * carries the synthesised slug+suffix so mDNSResponder broadcasts
	 * a real .local name on a fresh machine. SCPrefs set → both keys
	 * carry that user value (Apple-shape mirror). */
	read_prefs_names(g_state.prefs, &names);
	if (prefs_system_hostname(&names) == NULL && names.local == NULL)
		xlog("prefs_monitor: SCPrefs ComputerName absent at boot — "
		    "Setup:/System empty (engine falls through); "
		    "Setup:/Network/HostNames will carry synthesised name "
		    "(mDNSResponder broadcasts <synth>.local)");
	else if (prefs_system_hostname(&names) == NULL)
		xlog("prefs_monitor: SCPrefs ComputerName absent at boot — "
		    "Setup:/System empty (engine falls through); "
		    "Setup:/Network/HostNames = LocalHostName");
	else
		log_publish("initial publish", &names);
	apply(g_state.store, &names);
	release_prefs_names(&names);

	/* Register the callback for ongoing changes. */
	memset(&pctx, 0, sizeof(pctx));
	pctx.info = &g_state;
	if (!SCPreferencesSetCallback(g_state.prefs, prefs_cb, &pctx)) {
		xlog("prefs_monitor: SCPreferencesSetCallback failed");
		return (-1);
	}
	if (!SCPreferencesSetDispatchQueue(g_state.prefs, queue)) {
		xlog("prefs_monitor: SCPreferencesSetDispatchQueue failed");
		return (-1);
	}
	xlog("prefs_monitor: watching /System/System and "
	    "/System/Network/HostNames via SCPrefs");
	return (0);
}
