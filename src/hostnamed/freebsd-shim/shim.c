/*
 * freebsd-launchd-mach hostnamed freebsd-shim — implementations.
 *
 * Bundles four small categories of code that the vendored Apple
 * set-hostname.c needs but our environment doesn't supply:
 *
 *   1. ip_plugin.h surface — copy_dhcp_hostname (SCDS reader),
 *      check_if_service_expensive (always FALSE),
 *      hostnamed_xlog_cf (the my_log macro target).
 *   2. SCPrivate.h SPI subset — _SC_CFStringIsValidDNSName.
 *   3. freebsd_synthesize_hostname — SMBIOS hostname synthesis
 *      (e.g. "ThinkPad-T460s", "VirtualBox-68f9a871") for the
 *      "localhost" fallback substitution and prefs_monitor's boot-time
 *      value. The algorithm lives in the header-only
 *      nextbsd_hostname_synth.h, shared with launchd's early-init
 *      (launchd_early.c) so the two can't drift (nextbsd/nextbsd#325).
 *
 * Everything is in one file so the build picks up a small additional
 * SRCS surface; the individual responsibilities are demarcated by
 * #pragma mark sections below.
 */

#include "ip_plugin.h"
#include "SCPrivate.h"
#include "SCValidation.h"

#include <SystemConfiguration/SCDynamicStore.h>
#include <SystemConfiguration/SCSchemaDefinitions.h>

#include <CoreFoundation/CoreFoundation.h>

#include "nextbsd_hostname_synth.h"

#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>

#include <ctype.h>
#include <ifaddrs.h>
#include <kenv.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

/* hostnamed's plain-C log surface (definition in hostnamed.c). */
extern void	xlog(const char *fmt, ...);

#pragma mark -
#pragma mark my_log → xlog routing

void
hostnamed_xlog_cf(int level, CFStringRef format, ...)
{
	CFStringRef formatted = NULL;
	char buf[1024];
	va_list ap;

	(void)level;	/* drop syslog severity for now */
	if (format == NULL)
		return;
	va_start(ap, format);
	formatted = CFStringCreateWithFormatAndArguments(NULL, NULL, format,
	    ap);
	va_end(ap);
	if (formatted == NULL)
		return;
	if (CFStringGetCString(formatted, buf, sizeof(buf),
	    kCFStringEncodingUTF8))
		xlog("%s", buf);
	CFRelease(formatted);
}

#pragma mark -
#pragma mark synthesis (shared with launchd early-init)

/* Public synthesis entry — used by set-hostname.c's localhost carry
 * AND by hostnamed's prefs_monitor for the boot-time fallback value
 * when SCPrefs ComputerName is absent. See nextbsd_hostname_synth.h. */
CFStringRef
freebsd_synthesize_hostname(void)
{
	char name[NBHS_NAME_MAX + 1];

	nbhs_synthesize(name, sizeof(name));
	xlog("freebsd_synthesize_hostname -> '%s'", name);
	return (CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8));
}

#pragma mark -
#pragma mark ip_plugin.h externs

CFStringRef
copy_dhcp_hostname(CFStringRef serviceID)
{
	SCDynamicStoreRef store;
	CFStringRef key, opt12 = NULL;
	CFDictionaryRef dict;

	if (serviceID == NULL)
		return (NULL);
	store = SCDynamicStoreCreate(NULL, CFSTR("copy_dhcp_hostname"),
	    NULL, NULL);
	if (store == NULL)
		return (NULL);
	key = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
	    kSCDynamicStoreDomainState, serviceID, kSCEntNetDHCP);
	if (key == NULL) {
		CFRelease(store);
		return (NULL);
	}
	dict = (CFDictionaryRef)SCDynamicStoreCopyValue(store, key);
	CFRelease(key);
	CFRelease(store);
	if (dict == NULL)
		return (NULL);
	if (CFGetTypeID(dict) == CFDictionaryGetTypeID()) {
		CFStringRef v = CFDictionaryGetValue(dict, CFSTR("Option_12"));
		if (v != NULL && CFGetTypeID(v) == CFStringGetTypeID())
			opt12 = CFStringCreateCopy(NULL, v);
	}
	CFRelease(dict);
	return (opt12);
}

Boolean
check_if_service_expensive(CFStringRef serviceID)
{
	(void)serviceID;	/* no metered-network concept on FreeBSD */
	return (FALSE);
}

#pragma mark -
#pragma mark SCPrivate SPI subset

Boolean
_SC_CFStringIsValidDNSName(CFStringRef cfstr)
{
	char buf[256];
	size_t i, len, label_len = 0;
	int label_count = 0;

	if (cfstr == NULL)
		return (FALSE);
	if (!CFStringGetCString(cfstr, buf, sizeof(buf), kCFStringEncodingUTF8))
		return (FALSE);
	len = strlen(buf);
	if (len == 0 || len > 253)
		return (FALSE);
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)buf[i];
		if (c == '.') {
			if (label_len == 0 || label_len > 63)
				return (FALSE);
			if (buf[i - 1] == '-')
				return (FALSE);
			label_count++;
			label_len = 0;
			continue;
		}
		if (label_len == 0 && c == '-')
			return (FALSE);
		if (!isalnum(c) && c != '-')
			return (FALSE);
		label_len++;
	}
	if (label_len == 0 || label_len > 63)
		return (FALSE);
	if (buf[len - 1] == '-')
		return (FALSE);
	(void)label_count;
	return (TRUE);
}
