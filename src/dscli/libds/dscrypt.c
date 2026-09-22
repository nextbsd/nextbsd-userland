/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The NextBSD Project
 * Ported from Gershwin's dscli by Joe Maloney
 * (gershwin-desktop/gershwin-components, DirectoryServices/dscli, BSD-2-Clause).
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
 * Passwords: SHA-512 crypt(3) with 5000 rounds and a random 16-character
 * salt, the form Gershwin's dscli writes, so the two verify each other's
 * hashes. Verification recomputes crypt(password, hash) and compares in
 * constant time.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ds.h"

char *
ds_hash_password(const char *password)
{
	static const char chars[] =
	    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	unsigned char raw[16];
	char salt[17], setting[64];
	const char *hash;
	int i;

	arc4random_buf(raw, sizeof(raw));
	for (i = 0; i < 16; i++)
		salt[i] = chars[raw[i] % 64];
	salt[16] = '\0';
	snprintf(setting, sizeof(setting), "$6$rounds=5000$%s$", salt);
	hash = crypt(password, setting);
	if (hash == NULL)
		return (NULL);
	return (strdup(hash));
}

bool
ds_verify_password(const struct ds_user *u, const char *password)
{
	const char *computed;
	size_t i, n;
	unsigned char diff = 0;

	if (u->nopass)
		return (password[0] == '\0');
	if (u->hash == NULL || u->hash[0] == '\0')
		return (false);
	computed = crypt(password, u->hash);
	if (computed == NULL)
		return (false);
	n = strlen(u->hash);
	if (strlen(computed) != n)
		return (false);
	for (i = 0; i < n; i++)
		diff |= (unsigned char)(computed[i] ^ u->hash[i]);
	return (diff == 0);
}
