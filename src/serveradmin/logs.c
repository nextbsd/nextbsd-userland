/*
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * serveradmin log: the Logs section.
 *
 *   log sessions        who is signed in now, and from where
 *   log recent [N]      the last N lines of system.log (default 200)
 *   log search WORD     recent lines containing WORD (plain text)
 */

#include <sys/types.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <utmpx.h>

#include "serveradmin.h"

#define	SYSTEM_LOG	"/private/var/log/system.log"

static void
sessions(void)
{
	struct utmpx *u;
	char t[32];

	j_obj();
	j_key("sessions");
	j_arr();
	setutxent();
	while ((u = getutxent()) != NULL) {
		time_t s;

		if (u->ut_type != USER_PROCESS)
			continue;
		s = u->ut_tv.tv_sec;
		strftime(t, sizeof(t), "%Y-%m-%dT%H:%M:%S", localtime(&s));
		j_obj();
		j_kstr("user", u->ut_user);
		j_kstr("line", u->ut_line);
		j_kstr("from", u->ut_host);
		j_kstr("since", t);
		j_end();
	}
	endutxent();
	j_aend();
	j_end();
	j_flush();
}

static void
recent(int want, const char *word)
{
	FILE *f;
	long size, start;
	char *b;
	char **l;
	int n, first;

	j_obj();
	j_key("lines");
	j_arr();
	if ((f = fopen(SYSTEM_LOG, "r")) != NULL) {
		fseek(f, 0, SEEK_END);
		size = ftell(f);
		start = size > 512 * 1024 ? size - 512 * 1024 : 0;
		fseek(f, start, SEEK_SET);
		if ((b = malloc((size_t)(size - start) + 1)) != NULL) {
			size_t r = fread(b, 1, (size_t)(size - start), f);
			b[r] = '\0';
			l = sa_lines(b, &n);
			first = start > 0 ? 1 : 0;	/* a partial first line */
			{
				int keep[4096], k = 0;
				for (int i = n - 1; i >= first && k < want; i--)
					if (word == NULL || strstr(l[i], word) != NULL)
						keep[k++] = i;
				for (int i = k - 1; i >= 0; i--)
					j_str(l[keep[i]]);
			}
			free(l);
			free(b);
		}
		fclose(f);
	}
	j_aend();
	j_end();
	j_flush();
}

void
cmd_log(int argc, char **argv)
{
	if (argc == 1 && strcmp(argv[0], "sessions") == 0)
		sessions();
	else if ((argc == 1 || argc == 2) && strcmp(argv[0], "recent") == 0) {
		int n = argc == 2 ? atoi(argv[1]) : 200;
		if (n < 1 || n > 4000)
			sa_usage("recent takes 1 to 4000 lines");
		recent(n, NULL);
	} else if (argc == 2 && strcmp(argv[0], "search") == 0) {
		if (strlen(argv[1]) < 1 || strlen(argv[1]) > 128)
			sa_usage("search takes a word of 1 to 128 characters");
		recent(500, argv[1]);
	} else
		sa_usage("usage: serveradmin log sessions | recent [N] | search WORD");
}
