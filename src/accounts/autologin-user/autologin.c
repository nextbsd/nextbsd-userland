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
 * not what the system resolves. Joined means dsjoin's /Network line is in
 * /etc/fstab, not that /Network is mounted: getty asks this at boot, before
 * org.nextbsd.autofsd has mounted /Network, and while the server is down it
 * is not mounted at all -- either way the client would look standalone and
 * log its local admin in.
 *
 * Nor does a machine with Gershwin's login window installed. Installing it
 * means something else now asks who you are, and a console that has already
 * logged somebody in has answered the question first. It counts for the same
 * reason a second account or a password does, and is checked the same way:
 * a fact on disk, read once, failing toward the prompt.
 *
 * ONCE PER BOOT
 *
 * Automatic login answers "who is at the machine when it starts", not "who
 * is at it after they logged out". So the first time this prints a name it
 * records the boot it did so in AUTOLOGIN_MARKER, and for the rest of that
 * boot it prints nothing: getty, restarted by launchd after a logout, then
 * shows a login prompt, and a graphical login window shows its panel. The
 * marker holds kern.boottime, so a marker left from an earlier boot counts
 * for nothing even where /var/run survives a reboot.
 *
 * FOR A LOGIN WINDOW (-g)
 *
 * A graphical login window runs this with -g. The same rule decides, and
 * the same once-per-boot marker applies, but the "a login window is
 * installed" clause is skipped: the login window is the one asking.
 *
 * Darwin instead stores an explicit autoLoginUser in a loginwindow
 * preference and keeps an obfuscated password in /etc/kcpassword, and ships
 * it off by default because Setup Assistant always sets a password. This
 * reaches the same place without storing a credential anywhere.
 */

#include <sys/types.h>
#include <sys/stat.h>
#ifndef __linux__
#include <sys/sysctl.h>
#endif
#include <sys/time.h>

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
#ifndef LOCAL_DOMAIN
#define LOCAL_DOMAIN	"/Local/Library/DirectoryServices/Domain.plist"
#endif
#ifndef NETWORK_DOMAIN
#define NETWORK_DOMAIN	"/Network/Library/DirectoryServices/Domain.plist"
#endif
#ifndef FSTAB
#define FSTAB		"/etc/fstab"
#endif

/* Records the boot whose automatic login has happened; see ONCE PER BOOT. */
#ifndef AUTOLOGIN_MARKER
#define AUTOLOGIN_MARKER	"/var/run/autologin-used"
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

/*
 * /etc/fstab mounts something on /Network: the line dsjoin writes. Read by
 * hand rather than with getfsfile(3) so the tests can point it elsewhere.
 */
static int
fstab_has_network(void)
{
	char line[1024], *p, *mp;
	FILE *f;
	int found = 0;

	if ((f = fopen(FSTAB, "r")) == NULL)
		return (0);
	while (!found && fgets(line, sizeof(line), f) != NULL) {
		if ((p = strchr(line, '#')) != NULL)
			*p = '\0';
		p = line;
		if (strsep(&p, " \t\n") == NULL)
			continue;
		while ((mp = strsep(&p, " \t\n")) != NULL && *mp == '\0')
			;
		if (mp != NULL && (strcmp(mp, "/Network") == 0 ||
		    strcmp(mp, "/Network/") == 0))
			found = 1;
	}
	(void)fclose(f);
	return (found);
}

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

/* This boot, as text: kern.boottime's seconds. NULL if it can't be read. */
static const char *
this_boot(void)
{
	static char text[32];
	long long sec = -1;
#ifdef __linux__
	/* Only so the host tests run on a Linux CI runner. */
	char line[128];
	FILE *fp = fopen("/proc/stat", "r");

	if (fp == NULL)
		return (NULL);
	while (fgets(line, sizeof(line), fp) != NULL)
		if (sscanf(line, "btime %lld", &sec) == 1)
			break;
	fclose(fp);
#else
	struct timeval tv;
	size_t len = sizeof(tv);

	if (sysctlbyname("kern.boottime", &tv, &len, NULL, 0) == 0)
		sec = tv.tv_sec;
#endif
	if (sec < 0)
		return (NULL);
	snprintf(text, sizeof(text), "%lld\n", sec);
	return (text);
}

/* Automatic login already happened during this boot. */
static int
used_this_boot(void)
{
	const char *boot = this_boot();
	char *buf;
	size_t len;
	int used;

	if (boot == NULL)
		return (1);	/* can't tell: fail toward the prompt */
	buf = slurp(AUTOLOGIN_MARKER, &len);
	if (buf == NULL)
		return (0);
	used = len == strlen(boot) && memcmp(buf, boot, len) == 0;
	free(buf);
	return (used);
}

/* Record that automatic login happened during this boot. */
static int
mark_used(void)
{
	const char *boot = this_boot();
	int fd;
	ssize_t n;

	if (boot == NULL)
		return (-1);
	fd = open(AUTOLOGIN_MARKER, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
	    0644);
	if (fd < 0)
		return (-1);
	n = write(fd, boot, strlen(boot));
	close(fd);
	return (n == (ssize_t)strlen(boot) ? 0 : -1);
}

int
main(int argc, char *argv[])
{
	struct pl_node *root;
	const struct pl_node *rec;
	const char *name;
	char *buf;
	size_t len;
	int ch, for_loginwindow = 0, nopass = 0;

	while ((ch = getopt(argc, argv, "g")) != -1) {
		switch (ch) {
		case 'g':
			for_loginwindow = 1;
			break;
		default:
			return (0);	/* unknown option: no automatic login */
		}
	}

	/* It already happened this boot; see ONCE PER BOOT. */
	if (used_this_boot())
		return (0);

	/*
	 * A joined client takes its accounts from the server, so it does not
	 * autologin. Asked in the same order as everywhere else: a server owns
	 * the accounts and is not a client of itself, so it is answered from
	 * /Local alone and never consults /Network.
	 */
	if (access(LOCAL_DOMAIN, F_OK) != 0 &&
	    access(NETWORK_DOMAIN, F_OK) == 0)
		return (0);
	if (fstab_has_network())
		return (0);

	/*
	 * A login window is installed, so it does the asking. Its presence is
	 * the whole test, exactly as a second account or a password is: this
	 * decides once, at boot, and there is nothing to ask launchd that
	 * would be true yet this early anyway.
	 */
	if (!for_loginwindow && access(LOGINWINDOW_PLIST, F_OK) == 0)
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

	/*
	 * Only print the name once the marker is written: if it can't be,
	 * this would log in again after every logout, so fail toward the
	 * prompt instead.
	 */
	if (mark_used() == 0)
		puts(AUTOLOGIN_USER);
done:
	pl_free(root);
	return (0);
}
