/*
 * nextbsd_hostname_synth.h — the single SMBIOS hostname synthesis used by
 * both launchd (PID 1 early-init, launchd_early.c) and hostnamed
 * (freebsd_synthesize_hostname, shim.c).
 *
 * Header-only, libc-only (static inline) on purpose: PID 1 must not pick up
 * another shared library just for this, and hostnamed builds in a different
 * link context. Both already have src/launchd/freebsd-shims on their include
 * path. Keeping one copy is what keeps the early hostname (the one getty's
 * first login banner caches) identical to the one hostnamed publishes later.
 * The two used to be duplicated and drifted (nextbsd/nextbsd#325).
 *
 * Algorithm:
 *   1. smbios.system.version, if it is "identifying" (contains a letter —
 *      e.g. "ThinkPad T460s", not VirtualBox's bare revision "1.2"), is used
 *      bare: "ThinkPad-T460s".
 *   2. Otherwise smbios.system.product ("VirtualBox"), else "nextbsd". These
 *      are generic, so the first group of smbios.system.uuid is appended
 *      when present and non-zero: "VirtualBox-68f9a871".
 */
#ifndef _NEXTBSD_HOSTNAME_SYNTH_H_
#define _NEXTBSD_HOSTNAME_SYNTH_H_

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#ifdef __FreeBSD__
#include <kenv.h>
#endif

#define NBHS_SLUG_MAX		40
#define NBHS_NAME_MAX		63	/* DNS label limit, excluding NUL */
#define NBHS_LAST_RESORT	"nextbsd"

/* Collapse runs of non-alphanumerics to one '-', trim edge dashes, and cap
 * at NBHS_SLUG_MAX characters. Returns the resulting length. */
static inline size_t
nbhs_sanitize_slug(char *s)
{
	size_t i, j;
	int prev_dash;

	if (s == NULL)
		return (0);
	j = 0;
	prev_dash = 1;
	for (i = 0; s[i] != '\0' && j < NBHS_SLUG_MAX; i++) {
		unsigned char c = (unsigned char)s[i];
		if (isalnum(c)) {
			s[j++] = (char)c;
			prev_dash = 0;
		} else if (!prev_dash) {
			s[j++] = '-';
			prev_dash = 1;
		}
	}
	while (j > 0 && s[j - 1] == '-')
		j--;
	s[j] = '\0';
	return (j);
}

/*
 * An SMBIOS field is "identifying" only if it carries at least one letter.
 * Real OEMs put the model name in smbios.system.version, but synthetic
 * firmware such as VirtualBox stores the bare SMBIOS table revision there
 * ("1.2"), which would sanitize to the useless slug "1-2".
 */
static inline int
nbhs_value_is_identifying(const char *raw)
{
	size_t i;

	if (raw == NULL)
		return (0);
	for (i = 0; raw[i] != '\0'; i++) {
		if (isalpha((unsigned char)raw[i]))
			return (1);
	}
	return (0);
}

static inline int
nbhs_try_slug(const char *raw, char *out, size_t outsz)
{
	if (raw == NULL || !nbhs_value_is_identifying(raw))
		return (0);
	(void)strncpy(out, raw, outsz - 1);
	out[outsz - 1] = '\0';
	(void)nbhs_sanitize_slug(out);
	return (out[0] != '\0');
}

/* First UUID group as lowercase hex ("68F9A871-..." -> "68f9a871"); empty
 * when absent or all-zero (some firmware reports 00000000-...). */
static inline size_t
nbhs_uuid_suffix(const char *uuid, char *out, size_t outsz)
{
	size_t i, j;

	if (outsz == 0)
		return (0);
	out[0] = '\0';
	if (uuid == NULL)
		return (0);
	j = 0;
	for (i = 0; uuid[i] != '\0' && uuid[i] != '-' && j < 8 &&
	    j < outsz - 1; i++) {
		unsigned char c = (unsigned char)uuid[i];
		if (isxdigit(c))
			out[j++] = (char)tolower(c);
	}
	out[j] = '\0';
	if (j > 0 && strspn(out, "0") == j)
		out[0] = '\0';
	return (strlen(out));
}

/*
 * Pure synthesis from the three SMBIOS values (any may be NULL). Writes a
 * NUL-terminated hostname of at most NBHS_NAME_MAX characters into out.
 * Kept free of kenv so it can be unit-tested off-target
 * (tests/test_hostname_synth.c).
 */
static inline void
nbhs_synthesize_from(const char *version, const char *product,
    const char *uuid, char *out, size_t outsz)
{
	char slug[NBHS_SLUG_MAX + 1];
	char name[NBHS_NAME_MAX + 1];
	char suffix[16];
	size_t len;

	if (out == NULL || outsz == 0)
		return;
	if (nbhs_try_slug(version, slug, sizeof(slug))) {
		/* Real model name: unique enough to use bare. */
		(void)snprintf(name, sizeof(name), "%s", slug);
	} else {
		/* Generic product or last resort: add a per-machine suffix so
		 * every guest of one hypervisor doesn't share a hostname. */
		if (!nbhs_try_slug(product, slug, sizeof(slug)))
			(void)snprintf(slug, sizeof(slug), "%s",
			    NBHS_LAST_RESORT);
		(void)snprintf(name, sizeof(name), "%s", slug);
		if (nbhs_uuid_suffix(uuid, suffix, sizeof(suffix)) > 0) {
			len = strlen(name);
			(void)snprintf(name + len, sizeof(name) - len, "-%s",
			    suffix);
		}
	}
	(void)snprintf(out, outsz, "%s", name);
}

#ifdef __FreeBSD__
static inline const char *
nbhs_read_kenv(const char *key, char *buf, size_t bufsz)
{
	int n;

	if (bufsz == 0 || bufsz > INT_MAX)
		return (NULL);
	n = kenv(KENV_GET, key, buf, (int)bufsz);
	if (n <= 0)
		return (NULL);
	buf[bufsz - 1] = '\0';
	return (buf);
}

/* Synthesize the hostname from this machine's SMBIOS kenv values. */
static inline void
nbhs_synthesize(char *out, size_t outsz)
{
	char version[256], product[256], uuid[256];

	nbhs_synthesize_from(
	    nbhs_read_kenv("smbios.system.version", version, sizeof(version)),
	    nbhs_read_kenv("smbios.system.product", product, sizeof(product)),
	    nbhs_read_kenv("smbios.system.uuid", uuid, sizeof(uuid)),
	    out, outsz);
}
#endif /* __FreeBSD__ */

#endif /* _NEXTBSD_HOSTNAME_SYNTH_H_ */
