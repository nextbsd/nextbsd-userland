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
 * adduser — create a user account in the DirectoryServices plists
 * (nextbsd/nextbsd-userland#286, E18 U14).
 *
 * NextBSD's own, at FreeBSD's path, rather than a wrapper: FreeBSD's
 * adduser is a shell script that drives pw(8) and writes master.passwd,
 * and neither half is what we want.
 *
 * Six questions, because the data model has nowhere to put the answers to
 * FreeBSD's others. There is no home field, so the home is computed and
 * there is nothing to ask; no login class; no ageing or expiry. What is
 * left is username, full name, shell, whether the account administers the
 * machine, and a password twice.
 *
 * Exit status:
 *   0  the account was created
 *   1  usage error
 *   2  refused (see the messages below; nothing was written)
 *   3  database error: a plist would not parse, the lock was held, a write
 *      failed
 *   4  interrupted, or answered no at the confirmation
 */

#include <sys/types.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "acct.h"
#include "libds.h"

#define EX_OK		0
#define EX_USAGE	1
#define EX_REFUSED	2
#define EX_DATABASE	3
#define EX_CANCELLED	4

/* -w */
enum pwmode {
	PW_PROMPT,	/* yes: ask, or take -p */
	PW_NONE,	/* none: noPassword, login with no password */
	PW_LOCKED	/* no: an unmatchable hash, so no password works */
};

struct opts {
	const char	*name;		/* from the command line, if given */
	const char	*realname;	/* -c */
	const char	*shell;		/* -s */
	const char	*group;		/* -g */
	const char	*groups;	/* -G, comma separated */
	const char	*hash;		/* -p, pre-hashed */
	const char	*batch;		/* -f */
	uid_t		 uid;		/* -u */
	bool		 have_uid;
	enum pwmode	 pwmode;	/* -w */
	bool		 noshellcheck;	/* -S */
	bool		 nohome;	/* -D */
	bool		 quiet;		/* -q */
	bool		 admin;		/* --admin */
};

static void
usage(void)
{
	(void)fprintf(stderr,
"usage: adduser [-DSqh] [-u uid] [-g group] [-G group,...] [--admin]\n"
"               [-c \"full name\"] [-s shell] [-w no|none|yes] [-p hash]\n"
"               [-f file] [name]\n");
}

/* A yes/no question with a default. Returns the answer, or -1 on EOF. */
static int
ask_yesno(const char *prompt, bool dflt)
{
	char buf[64];

	for (;;) {
		(void)printf("%s (yes/no) [%s]: ", prompt, dflt ? "yes" : "no");
		(void)fflush(stdout);
		if (fgets(buf, sizeof(buf), stdin) == NULL)
			return (-1);
		buf[strcspn(buf, "\r\n")] = '\0';
		if (buf[0] == '\0')
			return (dflt ? 1 : 0);
		if (strcasecmp(buf, "y") == 0 || strcasecmp(buf, "yes") == 0)
			return (1);
		if (strcasecmp(buf, "n") == 0 || strcasecmp(buf, "no") == 0)
			return (0);
		(void)printf("Please answer yes or no.\n");
	}
}

/*
 * A line of input with an optional default. Returns false on EOF.
 * `out` is left holding the default when the answer is empty.
 */
static bool
ask_line(const char *prompt, const char *dflt, char *out, size_t len)
{
	char buf[1024];

	if (dflt != NULL && dflt[0] != '\0')
		(void)printf("%s [%s]: ", prompt, dflt);
	else
		(void)printf("%s: ", prompt);
	(void)fflush(stdout);
	if (fgets(buf, sizeof(buf), stdin) == NULL)
		return (false);
	buf[strcspn(buf, "\r\n")] = '\0';
	if (buf[0] == '\0' && dflt != NULL)
		(void)strlcpy(out, dflt, len);
	else
		(void)strlcpy(out, buf, len);
	return (true);
}

/* Resolve -g: a group name or a gid, which must already exist. */
static bool
resolve_group(struct ds_handle *h, const char *spec, gid_t *gid_out,
    char *name_out, size_t name_len)
{
	struct ds_grouprec g;
	struct group *sysg;
	char *end;
	long long v;

	/* A number is a gid. */
	errno = 0;
	v = strtoll(spec, &end, 10);
	if (errno == 0 && end != spec && *end == '\0' && v >= 0) {
		if (ds_group_get_by_gid(h, (gid_t)v, &g) == DS_OK) {
			*gid_out = g.gid;
			(void)strlcpy(name_out, g.groupname, name_len);
			return (true);
		}
		if ((sysg = getgrgid((gid_t)v)) != NULL) {
			*gid_out = sysg->gr_gid;
			(void)strlcpy(name_out, sysg->gr_name, name_len);
			return (true);
		}
		return (false);
	}
	if (ds_group_get(h, spec, &g) == DS_OK) {
		*gid_out = g.gid;
		(void)strlcpy(name_out, g.groupname, name_len);
		return (true);
	}
	if ((sysg = getgrnam(spec)) != NULL) {
		*gid_out = sysg->gr_gid;
		(void)strlcpy(name_out, sysg->gr_name, name_len);
		return (true);
	}
	return (false);
}

/*
 * Create one account. Returns an exit status. `interactive` drives the
 * prompts and the confirmation; a batch line supplies everything instead.
 */
static int
add_one(const struct opts *o, bool interactive, const char *bname,
    uid_t buid, gid_t bgid, const char *bgecos, const char *bshell,
    const char *bhash)
{
	struct ds_handle *h = NULL;
	struct ds_userrec u;
	struct ds_grouprec g;
	char name[DS_NAME_MAX], real[DS_REAL_MAX], shell[DS_SHELL_MAX];
	char gname[DS_NAME_MAX], server[256];
	char extra[1024], *tok, *brk;
	const char *hash = NULL;
	enum ds_error err;
	enum pwmode mode = o->pwmode;
	uid_t uid;
	gid_t gid;
	bool make_group = true, admin = o->admin;
	bool walk = interactive;	/* false once a name came from elsewhere */
	int rc;

	memset(name, 0, sizeof(name));
	memset(real, 0, sizeof(real));
	memset(shell, 0, sizeof(shell));
	memset(extra, 0, sizeof(extra));

	/*
	 * Gather the answers. The six-question walk happens only when there
	 * is nothing else to go on: a name on the command line, or a batch
	 * line, means every unset field takes its default instead, so the
	 * tool can be scripted. The password is the exception, below: it is
	 * never defaulted silently.
	 */
	if (!interactive) {
		(void)strlcpy(name, bname, sizeof(name));
		(void)strlcpy(real, bgecos, sizeof(real));
		(void)strlcpy(shell, bshell[0] != '\0' ? bshell :
		    ACCT_DEFAULT_SHELL, sizeof(shell));
		hash = bhash;
	} else if (o->name != NULL) {
		walk = false;
		(void)strlcpy(name, o->name, sizeof(name));
		if (o->realname != NULL)
			(void)strlcpy(real, o->realname, sizeof(real));
		(void)strlcpy(shell, o->shell != NULL ? o->shell :
		    ACCT_DEFAULT_SHELL, sizeof(shell));
	} else {
		if (!ask_line("Username", NULL, name, sizeof(name)))
			return (EX_CANCELLED);
		if (o->realname != NULL)
			(void)strlcpy(real, o->realname, sizeof(real));
		else if (!ask_line("Full name", NULL, real, sizeof(real)))
			return (EX_CANCELLED);
		if (o->shell != NULL)
			(void)strlcpy(shell, o->shell, sizeof(shell));
		else if (!ask_line("Shell", ACCT_DEFAULT_SHELL, shell,
		    sizeof(shell)))
			return (EX_CANCELLED);
	}

	/* ---- refusals that need no database ------------------------- */
	{
		const char *bad = acct_name_problem(name);

		if (bad != NULL) {
			warnx("%s: invalid name (%s)", name, bad);
			return (EX_REFUSED);
		}
	}
	if (acct_is_system_name(name)) {
		warnx("%s: system accounts belong in master.passwd; "
		    "use pw useradd", name);
		return (EX_REFUSED);
	}
	if (acct_bound_server(server, sizeof(server))) {
		if (server[0] != '\0')
			warnx("this machine is joined to %s; create the "
			    "account on the server", server);
		else
			warnx("this machine is joined to a directory server; "
			    "create the account there");
		return (EX_REFUSED);
	}
	if (!o->noshellcheck && !acct_shell_listed(shell)) {
		warnx("%s: not in %s (use -S to override)", shell, ACCT_SHELLS);
		return (EX_REFUSED);
	}

	/* ---- open the database -------------------------------------- */
	if ((err = ds_open(DS_LOCAL, DS_RDWR, &h)) != DS_OK) {
		warnx("%s: %s", ds_strerror(err),
		    err == DS_ELOCK ? "another account tool may be running" :
		    "cannot open the directory");
		return (EX_DATABASE);
	}
	if (ds_user_get(h, name, &u) == DS_OK) {
		warnx("%s: name already in use", name);
		ds_close(h);
		return (EX_REFUSED);
	}
	if (getpwnam(name) != NULL) {
		warnx("%s: name already in use (in master.passwd)", name);
		ds_close(h);
		return (EX_REFUSED);
	}

	/* ---- uid ---------------------------------------------------- */
	if (!interactive && buid != (uid_t)-1)
		uid = buid;
	else if (o->have_uid)
		uid = o->uid;
	else if ((err = ds_user_next_uid(h, ACCT_FIRST_ID, ACCT_LAST_ID, true,
	    &uid)) != DS_OK) {
		warnx("no free uid between %d and %d", ACCT_FIRST_ID,
		    ACCT_LAST_ID);
		ds_close(h);
		return (EX_DATABASE);
	}
	if (acct_is_system_id(uid)) {
		warnx("uid %u is in the system range (<= %d); "
		    "use pw useradd", (unsigned)uid, ACCT_SYSTEM_MAX);
		ds_close(h);
		return (EX_REFUSED);
	}
	if (ds_user_get_by_uid(h, uid, &u) == DS_OK) {
		warnx("uid %u is already used by %s", (unsigned)uid, u.username);
		ds_close(h);
		return (EX_REFUSED);
	}

	/* ---- primary group ------------------------------------------ */
	if (!interactive && bgid != (gid_t)-1) {
		gid = bgid;
		make_group = false;
		if (ds_group_get_by_gid(h, gid, &g) == DS_OK)
			(void)strlcpy(gname, g.groupname, sizeof(gname));
		else
			(void)snprintf(gname, sizeof(gname), "%u",
			    (unsigned)gid);
	} else if (o->group != NULL) {
		if (!resolve_group(h, o->group, &gid, gname, sizeof(gname))) {
			warnx("%s: no such group", o->group);
			ds_close(h);
			return (EX_REFUSED);
		}
		make_group = false;
	} else {
		/* A private group, same name and gid as the user. */
		gid = (gid_t)uid;
		(void)strlcpy(gname, name, sizeof(gname));
		if (ds_group_get(h, name, &g) == DS_OK ||
		    ds_group_get_by_gid(h, gid, &g) == DS_OK ||
		    getgrnam(name) != NULL || getgrgid(gid) != NULL) {
			warnx("%s: a group of that name or gid %u already "
			    "exists; use -g to choose one", name,
			    (unsigned)gid);
			ds_close(h);
			return (EX_REFUSED);
		}
	}

	/* ---- administrator ------------------------------------------ */
	if (walk && !o->admin && o->groups == NULL) {
		rc = ask_yesno("Administrator? (can use sudo)", false);
		if (rc == -1) {
			ds_close(h);
			return (EX_CANCELLED);
		}
		admin = (rc == 1);
	}
	if (o->groups != NULL)
		(void)strlcpy(extra, o->groups, sizeof(extra));
	if (admin) {
		if (extra[0] != '\0')
			(void)strlcat(extra, ",", sizeof(extra));
		(void)strlcat(extra, ACCT_ADMIN_GROUP, sizeof(extra));
	}
	/* Every named group has to exist before anything is written. */
	{
		char probe[1024];

		(void)strlcpy(probe, extra, sizeof(probe));
		for (tok = strtok_r(probe, ",", &brk); tok != NULL;
		    tok = strtok_r(NULL, ",", &brk)) {
			if (ds_group_get(h, tok, &g) == DS_OK)
				continue;
			if (strcmp(tok, ACCT_ADMIN_GROUP) == 0)
				warnx("%s: no such group; seed %s or use -G",
				    tok, DS_LOCAL_DIR);
			else
				warnx("%s: no such group in the directory",
				    tok);
			ds_close(h);
			return (EX_REFUSED);
		}
	}

	/* ---- password ----------------------------------------------- */
	if (o->hash != NULL) {
		hash = o->hash;
		mode = PW_PROMPT;
	} else if (interactive && mode == PW_PROMPT && !isatty(STDIN_FILENO) &&
	    !walk) {
		/*
		 * A name was given but no password and no -w, and there is no
		 * terminal to ask at. Refuse rather than quietly creating an
		 * account with no password.
		 */
		warnx("no password given: use -p hash, -w none or -w no, "
		    "or run without a name to be asked");
		ds_close(h);
		return (EX_USAGE);
	} else if (interactive && mode == PW_PROMPT) {
		char *p1, *p2;

		p1 = getpass("Password (empty for none): ");
		if (p1 == NULL) {
			ds_close(h);
			return (EX_CANCELLED);
		}
		if (p1[0] == '\0') {
			mode = PW_NONE;
		} else {
			char first[256];

			(void)strlcpy(first, p1, sizeof(first));
			acct_zero(p1, strlen(p1));
			p2 = getpass("Password again: ");
			if (p2 == NULL) {
				acct_zero(first, sizeof(first));
				ds_close(h);
				return (EX_CANCELLED);
			}
			if (strcmp(first, p2) != 0) {
				acct_zero(first, sizeof(first));
				acct_zero(p2, strlen(p2));
				warnx("passwords do not match");
				ds_close(h);
				return (EX_REFUSED);
			}
			acct_zero(p2, strlen(p2));
			hash = acct_hash_password(first);
			acct_zero(first, sizeof(first));
			if (hash == NULL) {
				warnx("could not hash the password");
				ds_close(h);
				return (EX_DATABASE);
			}
		}
	} else if (!interactive && hash != NULL && hash[0] != '\0') {
		/* a batch line's password field is already a hash */
		mode = PW_PROMPT;
	} else if (mode == PW_PROMPT) {
		mode = PW_NONE;		/* -w yes with nothing to ask */
	}

	/* ---- summary and confirmation ------------------------------- */
	if (!o->quiet) {
		(void)printf("\n");
		(void)printf("Username : %s\n", name);
		if (real[0] != '\0')
			(void)printf("Full name: %s\n", real);
		(void)printf("Uid      : %u\n", (unsigned)uid);
		(void)printf("Group    : %s (%u)\n", gname, (unsigned)gid);
		if (extra[0] != '\0')
			(void)printf("Groups   : %s\n", extra);
		(void)printf("Home     : %s/%s\n", ACCT_LOCAL_USERS, name);
		(void)printf("Shell    : %s\n", shell);
		(void)printf("Password : %s\n",
		    mode == PW_NONE ? "none" :
		    mode == PW_LOCKED ? "not set" : "set");
		(void)printf("\n");
	}
	if (walk) {
		rc = ask_yesno("OK?", true);
		if (rc != 1) {
			ds_close(h);
			return (rc == -1 ? EX_CANCELLED : EX_CANCELLED);
		}
	}

	/* ---- write --------------------------------------------------- */
	memset(&u, 0, sizeof(u));
	(void)strlcpy(u.username, name, sizeof(u.username));
	(void)strlcpy(u.realName, real, sizeof(u.realName));
	(void)strlcpy(u.shell, shell, sizeof(u.shell));
	u.uid = uid;
	u.gid = gid;
	switch (mode) {
	case PW_NONE:
		u.noPassword = true;
		break;
	case PW_LOCKED:
		(void)strlcpy(u.passwordHash, "*", sizeof(u.passwordHash));
		u.hasHash = true;
		break;
	case PW_PROMPT:
		if (hash != NULL && hash[0] != '\0') {
			(void)strlcpy(u.passwordHash, hash,
			    sizeof(u.passwordHash));
			u.hasHash = true;
		} else
			u.noPassword = true;
		break;
	}

	if (make_group) {
		memset(&g, 0, sizeof(g));
		(void)strlcpy(g.groupname, gname, sizeof(g.groupname));
		g.gid = gid;
		if ((err = ds_group_add(h, &g)) != DS_OK) {
			warnx("%s: could not create the group: %s", gname,
			    ds_strerror(err));
			ds_close(h);
			return (EX_DATABASE);
		}
	}
	if ((err = ds_user_add(h, &u)) != DS_OK) {
		warnx("%s: could not create the account: %s", name,
		    ds_strerror(err));
		ds_close(h);
		return (EX_DATABASE);
	}
	{
		char probe[1024];

		(void)strlcpy(probe, extra, sizeof(probe));
		for (tok = strtok_r(probe, ",", &brk); tok != NULL;
		    tok = strtok_r(NULL, ",", &brk))
			if ((err = ds_group_add_member(h, tok, name)) != DS_OK) {
				warnx("%s: could not add %s: %s", tok, name,
				    ds_strerror(err));
				ds_close(h);
				return (EX_DATABASE);
			}
	}
	if ((err = ds_commit(h)) != DS_OK) {
		warnx("could not write the directory: %s", ds_strerror(err));
		ds_close(h);
		return (EX_DATABASE);
	}
	ds_close(h);

	/* ---- the home ------------------------------------------------ */
	if (!o->nohome)
		(void)acct_make_home(name, o->quiet);
	if (!o->quiet)
		(void)printf("Added user %s.\n", name);
	return (EX_OK);
}

/*
 * A batch file. Six colon-separated fields, deliberately not FreeBSD's
 * ten: name:uid:gid:gecos:shell:password. Empty uid or gid means allocate.
 * The password field is a hash, not a plaintext password. A leading
 * "#format:" line is accepted and checked so a future format can be told
 * apart from this one.
 */
static int
run_batch(const struct opts *o)
{
	FILE *f;
	char line[2048];
	int status = EX_OK, n = 0;

	if (strcmp(o->batch, "-") == 0)
		f = stdin;
	else if ((f = fopen(o->batch, "r")) == NULL) {
		warn("%s", o->batch);
		return (EX_USAGE);
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		char *fields[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
		char *p = line, *brk;
		uid_t uid = (uid_t)-1;
		gid_t gid = (gid_t)-1;
		int i, rc;

		line[strcspn(line, "\r\n")] = '\0';
		n++;
		if (strncmp(line, "#format:", 8) == 0) {
			if (strstr(line, "nextbsd-1") == NULL) {
				warnx("%s:%d: unknown batch format: %s",
				    o->batch, n, line);
				status = EX_USAGE;
				break;
			}
			continue;
		}
		if (line[0] == '#' || line[0] == '\0')
			continue;

		for (i = 0; i < 6; i++) {
			fields[i] = strsep(&p, ":");
			if (fields[i] == NULL)
				break;
		}
		if (i < 6 || fields[0] == NULL || fields[0][0] == '\0') {
			warnx("%s:%d: needs six colon-separated fields "
			    "(name:uid:gid:gecos:shell:password)", o->batch, n);
			status = EX_USAGE;
			continue;
		}
		if (fields[1][0] != '\0')
			uid = (uid_t)strtoul(fields[1], &brk, 10);
		if (fields[2][0] != '\0')
			gid = (gid_t)strtoul(fields[2], &brk, 10);

		rc = add_one(o, false, fields[0], uid, gid, fields[3],
		    fields[4], fields[5]);
		if (rc != EX_OK) {
			warnx("%s:%d: %s was not created", o->batch, n,
			    fields[0]);
			status = EX_USAGE;	/* continue on error, like FreeBSD */
		}
	}
	if (f != stdin)
		(void)fclose(f);
	return (status);
}

int
main(int argc, char *argv[])
{
	static struct option longopts[] = {
		{ "admin", no_argument, NULL, 'A' },
		{ NULL, 0, NULL, 0 }
	};
	struct opts o;
	int ch, rc;

	memset(&o, 0, sizeof(o));
	o.pwmode = PW_PROMPT;
	o.uid = (uid_t)-1;

	while ((ch = getopt_long(argc, argv, "c:Df:g:G:hk:m:p:qs:Su:w:",
	    longopts, NULL)) != -1) {
		switch (ch) {
		case 'A':
			o.admin = true;
			break;
		case 'c':
			o.realname = optarg;
			break;
		case 'D':
			o.nohome = true;
			break;
		case 'f':
			o.batch = optarg;
			break;
		case 'g':
			o.group = optarg;
			break;
		case 'G':
			o.groups = optarg;
			break;
		case 'k':
			/*
			 * FreeBSD's skeleton directory. Our homes come from
			 * /System/Library/User Template through
			 * createhomedir(8), so there is nothing to point at.
			 * Accepted so existing scripts still run.
			 */
			warnx("-k is ignored: homes come from "
			    "/System/Library/User Template");
			break;
		case 'm':
			/* FreeBSD's welcome mail. NextBSD ships no MTA. */
			warnx("-m is ignored: no welcome message is sent");
			break;
		case 'p':
			o.hash = optarg;
			break;
		case 'q':
			o.quiet = true;
			break;
		case 's':
			o.shell = optarg;
			break;
		case 'S':
			o.noshellcheck = true;
			break;
		case 'u':
			{
				char *end;
				unsigned long v;

				errno = 0;
				v = strtoul(optarg, &end, 10);
				if (errno != 0 || *end != '\0' ||
				    v > ACCT_LAST_ID) {
					warnx("%s: not a usable uid", optarg);
					return (EX_USAGE);
				}
				o.uid = (uid_t)v;
				o.have_uid = true;
			}
			break;
		case 'w':
			if (strcmp(optarg, "no") == 0)
				o.pwmode = PW_LOCKED;
			else if (strcmp(optarg, "none") == 0)
				o.pwmode = PW_NONE;
			else if (strcmp(optarg, "yes") == 0)
				o.pwmode = PW_PROMPT;
			else {
				warnx("%s: -w takes no, none or yes", optarg);
				return (EX_USAGE);
			}
			break;
		case 'h':
			usage();
			return (EX_OK);
		default:
			usage();
			return (EX_USAGE);
		}
	}
	argc -= optind;
	argv += optind;
	if (argc > 1) {
		usage();
		return (EX_USAGE);
	}
	if (argc == 1)
		o.name = argv[0];

#ifdef LIBDS_TEST
	/*
	 * Test hook, the same shape as libds's ds_set_dirs: point the tool at
	 * a fixture directory and skip the root check, so the whole flow can
	 * be exercised on a build host. Compiled out of the shipped binary.
	 */
	{
		const char *fixture = getenv("NEXTBSD_DS_DIR");

		if (fixture != NULL && fixture[0] != '\0')
			ds_set_dirs(fixture, NULL);
		else if (geteuid() != 0) {
			warnx("must be run as root");
			return (EX_REFUSED);
		}
	}
#else
	if (geteuid() != 0) {
		warnx("must be run as root");
		return (EX_REFUSED);
	}
#endif
	if (o.batch != NULL) {
		if (o.name != NULL) {
			warnx("-f and a name on the command line are "
			    "mutually exclusive");
			return (EX_USAGE);
		}
		return (run_batch(&o));
	}

	for (;;) {
		rc = add_one(&o, true, NULL, (uid_t)-1, (gid_t)-1, "", "",
		    NULL);
		if (rc != EX_OK)
			return (rc);
		/* A name or -c on the command line means one account. */
		if (o.name != NULL)
			return (EX_OK);
		if (ask_yesno("Add another user?", false) != 1)
			return (EX_OK);
		(void)printf("\n");
	}
}
