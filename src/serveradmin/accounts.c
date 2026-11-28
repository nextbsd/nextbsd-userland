/*
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * serveradmin users, quota, directory: the Users section.
 *
 *   users list
 *   users add NAME "FULL NAME" [--admin]   (no password, like admin)
 *   users password NAME                    (the new password on stdin)
 *   users remove NAME
 *   users admin NAME on|off
 *   quota set USER SIZE|none               (zfs quota on the home dataset)
 *   directory status|servers|promote|demote|leave|join SERVER
 *
 * Accounts are NextBSD's directory (dsstatus, adduser, rmuser, pw); every
 * home folder under /Local/Users is its own dataset.
 */

#include <sys/types.h>

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include "settingshelper_types.h"
#include "settingshelperUser.h"

#include <ctype.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "serveradmin.h"

#define	ADDUSER		"/usr/sbin/adduser"
#define	RMUSER		"/usr/sbin/rmuser"
#define	PW		"/usr/sbin/pw"
#define	HOMES		"/Local/Users/"
#define	FIRST_UID	5000
#define	ADMIN_GROUP	"admin"

void	sa_mdns_browse_json(const char *type, const char *skip_host);

static bool
in_group(const char *user, const char *group)
{
	struct group *g = getgrnam(group);

	if (g == NULL)
		return (false);
	for (char **m = g->gr_mem; *m != NULL; m++)
		if (strcmp(*m, user) == 0)
			return (true);
	return (false);
}

static int
admin_count(void)
{
	struct group *g = getgrnam(ADMIN_GROUP);
	int n = 0;

	if (g != NULL)
		for (char **m = g->gr_mem; *m != NULL; m++)
			n++;
	return (n);
}

/* used and quota of /Local/Users/<name>, if it's its own dataset. */
static bool
home_space(const char *home, int64_t *used, int64_t *quota)
{
	const char *argv[] = { ZFS, "list", "-Hp", "-o", "mountpoint,used,quota",
	    home, NULL };
	struct sa_out o;
	char **f;
	int nf;
	bool ok = false;

	*used = *quota = 0;
	if (sa_run(argv, &o)) {
		o.out[strcspn(o.out, "\n")] = '\0';
		f = sa_fields(o.out, "\t", &nf);
		if (nf >= 3 && strcmp(f[0], home) == 0) {
			*used = strtoll(f[1], NULL, 10);
			*quota = strtoll(f[2], NULL, 10);
			ok = true;
		}
		free(f);
	}
	sa_out_free(&o);
	return (ok);
}

static struct passwd *must_user(const char *);

/*
 * The password goes to SettingsHelper (sh_set_password), which hashes it
 * as passwd(1) does and clears noPassword; root needs no right. pw(8)'s
 * -h isn't NextBSD's: directory accounts keep their hash in Users.plist.
 */
static void
set_password(const char *name)
{
	mach_port_t port = MACH_PORT_NULL;
	SHAuthForm auth;
	SHUserName user;
	SHPassword pass;
	size_t n;
	int status = SH_ERR_FAILED;

	(void)must_user(name);
	if (strlen(name) >= sizeof(user))
		sa_fail("invalid", "name too long");
	memset(auth, 0, sizeof(auth));
	memset(user, 0, sizeof(user));
	memset(pass, 0, sizeof(pass));
	strlcpy(user, name, sizeof(user));
	if (fgets(pass, sizeof(pass), stdin) == NULL || pass[0] == '\n' ||
	    pass[0] == '\0')
		sa_fail("invalid", "the new password goes on stdin");
	n = strlen(pass);
	if (pass[n - 1] == '\n')
		pass[--n] = '\0';
	else if (n == sizeof(pass) - 1)
		sa_fail("invalid", "password too long");
	if (bootstrap_look_up(bootstrap_port, SH_SERVICE_NAME, &port) !=
	    KERN_SUCCESS)
		sa_fail("unavailable", "SettingsHelper is not running");
	if (sh_set_password(port, auth, user, pass, &status) != KERN_SUCCESS)
		status = SH_ERR_FAILED;
	explicit_bzero(pass, sizeof(pass));
	if (status != SH_OK)
		sa_fail("failed", "SettingsHelper refused the password (%d)",
		    status);
}

static struct passwd *
must_user(const char *name)
{
	struct passwd *pw;

	if (!sa_is_user(name))
		sa_usage("not an account name: %s", name);
	if ((pw = getpwnam(name)) == NULL || pw->pw_uid < FIRST_UID ||
	    pw->pw_uid >= 65534)
		sa_fail("not-found", "no account named %s", name);
	return (pw);
}

static void
ok(void)
{
	j_obj();
	j_kbool("ok", true);
	j_end();
	j_flush();
}

static bool
valid_full_name(const char *s)
{
	size_t n = strlen(s);

	if (n < 1 || n > 128)
		return (false);
	for (size_t i = 0; i < n; i++)
		if (s[i] == ':' || s[i] == '\n' || s[i] == '\r' ||
		    (unsigned char)s[i] < 0x20)
			return (false);
	return (true);
}

void
cmd_users(int argc, char **argv)
{
	struct passwd *pw;

	if (argc == 1 && strcmp(argv[0], "list") == 0) {
		j_obj();
		j_kint("admins", admin_count());
		j_key("users");
		j_arr();
		setpwent();
		while ((pw = getpwent()) != NULL) {
			int64_t used, quota;
			char *comma;
			char full[256];

			if (pw->pw_uid < FIRST_UID || pw->pw_uid >= 65534)
				continue;
			strlcpy(full, pw->pw_gecos, sizeof(full));
			if ((comma = strchr(full, ',')) != NULL)
				*comma = '\0';
			j_obj();
			j_kstr("name", pw->pw_name);
			j_kstr("full_name", full[0] != '\0' ? full : pw->pw_name);
			j_kint("uid", pw->pw_uid);
			j_kstr("home", pw->pw_dir);
			j_kbool("admin", in_group(pw->pw_name, ADMIN_GROUP));
			if (strncmp(pw->pw_dir, HOMES, strlen(HOMES)) == 0 &&
			    home_space(pw->pw_dir, &used, &quota)) {
				j_kint("used", used);
				j_kint("quota", quota);
			} else {
				j_key("used"); j_null();
				j_key("quota"); j_null();
			}
			j_end();
		}
		endpwent();
		j_aend();
		j_end();
		j_flush();
		return;
	}
	if ((argc == 3 || argc == 4) && strcmp(argv[0], "add") == 0) {
		bool admin = argc == 4 && strcmp(argv[3], "--admin") == 0;
		const char *a[] = { ADDUSER, "-q", "-c", argv[2], "-w", "none",
		    admin ? "--admin" : NULL, NULL, NULL };

		if (argc == 4 && !admin)
			sa_usage("usage: serveradmin users add NAME \"FULL NAME\" [--admin]");
		if (!sa_is_user(argv[1]))
			sa_usage("not an account name: %s", argv[1]);
		if (!valid_full_name(argv[2]))
			sa_usage("not a full name");
		if (getpwnam(argv[1]) != NULL)
			sa_fail("exists", "an account named %s exists", argv[1]);
		if (admin)
			a[7] = argv[1];
		else
			a[6] = argv[1];
		sa_must("adduser", a);
		ok();
		return;
	}
	if (argc == 2 && strcmp(argv[0], "password") == 0) {
		set_password(argv[1]);
		ok();
		return;
	}
	if (argc == 2 && strcmp(argv[0], "remove") == 0) {
		const char *a[] = { RMUSER, "-y", argv[1], NULL };

		pw = must_user(argv[1]);
		if (in_group(argv[1], ADMIN_GROUP) && admin_count() <= 1)
			sa_fail("last-admin", "%s is the only administrator", argv[1]);
		sa_must("rmuser", a);
		ok();
		return;
	}
	if (argc == 3 && strcmp(argv[0], "admin") == 0) {
		bool on;
		const char *a[] = { PW, "groupmod", ADMIN_GROUP, "-m", argv[1], NULL };

		(void)must_user(argv[1]);
		if (!sa_is_onoff(argv[2], &on))
			sa_usage("admin takes on or off");
		if (!on) {
			if (!in_group(argv[1], ADMIN_GROUP)) {
				ok();
				return;
			}
			if (admin_count() <= 1)
				sa_fail("last-admin", "%s is the only administrator", argv[1]);
			a[3] = "-d";
		} else if (in_group(argv[1], ADMIN_GROUP)) {
			ok();
			return;
		}
		sa_must("pw groupmod", a);
		ok();
		return;
	}
	sa_usage("usage: serveradmin users list | add NAME \"FULL NAME\" [--admin] | password NAME | remove NAME | admin NAME on|off");
}

void
cmd_quota(int argc, char **argv)
{
	struct passwd *pw;
	char prop[64], ds[512];
	uint64_t size;
	int64_t used, quota;

	if (argc != 3 || strcmp(argv[0], "set") != 0)
		sa_usage("usage: serveradmin quota set USER SIZE|none");
	pw = must_user(argv[1]);
	if (strncmp(pw->pw_dir, HOMES, strlen(HOMES)) != 0 ||
	    !home_space(pw->pw_dir, &used, &quota))
		sa_fail("unsupported", "%s's home folder isn't its own dataset", argv[1]);
	if (strcmp(argv[2], "none") == 0)
		strlcpy(prop, "quota=none", sizeof(prop));
	else if (sa_parse_size(argv[2], &size)) {
		if (size < (uint64_t)used)
			sa_fail("invalid", "%s already uses more than that", argv[1]);
		snprintf(prop, sizeof(prop), "quota=%llu", (unsigned long long)size);
	} else
		sa_usage("not a size: %s", argv[2]);
	{
		const char *a[] = { ZFS, "list", "-H", "-o", "name", pw->pw_dir, NULL };
		struct sa_out o;

		if (!sa_run(a, &o))
			sa_fail("failed", "zfs list %s", pw->pw_dir);
		strlcpy(ds, o.out, sizeof(ds));
		ds[strcspn(ds, "\n")] = '\0';
		sa_out_free(&o);
	}
	{
		const char *a[] = { ZFS, "set", prop, ds, NULL };
		sa_must("zfs set quota", a);
	}
	home_space(pw->pw_dir, &used, &quota);
	j_obj();
	j_kint("used", used);
	j_kint("quota", quota);
	j_end();
	j_flush();
}

/* A server for dsjoin: a .local label (optionally with .local) or an
 * IPv4 address. */
static bool
valid_server(const char *s)
{
	char t[80];
	size_t n = strlen(s);
	int dots = 0;
	bool digits = true;

	if (n < 1 || n >= sizeof(t))
		return (false);
	strlcpy(t, s, sizeof(t));
	if (n > 6 && strcmp(t + n - 6, ".local") == 0)
		t[n - 6] = '\0';
	for (const char *p = s; *p; p++) {
		if (*p == '.')
			dots++;
		else if (!isdigit((unsigned char)*p))
			digits = false;
	}
	if (digits && dots == 3)
		return (true);
	return (sa_is_label(t));
}

/*
 * dsstatus' first line: "standalone", "directory server" (this machine
 * shares its accounts) or "bound to <server>" (it uses another's).
 */
static void
ds_role(char *role, size_t rlen, char *server, size_t slen)
{
	const char *a[] = { "/usr/sbin/dsstatus", NULL };
	struct sa_out o;

	strlcpy(role, "standalone", rlen);
	server[0] = '\0';
	if (sa_run(a, &o)) {
		char *l = o.out;
		l[strcspn(l, "\n")] = '\0';
		if (strncmp(l, "directory server", 16) == 0)
			strlcpy(role, "server", rlen);
		else if (strncmp(l, "bound to ", 9) == 0) {
			strlcpy(role, "client", rlen);
			strlcpy(server, l + 9, slen);
		}
	}
	sa_out_free(&o);
}

static void
ds_status(void)
{
	char role[32], server[256];

	ds_role(role, sizeof(role), server, sizeof(server));
	j_kstr("role", role);
	j_kstr("server", server);
}

void
cmd_directory(int argc, char **argv)
{
	char host[256];

	if (argc == 1 && strcmp(argv[0], "status") == 0) {
		j_obj();
		ds_status();
		j_end();
		j_flush();
		return;
	}
	if (argc == 1 && strcmp(argv[0], "servers") == 0) {
		gethostname(host, sizeof(host));
		j_obj();
		j_key("servers");
		sa_mdns_browse_json("_nextbsd-ds._tcp", host);
		j_end();
		j_flush();
		return;
	}
	if (argc == 1 && (strcmp(argv[0], "promote") == 0 ||
	    strcmp(argv[0], "demote") == 0 || strcmp(argv[0], "leave") == 0)) {
		const char *tool = argv[0][0] == 'p' ? "/usr/sbin/dspromote" :
		    argv[0][0] == 'd' ? "/usr/sbin/dsdemote" : "/usr/sbin/dsleave";
		const char *a[] = { tool, NULL };

		sa_must(tool + 10, a);
		j_obj();
		ds_status();
		j_end();
		j_flush();
		return;
	}
	if (argc == 2 && strcmp(argv[0], "join") == 0) {
		const char *a[] = { "/usr/sbin/dsjoin", argv[1], NULL };

		if (!valid_server(argv[1]))
			sa_usage("not a server: %s", argv[1]);
		sa_must("dsjoin", a);
		/* The server's /Local mounts on /Network a few seconds later;
		 * only then is this machine bound. */
		for (int i = 0; i < 30; i++) {
			char role[32], server[256];
			ds_role(role, sizeof(role), server, sizeof(server));
			if (strcmp(role, "client") == 0)
				break;
			sleep(1);
		}
		j_obj();
		ds_status();
		j_end();
		j_flush();
		return;
	}
	sa_usage("usage: serveradmin directory status|servers|promote|demote|leave|join SERVER");
}
