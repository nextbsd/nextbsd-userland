/*
 * btstate.c — bluetoothd's SCDynamicStore publication. See btstate.h.
 *
 * Called from the main loop only, so the one session needs no lock.
 */
#include "btstate.h"
#include "audio.h"
#include "btcf.h"

#include <SystemConfiguration/SCDynamicStore.h>

#include <notify.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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
	btcf_set_bool(d, CFSTR("AutoSwitchAudio"), s->prefs.autoswitch_audio);
	btcf_set_bool(d, CFSTR("A2DPSink"), s->prefs.a2dp_sink);

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

/* Set `k` to `d`, consuming `d`. 0 on success. */
static int
set_value(const char *k, CFMutableDictionaryRef d)
{
	CFStringRef key;
	Boolean ok;

	if (d == NULL)
		return (-1);
	if (store() == NULL) {
		CFRelease(d);
		return (-1);
	}
	key = btcf_str(k);
	ok = (key != NULL) && SCDynamicStoreSetValue(g_store, key, d);
	if (key != NULL)
		CFRelease(key);
	CFRelease(d);
	if (!ok) {
		xlog("could not set %s", k);
		return (-1);
	}
	return (0);
}

static CFMutableDictionaryRef
new_dict(void)
{
	return (CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
}

int
bt_state_publish_adapter(const struct bt_adapter_summary *s)
{
	CFMutableDictionaryRef d = new_dict();
	char key[128];

	if (d == NULL)
		return (-1);
	if (s->address[0] != '\0')
		btcf_set_str(d, CFSTR("Address"), s->address);
	if (s->name[0] != '\0')
		btcf_set_str(d, CFSTR("Name"), s->name);
	btcf_set_str(d, CFSTR("Transport"), s->transport);
	btcf_set_int(d, CFSTR("VendorID"), s->vendor);
	btcf_set_int(d, CFSTR("ProductID"), s->product);
	btcf_set_str(d, CFSTR("State"), s->state);
	btcf_set_bool(d, CFSTR("Powered"), s->powered);
	btcf_set_bool(d, CFSTR("Discoverable"), s->discoverable);
	btcf_set_bool(d, CFSTR("Connectable"), s->connectable);
	btcf_set_bool(d, CFSTR("Scanning"), s->scanning);
	(void)snprintf(key, sizeof(key), "%s%s", BT_STATE_ADAPTER_PREFIX,
	    s->node);
	return (set_value(key, d));
}

int
bt_state_publish_pairing(const char *key, const struct bt_pair_prompt *p,
    const char *method)
{
	CFMutableDictionaryRef d = new_dict();
	char addr[BT_ADDR_STRLEN];

	if (d == NULL)
		return (-1);
	btcf_set_str(d, CFSTR("Address"), bt_addr_ntoa(&p->peer,
	    BT_ADDR_FMT_DISPLAY, addr, sizeof(addr)));
	btcf_set_str(d, CFSTR("Method"), method);
	if (p->method == bt_pair_numeric_comparison_e ||
	    p->method == bt_pair_passkey_display_e)
		btcf_set_int(d, CFSTR("Passkey"), p->passkey);
	btcf_set_int(d, CFSTR("Expires"), (long long)p->expires);
	return (set_value(key, d));
}

int
bt_state_publish_device(const char *key, const struct bt_device_summary *s)
{
	CFMutableDictionaryRef d = new_dict();
	CFMutableArrayRef arr;
	char addr[BT_ADDR_STRLEN];
	uint32_t bit;

	if (d == NULL)
		return (-1);
	btcf_set_str(d, CFSTR("Address"), bt_addr_ntoa(s->addr,
	    BT_ADDR_FMT_DISPLAY, addr, sizeof(addr)));
	btcf_set_str(d, CFSTR("Name"), s->name);
	btcf_set_int(d, CFSTR("ClassOfDevice"), s->class_of_device);
	if (s->adapter[0] != '\0')
		btcf_set_str(d, CFSTR("Adapter"), s->adapter);
	btcf_set_bool(d, CFSTR("Paired"), s->paired);
	btcf_set_bool(d, CFSTR("Connected"), s->connected);
	btcf_set_bool(d, CFSTR("Connecting"), s->connecting);
	arr = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	if (arr != NULL) {
		for (bit = 1; bit != 0; bit <<= 1) {
			const char *name;
			CFStringRef n;

			if ((s->profiles & bit) == 0 ||
			    (name = bt_profile_name(bit)) == NULL ||
			    (n = btcf_str(name)) == NULL)
				continue;
			CFArrayAppendValue(arr, n);
			CFRelease(n);
		}
		CFDictionarySetValue(d, CFSTR("Profiles"), arr);
		CFRelease(arr);
	}
	if (s->connected && s->rssi != 127)
		btcf_set_int(d, CFSTR("RSSI"), s->rssi);
	return (set_value(key, d));
}

int
bt_state_publish_firmware(const char *ugen, int vendor, int product,
    const char *state)
{
	CFMutableDictionaryRef d = new_dict();
	char key[128];

	if (d == NULL)
		return (-1);
	btcf_set_int(d, CFSTR("VendorID"), vendor);
	btcf_set_int(d, CFSTR("ProductID"), product);
	btcf_set_str(d, CFSTR("State"), state);
	(void)snprintf(key, sizeof(key), "%s%s", BT_STATE_FIRMWARE_PREFIX,
	    ugen);
	return (set_value(key, d));
}

int
bt_state_publish_audio(const char *key, const struct audio_status *s)
{
	CFMutableDictionaryRef d = new_dict();

	if (d == NULL)
		return (-1);
	btcf_set_str(d, CFSTR("Role"), s->role == AUDIO_ROLE_SOURCE ?
	    "Source" : s->role == AUDIO_ROLE_SINK ? "Sink" : "None");
	btcf_set_str(d, CFSTR("Route"), s->route == AUDIO_ROUTE_HFP ? "HFP" :
	    s->route == AUDIO_ROUTE_A2DP ? "A2DP" : "None");
	btcf_set_bool(d, CFSTR("Active"), s->active);
	if (s->codec[0] != '\0')
		btcf_set_str(d, CFSTR("Codec"), s->codec);
	if (s->node[0] != '\0')
		btcf_set_str(d, CFSTR("Node"), s->node);
	btcf_set_bool(d, CFSTR("DefaultOutput"), s->default_out);
	btcf_set_bool(d, CFSTR("AVRCP"), s->avrcp);
	if (s->volume >= 0)
		btcf_set_int(d, CFSTR("Volume"), s->volume);
	/* The hands-free gateway (#525): connected, and the mic live. */
	btcf_set_bool(d, CFSTR("HandsFree"), s->hfp);
	btcf_set_bool(d, CFSTR("Microphone"), s->hfp_audio);
	if (s->battery >= 0)
		btcf_set_int(d, CFSTR("Battery"), s->battery);
	return (set_value(key, d));
}

int
bt_state_publish_a2dp_sink(const char *node, bool enabled,
    const struct bt_addr *peer)
{
	CFMutableDictionaryRef d = new_dict();
	char key[128], addr[BT_ADDR_STRLEN];

	if (d == NULL)
		return (-1);
	btcf_set_bool(d, CFSTR("Enabled"), enabled);
	if (peer != NULL)
		btcf_set_str(d, CFSTR("Peer"), bt_addr_ntoa(peer,
		    BT_ADDR_FMT_DISPLAY, addr, sizeof(addr)));
	(void)snprintf(key, sizeof(key), "%s%s/A2DPSink",
	    BT_STATE_ADAPTER_PREFIX, node);
	return (set_value(key, d));
}

void
bt_state_notify(const char *name)
{
	(void)notify_post(name);
}

bool
bt_state_computer_name(char *buf, size_t len)
{
	CFStringRef name = NULL;
	char *dot;

	if (store() != NULL)
		name = SCDynamicStoreCopyComputerName(g_store, NULL);
	if (name != NULL) {
		Boolean ok = CFStringGetCString(name, buf, (CFIndex)len,
		    kCFStringEncodingUTF8);

		CFRelease(name);
		if (ok && buf[0] != '\0')
			return (true);
	}
	if (gethostname(buf, len) != 0 || buf[0] == '\0')
		return (false);
	buf[len - 1] = '\0';
	if ((dot = strchr(buf, '.')) != NULL)
		*dot = '\0';
	return (buf[0] != '\0');
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
