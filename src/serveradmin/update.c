/*
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * serveradmin update: Update.app's operations, through the same
 * SettingsHelper (sh_update.c), so there is one implementation of
 * updating NextBSD. serveradmin runs as root, which the helper accepts
 * as a caller without asking for a credential.
 *
 *   update status            the job, the restart state, the versions
 *   update check|download|install|cancel|restart
 *   update start-version V | remove-version V | keep N
 */

#include "settingshelper_types.h"

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include "settingshelperUser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "serveradmin.h"

#define	KEEP_CONF	"/Local/Library/Preferences/org.nextbsd.bootenv-update.conf"

static const char *
sh_error(int s)
{
	switch (s) {
	case SH_OK: return ("ok");
	case SH_ERR_NOT_CONSOLE: return ("not-console");
	case SH_ERR_DENIED: return ("denied");
	case SH_ERR_AUTH_NEEDED: return ("auth-needed");
	case SH_ERR_AUTH_FAILED: return ("auth-failed");
	case SH_ERR_NOT_ADMIN: return ("not-admin");
	case SH_ERR_INVALID: return ("invalid");
	case SH_ERR_UNSUPPORTED: return ("unsupported");
	case SH_ERR_BUSY: return ("busy");
	default: return ("failed");
	}
}

static mach_port_t
helper(void)
{
	mach_port_t port = MACH_PORT_NULL;

	if (bootstrap_look_up(bootstrap_port, SH_SERVICE_NAME, &port) !=
	    KERN_SUCCESS)
		sa_fail("unavailable", "SettingsHelper is not running");
	return (port);
}

static void
done(int status, const char *what)
{
	if (status != SH_OK)
		sa_fail(sh_error(status), "%s: %s", what, sh_error(status));
	j_obj();
	j_kbool("ok", true);
	j_end();
	j_flush();
}

static int
read_keep(void)
{
	FILE *f;
	char line[128];
	int n = 3;

	if ((f = fopen(KEEP_CONF, "r")) == NULL)
		return (n);
	while (fgets(line, sizeof(line), f) != NULL)
		if (strncmp(line, "keep=", 5) == 0)
			n = atoi(line + 5);
	fclose(f);
	return (n < 2 ? 2 : n);
}

static void
versions(void)
{
	const char *argv[] = { BECTL, "list", "-H", NULL };
	struct sa_out o;
	char **l, **f;
	int n, nf;

	j_key("versions");
	j_arr();
	if (sa_run(argv, &o)) {
		l = sa_lines(o.out, &n);
		for (int i = 0; i < n; i++) {
			f = sa_fields(l[i], "\t", &nf);
			if (nf >= 4 && f[0][0] != '\0') {
				j_obj();
				j_kstr("name", f[0]);
				j_kbool("running", strchr(f[1], 'N') != NULL);
				j_kbool("next", strchr(f[1], 'R') != NULL);
				j_kstr("space", f[3]);
				j_end();
			}
			free(f);
		}
		free(l);
	}
	j_aend();
	sa_out_free(&o);
}

void
cmd_update(int argc, char **argv)
{
	mach_port_t port;
	SHAuthForm auth;
	int status = SH_ERR_FAILED;
	kern_return_t kr = KERN_FAILURE;

	if (argc < 1)
		sa_usage("usage: serveradmin update status|check|download|install|cancel|restart|start-version V|remove-version V|keep N");
	memset(auth, 0, sizeof(auth));
	port = helper();
	if (strcmp(argv[0], "status") == 0 && argc == 1) {
		int job = 0, state = 0, restart = 0;
		boolean_t pending = FALSE, changed = FALSE;
		SHUpdateVersion avail, rver;
		SHUpdateText line;
		int64_t size = 0, d = 0, total = 0;

		memset(avail, 0, sizeof(avail));
		memset(rver, 0, sizeof(rver));
		memset(line, 0, sizeof(line));
		kr = sh_update_status(port, auth, &job, &state, &pending, avail,
		    &size, &d, &total, line, &changed, &restart, rver, &status);
		if (kr != KERN_SUCCESS)
			sa_fail("unavailable", "SettingsHelper did not answer");
		if (status != SH_OK)
			sa_fail(sh_error(status), "update status: %s", sh_error(status));
		avail[sizeof(avail) - 1] = rver[sizeof(rver) - 1] = '\0';
		line[sizeof(line) - 1] = '\0';
		j_obj();
		j_kstr("job", job == SH_UPDATE_JOB_CHECK ? "check" :
		    job == SH_UPDATE_JOB_DOWNLOAD ? "download" :
		    job == SH_UPDATE_JOB_INSTALL ? "install" : "none");
		j_kstr("state", state == SH_UPDATE_RUNNING ? "running" :
		    state == SH_UPDATE_DONE ? "done" :
		    state == SH_UPDATE_FAILED ? "failed" :
		    state == SH_UPDATE_CANCELLED ? "cancelled" : "idle");
		j_kbool("pending", pending);
		j_kstr("available", avail);
		j_kint("size", size);
		j_kint("done", d);
		j_kint("total", total);
		j_kstr("line", line);
		j_kbool("changed", changed);
		j_kstr("restart", restart == SH_UPDATE_RESTART_PENDING ? "pending" :
		    restart == SH_UPDATE_RESTART_DONE ? "done" : "none");
		j_kstr("restart_version", rver);
		j_kint("keep", read_keep());
		versions();
		j_end();
		j_flush();
		return;
	}
	if (argc == 1 && strcmp(argv[0], "check") == 0)
		kr = sh_update_check(port, auth, &status);
	else if (argc == 1 && strcmp(argv[0], "download") == 0)
		kr = sh_update_download(port, auth, &status);
	else if (argc == 1 && strcmp(argv[0], "install") == 0)
		kr = sh_update_install(port, auth, &status);
	else if (argc == 1 && strcmp(argv[0], "cancel") == 0)
		kr = sh_update_cancel(port, auth, &status);
	else if (argc == 1 && strcmp(argv[0], "restart") == 0)
		kr = sh_update_restart(port, auth, &status);
	else if (argc == 2 && (strcmp(argv[0], "start-version") == 0 ||
	    strcmp(argv[0], "remove-version") == 0)) {
		SHUpdateVersion v;

		if (!sa_is_be(argv[1]))
			sa_usage("not a version name: %s", argv[1]);
		memset(v, 0, sizeof(v));
		strlcpy(v, argv[1], sizeof(v));
		kr = argv[0][0] == 's' ?
		    sh_update_start_version(port, auth, v, &status) :
		    sh_update_remove_version(port, auth, v, &status);
	} else if (argc == 2 && strcmp(argv[0], "keep") == 0) {
		int k = atoi(argv[1]);

		if (k < 2 || k > 10)
			sa_usage("keep takes 2 to 10");
		kr = sh_update_set_keep(port, auth, k, &status);
	} else
		sa_usage("unknown update command: %s", argv[0]);
	if (kr != KERN_SUCCESS)
		sa_fail("unavailable", "SettingsHelper did not answer");
	done(status, argv[0]);
}
