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
 * Algorithm (no table of vendors or machines; every rule is about the shape
 * of the value the firmware reports):
 *   1. Use smbios.system.product, unless it has no plain word in it -- only a
 *      part number such as Lenovo's "20F9CTO1WW" -- and then use
 *      smbios.system.version ("ThinkPad T460s").
 *   2. Split it into words and keep them up to and including the first
 *      model number (a word with a digit in it), then stop: "Raspberry Pi
 *      500 Rev 1.0" is "Raspberry-Pi-500". A short bare number followed by a
 *      longer model number is a size, not the model, so "XPS 13 9320" is
 *      "XPS-9320". A name with no number keeps at most NBHS_WORDS_MAX words.
 *   3. The firmware's capitalisation is kept and nothing is appended: two
 *      machines of one model on a network are told apart by mDNSResponder's
 *      own conflict renaming, as on macOS.
 *   4. No usable value at all: "NextBSD".
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
#define NBHS_WORDS_MAX		3
#define NBHS_LAST_RESORT	"NextBSD"

/* A word: a run of letters and digits. */
struct nbhs_word {
	const char	*p;
	size_t		 len;
	int		 letters;
	int		 digits;
};

#define NBHS_WORDS_SCAN		16

static inline size_t
nbhs_split(const char *raw, struct nbhs_word *w, size_t max)
{
	size_t n = 0, i;

	if (raw == NULL)
		return (0);
	while (*raw != '\0' && n < max) {
		if (!isalnum((unsigned char)*raw)) {
			raw++;
			continue;
		}
		w[n].p = raw;
		w[n].letters = w[n].digits = 0;
		for (i = 0; isalnum((unsigned char)raw[i]); i++) {
			if (isdigit((unsigned char)raw[i]))
				w[n].digits++;
			else
				w[n].letters++;
		}
		w[n].len = i;
		raw += i;
		n++;
	}
	return (n);
}

/* Has a plain word (letters only), so it names something. */
static inline int
nbhs_has_word(const struct nbhs_word *w, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (w[i].letters > 0 && w[i].digits == 0)
			return (1);
	return (0);
}

/*
 * Build the name from one firmware value. Returns its length, 0 when the
 * value has no letters at all: VirtualBox reports the bare table revision
 * "1.2" as its version, which names nothing.
 */
static inline size_t
nbhs_name_from(const char *raw, char *out, size_t outsz)
{
	struct nbhs_word w[NBHS_WORDS_SCAN];
	size_t n, i, len = 0, kept = 0;

	if (outsz == 0)
		return (0);
	out[0] = '\0';
	n = nbhs_split(raw, w, NBHS_WORDS_SCAN);
	for (i = 0; i < n && w[i].letters == 0; i++)
		;
	if (i == n)
		return (0);
	for (i = 0; i < n; i++) {
		int model = w[i].digits > 0;

		/*
		 * A bare number of one or two digits right before a longer
		 * model number ("13" in "XPS 13 9320") is a size: skip it.
		 */
		if (model && w[i].letters == 0 && w[i].len <= 2 &&
		    i + 1 < n && w[i + 1].digits > 0 &&
		    w[i + 1].len > w[i].len)
			continue;
		if (len + (len > 0) + w[i].len > NBHS_SLUG_MAX ||
		    len + (len > 0) + w[i].len >= outsz)
			break;
		if (len > 0)
			out[len++] = '-';
		(void)memcpy(out + len, w[i].p, w[i].len);
		len += w[i].len;
		out[len] = '\0';
		if (model || ++kept == NBHS_WORDS_MAX)
			break;
	}
	return (len);
}

/*
 * Pure synthesis from the SMBIOS values (any may be NULL). Writes a
 * NUL-terminated hostname of at most NBHS_NAME_MAX characters into out.
 * Kept free of kenv so it can be unit-tested off-target
 * (tests/test_hostname_synth.c).
 */
static inline void
nbhs_synthesize_from(const char *version, const char *product, char *out,
    size_t outsz)
{
	struct nbhs_word w[NBHS_WORDS_SCAN];
	char name[NBHS_NAME_MAX + 1];
	const char *pick = product;

	if (out == NULL || outsz == 0)
		return;
	if (!nbhs_has_word(w, nbhs_split(product, w, NBHS_WORDS_SCAN)) &&
	    nbhs_has_word(w, nbhs_split(version, w, NBHS_WORDS_SCAN)))
		pick = version;
	if (nbhs_name_from(pick, name, sizeof(name)) == 0 &&
	    nbhs_name_from(pick == product ? version : product, name,
	    sizeof(name)) == 0)
		(void)snprintf(name, sizeof(name), "%s", NBHS_LAST_RESORT);
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
	char version[256], product[256];

	nbhs_synthesize_from(
	    nbhs_read_kenv("smbios.system.version", version, sizeof(version)),
	    nbhs_read_kenv("smbios.system.product", product, sizeof(product)),
	    out, outsz);
}
#endif /* __FreeBSD__ */

#endif /* _NEXTBSD_HOSTNAME_SYNTH_H_ */
