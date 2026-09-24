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

/*
 * A small reader for XML property lists: dict, array, string, integer,
 * true and false, which is all the DirectoryServices plists use. Other
 * value types (real, date, data) are parsed and kept as PL_OTHER so the
 * document still loads. CoreFoundation is deliberately not used: this
 * code runs inside every process that resolves a user or group.
 */

#ifndef NSS_DS_PLIST_H
#define NSS_DS_PLIST_H

#include <stddef.h>

enum pl_type {
	PL_DICT,
	PL_ARRAY,
	PL_STRING,
	PL_INTEGER,
	PL_BOOL,
	PL_OTHER
};

struct pl_node {
	enum pl_type	 type;
	char		*key;		/* set on the children of a dict */
	char		*str;		/* PL_STRING */
	long long	 num;		/* PL_INTEGER, PL_BOOL (0 or 1) */
	struct pl_node	**children;	/* PL_DICT, PL_ARRAY */
	size_t		 nchildren;
};

/* Parse a whole document; returns the root value or NULL if malformed. */
struct pl_node	*pl_parse(const char *buf, size_t len);
void		 pl_free(struct pl_node *node);

/* Dict accessors. All return NULL / -1 when the key is absent or the
 * value has an unexpected type. Integers also accept a numeric string,
 * and booleans also accept an integer or the strings YES/NO/true/false,
 * since Gershwin's writers have used both forms. */
const struct pl_node	*pl_dict_get(const struct pl_node *dict, const char *key);
const char		*pl_dict_string(const struct pl_node *dict, const char *key);
int			 pl_dict_integer(const struct pl_node *dict, const char *key,
			    long long *out);
int			 pl_dict_bool(const struct pl_node *dict, const char *key,
			    int *out);

#endif /* NSS_DS_PLIST_H */
