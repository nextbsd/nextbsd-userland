/*
 * btstate.c — bluetoothd's SCDynamicStore publication. See btstate.h.
 *
 * Called from the main loop only, so the one session needs no lock.
 */
#include "btstate.h"
#include "btcf.h"

#include <SystemConfiguration/SCDynamicStore.h>

#include <stdarg.h>
#include <stdio.h>

static SCDynamicStoreRef	g_store;

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "bluetoothd[sc] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/* Open the session on first use; NULL while configd is not reachable. */
static SCDynamicStoreRef
store(void)
{
	if (g_store == NULL)
		g_store = SCDynamicStoreCreate(NULL,
		    CFSTR("bluetoothd"), NULL, NULL);
	return (g_store);
}

static const char *
state_name(bt_daemon_state_t s)
{
	switch (s) {
	case bt_daemon_active_e:		return ("Active");
	case bt_daemon_no_ioregistry_e:		return ("NoIORegistry");
	default:				return ("Idle");
	}
}

int
bt_state_publish_daemon(const struct bt_daemon_summary *s)
{
	CFMutableDictionaryRef d;
	CFMutableArrayRef arr;
	CFStringRef key;
	Boolean ok;
	int i;

	if (store() == NULL)
		return (-1);

	d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	if (d == NULL)
		return (-1);

	btcf_set_str(d, CFSTR("State"), state_name(s->state));
	arr = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	if (arr != NULL) {
		for (i = 0; i < s->nadapters; i++) {
			CFStringRef n = btcf_str(s->adapters[i]);

			if (n != NULL) {
				CFArrayAppendValue(arr, n);
				CFRelease(n);
			}
		}
		CFDictionarySetValue(d, CFSTR("Adapters"), arr);
		CFRelease(arr);
	}
	btcf_set_int(d, CFSTR("PairedDevices"), s->npaired);
	btcf_set_int(d, CFSTR("ControllerPowerState"), s->prefs.power_state);
	btcf_set_bool(d, CFSTR("DiscoverableState"), s->prefs.discoverable);
	btcf_set_bool(d, CFSTR("AutoConnect"), s->prefs.autoconnect);

	key = btcf_str(BT_STATE_KEY);
	ok = (key != NULL) && SCDynamicStoreSetValue(g_store, key, d);
	if (key != NULL)
		CFRelease(key);
	CFRelease(d);
	if (!ok) {
		xlog("could not set %s", BT_STATE_KEY);
		return (-1);
	}
	return (0);
}

int
bt_state_remove(const char *k)
{
	CFStringRef key;

	if (g_store == NULL)
		return (-1);
	key = btcf_str(k);
	if (key == NULL)
		return (-1);
	(void)SCDynamicStoreRemoveValue(g_store, key);
	CFRelease(key);
	return (0);
}

void
bt_state_close(void)
{
	if (g_store != NULL) {
		CFRelease(g_store);
		g_store = NULL;
	}
}
