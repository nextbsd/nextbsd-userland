/*
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * serveradmin crashdumps: kernel crash dumps over the network (netdump).
 * One computer collects them with netdumpd (nextbsd-contrib), run by the
 * org.nextbsd.netdumpd launchd job; others send theirs with dumpon -s.
 *
 *   crashdumps status
 *   crashdumps collect on|off          (this computer collects)
 *   crashdumps send COLLECTOR | off    (this computer sends to COLLECTOR)
 *   crashdumps refresh                 (at boot: resolve COLLECTOR again)
 *   crashdumps list                    (dumps received here)
 *   crashdumps delete COMPUTER N
 *
 * dumpon -s takes addresses, so COLLECTOR, a .local name, is resolved
 * here and again at every boot (org.nextbsd.crashdumps-refresh).
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "serveradmin.h"

#define	CONF		"/Local/Library/Preferences/org.nextbsd.serveradmin.crashdumps.conf"
#define	CRASH_DIR	"/var/crash"
#define	NETDUMPD_JOB	"/System/Library/LaunchDaemons/org.nextbsd.netdumpd.plist"
#define	REFRESH_JOB	"/System/Library/LaunchDaemons/org.nextbsd.crashdumps-refresh.plist"

static void
conf_read(char *collector, size_t len)
{
	FILE *f;
	char line[256];

	collector[0] = '\0';
	if ((f = fopen(CONF, "r")) == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL)
		if (strncmp(line, "send=", 5) == 0) {
			strlcpy(collector, line + 5, len);
			collector[strcspn(collector, "\n")] = '\0';
		}
	fclose(f);
}

static void
conf_write(const char *collector)
{
	char tmp[] = CONF ".XXXXXX", t[256];
	int fd, n;

	(void)mkdir("/Local/Library/Preferences", 0755);
	if (collector == NULL || collector[0] == '\0') {
		(void)unlink(CONF);
		return;
	}
	if ((fd = mkstemp(tmp)) == -1)
		sa_fail("failed", "%s: %s", CONF, strerror(errno));
	n = snprintf(t, sizeof(t), "send=%s\n", collector);
	if (fchmod(fd, 0644) == -1 || write(fd, t, (size_t)n) != n ||
	    close(fd) == -1 || rename(tmp, CONF) == -1) {
		(void)unlink(tmp);
		sa_fail("failed", "%s: %s", CONF, strerror(errno));
	}
}

static bool
collecting(void)
{
	const char *a[] = { LAUNCHCTL, "list", "org.nextbsd.netdumpd", NULL };
	struct sa_out o;
	bool on = sa_run(a, &o) && strstr(o.out, "\"PID\"") != NULL;

	sa_out_free(&o);
	return (on);
}

static void
status(void)
{
	char c[128], ifn[32];
	bool conn;
	struct stat st;

	conf_read(c, sizeof(c));
	j_obj();
	j_kbool("available", stat("/usr/sbin/netdumpd", &st) == 0 &&
	    stat(NETDUMPD_JOB, &st) == 0);
	j_kbool("collecting", collecting());
	j_kstr("sending_to", c);
	j_kbool("wired", sa_wired(ifn, sizeof(ifn), &conn) && conn);
	j_end();
	j_flush();
}

static void
launchd(const char *verb, const char *job)
{
	const char *a[] = { LAUNCHCTL, verb, "-w", job, NULL };
	struct sa_out o;

	(void)sa_run(a, &o);	/* unload of a job that isn't loaded fails */
	sa_out_free(&o);
}

/* Point dumpon at the collector; false if the driver can't netdump. */
static bool
send_to(const char *collector, char *why, size_t len)
{
	char addr[64], own[64], gw[64], ifn[32], host[256], *dot;
	bool conn;

	if (!sa_mdns_resolve(collector, addr, sizeof(addr))) {
		snprintf(why, len, "%s.local can't be found on the network", collector);
		return (false);
	}
	if (!sa_wired(ifn, sizeof(ifn), &conn) || !conn) {
		snprintf(why, len, "this computer has no connected Ethernet");
		return (false);
	}
	if (!sa_ipv4(ifn, own, sizeof(own))) {
		snprintf(why, len, "%s has no address", ifn);
		return (false);
	}
	if (!sa_gateway(gw, sizeof(gw)))
		strlcpy(gw, addr, sizeof(gw));
	/* Dumps land in <dumpdir>/<this computer's name> on the collector. */
	gethostname(host, sizeof(host));
	if ((dot = strchr(host, '.')) != NULL)
		*dot = '\0';
	(void)sysctlbyname("net.netdump.path", NULL, NULL, host, strlen(host) + 1);
	{
		const char *a[] = { DUMPON, "-s", addr, "-c", own, "-g", gw, ifn, NULL };
		struct sa_out o;
		bool ok = sa_run(a, &o);

		if (!ok)
			snprintf(why, len, "%s can't send crash dumps over the network (%s)",
			    ifn, o.err[0] != '\0' ? o.err : "no netdump support");
		why[strcspn(why, "\n")] = '\0';
		sa_out_free(&o);
		return (ok);
	}
}

static bool
dump_name_ok(const char *s)
{
	return (sa_is_label(s));
}

/* One dump: info.<host>.<n> (and vmcore.<host>.<n>) in dir. */
static void
emit_dump(const char *dir, const char *computer, const char *infoname)
{
	char info[600], vmcore[600], line[512], why[256] = "", when[64] = "";
	char host[128], num[16];
	const char *rest = infoname + 5, *dot;
	struct stat st;
	FILE *fp;

	if ((dot = strrchr(rest, '.')) == NULL || strcmp(dot + 1, "last") == 0)
		return;
	strlcpy(num, dot + 1, sizeof(num));
	for (const char *c = num; *c; c++)
		if (!isdigit((unsigned char)*c))
			return;
	snprintf(host, sizeof(host), "%.*s", (int)(dot - rest), rest);
	snprintf(info, sizeof(info), "%s/%s", dir, infoname);
	snprintf(vmcore, sizeof(vmcore), "%s/vmcore.%s.%s", dir, host, num);
	if ((fp = fopen(info, "r")) != NULL) {
		while (fgets(line, sizeof(line), fp) != NULL) {
			char *p = line;
			while (*p == ' ' || *p == '\t')
				p++;
			if (strncmp(p, "Panic String:", 13) == 0)
				strlcpy(why, p + 14, sizeof(why));
			else if (strncmp(p, "Dumptime:", 9) == 0)
				strlcpy(when, p + 10, sizeof(when));
		}
		fclose(fp);
	}
	why[strcspn(why, "\n")] = when[strcspn(when, "\n")] = '\0';
	j_obj();
	j_kstr("computer", computer != NULL ? computer : host);
	j_kstr("host", host);
	j_kstr("number", num);
	j_kstr("panic", why);
	j_kstr("when", when);
	j_kstr("info", info);
	j_kstr("vmcore", vmcore);
	j_kint("size", stat(vmcore, &st) == 0 ? st.st_size : 0);
	j_end();
}

static void
list(void)
{
	DIR *top, *d;
	struct dirent *e, *f;
	char path[512];

	j_obj();
	j_key("dumps");
	j_arr();
	if ((top = opendir(CRASH_DIR)) != NULL) {
		while ((e = readdir(top)) != NULL) {
			if (e->d_name[0] == '.')
				continue;
			/* Dumps from a computer whose directory wasn't made. */
			if (strncmp(e->d_name, "info.", 5) == 0) {
				emit_dump(CRASH_DIR, NULL, e->d_name);
				continue;
			}
			if (!dump_name_ok(e->d_name))
				continue;
			snprintf(path, sizeof(path), CRASH_DIR "/%s", e->d_name);
			if ((d = opendir(path)) == NULL)
				continue;
			while ((f = readdir(d)) != NULL)
				if (strncmp(f->d_name, "info.", 5) == 0)
					emit_dump(path, e->d_name, f->d_name);
			closedir(d);
		}
		closedir(top);
	}
	j_aend();
	j_end();
	j_flush();
}

/* The sender's own name, from host names netdumpd writes ("nb-desktop"
 * or "QEMU-Virtual-Machine-2"): plain text only. */
static bool
host_ok(const char *s)
{
	size_t n = strlen(s);

	if (n < 1 || n > 127)
		return (false);
	for (size_t i = 0; i < n; i++)
		if (!(isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '_' ||
		    s[i] == '.'))
			return (false);
	return (strstr(s, "..") == NULL && s[0] != '.');
}

void
cmd_crashdumps(int argc, char **argv)
{
	char why[512] = "", c[128];
	bool on;

	if (argc == 1 && strcmp(argv[0], "status") == 0) {
		status();
		return;
	}
	if (argc == 2 && strcmp(argv[0], "collect") == 0) {
		struct stat st;

		if (!sa_is_onoff(argv[1], &on))
			sa_usage("collect takes on or off");
		if (on && (stat("/usr/sbin/netdumpd", &st) == -1 ||
		    stat(NETDUMPD_JOB, &st) == -1))
			sa_fail("unsupported", "netdumpd isn't installed");
		(void)mkdir(CRASH_DIR, 0700);
		launchd(on ? "load" : "unload", NETDUMPD_JOB);
		status();
		return;
	}
	if (argc == 2 && strcmp(argv[0], "send") == 0) {
		if (strcmp(argv[1], "off") == 0) {
			const char *a[] = { DUMPON, "off", NULL };
			struct sa_out o;

			(void)sa_run(a, &o);
			sa_out_free(&o);
			conf_write(NULL);
			launchd("unload", REFRESH_JOB);
			status();
			return;
		}
		if (!sa_is_label(argv[1]))
			sa_usage("not a computer name: %s", argv[1]);
		if (!send_to(argv[1], why, sizeof(why)))
			sa_fail("unsupported", "%s", why);
		conf_write(argv[1]);
		launchd("load", REFRESH_JOB);
		status();
		return;
	}
	if (argc == 1 && strcmp(argv[0], "refresh") == 0) {
		conf_read(c, sizeof(c));
		/* At boot the network and mDNSResponder may not be up yet. */
		if (c[0] != '\0') {
			int tries = 0;
			while (!send_to(c, why, sizeof(why))) {
				if (++tries >= 30)
					sa_fail("failed", "%s", why);
				sleep(2);
			}
		}
		status();
		return;
	}
	if (argc == 1 && strcmp(argv[0], "list") == 0) {
		list();
		return;
	}
	if (argc == 2 && strcmp(argv[0], "expect") == 0) {
		char p[600];

		/* netdumpd only uses a sender's net.netdump.path directory if
		 * it already exists. */
		if (!dump_name_ok(argv[1]))
			sa_usage("not a computer name: %s", argv[1]);
		(void)mkdir(CRASH_DIR, 0700);
		snprintf(p, sizeof(p), CRASH_DIR "/%s", argv[1]);
		if (mkdir(p, 0700) == -1 && errno != EEXIST)
			sa_fail("failed", "%s: %s", p, strerror(errno));
		j_obj(); j_kbool("ok", true); j_end(); j_flush();
		return;
	}
	/* delete COMPUTER HOST N: COMPUTER is the directory ("-" for the
	 * top level), HOST and N as list reports them. */
	if (argc == 4 && strcmp(argv[0], "delete") == 0) {
		char p[700], dir[300];
		bool any = false;

		if (strcmp(argv[1], "-") == 0)
			strlcpy(dir, CRASH_DIR, sizeof(dir));
		else if (dump_name_ok(argv[1]))
			snprintf(dir, sizeof(dir), CRASH_DIR "/%s", argv[1]);
		else
			sa_usage("not a computer name: %s", argv[1]);
		if (!host_ok(argv[2]))
			sa_usage("not a host name: %s", argv[2]);
		for (const char *ch = argv[3]; *ch; ch++)
			if (!isdigit((unsigned char)*ch))
				sa_usage("not a dump number: %s", argv[3]);
		if (strlen(argv[3]) < 1 || strlen(argv[3]) > 6)
			sa_usage("not a dump number: %s", argv[3]);
		snprintf(p, sizeof(p), "%s/info.%s.%s", dir, argv[2], argv[3]);
		any |= unlink(p) == 0;
		snprintf(p, sizeof(p), "%s/vmcore.%s.%s", dir, argv[2], argv[3]);
		any |= unlink(p) == 0;
		if (!any)
			sa_fail("not-found", "no dump %s from %s", argv[3], argv[2]);
		j_obj(); j_kbool("ok", true); j_end(); j_flush();
		return;
	}
	sa_usage("usage: serveradmin crashdumps status | collect on|off | send COMPUTER|off | refresh | expect COMPUTER | list | delete COMPUTER|- HOST N");
}
