/*
 * DASupport.h — daemon preferences and support routines (E15 C5,
 * nextbsd-userland#195).
 *
 * The C counterpart of the preference half of Apple's DASupport.m
 * (DAPreferenceListRefresh, the kDAPreference* keys), the /etc/fstab mount
 * map (#196), which Apple keeps in DASupport.m too, and the console user
 * (#199), which Apple keeps in DAMain.c and DABase.c.
 */
#ifndef DISKARBITRATION_DASUPPORT_H
#define DISKARBITRATION_DASUPPORT_H

#include <CoreFoundation/CoreFoundation.h>

#include <SystemConfiguration/SCDynamicStore.h>

#include <dispatch/dispatch.h>

#include <sys/types.h>

struct da_disk;

/*
 * Apple's preference file, autodiskmount.plist in SCPreferences'
 * default directory (___PREFS_DEFAULT_DIR), which on NextBSD is the Local
 * domain's /Local/Library/Preferences/SystemConfiguration (libSystem
 * Configuration's PREFS_DEFAULT_DIR; NextBSD has no bare /Library). The
 * daemon reads it, never writes it; a missing file means every Apple
 * default. The base system ships one that sets
 * AutomountDisksWithoutUserLogin (#199): nothing publishes the console
 * user yet, so removable and external media mount without a login.
 */
#define DA_PREFERENCE_FILE \
	"/Local/Library/Preferences/SystemConfiguration/autodiskmount.plist"

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

/* --- the console user (#199) --------------------------------------------- */

/*
 * Apple's console-user globals (DAMain.c), from configd's
 * State:/Users/ConsoleUser: the user's name (NULL when nobody is logged
 * in, or when the name is "loginwindow"), uid and gid (root:wheel without
 * a user), and the console sessions whose login is done (NULL when there
 * are none). gDAConsoleUserList is the "somebody is logged in" test the
 * mount deferral uses, as on Apple.
 */
extern CFStringRef	gDAConsoleUser;
extern uid_t		gDAConsoleUserUID;
extern gid_t		gDAConsoleUserGID;
extern CFArrayRef	gDAConsoleUserList;

/*
 * Open an SCDynamicStore session as Apple's DAMain.c does, read the
 * console user once, and watch State:/Users/ConsoleUser on `queue`;
 * each change runs _DAConfigurationCallback (DAServer.c). If configd
 * cannot be reached the daemon carries on with nobody logged in and
 * retries every few seconds; once it gets through, the callback runs
 * for whatever it finds. Call once, before the first disk is staged.
 */
void		DAConsoleUserInit(dispatch_queue_t queue);

/*
 * Apple's DABase.c wrappers: the console user (NULL for "loginwindow")
 * with uid/gid defaulting to root:wheel, and the sessions whose login is
 * done, synthesized from the user name when the publisher sent none.
 */
CFStringRef	___SCDynamicStoreCopyConsoleUser(SCDynamicStoreRef store,
		    uid_t *uid, gid_t *gid);
CFArrayRef	___SCDynamicStoreCopyConsoleInformation(SCDynamicStoreRef store);

/*
 * Returns 1 and the console user's uid/gid when somebody is logged in
 * (gDAConsoleUserList is set), 0 and root:wheel otherwise.
 */
int		DAConsoleUserGet(uid_t *uid, gid_t *gid);

/*
 * Whether `disk` is media the console user may own and act on: its whole
 * disk is removable, or not internal (DADeviceInternal false or unknown,
 * the media Apple defers until a login), and no volume of that whole disk
 * is a system mount (kDADiskStateMountSystem). Mount-only disks never
 * are.
 */
int		DAConsoleUserMayOwn(const struct da_disk *disk);

/*
 * Hand media DAConsoleUserMayOwn accepts to the console user (the NextBSD
 * counterpart of Apple's /dev node chown at login and logout):
 *   - a whole disk belongs to the console user, or to root when nobody is
 *     logged in;
 *   - a mounted volume that is root's (it mounted while nobody was logged
 *     in) becomes the console user's. Its files keep the owner they were
 *     mounted with until it is mounted again.
 * Other disks are left alone; a volume's owner is otherwise set when it
 * mounts (DAMount.c).
 */
void		DAConsoleUserClaim(struct da_disk *disk);

/* One DA-CONSOLE line with the console user and the session count. */
void		DAConsoleUserLog(const char *why);

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
