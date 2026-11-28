/*
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * serveradmin pool, disk: the Storage section, at the pool level only.
 *
 *   pool list
 *   pool scrub POOL | scrub-stop POOL
 *   pool create NAME single|mirror|raidz DISK...
 *   pool add POOL single|mirror|raidz DISK...
 *   pool attach POOL DISK NEWDISK        (one disk becomes a mirror)
 *   disk list                            (disks not in use)
 *
 * A disk given to create, add or attach must be one disk list reports
 * as not in use; the layout must suit the number of disks.
 */

#include <sys/types.h>
#include <sys/sysctl.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "serveradmin.h"

#define	DISKINFO	"/usr/sbin/diskinfo"
#define	GEOM		"/sbin/geom"

/* ---- disks ---------------------------------------------------------- */

/* Every disk, from kern.disks ("vtbd2 vtbd1 vtbd0 cd0"). */
static int
all_disks(char names[][32], int max)
{
	char buf[1024], *p, *t;
	size_t n = sizeof(buf);
	int c = 0;

	if (sysctlbyname("kern.disks", buf, &n, NULL, 0) == -1)
		return (0);
	buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';
	p = buf;
	while ((t = strsep(&p, " ")) != NULL && c < max) {
		if (*t == '\0' || strncmp(t, "cd", 2) == 0 ||
		    strncmp(t, "md", 2) == 0)
			continue;
		strlcpy(names[c++], t, sizeof(names[0]));
	}
	return (c);
}

/* The disk a device belongs to: vtbd0p3 -> vtbd0, ada0s1a -> ada0. */
static void
disk_of(const char *dev, char *out, size_t len)
{
	const char *b = strrchr(dev, '/');
	size_t i;

	strlcpy(out, b != NULL ? b + 1 : dev, len);
	for (i = 0; out[i] != '\0' && !isdigit((unsigned char)out[i]); i++)
		;
	while (isdigit((unsigned char)out[i]))
		i++;
	out[i] = '\0';
}

/* Is the disk part of a pool, mounted, or swap? */
static bool
disk_in_use(const char *disk)
{
	const char *zs[] = { ZPOOL, "status", "-P", NULL };
	const char *mt[] = { "/sbin/mount", "-p", NULL };
	const char *sw[] = { "/usr/sbin/swapctl", "-l", NULL };
	const char *const *cmds[] = { zs, mt, sw };
	struct sa_out o;
	char d[32];
	bool used = false;

	for (int c = 0; c < 3 && !used; c++) {
		char **l;
		int n;

		if (!sa_run(cmds[c], &o)) {
			sa_out_free(&o);
			continue;
		}
		l = sa_lines(o.out, &n);
		for (int i = 0; i < n && !used; i++) {
			char *p = strstr(l[i], "/dev/");
			if (p == NULL)
				continue;
			p[strcspn(p, " \t")] = '\0';
			disk_of(p, d, sizeof(d));
			used = strcmp(d, disk) == 0;
		}
		free(l);
		sa_out_free(&o);
	}
	return (used);
}

static int64_t
disk_size(const char *disk)
{
	const char *a[] = { DISKINFO, disk, NULL };
	struct sa_out o;
	char **f;
	int nf;
	int64_t s = 0;

	if (sa_run(a, &o)) {
		o.out[strcspn(o.out, "\n")] = '\0';
		f = sa_fields(o.out, "\t", &nf);
		if (nf >= 3)
			s = strtoll(f[2], NULL, 10);
		free(f);
	}
	sa_out_free(&o);
	return (s);
}

static void
disk_descr(const char *disk, char *out, size_t len)
{
	const char *a[] = { GEOM, "disk", "list", disk, NULL };
	struct sa_out o;
	char *p;

	out[0] = '\0';
	if (sa_run(a, &o) && (p = strstr(o.out, "descr: ")) != NULL) {
		strlcpy(out, p + 7, len);
		out[strcspn(out, "\n")] = '\0';
	}
	sa_out_free(&o);
}

static bool
disk_free(const char *disk)
{
	char names[64][32];
	int n = all_disks(names, 64);

	for (int i = 0; i < n; i++)
		if (strcmp(names[i], disk) == 0)
			return (!disk_in_use(disk));
	return (false);
}

void
cmd_disk(int argc, char **argv)
{
	char names[64][32], descr[256];
	int n;

	(void)argv;
	if (argc != 1 || strcmp(argv[0], "list") != 0)
		sa_usage("usage: serveradmin disk list");
	n = all_disks(names, 64);
	j_obj();
	j_key("disks");
	j_arr();
	for (int i = n - 1; i >= 0; i--) {
		if (disk_in_use(names[i]))
			continue;
		disk_descr(names[i], descr, sizeof(descr));
		j_obj();
		j_kstr("name", names[i]);
		j_kint("size", disk_size(names[i]));
		j_kstr("description", descr);
		j_end();
	}
	j_aend();
	j_end();
	j_flush();
}

/* ---- pools ---------------------------------------------------------- */

static bool
pool_exists(const char *pool)
{
	const char *a[] = { ZPOOL, "list", "-H", "-o", "name", pool, NULL };
	struct sa_out o;
	bool ok = sa_run(a, &o);

	sa_out_free(&o);
	return (ok);
}

/* The config section of zpool status -P, as nested JSON groups. */
static void
pool_detail(const char *pool)
{
	const char *a[] = { ZPOOL, "status", "-P", pool, NULL };
	struct sa_out o;
	char **l, scan[512] = "";
	int n, i;
	bool in_config = false, group_open = false, top_open = false;
	char section[16] = "data";

	if (!sa_run(a, &o)) {
		sa_out_free(&o);
		return;
	}
	l = sa_lines(o.out, &n);
	for (i = 0; i < n; i++) {
		char *p = l[i];
		while (*p == ' ' || *p == '\t')
			p++;
		if (strncmp(p, "scan:", 5) == 0) {
			p += 5;
			while (*p == ' ')
				p++;
			strlcpy(scan, p, sizeof(scan));
			/* a scan line may continue on the next one */
			if (i + 1 < n && strncmp(l[i + 1], "\t", 1) == 0 &&
			    strchr(l[i + 1], ':') == NULL) {
				char *q = l[i + 1];
				while (*q == ' ' || *q == '\t')
					q++;
				strlcat(scan, " ", sizeof(scan));
				strlcat(scan, q, sizeof(scan));
			}
		}
	}
	j_kstr("scan", scan);
	j_kbool("scrubbing", strstr(scan, "scrub in progress") != NULL);
	j_key("groups");
	j_arr();
	for (i = 0; i < n; i++) {
		char *line = l[i], *p = line, **f;
		int depth, nf;

		if (strncmp(p, "\tNAME", 5) == 0 || strstr(p, "NAME ") == p + 1) {
			in_config = true;
			continue;
		}
		if (!in_config)
			continue;
		if (strncmp(p, "errors:", 7) == 0)
			break;
		if (*p != '\t')
			continue;
		p++;
		depth = 0;
		while (*p == ' ') {
			depth++;
			p++;
		}
		depth /= 2;
		f = sa_fields(p, " ", &nf);
		{
			/* fields: name state read write cksum (blank-separated) */
			char *name = f[0], *state = "";
			for (int k = 1; k < nf; k++)
				if (f[k][0] != '\0') {
					state = f[k];
					break;
				}
			if (depth == 0) {
				if (strcmp(name, pool) != 0) {
					/* logs, cache, spares */
					strlcpy(section, name, sizeof(section));
				}
				free(f);
				continue;
			}
			if (depth == 1) {
				if (group_open) {
					j_aend();
					j_end();
					group_open = false;
				}
				j_obj();
				j_kstr("section", section);
				if (strncmp(name, "mirror", 6) == 0)
					j_kstr("layout", "mirror");
				else if (strncmp(name, "raidz", 5) == 0)
					j_kstr("layout", "raidz");
				else
					j_kstr("layout", "single");
				j_kstr("state", state);
				j_key("disks");
				j_arr();
				group_open = true;
				top_open = true;
				if (strncmp(name, "/dev/", 5) == 0) {
					char d[32];
					disk_of(name, d, sizeof(d));
					j_obj();
					j_kstr("disk", d);
					j_kstr("device", name + 5);
					j_kstr("state", state);
					j_kint("size", disk_size(d));
					j_end();
				}
			} else if (depth >= 2 && group_open &&
			    strncmp(name, "/dev/", 5) == 0) {
				char d[32];
				disk_of(name, d, sizeof(d));
				j_obj();
				j_kstr("disk", d);
				j_kstr("device", name + 5);
				j_kstr("state", state);
				j_kint("size", disk_size(d));
				j_end();
			}
		}
		free(f);
	}
	if (group_open) {
		j_aend();
		j_end();
	}
	(void)top_open;
	j_aend();
	free(l);
	sa_out_free(&o);
}

static void
pool_list(void)
{
	const char *a[] = { ZPOOL, "list", "-Hp", "-o",
	    "name,size,alloc,free,health", NULL };
	struct sa_out o;
	char **l, **f;
	int n, nf;

	j_obj();
	j_kstr("main_pool", sa_main_pool());
	j_key("pools");
	j_arr();
	if (sa_run(a, &o)) {
		l = sa_lines(o.out, &n);
		for (int i = 0; i < n; i++) {
			f = sa_fields(l[i], "\t", &nf);
			if (nf >= 5) {
				j_obj();
				j_kstr("name", f[0]);
				j_kint("size", strtoll(f[1], NULL, 10));
				j_kint("used", strtoll(f[2], NULL, 10));
				j_kint("free", strtoll(f[3], NULL, 10));
				j_kstr("health", f[4]);
				j_kbool("main", sa_main_pool() != NULL &&
				    strcmp(f[0], sa_main_pool()) == 0);
				pool_detail(f[0]);
				j_end();
			}
			free(f);
		}
		free(l);
	}
	sa_out_free(&o);
	j_aend();
	j_end();
	j_flush();
}

/* Check the disks and the layout; fill argv from position *k. */
static void
layout_args(const char *layout, int nd, char **disks, const char **v, int *k,
    int min_mirror)
{
	if (strcmp(layout, "single") == 0) {
		if (nd < 1)
			sa_usage("single takes at least one disk");
	} else if (strcmp(layout, "mirror") == 0) {
		if (nd < min_mirror)
			sa_usage("a mirror takes at least %d disks", min_mirror);
		v[(*k)++] = "mirror";
	} else if (strcmp(layout, "raidz") == 0) {
		if (nd < 3)
			sa_usage("RAID-Z takes at least 3 disks");
		v[(*k)++] = "raidz";
	} else
		sa_usage("layout must be single, mirror or raidz");
	for (int i = 0; i < nd; i++) {
		for (int j = 0; j < i; j++)
			if (strcmp(disks[i], disks[j]) == 0)
				sa_usage("%s is listed twice", disks[i]);
		if (!disk_free(disks[i]))
			sa_fail("disk-in-use", "%s isn't a disk that's free to use", disks[i]);
		v[(*k)++] = disks[i];
	}
}

void
cmd_pool(int argc, char **argv)
{
	if (argc == 1 && strcmp(argv[0], "list") == 0) {
		pool_list();
		return;
	}
	if (argc == 2 && (strcmp(argv[0], "scrub") == 0 ||
	    strcmp(argv[0], "scrub-stop") == 0)) {
		const char *a[] = { ZPOOL, "scrub", NULL, NULL, NULL };

		if (!sa_is_pool(argv[1]) || !pool_exists(argv[1]))
			sa_fail("not-found", "no pool named %s", argv[1]);
		if (argv[0][5] == '-') {
			a[2] = "-s";
			a[3] = argv[1];
		} else
			a[2] = argv[1];
		sa_must("zpool scrub", a);
		j_obj(); j_kbool("ok", true); j_end(); j_flush();
		return;
	}
	if (argc >= 3 && (strcmp(argv[0], "create") == 0 ||
	    strcmp(argv[0], "add") == 0)) {
		const char *v[80];
		int k = 0, nd = argc - 3;
		bool create = argv[0][0] == 'c';
		char mnt[80];

		if (nd > 60)
			sa_usage("too many disks");
		if (!sa_is_pool(argv[1]))
			sa_usage("not a pool name: %s", argv[1]);
		if (create && pool_exists(argv[1]))
			sa_fail("exists", "a pool named %s exists", argv[1]);
		if (!create && !pool_exists(argv[1]))
			sa_fail("not-found", "no pool named %s", argv[1]);
		v[k++] = ZPOOL;
		v[k++] = argv[0];
		/* The person was told the disks are erased. */
		v[k++] = "-f";
		if (create) {
			snprintf(mnt, sizeof(mnt), "/%s", argv[1]);
			v[k++] = "-m";
			v[k++] = mnt;
		}
		v[k++] = argv[1];
		layout_args(argv[2], nd, argv + 3, v, &k, 2);
		v[k] = NULL;
		sa_must(create ? "zpool create" : "zpool add", v);
		j_obj(); j_kbool("ok", true); j_end(); j_flush();
		return;
	}
	if (argc == 4 && strcmp(argv[0], "attach") == 0) {
		const char *a[] = { ZPOOL, "attach", "-f", argv[1], NULL, argv[3], NULL };
		char dev[64] = "";

		if (!sa_is_pool(argv[1]) || !pool_exists(argv[1]))
			sa_fail("not-found", "no pool named %s", argv[1]);
		if (!disk_free(argv[3]))
			sa_fail("disk-in-use", "%s isn't a disk that's free to use", argv[3]);
		/* The existing device: the pool's member on that disk. */
		{
			const char *zs[] = { ZPOOL, "status", "-P", argv[1], NULL };
			struct sa_out o;
			char **l, d[32];
			int n;

			if (sa_run(zs, &o)) {
				l = sa_lines(o.out, &n);
				for (int i = 0; i < n; i++) {
					char *p = strstr(l[i], "/dev/");
					if (p == NULL)
						continue;
					p[strcspn(p, " \t")] = '\0';
					disk_of(p, d, sizeof(d));
					if (strcmp(d, argv[2]) == 0) {
						strlcpy(dev, p + 5, sizeof(dev));
						break;
					}
				}
				free(l);
			}
			sa_out_free(&o);
		}
		if (dev[0] == '\0')
			sa_fail("not-found", "%s isn't in pool %s", argv[2], argv[1]);
		a[4] = dev;
		sa_must("zpool attach", a);
		j_obj(); j_kbool("ok", true); j_end(); j_flush();
		return;
	}
	sa_usage("usage: serveradmin pool list | scrub POOL | scrub-stop POOL | create NAME single|mirror|raidz DISK... | add POOL single|mirror|raidz DISK... | attach POOL DISK NEWDISK");
}
