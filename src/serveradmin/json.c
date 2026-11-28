/*
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * A small JSON writer: the answer is built in memory and written once,
 * so a request that fails half way prints only its error.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "serveradmin.h"

static char	*buf;
static size_t	 len, cap;
static int	 depth;
static bool	 need_comma[64];
static bool	 after_key;

static void
put(const char *s, size_t n)
{
	if (len + n + 1 > cap) {
		cap = (len + n + 1) * 2 + 256;
		if ((buf = realloc(buf, cap)) == NULL) {
			fputs("{\"error\":\"out of memory\",\"code\":\"failed\"}\n", stdout);
			exit(1);
		}
	}
	memcpy(buf + len, s, n);
	len += n;
	buf[len] = '\0';
}

static void
puts_(const char *s)
{
	put(s, strlen(s));
}

/* Before a value: a comma if a sibling came first (not after a key). */
static void
value_start(void)
{
	if (after_key) {
		after_key = false;
		return;
	}
	if (depth > 0 && need_comma[depth])
		put(",", 1);
	if (depth > 0)
		need_comma[depth] = true;
}

static void
open_(const char *c)
{
	value_start();
	puts_(c);
	if (depth < 63)
		depth++;
	need_comma[depth] = false;
}

void j_obj(void) { open_("{"); }
void j_arr(void) { open_("["); }
void j_end(void) { if (depth > 0) depth--; puts_("}"); }
void j_aend(void) { if (depth > 0) depth--; puts_("]"); }

void
j_strn(const char *s, size_t n)
{
	char esc[8];
	size_t i;

	value_start();
	put("\"", 1);
	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '"' || c == '\\') {
			esc[0] = '\\'; esc[1] = (char)c;
			put(esc, 2);
		} else if (c == '\n')
			puts_("\\n");
		else if (c == '\t')
			puts_("\\t");
		else if (c == '\r')
			puts_("\\r");
		else if (c < 0x20) {
			snprintf(esc, sizeof(esc), "\\u%04x", c);
			puts_(esc);
		} else
			put((const char *)&s[i], 1);
	}
	put("\"", 1);
}

void j_str(const char *s) { if (s == NULL) j_null(); else j_strn(s, strlen(s)); }

void
j_key(const char *k)
{
	j_str(k);
	put(":", 1);
	after_key = true;
}

void
j_int(int64_t v)
{
	char t[32];

	value_start();
	snprintf(t, sizeof(t), "%lld", (long long)v);
	puts_(t);
}

void
j_dbl(double v)
{
	char t[48];

	value_start();
	snprintf(t, sizeof(t), "%.3f", v);
	puts_(t);
}

void j_bool(bool v) { value_start(); puts_(v ? "true" : "false"); }
void j_null(void) { value_start(); puts_("null"); }
void j_kstr(const char *k, const char *v) { j_key(k); j_str(v); }
void j_kint(const char *k, int64_t v) { j_key(k); j_int(v); }
void j_kbool(const char *k, bool v) { j_key(k); j_bool(v); }

void
j_flush(void)
{
	if (buf != NULL) {
		(void)write(STDOUT_FILENO, buf, len);
		(void)write(STDOUT_FILENO, "\n", 1);
	}
	len = 0;
	depth = 0;
	after_key = false;
}

void (*sa_on_fail)(void);

void
sa_fail(const char *code, const char *fmt, ...)
{
	char msg[2048];
	va_list ap;
	void (*undo)(void) = sa_on_fail;

	sa_on_fail = NULL;
	if (undo != NULL)
		undo();

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	len = 0;
	depth = 0;
	after_key = false;
	j_obj();
	j_kstr("error", msg);
	j_kstr("code", code);
	j_end();
	j_flush();
	exit(strcmp(code, "usage") == 0 ? 2 : 1);
}
