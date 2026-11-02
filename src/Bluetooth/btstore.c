/*
 * btstore.c — the pairing store and persisted policy. See btstore.h.
 *
 * Each call opens the preferences, does its one thing and releases them, as
 * wland's known-networks code does: the file is small, the rate is a handful
 * of operations per minute, and re-reading means an edit made on disk while
 * bluetoothd runs is never overwritten by a stale in-memory copy.
 */
#include "btstore.h"
#include "btcf.h"
#include "keystore.h"

#include <sys/stat.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define	KEY_POWER		CFSTR("ControllerPowerState")
#define	KEY_DISCOVERABLE	CFSTR("DiscoverableState")
#define	KEY_AUTOCONNECT		CFSTR("AutoConnect")
#define	KEY_AUTOSWITCH_AUDIO	CFSTR("AutoSwitchAudio")
#define	KEY_A2DP_SINK		CFSTR("A2DPSink")
#define	KEY_PAIRED		CFSTR("PairedDevices")

/* PairedDevices/<addr> fields. */
#define	DEV_NAME		CFSTR("Name")
#define	DEV_ADDRTYPE		CFSTR("AddressType")
#define	DEV_TRANSPORTS		CFSTR("Transports")
#define	DEV_COD			CFSTR("ClassOfDevice")
#define	DEV_APPEARANCE		CFSTR("Appearance")
#define	DEV_PNP			CFSTR("PnPID")
#define	DEV_PROFILES		CFSTR("Profiles")
#define	DEV_HIDDESC		CFSTR("HIDDescriptor")
#define	DEV_HID_VCABLE		CFSTR("HIDVirtualCable")
#define	DEV_HID_RECONNECT	CFSTR("HIDReconnectInitiate")
#define	DEV_HID_CONNECTABLE	CFSTR("HIDNormallyConnectable")
#define	DEV_HID_BOOT		CFSTR("HIDBootDevice")
#define	DEV_HID_COUNTRY		CFSTR("HIDCountryCode")
#define	DEV_HID_SESSION		CFSTR("HIDSessionEstablished")
#define	DEV_AUTOCONNECT		CFSTR("AutoConnect")
#define	DEV_TRUSTED		CFSTR("Trusted")
#define	DEV_AUDIO		CFSTR("Audio")
#define	DEV_PAIRED		CFSTR("Paired")
#define	DEV_LASTCONN		CFSTR("LastConnected")

#define	PNP_SOURCE		CFSTR("VendorIDSource")
#define	PNP_VENDOR		CFSTR("VendorID")
#define	PNP_PRODUCT		CFSTR("ProductID")
#define	PNP_VERSION		CFSTR("Version")

#define	TRANSPORT_BREDR		"BR/EDR"
#define	TRANSPORT_LE		"LE"

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "bluetoothd[store] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

void
bt_store_init(void)
{
	struct stat sb;

	/*
	 * 077: SCPreferences creates its temp file 0644 and renames it into
	 * place, so without this the key file would be world-readable from
	 * the write until btcf_prefs_commit()'s chmod. The policy file is
	 * chmod'ed back to 0644 after each commit.
	 */
	(void)umask(077);

	/*
	 * SCPreferences creates missing parents 0755, which the umask above
	 * would turn into 0700 and hide the 0644 policy file from everyone
	 * else. Create the directory here, with its mode set explicitly.
	 */
	if (stat(BT_PREFS_DIR, &sb) != 0) {
		char dir[] = BT_PREFS_DIR;
		char *p;

		for (p = dir + 1; ; p++) {
			if (*p != '/' && *p != '\0')
				continue;
			{
				char c = *p;

				*p = '\0';
				if (mkdir(dir, 0755) == 0)
					(void)chmod(dir, 0755);
				else if (errno != EEXIST)
					xlog("mkdir %s: %s", dir,
					    strerror(errno));
				*p = c;
			}
			if (*p == '\0')
				break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Policy                                                              */
/* ------------------------------------------------------------------ */

void
bt_prefs_load(struct bt_prefs *out)
{
	SCPreferencesRef prefs;
	CFPropertyListRef v;
	long long n;

	out->power_state = 1;		/* on by default */
	out->discoverable = false;
	out->autoconnect = true;
	out->autoswitch_audio = true;
	out->a2dp_sink = false;	/* it would advertise us as a speaker */

	prefs = btcf_prefs_open(BT_PREFS_PATH);
	if (prefs == NULL)
		return;

	v = SCPreferencesGetValue(prefs, KEY_POWER);
	if (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID() &&
	    CFNumberGetValue(v, kCFNumberLongLongType, &n))
		out->power_state = (n != 0);
	else if (v != NULL && CFGetTypeID(v) == CFBooleanGetTypeID())
		out->power_state = CFBooleanGetValue(v) ? 1 : 0;

	v = SCPreferencesGetValue(prefs, KEY_DISCOVERABLE);
	if (v != NULL && CFGetTypeID(v) == CFBooleanGetTypeID())
		out->discoverable = CFBooleanGetValue(v);
	else if (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID() &&
	    CFNumberGetValue(v, kCFNumberLongLongType, &n))
		out->discoverable = (n != 0);

	v = SCPreferencesGetValue(prefs, KEY_AUTOCONNECT);
	if (v != NULL && CFGetTypeID(v) == CFBooleanGetTypeID())
		out->autoconnect = CFBooleanGetValue(v);
	else if (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID() &&
	    CFNumberGetValue(v, kCFNumberLongLongType, &n))
		out->autoconnect = (n != 0);

	v = SCPreferencesGetValue(prefs, KEY_AUTOSWITCH_AUDIO);
	if (v != NULL && CFGetTypeID(v) == CFBooleanGetTypeID())
		out->autoswitch_audio = CFBooleanGetValue(v);

	v = SCPreferencesGetValue(prefs, KEY_A2DP_SINK);
	if (v != NULL && CFGetTypeID(v) == CFBooleanGetTypeID())
		out->a2dp_sink = CFBooleanGetValue(v);

	CFRelease(prefs);
}

static int
prefs_set(CFStringRef key, CFPropertyListRef value)
{
	SCPreferencesRef prefs;
	int rc = -1;

	prefs = btcf_prefs_open(BT_PREFS_PATH);
	if (prefs == NULL)
		return (-1);
	if (SCPreferencesSetValue(prefs, key, value))
		rc = btcf_prefs_commit(prefs, BT_PREFS_PATH, 0644);
	if (rc != 0)
		xlog("could not save %s", BT_PREFS_PATH);
	CFRelease(prefs);
	return (rc);
}

int
bt_prefs_set_power_state(int on)
{
	CFNumberRef n;
	int v = on ? 1 : 0, rc;

	/* An int, as macOS's ControllerPowerState. */
	n = CFNumberCreate(NULL, kCFNumberIntType, &v);
	if (n == NULL)
		return (-1);
	rc = prefs_set(KEY_POWER, n);
	CFRelease(n);
	return (rc);
}

int
bt_prefs_set_discoverable(bool on)
{
	return (prefs_set(KEY_DISCOVERABLE, on ? kCFBooleanTrue
	                                       : kCFBooleanFalse));
}

int
bt_prefs_set_autoconnect(bool on)
{
	return (prefs_set(KEY_AUTOCONNECT, on ? kCFBooleanTrue
	                                      : kCFBooleanFalse));
}

int
bt_prefs_set_autoswitch_audio(bool on)
{
	return (prefs_set(KEY_AUTOSWITCH_AUDIO, on ? kCFBooleanTrue
	                                           : kCFBooleanFalse));
}

int
bt_prefs_set_a2dp_sink(bool on)
{
	return (prefs_set(KEY_A2DP_SINK, on ? kCFBooleanTrue
	                                    : kCFBooleanFalse));
}

/* ------------------------------------------------------------------ */
/* Paired devices                                                      */
/* ------------------------------------------------------------------ */

/* One HID* boolean: present means the HID attributes are known. */
static void
hid_flag_get(CFDictionaryRef e, CFStringRef key, uint8_t bit,
    struct bt_bond *b)
{
	bool v;

	if (!btcf_get_bool(e, key, &v))
		return;
	b->flags |= BT_BOND_HID_ATTRS;
	if (v)
		b->hid_flags |= bit;
}

/* Fill the metadata half of `b` from a PairedDevices entry. */
static bool
entry_to_bond(CFStringRef cf_key, CFDictionaryRef e, struct bt_bond *b)
{
	char key[BT_ADDR_STRLEN], s[16];
	CFDictionaryRef pnp;
	CFTypeRef v;
	long long n;
	size_t got;
	bool session;

	if (!CFStringGetCString(cf_key, key, sizeof(key),
	    kCFStringEncodingUTF8) || !bt_addr_aton(key, &b->addr))
		return (false);

	if (btcf_get_str(e, DEV_ADDRTYPE, s, sizeof(s)))
		b->addr.type = (strcmp(s, "random") == 0) ? BT_ADDR_RANDOM
		                                          : BT_ADDR_PUBLIC;
	(void)btcf_get_str(e, DEV_NAME, b->name, sizeof(b->name));
	if (btcf_get_int(e, DEV_COD, &n))
		b->class_of_device = (uint32_t)n;
	if (btcf_get_int(e, DEV_APPEARANCE, &n))
		b->appearance = (uint16_t)n;
	b->autoconnect = true;
	(void)btcf_get_bool(e, DEV_AUTOCONNECT, &b->autoconnect);
	(void)btcf_get_bool(e, DEV_TRUSTED, &b->trusted);
	{
		bool audio = true;	/* absent: on */

		(void)btcf_get_bool(e, DEV_AUDIO, &audio);
		b->no_audio = !audio;
	}
	if (btcf_get_int(e, DEV_PAIRED, &n))
		b->paired_at = (time_t)n;
	if (btcf_get_int(e, DEV_LASTCONN, &n))
		b->last_connected = (time_t)n;

	pnp = btcf_get_dict(e, DEV_PNP);
	if (pnp != NULL) {
		b->flags |= BT_BOND_PNP_ID;
		if (btcf_get_int(pnp, PNP_SOURCE, &n))
			b->pnp.vendor_id_source = (uint8_t)n;
		if (btcf_get_int(pnp, PNP_VENDOR, &n))
			b->pnp.vendor_id = (uint16_t)n;
		if (btcf_get_int(pnp, PNP_PRODUCT, &n))
			b->pnp.product_id = (uint16_t)n;
		if (btcf_get_int(pnp, PNP_VERSION, &n))
			b->pnp.version = (uint16_t)n;
	}

	if (btcf_get_data(e, DEV_HIDDESC, b->hid_desc, sizeof(b->hid_desc),
	    &got) && got > 0 && got <= sizeof(b->hid_desc)) {
		b->hid_desc_len = (uint16_t)got;
		b->flags |= BT_BOND_HID_DESC;
	}
	hid_flag_get(e, DEV_HID_VCABLE, BT_HID_VIRTUAL_CABLE, b);
	hid_flag_get(e, DEV_HID_RECONNECT, BT_HID_RECONNECT_INITIATE, b);
	hid_flag_get(e, DEV_HID_CONNECTABLE, BT_HID_NORMALLY_CONNECTABLE, b);
	hid_flag_get(e, DEV_HID_BOOT, BT_HID_BOOT_DEVICE, b);
	if (btcf_get_bool(e, DEV_HID_SESSION, &session) && session)
		b->hid_flags |= BT_HID_SESSION_ESTABLISHED;
	if (btcf_get_int(e, DEV_HID_COUNTRY, &n)) {
		b->hid_country_code = (uint8_t)n;
		b->flags |= BT_BOND_HID_ATTRS;
	}

	v = CFDictionaryGetValue(e, DEV_TRANSPORTS);
	if (v != NULL && CFGetTypeID(v) == CFArrayGetTypeID()) {
		CFIndex i;

		for (i = 0; i < CFArrayGetCount(v); i++) {
			CFTypeRef t = CFArrayGetValueAtIndex(v, i);

			if (CFGetTypeID(t) != CFStringGetTypeID() ||
			    !CFStringGetCString(t, s, sizeof(s),
			    kCFStringEncodingUTF8))
				continue;
			if (strcmp(s, TRANSPORT_BREDR) == 0)
				b->transports |= BT_TRANSPORT_BREDR;
			else if (strcmp(s, TRANSPORT_LE) == 0)
				b->transports |= BT_TRANSPORT_LE;
		}
	}

	v = CFDictionaryGetValue(e, DEV_PROFILES);
	if (v != NULL && CFGetTypeID(v) == CFArrayGetTypeID()) {
		char pname[32];
		CFIndex i;

		for (i = 0; i < CFArrayGetCount(v); i++) {
			CFTypeRef t = CFArrayGetValueAtIndex(v, i);

			if (CFGetTypeID(t) == CFStringGetTypeID() &&
			    CFStringGetCString(t, pname, sizeof(pname),
			    kCFStringEncodingUTF8))
				b->profiles |= bt_profile_bit(pname);
		}
	}
	return (true);
}

/* Build a PairedDevices entry from the metadata half of `b`. */
static CFMutableDictionaryRef
bond_to_entry(const struct bt_bond *b)
{
	CFMutableDictionaryRef e, pnp;
	CFMutableArrayRef arr;
	CFStringRef s;
	uint32_t bit;

	e = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	if (e == NULL)
		return (NULL);

	btcf_set_str(e, DEV_NAME, b->name);
	btcf_set_str(e, DEV_ADDRTYPE,
	    b->addr.type == BT_ADDR_RANDOM ? "random" : "public");
	if (b->class_of_device != 0)
		btcf_set_int(e, DEV_COD, b->class_of_device);
	if (b->appearance != 0)
		btcf_set_int(e, DEV_APPEARANCE, b->appearance);
	btcf_set_bool(e, DEV_AUTOCONNECT, b->autoconnect);
	btcf_set_bool(e, DEV_TRUSTED, b->trusted);
	btcf_set_bool(e, DEV_AUDIO, !b->no_audio);
	btcf_set_int(e, DEV_PAIRED, (long long)(b->paired_at != 0 ?
	    b->paired_at : time(NULL)));
	if (b->last_connected != 0)
		btcf_set_int(e, DEV_LASTCONN, (long long)b->last_connected);

	if ((b->flags & BT_BOND_PNP_ID) != 0) {
		pnp = CFDictionaryCreateMutable(NULL, 0,
		    &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		if (pnp != NULL) {
			btcf_set_int(pnp, PNP_SOURCE, b->pnp.vendor_id_source);
			btcf_set_int(pnp, PNP_VENDOR, b->pnp.vendor_id);
			btcf_set_int(pnp, PNP_PRODUCT, b->pnp.product_id);
			btcf_set_int(pnp, PNP_VERSION, b->pnp.version);
			CFDictionarySetValue(e, DEV_PNP, pnp);
			CFRelease(pnp);
		}
	}

	if ((b->flags & BT_BOND_HID_DESC) != 0 && b->hid_desc_len > 0 &&
	    b->hid_desc_len <= sizeof(b->hid_desc))
		btcf_set_data(e, DEV_HIDDESC, b->hid_desc, b->hid_desc_len);
	if ((b->flags & BT_BOND_HID_ATTRS) != 0) {
		btcf_set_bool(e, DEV_HID_VCABLE,
		    (b->hid_flags & BT_HID_VIRTUAL_CABLE) != 0);
		btcf_set_bool(e, DEV_HID_RECONNECT,
		    (b->hid_flags & BT_HID_RECONNECT_INITIATE) != 0);
		btcf_set_bool(e, DEV_HID_CONNECTABLE,
		    (b->hid_flags & BT_HID_NORMALLY_CONNECTABLE) != 0);
		btcf_set_bool(e, DEV_HID_BOOT,
		    (b->hid_flags & BT_HID_BOOT_DEVICE) != 0);
		btcf_set_int(e, DEV_HID_COUNTRY, b->hid_country_code);
	}
	/* Learnt by the HID host, so kept even without the SDP record. */
	if ((b->hid_flags & BT_HID_SESSION_ESTABLISHED) != 0)
		btcf_set_bool(e, DEV_HID_SESSION, true);

	arr = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	if (arr != NULL) {
		if ((b->transports & BT_TRANSPORT_BREDR) != 0)
			CFArrayAppendValue(arr, CFSTR(TRANSPORT_BREDR));
		if ((b->transports & BT_TRANSPORT_LE) != 0)
			CFArrayAppendValue(arr, CFSTR(TRANSPORT_LE));
		CFDictionarySetValue(e, DEV_TRANSPORTS, arr);
		CFRelease(arr);
	}

	arr = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	if (arr != NULL) {
		for (bit = 1; bit != 0; bit <<= 1) {
			const char *name;

			if ((b->profiles & bit) == 0 ||
			    (name = bt_profile_name(bit)) == NULL)
				continue;
			s = btcf_str(name);
			if (s != NULL) {
				CFArrayAppendValue(arr, s);
				CFRelease(s);
			}
		}
		CFDictionarySetValue(e, DEV_PROFILES, arr);
		CFRelease(arr);
	}
	return (e);
}

static CFComparisonResult
key_compare(const void *a, const void *b, void *ctx)
{
	(void)ctx;
	return (CFStringCompare(a, b, 0));
}

/*
 * The PairedDevices keys in address order, so bt_store_paired_at(i) is stable
 * between a count and the entries that follow it. Caller releases.
 */
static CFMutableArrayRef
sorted_keys(CFDictionaryRef devs)
{
	CFMutableArrayRef arr;
	CFIndex n;
	const void **keys;

	arr = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	if (arr == NULL)
		return (NULL);
	n = CFDictionaryGetCount(devs);
	if (n == 0)
		return (arr);
	keys = calloc((size_t)n, sizeof(*keys));
	if (keys == NULL)
		return (arr);
	CFDictionaryGetKeysAndValues(devs, keys, NULL);
	for (CFIndex i = 0; i < n; i++) {
		if (CFGetTypeID(keys[i]) == CFStringGetTypeID())
			CFArrayAppendValue(arr, keys[i]);
	}
	free(keys);
	CFArraySortValues(arr, CFRangeMake(0, CFArrayGetCount(arr)),
	    key_compare, NULL);
	return (arr);
}

int
bt_store_paired_count(void)
{
	SCPreferencesRef prefs;
	CFMutableDictionaryRef devs;
	int n;

	prefs = btcf_prefs_open(BT_PREFS_PATH);
	if (prefs == NULL)
		return (0);
	devs = btcf_prefs_copy_dict(prefs, KEY_PAIRED);
	n = (devs != NULL) ? (int)CFDictionaryGetCount(devs) : 0;
	if (devs != NULL)
		CFRelease(devs);
	CFRelease(prefs);
	return (n);
}

bool
bt_store_paired_at(int idx, struct bt_bond *out)
{
	SCPreferencesRef prefs;
	CFMutableDictionaryRef devs;
	CFMutableArrayRef keys = NULL;
	CFDictionaryRef e;
	CFStringRef k;
	bool ok = false;

	if (idx < 0 || out == NULL)
		return (false);
	prefs = btcf_prefs_open(BT_PREFS_PATH);
	if (prefs == NULL)
		return (false);
	devs = btcf_prefs_copy_dict(prefs, KEY_PAIRED);
	if (devs != NULL)
		keys = sorted_keys(devs);
	if (keys != NULL && idx < CFArrayGetCount(keys)) {
		k = CFArrayGetValueAtIndex(keys, idx);
		e = btcf_get_dict(devs, k);
		if (e != NULL) {
			(void)memset(out, 0, sizeof(*out));
			ok = entry_to_bond(k, e, out);
		}
	}
	if (keys != NULL)
		CFRelease(keys);
	if (devs != NULL)
		CFRelease(devs);
	CFRelease(prefs);
	return (ok);
}

bool
bt_store_bond_find(const struct bt_addr *addr, bool with_keys,
    struct bt_bond *out)
{
	SCPreferencesRef prefs;
	CFMutableDictionaryRef devs;
	CFDictionaryRef e;
	CFStringRef k;
	char key[BT_ADDR_STRLEN];
	bool ok = false;

	if (addr == NULL || out == NULL)
		return (false);
	(void)bt_addr_ntoa(addr, BT_ADDR_FMT_KEY, key, sizeof(key));
	prefs = btcf_prefs_open(BT_PREFS_PATH);
	if (prefs == NULL)
		return (false);
	devs = btcf_prefs_copy_dict(prefs, KEY_PAIRED);
	k = btcf_str(key);
	if (devs != NULL && k != NULL) {
		e = btcf_get_dict(devs, k);
		if (e != NULL) {
			(void)memset(out, 0, sizeof(*out));
			ok = entry_to_bond(k, e, out);
		}
	}
	if (k != NULL)
		CFRelease(k);
	if (devs != NULL)
		CFRelease(devs);
	CFRelease(prefs);

	if (ok && with_keys)
		(void)keystore_load(key, out);
	return (ok);
}

int
bt_store_bond_save(const struct bt_bond *b)
{
	SCPreferencesRef prefs;
	CFMutableDictionaryRef devs, e;
	CFStringRef k;
	char key[BT_ADDR_STRLEN], disp[BT_ADDR_STRLEN];
	int rc = -1;

	if (b == NULL)
		return (-1);
	(void)bt_addr_ntoa(&b->addr, BT_ADDR_FMT_KEY, key, sizeof(key));
	(void)bt_addr_ntoa(&b->addr, BT_ADDR_FMT_DISPLAY, disp, sizeof(disp));

	/*
	 * Keys first. A bond whose metadata is saved but whose keys are not
	 * would list as paired and then fail every reconnect; the other way
	 * round, an orphan key is harmless and the next save replaces it.
	 */
	if (keystore_save(key, b) != 0) {
		xlog("%s: could not save keys", disp);
		return (-1);
	}

	prefs = btcf_prefs_open(BT_PREFS_PATH);
	if (prefs == NULL)
		return (-1);
	devs = btcf_prefs_copy_dict(prefs, KEY_PAIRED);
	e = bond_to_entry(b);
	k = btcf_str(key);
	if (devs != NULL && e != NULL && k != NULL) {
		CFDictionarySetValue(devs, k, e);
		if (SCPreferencesSetValue(prefs, KEY_PAIRED, devs))
			rc = btcf_prefs_commit(prefs, BT_PREFS_PATH, 0644);
	}
	if (rc == 0)
		xlog("%s: bond saved ('%s')", disp, b->name);
	else
		xlog("%s: could not save to %s", disp, BT_PREFS_PATH);

	if (k != NULL)
		CFRelease(k);
	if (e != NULL)
		CFRelease(e);
	if (devs != NULL)
		CFRelease(devs);
	CFRelease(prefs);
	return (rc);
}

int
bt_store_bond_forget(const struct bt_addr *addr)
{
	SCPreferencesRef prefs;
	CFMutableDictionaryRef devs;
	CFStringRef k;
	char key[BT_ADDR_STRLEN], disp[BT_ADDR_STRLEN];
	int rc = -1;

	if (addr == NULL)
		return (-1);
	(void)bt_addr_ntoa(addr, BT_ADDR_FMT_KEY, key, sizeof(key));
	(void)bt_addr_ntoa(addr, BT_ADDR_FMT_DISPLAY, disp, sizeof(disp));

	prefs = btcf_prefs_open(BT_PREFS_PATH);
	if (prefs == NULL)
		return (-1);
	devs = btcf_prefs_copy_dict(prefs, KEY_PAIRED);
	k = btcf_str(key);
	if (devs != NULL && k != NULL) {
		CFDictionaryRemoveValue(devs, k);
		if (SCPreferencesSetValue(prefs, KEY_PAIRED, devs))
			rc = btcf_prefs_commit(prefs, BT_PREFS_PATH, 0644);
	}
	if (k != NULL)
		CFRelease(k);
	if (devs != NULL)
		CFRelease(devs);
	CFRelease(prefs);

	if (keystore_forget(key) != 0)
		rc = -1;
	if (rc == 0)
		xlog("%s: forgotten", disp);
	return (rc);
}
