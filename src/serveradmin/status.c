/*
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * serveradmin status: what Server's sidebar and Overview show, and what
 * Server checks before it manages a computer at all (NextBSD only).
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/route.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <ifaddrs.h>
#include <kenv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "serveradmin.h"

#define	SYSTEM_VERSION	"/System/Library/CoreServices/SystemVersion.plist"

static char main_pool[256];

const char *
sa_main_pool(void)
{
	struct statfs sf;
	char *slash;

	if (main_pool[0] != '\0')
		return (main_pool);
	if (statfs("/", &sf) == -1 || strcmp(sf.f_fstypename, "zfs") != 0)
		return (NULL);
	strlcpy(main_pool, sf.f_mntfromname, sizeof(main_pool));
	if ((slash = strchr(main_pool, '/')) != NULL)
		*slash = '\0';
	return (main_pool);
}

/* A <key>k</key><string>v</string> pair from SystemVersion.plist. */
static bool
plist_string(const char *key, char *val, size_t len)
{
	char want[128], line[512], *p, *q;
	FILE *f;
	bool next = false;

	if ((f = fopen(SYSTEM_VERSION, "r")) == NULL)
		return (false);
	snprintf(want, sizeof(want), "<key>%s</key>", key);
	while (fgets(line, sizeof(line), f) != NULL) {
		if (next && (p = strstr(line, "<string>")) != NULL) {
			p += 8;
			if ((q = strstr(p, "</string>")) != NULL)
				*q = '\0';
			strlcpy(val, p, len);
			fclose(f);
			return (true);
		}
		next = strstr(line, want) != NULL;
	}
	fclose(f);
	return (false);
}

static char release[64];

const char *
sa_freebsd_release(void)
{
	const char *argv[] = { "/bin/freebsd-version", "-u", NULL };
	struct sa_out o;
	char *p;

	if (release[0] != '\0')
		return (release);
	if (sa_run(argv, &o)) {
		strlcpy(release, o.out, sizeof(release));
		release[strcspn(release, "\n")] = '\0';
		/* 15.1-RELEASE-p7 -> 15.1-RELEASE: patch levels don't matter */
		if ((p = strstr(release, "-p")) != NULL)
			*p = '\0';
	}
	sa_out_free(&o);
	return (release);
}

static int64_t
sysctl_int(const char *name)
{
	int64_t v = 0;
	size_t n = sizeof(v);
	unsigned int u;

	if (sysctlbyname(name, &v, &n, NULL, 0) == -1)
		return (-1);
	if (n == sizeof(u)) {
		memcpy(&u, &v, sizeof(u));
		return (u);
	}
	return (v);
}

/* Busy fraction of all CPUs over a quarter second, 0-100. */
static int
cpu_busy(void)
{
	long a[5], b[5];
	size_t n = sizeof(a);
	long busy = 0, total = 0;

	if (sysctlbyname("kern.cp_time", a, &n, NULL, 0) == -1)
		return (-1);
	usleep(250000);
	n = sizeof(b);
	if (sysctlbyname("kern.cp_time", b, &n, NULL, 0) == -1)
		return (-1);
	for (int i = 0; i < 5; i++) {
		total += b[i] - a[i];
		if (i != 4)		/* CP_IDLE */
			busy += b[i] - a[i];
	}
	return (total > 0 ? (int)(busy * 100 / total) : 0);
}

static bool
skip_if(const char *name)
{
	static const char *const virt[] = {
		"lo", "bridge", "epair", "e0a_", "e0b_", "wlan", "tap", "tun",
		"pflog", "pfsync", "enc", "gif", "gre", "vlan", "lagg", NULL
	};
	for (int i = 0; virt[i] != NULL; i++)
		if (strncmp(name, virt[i], strlen(virt[i])) == 0)
			return (true);
	return (false);
}

/* The first wired Ethernet interface, preferring one with a link. */
bool
sa_wired(char *ifname, size_t len, bool *connected)
{
	struct ifaddrs *ifa, *i;
	struct ifmediareq ifmr;
	char first[IFNAMSIZ] = "";
	int s;
	bool found = false;

	*connected = false;
	if (getifaddrs(&ifa) == -1)
		return (false);
	s = socket(AF_INET, SOCK_DGRAM, 0);
	for (i = ifa; i != NULL; i = i->ifa_next) {
		struct sockaddr_dl *sdl = (struct sockaddr_dl *)i->ifa_addr;
		if (sdl == NULL || sdl->sdl_family != AF_LINK ||
		    sdl->sdl_type != IFT_ETHER || skip_if(i->ifa_name))
			continue;
		if (first[0] == '\0')
			strlcpy(first, i->ifa_name, sizeof(first));
		memset(&ifmr, 0, sizeof(ifmr));
		strlcpy(ifmr.ifm_name, i->ifa_name, sizeof(ifmr.ifm_name));
		if (s != -1 && ioctl(s, SIOCGIFMEDIA, &ifmr) == 0 &&
		    (ifmr.ifm_status & IFM_AVALID) &&
		    (ifmr.ifm_status & IFM_ACTIVE) &&
		    IFM_TYPE(ifmr.ifm_active) == IFM_ETHER) {
			strlcpy(ifname, i->ifa_name, len);
			*connected = true;
			found = true;
			break;
		}
	}
	if (!found && first[0] != '\0') {
		strlcpy(ifname, first, len);
		found = true;
	}
	if (s != -1)
		close(s);
	freeifaddrs(ifa);
	return (found);
}

bool
sa_ipv4(const char *ifname, char *addr, size_t len)
{
	struct ifaddrs *ifa, *i;
	bool ok = false;

	if (getifaddrs(&ifa) == -1)
		return (false);
	for (i = ifa; i != NULL; i = i->ifa_next)
		if (i->ifa_addr != NULL && i->ifa_addr->sa_family == AF_INET &&
		    strcmp(i->ifa_name, ifname) == 0) {
			inet_ntop(AF_INET,
			    &((struct sockaddr_in *)i->ifa_addr)->sin_addr,
			    addr, (socklen_t)len);
			ok = true;
			break;
		}
	freeifaddrs(ifa);
	return (ok);
}

bool
sa_gateway(char *addr, size_t len)
{
	const char *argv[] = { "/sbin/route", "-n", "get", "default", NULL };
	struct sa_out o;
	char *p;
	bool ok = false;

	if (sa_run(argv, &o) && (p = strstr(o.out, "gateway:")) != NULL) {
		p += 8;
		while (*p == ' ')
			p++;
		strlcpy(addr, p, len);
		addr[strcspn(addr, "\n ")] = '\0';
		ok = addr[0] != '\0';
	}
	sa_out_free(&o);
	return (ok);
}

static void
pools(void)
{
	const char *argv[] = { ZPOOL, "list", "-Hp", "-o",
	    "name,size,alloc,free,health", NULL };
	struct sa_out o;
	char **l, **f;
	int n, nf;

	j_key("pools");
	j_arr();
	if (sa_run(argv, &o)) {
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
				j_end();
			}
			free(f);
		}
		free(l);
	}
	j_aend();
	sa_out_free(&o);
}

static const char *
accounts_role(void)
{
	const char *argv[] = { "/usr/sbin/dsstatus", NULL };
	struct sa_out o;
	static char role[32];

	strlcpy(role, "standalone", sizeof(role));
	if (sa_run(argv, &o)) {
		/* "standalone", "directory server", "bound to <server>" */
		if (strncmp(o.out, "directory server", 16) == 0)
			strlcpy(role, "server", sizeof(role));
		else if (strncmp(o.out, "bound to ", 9) == 0)
			strlcpy(role, "client", sizeof(role));
	}
	sa_out_free(&o);
	return (role);
}

static int
jail_count(void)
{
	char ds[300];
	const char *argv[] = { ZFS, "list", "-H", "-d", "1", "-o",
	    "name,org.nextbsd.jail:managed", ds, NULL };
	struct sa_out o;
	char **l, **f;
	int n, nf, c = 0;

	if (sa_main_pool() == NULL)
		return (0);
	snprintf(ds, sizeof(ds), "%s/jails/containers", sa_main_pool());
	if (sa_run(argv, &o)) {
		l = sa_lines(o.out, &n);
		for (int i = 0; i < n; i++) {
			f = sa_fields(l[i], "\t", &nf);
			if (nf >= 2 && strcmp(f[1], "on") == 0)
				c++;
			free(f);
		}
		free(l);
	}
	sa_out_free(&o);
	return (c);
}

void
cmd_status(int argc, char **argv)
{
	char v[128], host[256], model[256] = "", ifn[IFNAMSIZ], addr[64];
	char ostype[64] = "";
	size_t n = sizeof(ostype);
	int64_t pages, freep, inact, psize;
	bool connected;

	(void)argv;
	if (argc != 0)
		sa_usage("usage: serveradmin status [--json]");
	(void)sysctlbyname("kern.ostype", ostype, &n, NULL, 0);
	j_obj();
	j_kstr("os", ostype);
	j_kint("protocol", SA_PROTOCOL);
	j_kstr("version", plist_string("ProductVersion", v, sizeof(v)) ? v : "");
	j_kstr("build", plist_string("ProductBuildVersion", v, sizeof(v)) ? v : "");
	j_kstr("freebsd", sa_freebsd_release());
	gethostname(host, sizeof(host));
	j_kstr("hostname", host);
	if (kenv(KENV_GET, "smbios.system.product", model, sizeof(model)) <= 0 ||
	    strcmp(model, "None") == 0) {
		n = sizeof(model);
		(void)sysctlbyname("hw.model", model, &n, NULL, 0);
	}
	j_kstr("model", model);
	n = sizeof(v);
	if (sysctlbyname("hw.machine_arch", v, &n, NULL, 0) == 0)
		j_kstr("arch", v);
	j_kint("cpus", sysctl_int("hw.ncpu"));
	j_kint("cpu", cpu_busy());
	j_kint("memory", sysctl_int("hw.physmem"));
	pages = sysctl_int("vm.stats.vm.v_page_count");
	freep = sysctl_int("vm.stats.vm.v_free_count");
	inact = sysctl_int("vm.stats.vm.v_inactive_count");
	psize = sysctl_int("hw.pagesize");
	j_kint("memory_used", (pages - freep - inact) * psize);
	j_kstr("main_pool", sa_main_pool());
	pools();
	j_kstr("accounts", accounts_role());
	j_kint("jails", jail_count());
	j_key("ethernet");
	if (sa_wired(ifn, sizeof(ifn), &connected)) {
		j_obj();
		j_kstr("interface", ifn);
		j_kbool("connected", connected);
		j_kstr("address", sa_ipv4(ifn, addr, sizeof(addr)) ? addr : "");
		j_end();
	} else
		j_null();
	{
		struct timeval bt;
		size_t bn = sizeof(bt);
		if (sysctlbyname("kern.boottime", &bt, &bn, NULL, 0) == 0)
			j_kint("uptime", (int64_t)(time(NULL) - bt.tv_sec));
	}
	j_end();
	j_flush();
}
