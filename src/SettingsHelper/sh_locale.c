/*
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * sh_locale.c — SetSystemLocale: Language & Region.app's operation
 * (nextbsd/nextbsd#652).
 *
 * The system's language is the default login class's `lang` (and
 * `charset`, the locale's codeset) in /etc/login.conf, which every
 * account without a language of its own gets at login: setusercontext(3)
 * sets LANG and MM_CHARSET from it, in login(1), sshd and loginwindow
 * alike, and a user's ~/.login_conf wins over it. GNUstep apps take
 * their languages from LANG when the user has set none (NSLanguages).
 *
 * Only those two capabilities of the `default` record change; the rest
 * of the file, comments and layout included, is kept. Then cap_mkdb(1)
 * rebuilds login.conf.db, which is what getcap(3) reads first; if it
 * fails, the old file is put back so the two stay in step.
 */
#include <sys/types.h>
#include <sys/stat.h>

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SettingsHelper.h"

#define	LOGIN_CONF	"/etc/login.conf"
#define	LOCALE_DIR	"/usr/share/locale"
#define	CAP_MKDB	"/usr/bin/cap_mkdb"
#define	RECORD		"default"

/*
 * A locale name as setlocale(3) takes it ("de_DE.UTF-8", "C.UTF-8"):
 * letters, digits, '_', '.', '-' and '@', starting with a letter, and
 * one the system has, "C" and "POSIX" being built in. Nothing here is
 * a getcap(3) separator, so it can't add a capability, and no '/', so
 * the name can't leave LOCALE_DIR.
 */
static bool
locale_name_ok(const char *locale)
{
	char path[PATH_MAX];
	struct stat st;
	const char *p;

	if (!isalpha((unsigned char)locale[0]) ||
	    strlen(locale) > SH_LOCALE_MAX)
		return (false);
	for (p = locale; *p != '\0'; p++)
		if (!isalnum((unsigned char)*p) && *p != '_' && *p != '.' &&
		    *p != '-' && *p != '@')
			return (false);
	if (strcmp(locale, "C") == 0 || strcmp(locale, "POSIX") == 0)
		return (true);
	(void)snprintf(path, sizeof(path), "%s/%s", LOCALE_DIR, locale);
	return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

/* Is the line starting at `line` the first line of record `name`? */
static bool
record_starts(const char *line, const char *name)
{
	size_t n = strlen(name);
	const char *p = line;

	if (*p == '#' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\0')
		return (false);
	/* The names, separated by '|', end at the first ':'. */
	while (*p != '\0' && *p != '\n' && *p != ':') {
		if (strncmp(p, name, n) == 0 &&
		    (p[n] == '|' || p[n] == ':'))
			return (true);
		while (*p != '\0' && *p != '\n' && *p != ':' && *p != '|')
			p++;
		if (*p == '|')
			p++;
	}
	return (false);
}

/* The end of the line at p, past its newline. */
static const char *
line_end(const char *p)
{
	const char *nl = strchr(p, '\n');

	return (nl != NULL ? nl + 1 : p + strlen(p));
}

/* Does the line [p, end) continue the record (end in a backslash)? */
static bool
continues(const char *p, const char *end)
{
	while (end > p && (end[-1] == '\n' || end[-1] == '\r'))
		end--;
	return (end > p && end[-1] == '\\');
}

/*
 * Set capability `cap` to `value` in record `name` of `text`: replace its
 * value where it is, or add it on a line of its own after the record's
 * first line. Returns the new text (to free), or NULL when there's no
 * such record or no memory.
 */
static char *
cap_set(const char *text, const char *name, const char *cap,
    const char *value)
{
	char key[32], *out;
	const char *rec, *rec_end, *first_end, *p, *found = NULL;
	size_t keylen, outlen;

	(void)snprintf(key, sizeof(key), ":%s=", cap);
	keylen = strlen(key);
	for (rec = text; *rec != '\0'; rec = line_end(rec))
		if (record_starts(rec, name))
			break;
	if (*rec == '\0')
		return (NULL);
	first_end = line_end(rec);
	for (p = rec, rec_end = first_end; ; p = rec_end) {
		rec_end = line_end(p);
		if (found == NULL) {
			const char *k = strstr(p, key);

			if (k != NULL && k < rec_end)
				found = k;
		}
		if (!continues(p, rec_end) || *rec_end == '\0')
			break;
	}

	outlen = strlen(text) + strlen(cap) + strlen(value) + 8;
	if ((out = malloc(outlen)) == NULL)
		return (NULL);
	if (found != NULL) {
		const char *v = found + keylen, *v_end = v;

		while (*v_end != ':' && *v_end != '\n' && *v_end != '\0')
			v_end++;
		(void)snprintf(out, outlen, "%.*s%s%s", (int)(v - text), text,
		    value, v_end);
	} else if (continues(rec, first_end)) {
		(void)snprintf(out, outlen, "%.*s\t:%s=%s:\\\n%s",
		    (int)(first_end - text), text, cap, value, first_end);
	} else {
		/* A one-line record: "default:...:"; add after the names. */
		p = strchr(rec, ':');
		(void)snprintf(out, outlen, "%.*s%s=%s:%s",
		    (int)(p + 1 - text), text, cap, value, p + 1);
	}
	return (out);
}

static bool
cap_mkdb(void)
{
	const char *argv[] = { CAP_MKDB, LOGIN_CONF, NULL };

	return (sh_run(argv));
}

int
sh_locale_set(const char *locale)
{
	char *old, *with_lang, *new;
	const char *codeset;
	size_t len;

	if (!locale_name_ok(locale))
		return (SH_ERR_INVALID);
	if ((old = sh_read_file(LOGIN_CONF, &len)) == NULL)
		return (SH_ERR_FAILED);
	with_lang = cap_set(old, RECORD, "lang", locale);
	if (with_lang == NULL) {
		sh_log("%s: no \"%s\" class", LOGIN_CONF, RECORD);
		free(old);
		return (SH_ERR_FAILED);
	}
	/* The codeset, "UTF-8" of "de_DE.UTF-8"; "C" and "POSIX" have
	 * none, and charset stays as it is. */
	codeset = strchr(locale, '.');
	if (codeset != NULL) {
		new = cap_set(with_lang, RECORD, "charset", codeset + 1);
		free(with_lang);
		if (new == NULL) {
			free(old);
			return (SH_ERR_FAILED);
		}
	} else
		new = with_lang;

	if (strcmp(new, old) == 0) {
		free(new);
		free(old);
		return (SH_OK);
	}
	if (!sh_write_file(LOGIN_CONF, new, strlen(new))) {
		free(new);
		free(old);
		return (SH_ERR_FAILED);
	}
	if (!cap_mkdb()) {
		sh_log("cap_mkdb %s failed; putting the old file back",
		    LOGIN_CONF);
		if (sh_write_file(LOGIN_CONF, old, len))
			(void)cap_mkdb();
		free(new);
		free(old);
		return (SH_ERR_FAILED);
	}
	free(new);
	free(old);
	return (SH_OK);
}
