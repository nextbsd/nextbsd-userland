/*
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * sh_pm.c — SetPMPreferences: the Energy Saver settings.
 *
 * pmset's way: read the active settings with IOPMCopyPMPreferences,
 * change one power source's values, and write every source back with
 * IOPMSetPMPreferences (which needs root, and posts
 * kIOPMPrefsChangeNotify, which powerd follows). Writing back every
 * source matters: IOPMSetPMPreferences clears a source that's missing
 * from the dictionary.
 *
 * Only the settings in settingshelper_types.h's SH_PM_* list can be set,
 * each checked against its range and against IOPMFeatureIsAvailable for
 * that power source, as pmset checks them.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/pwr_mgt/IOPMLibPrivate.h>

#include <string.h>

#include "SettingsHelper.h"

/* Sleep timers: minutes, 0 for never; pmset takes the same range. */
#define	MAX_MINUTES	(24 * 60)

static const struct {
	const char	*key;
	int		max;
} pm_settings[SH_PM_KEY_LIMIT] = {
	[SH_PM_DISPLAY_SLEEP]		= { kIOPMDisplaySleepKey, MAX_MINUTES },
	[SH_PM_SYSTEM_SLEEP]		= { kIOPMSystemSleepKey, MAX_MINUTES },
	[SH_PM_DISK_SLEEP]		= { kIOPMDiskSleepKey, MAX_MINUTES },
	[SH_PM_WAKE_ON_LAN]		= { kIOPMWakeOnLANKey, 1 },
	[SH_PM_REDUCE_BRIGHTNESS]	= { kIOPMReduceBrightnessKey, 1 },
	[SH_PM_TTYS_PREVENT_SLEEP]	= { kIOPMTTYSPreventSleepKey, 1 },
};

static CFStringRef
source_key(int source)
{
	switch (source) {
	case SH_PM_SOURCE_AC:
		return (CFSTR(kIOPMACPowerKey));
	case SH_PM_SOURCE_BATTERY:
		return (CFSTR(kIOPMBatteryPowerKey));
	case SH_PM_SOURCE_UPS:
		return (CFSTR(kIOPMUPSPowerKey));
	default:
		return (NULL);
	}
}

int
sh_pm_set(int source, const int32_t values[SH_PM_VALUES])
{
	CFMutableDictionaryRef prefs = NULL, src = NULL;
	CFDictionaryRef defaults = NULL, defsrc = NULL;
	CFStringRef skey = source_key(source), key;
	CFTypeRef cur;
	IOReturn ret;
	int i, rc = SH_OK, changed = 0;

	if (skey == NULL)
		return (SH_ERR_INVALID);
	for (i = 0; i < SH_PM_VALUES; i++) {
		if (values[i] == SH_PM_UNCHANGED)
			continue;
		if (i >= SH_PM_KEY_LIMIT)
			return (SH_ERR_INVALID);
		if (values[i] != SH_PM_DEFAULT &&
		    (values[i] < 0 || values[i] > pm_settings[i].max))
			return (SH_ERR_INVALID);
	}

	prefs = IOPMCopyPMPreferences();
	if (prefs == NULL)
		return (SH_ERR_FAILED);
	cur = CFDictionaryGetValue(prefs, skey);
	if (cur == NULL || CFGetTypeID(cur) != CFDictionaryGetTypeID()) {
		rc = SH_ERR_UNSUPPORTED;	/* no battery, no UPS */
		goto out;
	}
	src = CFDictionaryCreateMutableCopy(NULL, 0, cur);

	for (i = 0; i < SH_PM_KEY_LIMIT && rc == SH_OK; i++) {
		CFNumberRef num;
		CFTypeRef def;
		int v = values[i];

		if (v == SH_PM_UNCHANGED)
			continue;
		key = CFStringCreateWithCString(NULL, pm_settings[i].key,
		    kCFStringEncodingUTF8);
		if (!IOPMFeatureIsAvailable(key, skey)) {
			rc = SH_ERR_UNSUPPORTED;
		} else if (v == SH_PM_DEFAULT) {
			if (defaults == NULL &&
			    (defaults = IOPMCopyDefaultPreferences()) != NULL) {
				defsrc = CFDictionaryGetValue(defaults, skey);
				if (defsrc != NULL &&
				    CFGetTypeID(defsrc) != CFDictionaryGetTypeID())
					defsrc = NULL;
			}
			def = defsrc != NULL ?
			    CFDictionaryGetValue(defsrc, key) : NULL;
			if (def != NULL)
				CFDictionarySetValue(src, key, def);
			else
				CFDictionaryRemoveValue(src, key);
			changed++;
		} else {
			num = CFNumberCreate(NULL, kCFNumberIntType, &v);
			CFDictionarySetValue(src, key, num);
			CFRelease(num);
			changed++;
		}
		CFRelease(key);
	}
	if (rc != SH_OK || changed == 0)
		goto out;

	CFDictionarySetValue(prefs, skey, src);
	ret = IOPMSetPMPreferences(prefs);
	if (ret != kIOReturnSuccess) {
		sh_log("IOPMSetPMPreferences: 0x%x", (unsigned)ret);
		rc = SH_ERR_FAILED;
	}
out:
	if (src != NULL)
		CFRelease(src);
	if (defaults != NULL)
		CFRelease(defaults);
	CFRelease(prefs);
	return (rc);
}
