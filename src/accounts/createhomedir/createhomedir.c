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
 * createhomedir -- build a user's home directory from the user template.
 *
 * Darwin creates a local account's home when the account is created, and
 * uses createhomedir(8) to build one for a directory account that has not
 * logged in yet. NextBSD creates homes at account creation too, in both the
 * local and the network case, because a network user is created on the
 * server where the home lives. So this command exists for REPAIR: a home
 * missing because a plist was restored, hand-edited, or copied between
 * machines. login(1) degrades rather than failing in that case, printing
 * "No home directory. Logging in with home = /", so nobody is locked out
 * and this can be run afterwards.
 *
 * Users are enumerated through getpwent(3), so the DirectoryServices plists
 * are read by nss_directory_services(8) rather than by this program, and any
 * other name source works the same way. Only homes under /Local/Users or
 * /Network/Users are touched: system accounts in master.passwd point at
 * /var/empty, /nonexistent and the like, which must stay absent.
 *
 * The template is /System/Library/User Template. Its Non_localized tree is
 * copied verbatim, then __permissions.plist is applied for the two modes the
 * tree cannot carry. The directories exist in the tree because each holds an
 * empty .localized marker, which is also Darwin's hook for showing a
 * translated folder name in a file manager: the names on disk stay English in
 * every language, and only the display is localised.
 */

#include <sys/param.h>
#include <sys/stat.h>

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <grp.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "plist.h"

#ifndef nitems
#define nitems(x)	(sizeof((x)) / sizeof((x)[0]))
#endif

/* Overridable only so the tests can stage a template outside the system. */
#ifndef TEMPLATE_DIR
#define TEMPLATE_DIR	"/System/Library/User Template"
#endif
#define TEMPLATE_TREE	TEMPLATE_DIR "/Non_localized"
#define TEMPLATE_PERMS	TEMPLATE_DIR "/__permissions.plist"

/*
 * Homes we are willing to create. Anything else is left alone: system
 * accounts in master.passwd point at /var/empty, /nonexistent and the like,
 * which must stay absent. Darwin's -c, -s and -b select local, server or
 * both home paths; ours are the two roots the NSS module computes.
 */
#define ROOT_LOCAL	"/Local/Users/"
#define ROOT_NETWORK	"/Network/Users/"

static bool want_local = false;		/* -c */
static bool want_network = false;	/* -s */
static bool dry_run;
static bool verbose;
static int exit_status;

static void
say(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;
	va_start(ap, fmt);
	(void)vfprintf(stdout, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stdout);
}

static bool
under(const char *dir, const char *root)
{
	size_t n = strlen(root);

	/* Must be under the root, and name something inside it. */
	return (strncmp(dir, root, n) == 0 && dir[n] != '\0');
}

static bool
managed(const char *dir)
{
	if (dir == NULL || strstr(dir, "/..") != NULL)
		return (false);
	if (want_local && under(dir, ROOT_LOCAL))
		return (true);
	if (want_network && under(dir, ROOT_NETWORK))
		return (true);
	return (false);
}

/* mkdir -p for a path already known to sit under a managed root. */
static int
make_dirs(const char *path, mode_t mode, uid_t uid, gid_t gid)
{
	char buf[PATH_MAX];
	char *p;

	if (strlcpy(buf, path, sizeof(buf)) >= sizeof(buf)) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	for (p = buf + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, 0755) == 0)
			(void)chown(buf, uid, gid);
		else if (errno != EEXIST)
			return (-1);
		*p = '/';
	}
	if (mkdir(buf, mode) == -1) {
		if (errno != EEXIST)
			return (-1);
	} else if (chown(buf, uid, gid) == -1)
		return (-1);
	return (0);
}

static int
copy_file(const char *from, const char *to, uid_t uid, gid_t gid)
{
	char buf[65536];
	struct stat st;
	ssize_t got, put, off;
	int in, out, saved;

	in = open(from, O_RDONLY | O_CLOEXEC);
	if (in == -1)
		return (-1);
	if (fstat(in, &st) == -1) {
		saved = errno; (void)close(in); errno = saved; return (-1);
	}
	/* Never clobber what is already there: this command repairs, and a
	 * user's own dot files outrank the template's. */
	out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
	    st.st_mode & 0777);
	if (out == -1) {
		saved = errno; (void)close(in); errno = saved; return (-1);
	}
	while ((got = read(in, buf, sizeof(buf))) > 0) {
		for (off = 0; off < got; off += put) {
			put = write(out, buf + off, (size_t)(got - off));
			if (put < 0) {
				saved = errno;
				(void)close(in); (void)close(out);
				errno = saved;
				return (-1);
			}
		}
	}
	saved = (got < 0) ? errno : 0;
	(void)fchown(out, uid, gid);
	(void)close(in);
	if (close(out) == -1 && saved == 0)
		saved = errno;
	if (saved != 0) {
		errno = saved;
		return (-1);
	}
	return (0);
}

/* Copy the template tree's regular files and directories into the home. */
static int
copy_tree(const char *src, const char *dst, uid_t uid, gid_t gid)
{
	char from[PATH_MAX], to[PATH_MAX];
	struct dirent *de;
	struct stat st;
	DIR *d;
	int rv = 0;

	d = opendir(src);
	if (d == NULL)
		return (-1);
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if ((size_t)snprintf(from, sizeof(from), "%s/%s", src, de->d_name) >= sizeof(from) ||
		    (size_t)snprintf(to, sizeof(to), "%s/%s", dst, de->d_name) >= sizeof(to)) {
			warnx("path too long: %s/%s", src, de->d_name);
			rv = -1;
			continue;
		}
		if (lstat(from, &st) == -1) {
			warn("%s", from);
			rv = -1;
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			if (dry_run) {
				say("  would create %s", to);
			} else if (mkdir(to, st.st_mode & 0777) == -1 && errno != EEXIST) {
				warn("%s", to);
				rv = -1;
				continue;
			} else
				(void)chown(to, uid, gid);
			if (copy_tree(from, to, uid, gid) == -1)
				rv = -1;
		} else if (S_ISREG(st.st_mode)) {
			if (dry_run) {
				say("  would copy %s", to);
				continue;
			}
			if (copy_file(from, to, uid, gid) == -1) {
				if (errno == EEXIST)
					say("  kept existing %s", to);
				else {
					warn("%s", to);
					rv = -1;
				}
			} else
				say("  %s", to);
		}
		/* Anything else in the template is ignored on purpose. */
	}
	(void)closedir(d);
	return (rv);
}

/*
 * Apply __permissions.plist: an array of { Command, Argument, Paths },
 * in order, so a parent is created before its child.
 */
static int
apply_permissions(const char *home)
{
	char path[PATH_MAX];
	struct pl_node *root;
	const struct pl_node *entry, *paths, *p;
	const char *cmd, *arg;
	struct stat st;
	char *buf, *ep;
	unsigned long mode;
	size_t i, j;
	int fd, rv = 0;

	fd = open(TEMPLATE_PERMS, O_RDONLY | O_CLOEXEC);
	if (fd == -1) {
		/* A template without a permissions file is legal: the tree
		 * was copied, there is simply nothing to declare. */
		if (errno == ENOENT)
			return (0);
		warn("%s", TEMPLATE_PERMS);
		return (-1);
	}
	if (fstat(fd, &st) == -1 || st.st_size < 0 || st.st_size > (off_t)(1 << 20)) {
		warnx("%s: not a plausible plist", TEMPLATE_PERMS);
		(void)close(fd);
		return (-1);
	}
	buf = malloc((size_t)st.st_size + 1);
	if (buf == NULL || read(fd, buf, (size_t)st.st_size) != (ssize_t)st.st_size) {
		warn("%s", TEMPLATE_PERMS);
		free(buf);
		(void)close(fd);
		return (-1);
	}
	buf[st.st_size] = '\0';
	(void)close(fd);
	root = pl_parse(buf, (size_t)st.st_size);
	free(buf);
	if (root == NULL || root->type != PL_ARRAY) {
		warnx("%s: expected an array of commands", TEMPLATE_PERMS);
		pl_free(root);
		return (-1);
	}

	for (i = 0; i < root->nchildren; i++) {
		entry = root->children[i];
		cmd = pl_dict_string(entry, "Command");
		arg = pl_dict_string(entry, "Argument");
		paths = pl_dict_get(entry, "Paths");
		if (cmd == NULL || arg == NULL || paths == NULL ||
		    paths->type != PL_ARRAY) {
			warnx("%s: entry %zu is malformed; skipped",
			    TEMPLATE_PERMS, i);
			rv = -1;
			continue;
		}
		for (j = 0; j < paths->nchildren; j++) {
			p = paths->children[j];
			if (p->type != PL_STRING || p->str[0] != '/')
				continue;
			if ((size_t)snprintf(path, sizeof(path), "%s%s", home,
			    p->str) >= sizeof(path)) {
				warnx("path too long: %s%s", home, p->str);
				rv = -1;
				continue;
			}
			if (strcmp(cmd, "CHMOD") == 0) {
				errno = 0;
				mode = strtoul(arg, &ep, 8);
				if (*ep != '\0' || errno != 0 || mode > 07777) {
					warnx("%s: bad mode %s", TEMPLATE_PERMS, arg);
					rv = -1;
					continue;
				}
				if (dry_run) {
					say("  would chmod %s to %s", path, arg);
					continue;
				}
				if (chmod(path, (mode_t)mode) == -1) {
					warn("chmod %s", path);
					rv = -1;
				} else
					say("  chmod %s %s", arg, path);
			} else if (strcmp(cmd, "CHGRP") == 0) {
				struct group *gr = getgrnam(arg);

				if (gr == NULL) {
					warnx("no group %s", arg);
					rv = -1;
					continue;
				}
				if (dry_run) {
					say("  would chgrp %s to %s", path, arg);
					continue;
				}
				if (chown(path, (uid_t)-1, gr->gr_gid) == -1) {
					warn("chgrp %s", path);
					rv = -1;
				}
			} else {
				warnx("%s: unknown command %s", TEMPLATE_PERMS, cmd);
				rv = -1;
			}
		}
	}
	pl_free(root);
	return (rv);
}

static void
build_home(const struct passwd *pw)
{
	struct stat st;

	if (!managed(pw->pw_dir)) {
		say("%s: home %s is not under a managed root; skipped",
		    pw->pw_name, pw->pw_dir != NULL ? pw->pw_dir : "(none)");
		return;
	}
	if (stat(pw->pw_dir, &st) == 0) {
		if (!S_ISDIR(st.st_mode)) {
			warnx("%s: %s exists and is not a directory",
			    pw->pw_name, pw->pw_dir);
			exit_status = 1;
		} else
			say("%s: %s exists", pw->pw_name, pw->pw_dir);
		return;
	}
	if (errno != ENOENT) {
		warn("%s", pw->pw_dir);
		exit_status = 1;
		return;
	}

	printf("%s%s: %s\n", dry_run ? "would create " : "creating ",
	    pw->pw_name, pw->pw_dir);
	if (dry_run) {
		(void)copy_tree(TEMPLATE_TREE, pw->pw_dir, pw->pw_uid, pw->pw_gid);
		(void)apply_permissions(pw->pw_dir);
		return;
	}
	if (make_dirs(pw->pw_dir, 0755, pw->pw_uid, pw->pw_gid) == -1) {
		warn("%s", pw->pw_dir);
		exit_status = 1;
		return;
	}
	if (copy_tree(TEMPLATE_TREE, pw->pw_dir, pw->pw_uid, pw->pw_gid) == -1)
		exit_status = 1;
	if (apply_permissions(pw->pw_dir) == -1)
		exit_status = 1;
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: createhomedir [-bcs] [-nv] -a\n"
	    "       createhomedir [-bcs] [-nv] -u user [-u user ...]\n"
	    "       createhomedir [-bcs] [-nv] -i\n");
	exit(2);
}

/* Build the home for one named user, reporting a name we cannot resolve. */
static void
build_named(const char *name)
{
	struct passwd *pw = getpwnam(name);

	if (pw == NULL) {
		warnx("no such user: %s", name);
		exit_status = 1;
		return;
	}
	build_home(pw);
}

int
main(int argc, char **argv)
{
	char line[LINE_MAX], *nl;
	struct passwd *pw;
	char **names;
	size_t nnames = 0, i;
	bool all = false, from_stdin = false, both = false;
	int ch;

	/* At most one name per argument, so this is allocated once. */
	names = calloc((size_t)argc + 1, sizeof(*names));
	if (names == NULL)
		err(1, NULL);

	while ((ch = getopt(argc, argv, "abcinsu:v")) != -1) {
		switch (ch) {
		case 'a': all = true; break;
		case 'b': both = true; break;
		case 'c': want_local = true; break;
		case 'i': from_stdin = true; break;
		case 'n': dry_run = true; break;
		case 's': want_network = true; break;
		case 'u': names[nnames++] = optarg; break;
		case 'v': verbose = true; break;
		default: usage();
		}
	}
	if (argc != optind)
		usage();

	/* -b, or none of -b/-c/-s, means both roots. */
	if (both || (!want_local && !want_network))
		want_local = want_network = true;
	/* With no selection at all, every user is the sensible default. */
	if (!all && !from_stdin && nnames == 0)
		all = true;
	if (all && (from_stdin || nnames > 0))
		usage();
	if (!dry_run && geteuid() != 0)
		errx(1, "must be root to create home directories");

	if (access(TEMPLATE_TREE, X_OK) == -1)
		err(1, "%s", TEMPLATE_TREE);

	if (all) {
		setpwent();
		while ((pw = getpwent()) != NULL)
			build_home(pw);
		endpwent();
	}
	for (i = 0; i < nnames; i++)
		build_named(names[i]);
	if (from_stdin) {
		while (fgets(line, sizeof(line), stdin) != NULL) {
			if ((nl = strchr(line, '\n')) != NULL)
				*nl = '\0';
			if (line[0] != '\0')
				build_named(line);
		}
	}
	free(names);
	return (exit_status);
}
