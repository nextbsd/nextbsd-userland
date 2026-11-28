/*
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * serveradmin(8): the command Server.app runs on each NextBSD computer,
 * as root over SSH, this computer included (root@localhost). See
 * serveradmin.h and the E42 plan on the nextbsd-userland wiki.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "serveradmin.h"

static const struct {
	const char *name;
	void (*fn)(int, char **);
} cmds[] = {
	{ "status",	cmd_status },
	{ "pair",	cmd_pair },
	{ "update",	cmd_update },
	{ "users",	cmd_users },
	{ "quota",	cmd_quota },
	{ "directory",	cmd_directory },
	{ "pool",	cmd_pool },
	{ "disk",	cmd_disk },
	{ "jailbase",	cmd_jailbase },
	{ "jail",	cmd_jail },
	{ "crashdumps",	cmd_crashdumps },
	{ "log",	cmd_log },
	{ NULL,		NULL }
};

static void
usage(void)
{
	fputs("usage: serveradmin <command> [arguments] [--json]\n"
	    "commands: status pair update users quota directory pool disk\n"
	    "          jailbase jail crashdumps log\n"
	    "See serveradmin(8).\n", stderr);
	exit(2);
}

int
main(int argc, char **argv)
{
	int n = 0;

	if (argc < 2)
		usage();
	/* --json is accepted anywhere and always on: every answer is JSON. */
	for (int i = 1; i < argc; i++)
		if (strcmp(argv[i], "--json") != 0)
			argv[++n] = argv[i];
	argv[n + 1] = NULL;
	argc = n + 1;
	if (argc < 2)
		usage();
	if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
		usage();
	if (geteuid() != 0)
		sa_fail("not-root", "serveradmin must run as root");
	for (int i = 0; cmds[i].name != NULL; i++)
		if (strcmp(argv[1], cmds[i].name) == 0) {
			cmds[i].fn(argc - 2, argv + 2);
			return (0);
		}
	sa_usage("unknown command: %s", argv[1]);
}
