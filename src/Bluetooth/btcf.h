/*
 * btcf.h — CoreFoundation helpers shared by bluetoothd's CF modules
 * (btstore.c, keystore.c, btstate.c).
 *
 * CF stays inside those modules, as wland keeps it in airport.c; the engines
 * and the policy layer are plain C. Strings are built with
 * CFStringCreateWithCString rather than CFSTR() wherever the text is not a
 * literal, as airport.c does.
 */
#ifndef _BLUETOOTHD_BTCF_H_
#define _BLUETOOTHD_BTCF_H_

#include <SystemConfiguration/SCPreferences.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

CFStringRef	btcf_str(const char *s);

/* Setters: a NULL or empty value removes nothing and sets nothing. */
void	btcf_set_str(CFMutableDictionaryRef d, CFStringRef key, const char *v);
void	btcf_set_int(CFMutableDictionaryRef d, CFStringRef key, long long v);
void	btcf_set_bool(CFMutableDictionaryRef d, CFStringRef key, bool v);
void	btcf_set_data(CFMutableDictionaryRef d, CFStringRef key,
	    const uint8_t *p, size_t len);

/* Getters: type-checked; return false (and leave *out) on a missing or
 * mistyped value. */
bool	btcf_get_str(CFDictionaryRef d, CFStringRef key, char *out, size_t len);
bool	btcf_get_int(CFDictionaryRef d, CFStringRef key, long long *out);
bool	btcf_get_bool(CFDictionaryRef d, CFStringRef key, bool *out);
/* Copies at most `len` bytes; *got is the stored length. */
bool	btcf_get_data(CFDictionaryRef d, CFStringRef key, uint8_t *out,
	    size_t len, size_t *got);
CFDictionaryRef	btcf_get_dict(CFDictionaryRef d, CFStringRef key);

/*
 * Open the SCPreferences file at `path`. SCPreferences takes a prefsID with a
 * '/' as the path itself, so the file opened and the file chmod'ed after a
 * commit are named by the same string. NULL on failure.
 */
SCPreferencesRef	btcf_prefs_open(const char *path);

/*
 * A mutable copy of top-level dictionary `key` in `prefs`, or a new empty one
 * when it is missing or not a dictionary. Caller releases.
 */
CFMutableDictionaryRef	btcf_prefs_copy_dict(SCPreferencesRef prefs,
			    CFStringRef key);

/*
 * Commit `prefs` and set `path` to `mode`. SCPreferences writes a temp file
 * and renames it into place, so the mode is set after every commit: a rename
 * yields a new inode. 0 on success.
 */
int	btcf_prefs_commit(SCPreferencesRef prefs, const char *path, mode_t mode);

#endif /* _BLUETOOTHD_BTCF_H_ */
