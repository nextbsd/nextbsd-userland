/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Joseph Maloney
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "plist.h"

#define PL_MAX_DEPTH	32

struct parser {
	const char	*p;
	const char	*end;
	int		 depth;
};

static struct pl_node *parse_value(struct parser *ps);

static struct pl_node *
node_new(enum pl_type type)
{
	struct pl_node *n;

	n = calloc(1, sizeof(*n));
	if (n != NULL)
		n->type = type;
	return (n);
}

void
pl_free(struct pl_node *node)
{
	size_t i;

	if (node == NULL)
		return;
	for (i = 0; i < node->nchildren; i++)
		pl_free(node->children[i]);
	free(node->children);
	free(node->key);
	free(node->str);
	free(node);
}

static int
node_append(struct pl_node *parent, struct pl_node *child)
{
	struct pl_node **nc;
	size_t cap;

	if ((parent->nchildren & (parent->nchildren + 1)) == 0) {
		/* grow at 0, 1, 3, 7, ... */
		cap = parent->nchildren * 2 + 1;
		nc = realloc(parent->children, cap * sizeof(*nc));
		if (nc == NULL)
			return (-1);
		parent->children = nc;
	}
	parent->children[parent->nchildren++] = child;
	return (0);
}

static void
skip_ws(struct parser *ps)
{
	while (ps->p < ps->end && isspace((unsigned char)*ps->p))
		ps->p++;
}

static int
starts_with(const struct parser *ps, const char *s)
{
	size_t n = strlen(s);

	return ((size_t)(ps->end - ps->p) >= n && memcmp(ps->p, s, n) == 0);
}

/* Skip to just past the next occurrence of `s`. */
static int
skip_past(struct parser *ps, const char *s)
{
	size_t n = strlen(s);
	const char *q;

	for (q = ps->p; q + n <= ps->end; q++) {
		if (memcmp(q, s, n) == 0) {
			ps->p = q + n;
			return (0);
		}
	}
	return (-1);
}

/*
 * Skip the XML declaration, comments, the DOCTYPE and whitespace that may
 * sit between elements.
 */
static int
skip_misc(struct parser *ps)
{
	for (;;) {
		skip_ws(ps);
		if (starts_with(ps, "<?")) {
			if (skip_past(ps, "?>") == -1)
				return (-1);
		} else if (starts_with(ps, "<!--")) {
			if (skip_past(ps, "-->") == -1)
				return (-1);
		} else if (starts_with(ps, "<!")) {
			if (skip_past(ps, ">") == -1)
				return (-1);
		} else {
			return (0);
		}
	}
}

/*
 * Read an opening tag at ps->p: "<name ...>" or "<name .../>". Stores the
 * name (NUL-terminated, at most namelen-1 bytes) and whether it was
 * self-closing. Leaves ps->p just past the '>'.
 */
static int
read_open_tag(struct parser *ps, char *name, size_t namelen, int *selfclose)
{
	size_t i = 0;

	if (ps->p >= ps->end || *ps->p != '<')
		return (-1);
	ps->p++;
	while (ps->p < ps->end && !isspace((unsigned char)*ps->p) &&
	    *ps->p != '>' && *ps->p != '/') {
		if (i + 1 >= namelen)
			return (-1);
		name[i++] = *ps->p++;
	}
	name[i] = '\0';
	if (i == 0)
		return (-1);
	*selfclose = 0;
	/* attributes are not used by plists; skip to the end of the tag */
	while (ps->p < ps->end && *ps->p != '>') {
		if (*ps->p == '/' && ps->p + 1 < ps->end && ps->p[1] == '>')
			*selfclose = 1;
		ps->p++;
	}
	if (ps->p >= ps->end)
		return (-1);
	ps->p++;		/* '>' */
	return (0);
}

/* Expect "</name>" at ps->p (after optional whitespace). */
static int
read_close_tag(struct parser *ps, const char *name)
{
	size_t n = strlen(name);

	skip_ws(ps);
	if (!starts_with(ps, "</"))
		return (-1);
	ps->p += 2;
	if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, name, n) != 0)
		return (-1);
	ps->p += n;
	skip_ws(ps);
	if (ps->p >= ps->end || *ps->p != '>')
		return (-1);
	ps->p++;
	return (0);
}

static int
append_char(char **buf, size_t *len, size_t *cap, char c)
{
	char *nb;

	if (*len + 1 >= *cap) {
		*cap = *cap ? *cap * 2 : 32;
		nb = realloc(*buf, *cap);
		if (nb == NULL)
			return (-1);
		*buf = nb;
	}
	(*buf)[(*len)++] = c;
	(*buf)[*len] = '\0';
	return (0);
}

static int
append_utf8(char **buf, size_t *len, size_t *cap, unsigned long cp)
{
	unsigned char out[4];
	int n, i;

	if (cp < 0x80) {
		out[0] = (unsigned char)cp; n = 1;
	} else if (cp < 0x800) {
		out[0] = 0xc0 | (cp >> 6);
		out[1] = 0x80 | (cp & 0x3f); n = 2;
	} else if (cp < 0x10000) {
		out[0] = 0xe0 | (cp >> 12);
		out[1] = 0x80 | ((cp >> 6) & 0x3f);
		out[2] = 0x80 | (cp & 0x3f); n = 3;
	} else if (cp < 0x110000) {
		out[0] = 0xf0 | (cp >> 18);
		out[1] = 0x80 | ((cp >> 12) & 0x3f);
		out[2] = 0x80 | ((cp >> 6) & 0x3f);
		out[3] = 0x80 | (cp & 0x3f); n = 4;
	} else {
		return (-1);
	}
	for (i = 0; i < n; i++)
		if (append_char(buf, len, cap, (char)out[i]) == -1)
			return (-1);
	return (0);
}

/*
 * Read text content up to the next '<', decoding the five XML entities
 * and numeric character references. Returns a malloc'd string ("" when
 * empty).
 */
static char *
read_text(struct parser *ps)
{
	char *buf = NULL;
	size_t len = 0, cap = 0;
	const char *semi;
	unsigned long cp;
	char *ep;

	if (append_char(&buf, &len, &cap, '\0') == -1)
		return (NULL);
	len = 0;
	while (ps->p < ps->end && *ps->p != '<') {
		if (*ps->p != '&') {
			if (append_char(&buf, &len, &cap, *ps->p++) == -1)
				goto fail;
			continue;
		}
		semi = memchr(ps->p, ';', ps->end - ps->p);
		if (semi == NULL || semi - ps->p > 10)
			goto fail;
		if (starts_with(ps, "&amp;"))
			cp = '&';
		else if (starts_with(ps, "&lt;"))
			cp = '<';
		else if (starts_with(ps, "&gt;"))
			cp = '>';
		else if (starts_with(ps, "&quot;"))
			cp = '"';
		else if (starts_with(ps, "&apos;"))
			cp = '\'';
		else if (starts_with(ps, "&#x") || starts_with(ps, "&#X")) {
			errno = 0;
			cp = strtoul(ps->p + 3, &ep, 16);
			if (ep != semi || errno != 0)
				goto fail;
		} else if (starts_with(ps, "&#")) {
			errno = 0;
			cp = strtoul(ps->p + 2, &ep, 10);
			if (ep != semi || errno != 0)
				goto fail;
		} else
			goto fail;
		if (append_utf8(&buf, &len, &cap, cp) == -1)
			goto fail;
		ps->p = semi + 1;
	}
	return (buf);
fail:
	free(buf);
	return (NULL);
}

static struct pl_node *
parse_dict(struct parser *ps)
{
	struct pl_node *dict, *val;
	char tag[16];
	char *key;
	int selfclose;

	dict = node_new(PL_DICT);
	if (dict == NULL)
		return (NULL);
	for (;;) {
		if (skip_misc(ps) == -1)
			goto fail;
		if (starts_with(ps, "</dict"))
			break;
		if (read_open_tag(ps, tag, sizeof(tag), &selfclose) == -1 ||
		    strcmp(tag, "key") != 0)
			goto fail;
		if (selfclose)
			key = strdup("");
		else {
			key = read_text(ps);
			if (key == NULL || read_close_tag(ps, "key") == -1) {
				free(key);
				goto fail;
			}
		}
		if (key == NULL)
			goto fail;
		val = parse_value(ps);
		if (val == NULL) {
			free(key);
			goto fail;
		}
		val->key = key;
		if (node_append(dict, val) == -1) {
			pl_free(val);
			goto fail;
		}
	}
	if (read_close_tag(ps, "dict") == -1)
		goto fail;
	return (dict);
fail:
	pl_free(dict);
	return (NULL);
}

static struct pl_node *
parse_array(struct parser *ps)
{
	struct pl_node *arr, *val;

	arr = node_new(PL_ARRAY);
	if (arr == NULL)
		return (NULL);
	for (;;) {
		if (skip_misc(ps) == -1)
			goto fail;
		if (starts_with(ps, "</array"))
			break;
		val = parse_value(ps);
		if (val == NULL)
			goto fail;
		if (node_append(arr, val) == -1) {
			pl_free(val);
			goto fail;
		}
	}
	if (read_close_tag(ps, "array") == -1)
		goto fail;
	return (arr);
fail:
	pl_free(arr);
	return (NULL);
}

/* A leaf element with text content: string, integer, real, date, data. */
static struct pl_node *
parse_leaf(struct parser *ps, const char *tag, enum pl_type type, int selfclose)
{
	struct pl_node *n;
	char *text, *ep;

	n = node_new(type);
	if (n == NULL)
		return (NULL);
	if (selfclose)
		text = strdup("");
	else {
		text = read_text(ps);
		if (text != NULL && read_close_tag(ps, tag) == -1) {
			free(text);
			text = NULL;
		}
	}
	if (text == NULL)
		goto fail;
	switch (type) {
	case PL_STRING:
		n->str = text;
		return (n);
	case PL_INTEGER:
		errno = 0;
		n->num = strtoll(text, &ep, 10);
		if (ep == text || *ep != '\0' || errno != 0) {
			free(text);
			goto fail;
		}
		free(text);
		return (n);
	default:
		free(text);
		return (n);
	}
fail:
	pl_free(n);
	return (NULL);
}

static struct pl_node *
parse_value(struct parser *ps)
{
	struct pl_node *n;
	char tag[16];
	int selfclose;

	if (ps->depth >= PL_MAX_DEPTH)
		return (NULL);
	if (skip_misc(ps) == -1)
		return (NULL);
	if (read_open_tag(ps, tag, sizeof(tag), &selfclose) == -1)
		return (NULL);
	ps->depth++;
	if (strcmp(tag, "dict") == 0)
		n = selfclose ? node_new(PL_DICT) : parse_dict(ps);
	else if (strcmp(tag, "array") == 0)
		n = selfclose ? node_new(PL_ARRAY) : parse_array(ps);
	else if (strcmp(tag, "string") == 0)
		n = parse_leaf(ps, tag, PL_STRING, selfclose);
	else if (strcmp(tag, "integer") == 0)
		n = parse_leaf(ps, tag, PL_INTEGER, selfclose);
	else if (strcmp(tag, "true") == 0 || strcmp(tag, "false") == 0) {
		n = NULL;
		if (selfclose || read_close_tag(ps, tag) == 0) {
			n = node_new(PL_BOOL);
			if (n != NULL)
				n->num = (tag[0] == 't');
		}
	} else if (strcmp(tag, "real") == 0 || strcmp(tag, "date") == 0 ||
	    strcmp(tag, "data") == 0)
		n = parse_leaf(ps, tag, PL_OTHER, selfclose);
	else
		n = NULL;
	ps->depth--;
	return (n);
}

struct pl_node *
pl_parse(const char *buf, size_t len)
{
	struct parser ps;
	struct pl_node *root;
	char tag[16];
	int selfclose;

	ps.p = buf;
	ps.end = buf + len;
	ps.depth = 0;
	if (skip_misc(&ps) == -1)
		return (NULL);
	if (read_open_tag(&ps, tag, sizeof(tag), &selfclose) == -1 ||
	    strcmp(tag, "plist") != 0 || selfclose)
		return (NULL);
	root = parse_value(&ps);
	if (root == NULL)
		return (NULL);
	if (read_close_tag(&ps, "plist") == -1) {
		pl_free(root);
		return (NULL);
	}
	return (root);
}

const struct pl_node *
pl_dict_get(const struct pl_node *dict, const char *key)
{
	size_t i;

	if (dict == NULL || dict->type != PL_DICT)
		return (NULL);
	for (i = 0; i < dict->nchildren; i++)
		if (strcmp(dict->children[i]->key, key) == 0)
			return (dict->children[i]);
	return (NULL);
}

const char *
pl_dict_string(const struct pl_node *dict, const char *key)
{
	const struct pl_node *n = pl_dict_get(dict, key);

	if (n == NULL || n->type != PL_STRING)
		return (NULL);
	return (n->str);
}

int
pl_dict_integer(const struct pl_node *dict, const char *key, long long *out)
{
	const struct pl_node *n = pl_dict_get(dict, key);
	char *ep;
	long long v;

	if (n == NULL)
		return (-1);
	switch (n->type) {
	case PL_INTEGER:
		*out = n->num;
		return (0);
	case PL_STRING:
		if (n->str[0] == '\0')
			return (-1);
		errno = 0;
		v = strtoll(n->str, &ep, 10);
		if (*ep != '\0' || errno != 0)
			return (-1);
		*out = v;
		return (0);
	default:
		return (-1);
	}
}

int
pl_dict_bool(const struct pl_node *dict, const char *key, int *out)
{
	const struct pl_node *n = pl_dict_get(dict, key);

	if (n == NULL)
		return (-1);
	switch (n->type) {
	case PL_BOOL:
	case PL_INTEGER:
		*out = (n->num != 0);
		return (0);
	case PL_STRING:
		if (strcasecmp(n->str, "yes") == 0 ||
		    strcasecmp(n->str, "true") == 0 || strcmp(n->str, "1") == 0)
			*out = 1;
		else if (strcasecmp(n->str, "no") == 0 ||
		    strcasecmp(n->str, "false") == 0 ||
		    strcmp(n->str, "0") == 0 || n->str[0] == '\0')
			*out = 0;
		else
			return (-1);
		return (0);
	default:
		return (-1);
	}
}
