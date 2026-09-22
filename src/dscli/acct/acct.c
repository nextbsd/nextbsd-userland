/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The NextBSD Project
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

#include <readpassphrase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>		/* explicit_bzero */
#include <unistd.h>

#include "acct.h"
#include "ds.h"

#define ACCT_PASS_MAX	1024

static char *
read_line(FILE *fp)
{
	char *line = NULL;
	size_t cap = 0;
	ssize_t got;

	got = getline(&line, &cap, fp);
	if (got == -1) {
		free(line);
		return (NULL);
	}
	if (got > 0 && line[got - 1] == '\n')
		line[got - 1] = '\0';
	return (line);
}

char *
acct_read_password(const char *prog, const char *what, bool confirm)
{
	char prompt[128], buf1[ACCT_PASS_MAX], buf2[ACCT_PASS_MAX];
	char *p;

	if (!isatty(STDIN_FILENO)) {
		p = read_line(stdin);
		if (p == NULL)
			fprintf(stderr, "%s: no password on stdin\n", prog);
		return (p);
	}
	snprintf(prompt, sizeof(prompt), "%s: ", what);
	if (readpassphrase(prompt, buf1, sizeof(buf1), RPP_ECHO_OFF) == NULL) {
		fprintf(stderr, "%s: unable to read password\n", prog);
		return (NULL);
	}
	if (confirm) {
		snprintf(prompt, sizeof(prompt), "Retype %s: ", what);
		if (readpassphrase(prompt, buf2, sizeof(buf2), RPP_ECHO_OFF) == NULL ||
		    strcmp(buf1, buf2) != 0) {
			fprintf(stderr, "%s: passwords do not match\n", prog);
			explicit_bzero(buf1, sizeof(buf1));
			explicit_bzero(buf2, sizeof(buf2));
			return (NULL);
		}
		explicit_bzero(buf2, sizeof(buf2));
	}
	p = strdup(buf1);
	explicit_bzero(buf1, sizeof(buf1));
	return (p);
}

void
acct_wipe(char *s)
{
	if (s != NULL) {
		explicit_bzero(s, strlen(s));
		free(s);
	}
}

bool
acct_refuse_if_joined(const char *prog)
{
	if (!ds_from_network())
		return (false);
	fprintf(stderr, "%s: this machine is joined to a directory server; "
	    "the accounts in use are the server's.\n"
	    "Make the change on the server, or run `dscli leave` first.\n", prog);
	return (true);
}
