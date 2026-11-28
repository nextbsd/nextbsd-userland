/*
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Every argument serveradmin passes to a tool is checked here first.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "serveradmin.h"

bool
sa_is_label(const char *s)
{
	size_t n = strlen(s);

	if (n < 1 || n > 63 || s[0] == '-' || s[n - 1] == '-')
		return (false);
	for (size_t i = 0; i < n; i++)
		if (!(islower((unsigned char)s[i]) ||
		    isdigit((unsigned char)s[i]) || s[i] == '-'))
			return (false);
	return (true);
}

bool
sa_is_user(const char *s)
{
	size_t n = strlen(s);

	if (n < 1 || n > 32 || !(islower((unsigned char)s[0]) || s[0] == '_'))
		return (false);
	for (size_t i = 0; i < n; i++)
		if (!(islower((unsigned char)s[i]) ||
		    isdigit((unsigned char)s[i]) || s[i] == '-' ||
		    s[i] == '_' || s[i] == '.'))
			return (false);
	return (true);
}

/* zpool(8)'s rules, narrowed: a letter first, then letters, digits, '-',
 * '_' and '.'; not a reserved word. */
bool
sa_is_pool(const char *s)
{
	static const char *const reserved[] = {
		"mirror", "raidz", "raidz1", "raidz2", "raidz3", "draid",
		"spare", "log", "cache", "special", "dedup", NULL
	};
	size_t n = strlen(s);

	if (n < 1 || n > 64 || !isalpha((unsigned char)s[0]))
		return (false);
	for (size_t i = 0; i < n; i++)
		if (!(isalnum((unsigned char)s[i]) || s[i] == '-' ||
		    s[i] == '_' || s[i] == '.'))
			return (false);
	for (int i = 0; reserved[i] != NULL; i++)
		if (strcmp(s, reserved[i]) == 0)
			return (false);
	return (true);
}

bool
sa_is_be(const char *s)
{
	size_t n = strlen(s);

	if (n < 1 || n > 63)
		return (false);
	for (size_t i = 0; i < n; i++)
		if (!(isalnum((unsigned char)s[i]) || s[i] == '-' ||
		    s[i] == '_' || s[i] == '.'))
			return (false);
	return (true);
}

bool
sa_is_onoff(const char *s, bool *v)
{
	if (strcmp(s, "on") == 0)
		*v = true;
	else if (strcmp(s, "off") == 0)
		*v = false;
	else
		return (false);
	return (true);
}

bool
sa_parse_size(const char *s, uint64_t *v)
{
	char *end;
	unsigned long long n;
	uint64_t m = 1;

	if (!isdigit((unsigned char)s[0]))
		return (false);
	n = strtoull(s, &end, 10);
	switch (*end) {
	case '\0': break;
	case 'K': case 'k': m = 1ULL << 10; end++; break;
	case 'M': case 'm': m = 1ULL << 20; end++; break;
	case 'G': case 'g': m = 1ULL << 30; end++; break;
	case 'T': case 't': m = 1ULL << 40; end++; break;
	default: return (false);
	}
	if (*end != '\0' || n == 0 || n > (UINT64_MAX / m))
		return (false);
	*v = (uint64_t)n * m;
	return (true);
}
