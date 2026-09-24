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
 * autologin-user -- print the account the console should log in automatically,
 * or nothing at all.
 *
 * The getty job runs this and, when it prints a name, starts getty with a
 * gettytab class whose "al" capability names that account, so getty execs
 * login -f and no password is asked. Reading XML from the job's shell would
 * be the wrong tool, so the rule lives here in one place.
 *
 * The rule: exactly one user exists, it is named "admin", and it has
 * noPassword set. Each clause matters.
 *
 *   One user     Adding a second account means somebody is sharing the
 *                machine, and a login prompt is then the right default.
 *   Named admin  Not cosmetic: the gettytab class hardcodes "al=admin",
 *                because a gettytab capability cannot be computed at run
 *                time. Authorising any other name here would silently log
 *                in the wrong account.
 *   noPassword   Setting a password must turn this off by itself, which is
 *                what someone setting a password expects. It also means
 *                automatic login never bypasses a credential: it only ever
 *                fires where there is none to bypass.
 *
 * Both failing clauses fail toward asking for a login, never away from it.
 *
 * A joined directory client never logs in automatically, whatever its local
 * database says: its accounts come from the server, and the local plist is
 * not what the system resolves.
 *
 * Nor does a machine with Gershwin's login window installed. Installing it
 * means something else now asks who you are, and a console that has already
 * logged somebody in has answered the question first. It counts for the same
 * reason a second account or a password does, and is checked the same way:
 * a fact on disk, read once, failing toward the prompt.
 *
 * Darwin instead stores an explicit autoLoginUser in a loginwindow
 * preference and keeps an obfuscated password in /etc/kcpassword, and ships
 * it off by default because Setup Assistant always sets a password. This
 * reaches the same place without storing a credential anywhere.
 */

#include <sys/stat.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "plist.h"

/* Overridable only so the tests can stage plists outside the system. */
#ifndef LOCAL_USERS
#define LOCAL_USERS	"/Local/Library/DirectoryServices/Users.plist"
#endif
#ifndef NETWORK_USERS
#define NETWORK_USERS	"/Network/Library/DirectoryServices/Users.plist"
#endif

/* The one account this will ever authorise; see the comment above. */
#define AUTOLOGIN_USER	"admin"

/*
 * Gershwin's login window, which asks for a user and a password on the
 * display. Where it is installed it owns logging in, so the console must not
 * have already done it. Overridable only so the tests can point elsewhere.
 */
#ifndef LOGINWINDOW_PLIST
#define LOGINWINDOW_PLIST \
	"/System/Library/LaunchDaemons/io.github.gershwin-desktop.loginwindow.plist"
#endif

/* A plist bigger than this is not a fresh single-user database. */
#define MAX_FILE	(4 * 1024 * 1024)

static char *
slurp(const char *path, size_t *lenp)
{
	struct stat st;
	char *buf;
	ssize_t got;
	size_t have = 0;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		return (NULL);
	if (fstat(fd, &st) == -1 || !S_ISREG(st.st_mode) ||
	    st.st_size <= 0 || st.st_size > MAX_FILE) {
		(void)close(fd);
		return (NULL);
	}
	buf = malloc((size_t)st.st_size + 1);
	if (buf == NULL) {
		(void)close(fd);
		return (NULL);
	}
	while (have < (size_t)st.st_size) {
		got = read(fd, buf + have, (size_t)st.st_size - have);
		if (got <= 0)
			break;
		have += (size_t)got;
	}
	(void)close(fd);
	if (have != (size_t)st.st_size) {
		free(buf);
		return (NULL);
	}
	buf[have] = '\0';
	*lenp = have;
	return (buf);
}

int
main(void)
{
	struct pl_node *root;
	const struct pl_node *rec;
	const char *name;
	char *buf;
	size_t len;
	int nopass = 0;

	/* A joined client takes its accounts from the server. */
	if (access(NETWORK_USERS, F_OK) == 0)
		return (0);

	/*
	 * A login window is installed, so it does the asking. Its presence is
	 * the whole test, exactly as a second account or a password is: this
	 * decides once, at boot, and there is nothing to ask launchd that
	 * would be true yet this early anyway.
	 */
	if (access(LOGINWINDOW_PLIST, F_OK) == 0)
		return (0);

	buf = slurp(LOCAL_USERS, &len);
	if (buf == NULL)
		return (0);
	root = pl_parse(buf, len);
	free(buf);
	if (root == NULL)
		return (0);

	/* Exactly one record, and it must be a user. */
	if (root->type != PL_DICT || root->nchildren != 1)
		goto done;
	rec = root->children[0];
	if (rec->type != PL_DICT)
		goto done;

	/* The record's own username wins; the key it is filed under is the
	 * fallback, matching how the NSS module reads these files. */
	name = pl_dict_string(rec, "username");
	if (name == NULL || name[0] == '\0')
		name = rec->key;
	if (name == NULL || strcmp(name, AUTOLOGIN_USER) != 0)
		goto done;

	if (pl_dict_bool(rec, "noPassword", &nopass) != 0 || !nopass)
		goto done;

	puts(AUTOLOGIN_USER);
done:
	pl_free(root);
	return (0);
}
