/*
 * DASupport.h — daemon preferences and support routines (E15 C5,
 * nextbsd-userland#195).
 *
 * The C counterpart of the preference half of Apple's DASupport.m
 * (DAPreferenceListRefresh, the kDAPreference* keys) plus the console-user
 * input DAMount.c asks for, and the /etc/fstab mount map (#196), which
 * Apple keeps in DASupport.m too.
 */
#ifndef DISKARBITRATION_DASUPPORT_H
#define DISKARBITRATION_DASUPPORT_H

#include <CoreFoundation/CoreFoundation.h>

#include <sys/types.h>

/*
 * Apple's preference file (DABase.h ___PREFS_DEFAULT_DIR). The daemon
 * reads it, never writes it; a missing file means every default.
 */
#define DA_PREFERENCE_FILE \
	"/Library/Preferences/SystemConfiguration/autodiskmount.plist"

/* Apple's keys (DASupport.m). */
#define kDAPreferenceMountDeferExternalKey	CFSTR("DAMountDeferExternal")
#define kDAPreferenceMountDeferInternalKey	CFSTR("DAMountDeferInternal")
#define kDAPreferenceMountDeferRemovableKey	CFSTR("DAMountDeferRemovable")
#define kDAPreferenceMountTrustExternalKey	CFSTR("DAMountTrustExternal")
#define kDAPreferenceMountTrustInternalKey	CFSTR("DAMountTrustInternal")
#define kDAPreferenceMountTrustRemovableKey	CFSTR("DAMountTrustRemovable")
#define kDAPreferenceAutoMountDisableKey	CFSTR("DAAutoMountDisable")
#define kDAPreferenceAutomountWithoutLoginKey	CFSTR("AutomountDisksWithoutUserLogin")

/*
 * NextBSD keys, same file. Apple sets neither option; both are policy for
 * untrusted media (removable, or external), see DAMount.c.
 *   DAMountNoAccessTimeUntrusted  default true:  add noatime
 *   DAMountNoExecuteUntrusted     default false: add noexec
 */
#define kDAPreferenceMountNoAccessTimeUntrustedKey \
	CFSTR("DAMountNoAccessTimeUntrusted")
#define kDAPreferenceMountNoExecuteUntrustedKey \
	CFSTR("DAMountNoExecuteUntrusted")

/*
 * Re-read DA_PREFERENCE_FILE if its mtime changed (Apple gates the same
 * way). Called at startup and at the first stage dispatch after idle.
 */
void		DAPreferenceListRefresh(void);

/* The boolean `key`, or `fallback` when the file does not set it. */
int		DAPreferenceGetBool(CFStringRef key, int fallback);

/*
 * A property-list dictionary read from `path` (NULL if missing, too big or
 * not a dictionary). Caller releases.
 */
CFDictionaryRef	DAPropertyListCreateFromFile(const char *path);

/*
 * Who is at the console (Apple: gDAConsoleUserList from SCDynamicStore,
 * State:/Users/ConsoleUser). Returns 1 and the user's uid/gid when there
 * is one, 0 when nobody is logged in.
 *
 * NextBSD has no console-user source yet (#199). Until then this reports
 * root as the console user, so removable and external media mount when
 * they appear, owned by root. #199 replaces the body with the
 * SCDynamicStore read; DAMount.c's defer branch then takes effect with no
 * other change.
 */
int		DAConsoleUserGet(uid_t *uid, gid_t *gid);

/* --- the /etc/fstab mount map (#196) -------------------------------------- */

/*
 * Apple's gDAMountMapList1: one CFDictionary per /etc/fstab line whose spec
 * is UUID=, LABEL= or DEVICE=, in file order. Keys are Apple's:
 *   DAProbeID         CFUUID (UUID=), CFString (LABEL=) or CFDictionary
 *                     (DEVICE=, an XML property list); always present
 *   DAProbeKind       CFString, the fs_vfstype unless it is "auto"
 *   DAMountAutomatic  CFBoolean, from "auto" / "noauto"; absent if neither
 *   DAMountOptions    CFString, every other fs_mntops token, verbatim
 *   DAMountPath       CFString, fs_file unless it is "none" (Apple keeps a
 *                     CFURL; the mount executor here takes a path)
 * plus one NextBSD key for log lines:
 *   _DAMountMapSpec   CFString, the fs_spec as written ("LABEL=STICK")
 * The list is empty, never NULL, after DAMountMapListRefresh1.
 */
#define kDAMountMapMountAutomaticKey	CFSTR("DAMountAutomatic")
#define kDAMountMapMountOptionsKey	CFSTR("DAMountOptions")
#define kDAMountMapMountPathKey		CFSTR("DAMountPath")
#define kDAMountMapProbeIDKey		CFSTR("DAProbeID")
#define kDAMountMapProbeKindKey		CFSTR("DAProbeKind")
#define kDAMountMapSpecKey		CFSTR("_DAMountMapSpec")

extern CFMutableArrayRef gDAMountMapList1;

/*
 * Apple's DAMountMapListRefresh1: rebuild gDAMountMapList1 from /etc/fstab
 * when the file's mtime changed (a missing file is an empty map and logs
 * nothing). There is no file watch: called at startup and at the first
 * probe after idle, so an edit takes effect at the next disk event.
 */
void		DAMountMapListRefresh1(void);

#endif /* DISKARBITRATION_DASUPPORT_H */
