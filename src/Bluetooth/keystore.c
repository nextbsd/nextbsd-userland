/*
 * keystore.c — bluetoothd's secrets. See keystore.h.
 *
 * The only file that reads or writes key bytes, and the one a keychain
 * replaces. Log lines name the device and never the key.
 */
#include "keystore.h"
#include "btcf.h"
#include "btstore.h"		/* BT_KEYS_PATH */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define	FAM_LINK	CFSTR("LinkKeys")
#define	FAM_LTK		CFSTR("LongTermKeys")
#define	FAM_IRK		CFSTR("IdentityResolvingKeys")
#define	FAM_CSRK	CFSTR("SignatureKeys")

#define	LK_KEY		CFSTR("Key")
#define	LK_TYPE		CFSTR("Type")

#define	LTK_KEY		CFSTR("LTK")
#define	LTK_EDIV	CFSTR("EDIV")
#define	LTK_RAND	CFSTR("Rand")
#define	LTK_SIZE	CFSTR("KeySize")
#define	LTK_AUTH	CFSTR("Authenticated")
#define	LTK_SC		CFSTR("SecureConnections")

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "bluetoothd[keys] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/* A 16-byte key, exactly, or nothing. */
static bool
get_key16(CFTypeRef v, uint8_t out[BT_KEY_LEN])
{
	if (v == NULL || CFGetTypeID(v) != CFDataGetTypeID() ||
	    CFDataGetLength(v) != BT_KEY_LEN)
		return (false);
	CFDataGetBytes(v, CFRangeMake(0, BT_KEY_LEN), out);
	return (true);
}

/* The family dictionary's entry for `k`, or NULL. */
static CFTypeRef
family_get(SCPreferencesRef prefs, CFStringRef family, CFStringRef k)
{
	CFPropertyListRef fam;

	fam = SCPreferencesGetValue(prefs, family);
	if (fam == NULL || CFGetTypeID(fam) != CFDictionaryGetTypeID())
		return (NULL);
	return (CFDictionaryGetValue(fam, k));
}

/* Set (value != NULL) or remove (value == NULL) `k` in a family. */
static bool
family_put(SCPreferencesRef prefs, CFStringRef family, CFStringRef k,
    CFTypeRef value)
{
	CFMutableDictionaryRef fam;
	bool ok;

	fam = btcf_prefs_copy_dict(prefs, family);
	if (fam == NULL)
		return (false);
	if (value != NULL)
		CFDictionarySetValue(fam, k, value);
	else
		CFDictionaryRemoveValue(fam, k);
	ok = SCPreferencesSetValue(prefs, family, fam);
	CFRelease(fam);
	return (ok);
}

bool
keystore_load(const char *key, struct bt_bond *b)
{
	SCPreferencesRef prefs;
	CFStringRef k;
	CFTypeRef v;
	long long n;
	size_t got;
	uint8_t rand[8];
	bool any = false;

	if (key == NULL || b == NULL)
		return (false);
	prefs = btcf_prefs_open(BT_KEYS_PATH);
	if (prefs == NULL)
		return (false);
	k = btcf_str(key);
	if (k == NULL) {
		CFRelease(prefs);
		return (false);
	}

	v = family_get(prefs, FAM_LINK, k);
	if (v != NULL && CFGetTypeID(v) == CFDictionaryGetTypeID() &&
	    get_key16(CFDictionaryGetValue(v, LK_KEY), b->link_key)) {
		b->link_key_type = btcf_get_int(v, LK_TYPE, &n) ?
		    (uint8_t)n : 0;
		b->flags |= BT_BOND_LINK_KEY;
		b->transports |= BT_TRANSPORT_BREDR;
		any = true;
	}

	v = family_get(prefs, FAM_LTK, k);
	if (v != NULL && CFGetTypeID(v) == CFDictionaryGetTypeID() &&
	    get_key16(CFDictionaryGetValue(v, LTK_KEY), b->ltk.key)) {
		b->ltk.ediv = btcf_get_int(v, LTK_EDIV, &n) ? (uint16_t)n : 0;
		b->ltk.rand = 0;
		if (btcf_get_data(v, LTK_RAND, rand, sizeof(rand), &got) &&
		    got == sizeof(rand)) {
			int i;

			/* Wire order: rand[0] is the least significant. */
			for (i = 7; i >= 0; i--)
				b->ltk.rand = (b->ltk.rand << 8) | rand[i];
		}
		b->ltk.key_size = btcf_get_int(v, LTK_SIZE, &n) ?
		    (uint8_t)n : BT_KEY_LEN;
		b->ltk.authenticated = false;
		(void)btcf_get_bool(v, LTK_AUTH, &b->ltk.authenticated);
		b->ltk.secure_conn = false;
		(void)btcf_get_bool(v, LTK_SC, &b->ltk.secure_conn);
		b->flags |= BT_BOND_LTK;
		b->transports |= BT_TRANSPORT_LE;
		any = true;
	}

	if (get_key16(family_get(prefs, FAM_IRK, k), b->irk)) {
		b->flags |= BT_BOND_IRK;
		any = true;
	}
	if (get_key16(family_get(prefs, FAM_CSRK, k), b->csrk)) {
		b->flags |= BT_BOND_CSRK;
		any = true;
	}

	CFRelease(k);
	CFRelease(prefs);
	return (any);
}

int
keystore_save(const char *key, const struct bt_bond *b)
{
	SCPreferencesRef prefs;
	CFMutableDictionaryRef d;
	CFDataRef data;
	CFStringRef k;
	bool ok = true;
	int rc = -1;

	if (key == NULL || b == NULL)
		return (-1);
	prefs = btcf_prefs_open(BT_KEYS_PATH);
	if (prefs == NULL)
		return (-1);
	k = btcf_str(key);
	if (k == NULL) {
		CFRelease(prefs);
		return (-1);
	}

	/* Classic link key. */
	d = NULL;
	if ((b->flags & BT_BOND_LINK_KEY) != 0) {
		d = CFDictionaryCreateMutable(NULL, 0,
		    &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		if (d == NULL)
			ok = false;
		else {
			btcf_set_data(d, LK_KEY, b->link_key, BT_KEY_LEN);
			btcf_set_int(d, LK_TYPE, b->link_key_type);
		}
	}
	if (ok)
		ok = family_put(prefs, FAM_LINK, k, d);
	if (d != NULL)
		CFRelease(d);

	/* LE long-term key. */
	d = NULL;
	if (ok && (b->flags & BT_BOND_LTK) != 0) {
		uint8_t rand[8];
		int i;

		for (i = 0; i < 8; i++)
			rand[i] = (uint8_t)(b->ltk.rand >> (8 * i));
		d = CFDictionaryCreateMutable(NULL, 0,
		    &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		if (d == NULL)
			ok = false;
		else {
			btcf_set_data(d, LTK_KEY, b->ltk.key, BT_KEY_LEN);
			btcf_set_int(d, LTK_EDIV, b->ltk.ediv);
			btcf_set_data(d, LTK_RAND, rand, sizeof(rand));
			btcf_set_int(d, LTK_SIZE, b->ltk.key_size);
			btcf_set_bool(d, LTK_AUTH, b->ltk.authenticated);
			btcf_set_bool(d, LTK_SC, b->ltk.secure_conn);
		}
	}
	if (ok)
		ok = family_put(prefs, FAM_LTK, k, d);
	if (d != NULL)
		CFRelease(d);

	/* LE identity resolving and signature keys. */
	data = NULL;
	if (ok && (b->flags & BT_BOND_IRK) != 0)
		data = CFDataCreate(NULL, b->irk, BT_KEY_LEN);
	if (ok)
		ok = family_put(prefs, FAM_IRK, k, data);
	if (data != NULL)
		CFRelease(data);

	data = NULL;
	if (ok && (b->flags & BT_BOND_CSRK) != 0)
		data = CFDataCreate(NULL, b->csrk, BT_KEY_LEN);
	if (ok)
		ok = family_put(prefs, FAM_CSRK, k, data);
	if (data != NULL)
		CFRelease(data);

	if (ok)
		rc = btcf_prefs_commit(prefs, BT_KEYS_PATH, 0600);
	if (rc != 0)
		xlog("%s: could not write %s", key, BT_KEYS_PATH);

	CFRelease(k);
	CFRelease(prefs);
	return (rc);
}

int
keystore_forget(const char *key)
{
	SCPreferencesRef prefs;
	CFStringRef k;
	int rc = -1;

	if (key == NULL)
		return (-1);
	prefs = btcf_prefs_open(BT_KEYS_PATH);
	if (prefs == NULL)
		return (-1);
	k = btcf_str(key);
	if (k != NULL &&
	    family_put(prefs, FAM_LINK, k, NULL) &&
	    family_put(prefs, FAM_LTK, k, NULL) &&
	    family_put(prefs, FAM_IRK, k, NULL) &&
	    family_put(prefs, FAM_CSRK, k, NULL))
		rc = btcf_prefs_commit(prefs, BT_KEYS_PATH, 0600);
	if (rc != 0)
		xlog("%s: could not remove keys from %s", key, BT_KEYS_PATH);
	if (k != NULL)
		CFRelease(k);
	CFRelease(prefs);
	return (rc);
}
