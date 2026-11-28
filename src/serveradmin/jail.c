/*
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * serveradmin jailbase, jail: the Jails section.
 *
 * One base system per computer, <pool>/jails/base: FreeBSD's pkgbase set
 * FreeBSD-set-base-jail at the release this computer's userland is synced
 * to (freebsd-version -u). Every jail is <pool>/jails/containers/<name>,
 * holding its own /etc, /var, /root, /home, /tmp and /usr/local; the rest
 * is the base, nullfs-mounted read-only. A jail's settings are ZFS user
 * properties on its own dataset (org.nextbsd.jail:*); its name is the
 * dataset's name and nothing stores it, so a clone needs no rewriting.
 * There is no jail.conf: start builds the jail(8) command line.
 *
 * Networking: vNET, an epair on bridge0 (with this computer's wired
 * interface), DHCP for every interface (ifconfig_DEFAULT), and Bonjour:
 * mDNSResponderPosix announces <name>.local, mdnsd and mDNSResponder_nss
 * resolve other .local names. bridge0 carries an IPv6 link-local address
 * so this computer and its jails find each other over mDNS too.
 *
 *   jailbase status | setup | upgrade | revert
 *   jail list | create NAME | start NAME|--all | stop NAME | restart NAME
 *   jail delete NAME | security NAME raw_sockets|sysvipc on|off
 *   jail name-check NAME | send NAME | receive NAME | console-check NAME
 */

#include <sys/types.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jail.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "serveradmin.h"

#define	JAILS_MNT	"/usr/local/jails"
#define	BASE_MNT	JAILS_MNT "/base"
#define	CONT_MNT	JAILS_MNT "/containers"
#define	REPOS		JAILS_MNT "/.repos"
#define	PKG_CACHE	JAILS_MNT "/.cache"
#define	FOREIGN_MARK	".nextbsd-foreign-root"
#define	BRIDGE		"bridge0"
#define	JAILS_JOB	"/System/Library/LaunchDaemons/org.nextbsd.jails.plist"
#define	PROP		"org.nextbsd.jail:"

/* The base's read-only directories, mounted into every jail. */
static const char *const shared_dirs[] = {
	"bin", "sbin", "lib", "libexec", "boot", "usr/bin", "usr/sbin",
	"usr/lib", "usr/libexec", "usr/share", "usr/include", "usr/libdata",
	"usr/lib32", NULL
};
/* Copied from the base into a new jail: its own from then on. */
static const char *const own_dirs[] = { "etc", "var", "root", NULL };

static char pool[256], ds_jails[300], ds_base[300], ds_cont[300];

static void
names(void)
{
	if (sa_main_pool() == NULL)
		sa_fail("unsupported", "this computer isn't installed on ZFS");
	strlcpy(pool, sa_main_pool(), sizeof(pool));
	snprintf(ds_jails, sizeof(ds_jails), "%s/jails", pool);
	snprintf(ds_base, sizeof(ds_base), "%s/jails/base", pool);
	snprintf(ds_cont, sizeof(ds_cont), "%s/jails/containers", pool);
}

static bool
ds_exists(const char *ds)
{
	const char *a[] = { ZFS, "list", "-H", "-o", "name", ds, NULL };
	struct sa_out o;
	bool ok = sa_run(a, &o);

	sa_out_free(&o);
	return (ok);
}

static void
ok(void)
{
	j_obj();
	j_kbool("ok", true);
	j_end();
	j_flush();
}

/* "aarch64" -> FreeBSD:15:aarch64 for the release in use. */
static void
freebsd_abi(char *abi, size_t len, char *osver, size_t olen)
{
	char arch[32] = "";
	size_t n = sizeof(arch);
	int major = atoi(sa_freebsd_release());
	int minor = 0;
	const char *dot = strchr(sa_freebsd_release(), '.');

	if (dot != NULL)
		minor = atoi(dot + 1);
	(void)sysctlbyname("hw.machine_arch", arch, &n, NULL, 0);
	snprintf(abi, len, "FreeBSD:%d:%s", major, arch);
	snprintf(osver, olen, "%d%02d000", major, minor);
}

static void
write_file(const char *path, const char *text, mode_t mode)
{
	char tmp[PATH_MAX];
	int fd;
	size_t n = strlen(text);

	snprintf(tmp, sizeof(tmp), "%s.XXXXXX", path);
	if ((fd = mkstemp(tmp)) == -1)
		sa_fail("failed", "%s: %s", path, strerror(errno));
	if (fchmod(fd, mode) == -1 || write(fd, text, n) != (ssize_t)n ||
	    close(fd) == -1 || rename(tmp, path) == -1) {
		(void)unlink(tmp);
		sa_fail("failed", "%s: %s", path, strerror(errno));
	}
}

/* The repository files for pkg -R: FreeBSD's pkgbase for the base, and
 * FreeBSD's packages for what goes into a jail's /usr/local. */
static void
write_repos(void)
{
	char t[1024];
	int minor = 0;
	const char *dot = strchr(sa_freebsd_release(), '.');

	if (dot != NULL)
		minor = atoi(dot + 1);
	(void)mkdir(REPOS, 0755);
	snprintf(t, sizeof(t),
	    "# Written by serveradmin(8): FreeBSD's base system for jails.\n"
	    "FreeBSD-base: {\n"
	    "  url: \"pkg+https://pkg.FreeBSD.org/${ABI}/base_release_%d\",\n"
	    "  mirror_type: \"srv\",\n"
	    "  signature_type: \"fingerprints\",\n"
	    "  fingerprints: \"/usr/share/keys/pkgbase-%d\",\n"
	    "  enabled: yes\n"
	    "}\n", minor, atoi(sa_freebsd_release()));
	write_file(REPOS "/FreeBSD-base.conf", t, 0644);
	write_file(REPOS "/FreeBSD.conf",
	    "# Written by serveradmin(8): FreeBSD's packages, for jails.\n"
	    "FreeBSD: {\n"
	    "  url: \"pkg+https://pkg.FreeBSD.org/${ABI}/quarterly\",\n"
	    "  mirror_type: \"srv\",\n"
	    "  signature_type: \"fingerprints\",\n"
	    "  fingerprints: \"/usr/share/keys/pkg\",\n"
	    "  enabled: yes\n"
	    "}\n", 0644);
}

/* pkg -r ROOT with the jail repositories: args after the fixed ones. */
static bool
jail_pkg(const char *root, const char *const extra[], char **err)
{
	char abi[64], osver[16], abio[80], osvo[32];
	const char *a[32];
	int k = 0;
	struct sa_out o;
	bool ok_;

	freebsd_abi(abi, sizeof(abi), osver, sizeof(osver));
	snprintf(abio, sizeof(abio), "ABI=%s", abi);
	snprintf(osvo, sizeof(osvo), "OSVERSION=%s", osver);
	a[k++] = PKG;
	a[k++] = "-r"; a[k++] = root;
	a[k++] = "-R"; a[k++] = REPOS;
	a[k++] = "-o"; a[k++] = abio;
	a[k++] = "-o"; a[k++] = osvo;
	a[k++] = "-o"; a[k++] = "IGNORE_OSVERSION=yes";
	a[k++] = "-o"; a[k++] = "PKG_CACHEDIR=" PKG_CACHE;
	a[k++] = "-o"; a[k++] = "REPOS_DIR=" REPOS;
	for (int i = 0; extra[i] != NULL && k < 30; i++)
		a[k++] = extra[i];
	a[k] = NULL;
	ok_ = sa_run(a, &o);
	if (!ok_ && err != NULL) {
		*err = strdup(o.err[0] != '\0' ? o.err : o.out);
	}
	sa_out_free(&o);
	return (ok_);
}

static const char *
base_version(void)
{
	static char v[64];
	char p[PATH_MAX], line[256];
	FILE *f;

	v[0] = '\0';
	snprintf(p, sizeof(p), BASE_MNT "/bin/freebsd-version");
	if ((f = fopen(p, "r")) == NULL)
		return (NULL);
	while (fgets(line, sizeof(line), f) != NULL)
		if (strncmp(line, "USERLAND_VERSION=\"", 18) == 0) {
			strlcpy(v, line + 18, sizeof(v));
			v[strcspn(v, "\"")] = '\0';
		}
	fclose(f);
	return (v[0] != '\0' ? v : NULL);
}

/* 15.1-RELEASE-p7 -> 15.1-RELEASE */
static void
release_of(const char *v, char *out, size_t len)
{
	char *p;

	strlcpy(out, v, len);
	if ((p = strstr(out, "-p")) != NULL)
		*p = '\0';
}

static const char *
upgrade_available(void)
{
	static char v[64];
	const char *ex[] = { "rquery", "-r", "FreeBSD-base", "%v",
	    "FreeBSD-runtime", NULL };
	struct sa_out o;
	char abi[64], osver[16], abio[80], osvo[32];
	const char *a[] = { PKG, "-r", BASE_MNT, "-R", REPOS, "-o", NULL, "-o",
	    NULL, "-o", "IGNORE_OSVERSION=yes", "-o", "REPOS_DIR=" REPOS,
	    "-o", "PKG_CACHEDIR=" PKG_CACHE, "upgrade", "-n", NULL };

	(void)ex;
	freebsd_abi(abi, sizeof(abi), osver, sizeof(osver));
	snprintf(abio, sizeof(abio), "ABI=%s", abi);
	snprintf(osvo, sizeof(osvo), "OSVERSION=%s", osver);
	a[6] = abio;
	a[8] = osvo;
	v[0] = '\0';
	/* pkg upgrade -n exits 1 when something would be upgraded. */
	(void)sa_run(a, &o);
	if (strstr(o.out, "FreeBSD-runtime:") != NULL) {
		char *p = strstr(o.out, "FreeBSD-runtime:");
		char *arrow = strstr(p, "-> ");
		if (arrow != NULL) {
			strlcpy(v, arrow + 3, sizeof(v));
			v[strcspn(v, " \n")] = '\0';
		}
	}
	sa_out_free(&o);
	return (v);
}

/* ---- jails ---------------------------------------------------------- */

static void
jail_ds(const char *name, char *ds, size_t len)
{
	snprintf(ds, len, "%s/%s", ds_cont, name);
}

static void
jail_root(const char *name, char *root, size_t len)
{
	snprintf(root, len, CONT_MNT "/%s", name);
}

static bool
jail_running(const char *name, int *jid)
{
	int j = jail_getid(name);

	if (jid != NULL)
		*jid = j;
	return (j > 0);
}

static bool
managed(const char *name)
{
	char ds[400];
	const char *a[] = { ZFS, "get", "-H", "-o", "value", PROP "managed", ds, NULL };
	struct sa_out o;
	bool m;

	jail_ds(name, ds, sizeof(ds));
	m = sa_run(a, &o) && strncmp(o.out, "on", 2) == 0;
	sa_out_free(&o);
	return (m);
}

static bool
prop_on(const char *name, const char *prop)
{
	char ds[400], p[80];
	const char *a[] = { ZFS, "get", "-H", "-o", "value", p, ds, NULL };
	struct sa_out o;
	bool on;

	jail_ds(name, ds, sizeof(ds));
	snprintf(p, sizeof(p), PROP "%s", prop);
	on = sa_run(a, &o) && strncmp(o.out, "on", 2) == 0;
	sa_out_free(&o);
	return (on);
}

static void
prop_get(const char *name, const char *prop, char *v, size_t len)
{
	char ds[400], p[80];
	const char *a[] = { ZFS, "get", "-H", "-o", "value", p, ds, NULL };
	struct sa_out o;

	jail_ds(name, ds, sizeof(ds));
	snprintf(p, sizeof(p), PROP "%s", prop);
	v[0] = '\0';
	if (sa_run(a, &o)) {
		strlcpy(v, o.out, len);
		v[strcspn(v, "\n")] = '\0';
		if (strcmp(v, "-") == 0)
			v[0] = '\0';
	}
	sa_out_free(&o);
}

static void
prop_set(const char *name, const char *prop, const char *val)
{
	char ds[400], p[160];
	const char *a[] = { ZFS, "set", p, ds, NULL };

	jail_ds(name, ds, sizeof(ds));
	snprintf(p, sizeof(p), PROP "%s=%s", prop, val);
	sa_must("zfs set", a);
}

/* A short, stable interface stem and MAC for a jail: from its name. */
static uint32_t
name_hash(const char *s)
{
	uint32_t h = 2166136261u;

	for (; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 16777619u;
	}
	return (h);
}

static void
epair_names(const char *name, char *a, char *b, size_t len)
{
	uint32_t h = name_hash(name);

	snprintf(a, len, "sj%08xa", h);
	snprintf(b, len, "sj%08xb", h);
}

static bool
if_exists(const char *ifn)
{
	const char *a[] = { IFCONFIG, ifn, NULL };
	struct sa_out o;
	bool ok_ = sa_run(a, &o);

	sa_out_free(&o);
	return (ok_);
}

/* bridge0 with the wired interface, and an IPv6 link-local address so
 * this computer's mDNSResponder also answers its jails. */
static void
bridge_up(void)
{
	char ifn[32];
	bool conn;

	if (!sa_wired(ifn, sizeof(ifn), &conn) || !conn)
		sa_fail("no-ethernet", "Jails are not supported on this system: "
		    "it has no connected Ethernet");
	if (!if_exists(BRIDGE)) {
		const char *c[] = { IFCONFIG, BRIDGE, "create", NULL };
		sa_must("ifconfig create", c);
	}
	{
		const char *add[] = { IFCONFIG, BRIDGE, "addm", ifn, "up", NULL };
		struct sa_out o;
		(void)sa_run(add, &o);	/* already a member: fine */
		sa_out_free(&o);
	}
	{
		const char *v6[] = { IFCONFIG, BRIDGE, "inet6", "-ifdisabled",
		    "auto_linklocal", "up", NULL };
		struct sa_out o;
		(void)sa_run(v6, &o);
		sa_out_free(&o);
	}
}

static void
mount_shared(const char *root)
{
	struct statfs *mnt;
	int n = getmntinfo(&mnt, MNT_NOWAIT);

	for (int i = 0; shared_dirs[i] != NULL; i++) {
		char src[PATH_MAX], dst[PATH_MAX];
		struct stat st;
		bool mounted = false;

		snprintf(src, sizeof(src), BASE_MNT "/%s", shared_dirs[i]);
		snprintf(dst, sizeof(dst), "%s/%s", root, shared_dirs[i]);
		if (stat(src, &st) == -1)
			continue;
		(void)mkdir(dst, 0755);
		for (int m = 0; m < n; m++)
			if (strcmp(mnt[m].f_mntonname, dst) == 0)
				mounted = true;
		if (!mounted) {
			const char *a[] = { MOUNT_NULLFS, "-o", "ro", src, dst, NULL };
			sa_must("mount_nullfs", a);
		}
	}
}

static void
unmount_shared(const char *root)
{
	char dev[PATH_MAX];

	snprintf(dev, sizeof(dev), "%s/dev", root);
	{
		const char *a[] = { UMOUNT, "-f", dev, NULL };
		struct sa_out o;
		(void)sa_run(a, &o);
		sa_out_free(&o);
	}
	int last = 0;

	while (shared_dirs[last] != NULL)
		last++;
	for (int i = last - 1; i >= 0; i--) {
		char dst[PATH_MAX];
		const char *a[] = { UMOUNT, "-f", dst, NULL };
		struct sa_out o;

		snprintf(dst, sizeof(dst), "%s/%s", root, shared_dirs[i]);
		(void)sa_run(a, &o);
		sa_out_free(&o);
	}
}

static void
start_jail(const char *name)
{
	char root[PATH_MAX], ea[32], eb[32], mac[32], hn[128], pathp[PATH_MAX + 8];
	char vif[48], osr[80], osd[32], rel[64];
	const char *base = base_version();
	uint32_t h;

	if (jail_running(name, NULL))
		return;
	if (base == NULL)
		sa_fail("no-base", "the base system isn't set up");
	jail_root(name, root, sizeof(root));
	bridge_up();
	mount_shared(root);
	epair_names(name, ea, eb, sizeof(ea));
	if (!if_exists(ea)) {
		const char *c[] = { IFCONFIG, "epair", "create", NULL };
		struct sa_out o;
		char made[32], other[32];

		if (!sa_run(c, &o))
			sa_fail("failed", "ifconfig epair create: %s", o.err);
		strlcpy(made, o.out, sizeof(made));
		made[strcspn(made, "\n")] = '\0';
		sa_out_free(&o);
		strlcpy(other, made, sizeof(other));
		other[strlen(other) - 1] = 'b';
		{
			const char *r1[] = { IFCONFIG, made, "name", ea, NULL };
			const char *r2[] = { IFCONFIG, other, "name", eb, NULL };
			sa_must("ifconfig name", r1);
			sa_must("ifconfig name", r2);
		}
	}
	/* A stable, locally administered MAC from the computer and jail
	 * names, so the router hands the jail the same address each time. */
	gethostname(hn, sizeof(hn));
	h = name_hash(hn) ^ name_hash(name);
	snprintf(mac, sizeof(mac), "02:5a:%02x:%02x:%02x:%02x",
	    (h >> 24) & 0xff, (h >> 16) & 0xff, (h >> 8) & 0xff, h & 0xff);
	{
		const char *up[] = { IFCONFIG, ea, "up", NULL };
		const char *ether[] = { IFCONFIG, eb, "ether", mac, NULL };
		const char *add[] = { IFCONFIG, BRIDGE, "addm", ea, NULL };
		struct sa_out o;
		sa_must("ifconfig up", up);
		(void)sa_run(ether, &o); sa_out_free(&o);
		(void)sa_run(add, &o); sa_out_free(&o);
	}
	prop_get(name, "release", rel, sizeof(rel));
	snprintf(pathp, sizeof(pathp), "path=%s", root);
	snprintf(vif, sizeof(vif), "vnet.interface=%s", eb);
	snprintf(osr, sizeof(osr), "osrelease=%s", base);
	snprintf(osd, sizeof(osd), "osreldate=%d%02d000", atoi(base),
	    strchr(base, '.') != NULL ? atoi(strchr(base, '.') + 1) : 0);
	{
		char namep[100], hostp[100];
		const char *a[24];
		int k = 0;

		snprintf(namep, sizeof(namep), "name=%s", name);
		snprintf(hostp, sizeof(hostp), "host.hostname=%s", name);
		a[k++] = JAIL; a[k++] = "-c";
		a[k++] = namep; a[k++] = hostp; a[k++] = pathp;
		a[k++] = "vnet"; a[k++] = vif;
		a[k++] = "mount.devfs"; a[k++] = "devfs_ruleset=4";
		a[k++] = osr; a[k++] = osd;
		/* FreeBSD inside, NextBSD outside (nextbsd-kernel#440). */
		if (sysctlbyname("security.jail.param.ostype", NULL, &(size_t){0},
		    NULL, 0) == 0)
			a[k++] = "ostype=FreeBSD";
		a[k++] = "allow.set_hostname=0";
		if (prop_on(name, "raw_sockets"))
			a[k++] = "allow.raw_sockets";
		if (prop_on(name, "sysvipc"))
			a[k++] = "allow.sysvipc";
		a[k++] = "exec.start=/bin/sh /etc/rc";
		a[k++] = "exec.stop=/bin/sh /etc/rc.shutdown jail";
		a[k++] = "exec.consolelog=/var/log/jail-console.log";
		a[k] = NULL;
		sa_must("jail -c", a);
	}
}

static void
stop_jail(const char *name)
{
	char root[PATH_MAX], ea[32], eb[32];
	int jid;

	jail_root(name, root, sizeof(root));
	if (jail_running(name, &jid)) {
		char jidbuf[16];
		const char *a[] = { JAIL, "-r", jidbuf, NULL };
		snprintf(jidbuf, sizeof(jidbuf), "%d", jid);
		/* jail -r runs exec.stop and returns the vnet interface. */
		sa_must("jail -r", a);
	}
	epair_names(name, ea, eb, sizeof(ea));
	if (if_exists(ea)) {
		const char *a[] = { IFCONFIG, ea, "destroy", NULL };
		struct sa_out o;
		(void)sa_run(a, &o);
		sa_out_free(&o);
	}
	unmount_shared(root);
}

/* Every Server-made jail, by dataset. */
static char **
jail_names(int *n)
{
	const char *a[] = { ZFS, "list", "-H", "-d", "1", "-o",
	    "name," PROP "managed", ds_cont, NULL };
	struct sa_out o;
	char **l, **out;
	int c = 0, nl;

	out = calloc(256, sizeof(*out));
	*n = 0;
	if (out == NULL)
		sa_fail("failed", "out of memory");
	if (!sa_run(a, &o)) {
		sa_out_free(&o);
		return (out);
	}
	l = sa_lines(o.out, &nl);
	for (int i = 0; i < nl && c < 255; i++) {
		char **f;
		int nf;
		const char *slash;

		f = sa_fields(l[i], "\t", &nf);
		if (nf >= 2 && strcmp(f[1], "on") == 0 &&
		    (slash = strrchr(f[0], '/')) != NULL &&
		    strcmp(f[0], ds_cont) != 0)
			out[c++] = strdup(slash + 1);
		free(f);
	}
	free(l);
	sa_out_free(&o);
	*n = c;
	return (out);
}

static void
must_jail(const char *name)
{
	char ds[400];

	if (!sa_is_label(name))
		sa_usage("not a jail name: %s", name);
	jail_ds(name, ds, sizeof(ds));
	if (!ds_exists(ds) || !managed(name))
		sa_fail("not-found", "no jail named %s", name);
}

/* Why `name` can't be used for a new jail here, or "" if it can. */
static const char *
name_problem(const char *name, bool check_network)
{
	static char why[256];
	char ds[400], addr[64], host[256], *dot;

	why[0] = '\0';
	if (!sa_is_label(name)) {
		snprintf(why, sizeof(why), "Use lowercase letters, digits and hyphens.");
		return (why);
	}
	jail_ds(name, ds, sizeof(ds));
	if (ds_exists(ds) || jail_getid(name) > 0) {
		snprintf(why, sizeof(why), "This computer already has a jail named %s.", name);
		return (why);
	}
	gethostname(host, sizeof(host));
	if ((dot = strchr(host, '.')) != NULL)
		*dot = '\0';
	if (strcasecmp(host, name) == 0) {
		snprintf(why, sizeof(why), "This computer is called %s.local.", name);
		return (why);
	}
	if (check_network && sa_mdns_taken(name, addr, sizeof(addr))) {
		snprintf(why, sizeof(why),
		    "Something on your network is already called %s.local.", name);
		return (why);
	}
	return (why);
}

/* A clone is a separate machine: drop the SSH host keys (sshd makes new
 * ones) and DHCP leases it brought along. */
static void
forget_identity(const char *root)
{
	static const struct { const char *dir, *prefix; } gone[] = {
		{ "etc/ssh", "ssh_host_" },
		{ "var/db", "dhclient.leases." },
		{ NULL, NULL }
	};

	for (int i = 0; gone[i].dir != NULL; i++) {
		char d[PATH_MAX], f[PATH_MAX];
		DIR *dp;
		struct dirent *e;

		snprintf(d, sizeof(d), "%s/%s", root, gone[i].dir);
		if ((dp = opendir(d)) == NULL)
			continue;
		while ((e = readdir(dp)) != NULL)
			if (strncmp(e->d_name, gone[i].prefix,
			    strlen(gone[i].prefix)) == 0) {
				snprintf(f, sizeof(f), "%s/%s", d, e->d_name);
				(void)unlink(f);
			}
		closedir(dp);
	}
}

/* Set up a new jail's own files: rc.conf, nsswitch, mDNSResponder. */
static void
prepare_jail(const char *name, const char *root)
{
	char p[PATH_MAX], *err = NULL;
	FILE *f;

	snprintf(p, sizeof(p), "%s/etc/rc.conf", root);
	if ((f = fopen(p, "a")) == NULL)
		sa_fail("failed", "%s: %s", p, strerror(errno));
	fputs("\n# Written by serveradmin(8) for Server.app. No name is stored\n"
	    "# here: the hostname is the jail's name, given at every start.\n"
	    "# SYNCDHCP: a jail has no devd to start dhclient later. No -x on\n"
	    "# mDNSResponderPosix: everything after it would be TXT, rc's -b too.\n"
	    "ifconfig_DEFAULT=\"SYNCDHCP\"\n"
	    "sendmail_enable=\"NONE\"\n"
	    "syslogd_flags=\"-ss\"\n"
	    "mdnsd_enable=\"YES\"\n"
	    "mdnsresponderposix_enable=\"YES\"\n"
	    "mdnsresponderposix_flags=\"-n $(/bin/hostname -s) "
	    "-t _device-info._tcp. -p 9\"\n", f);
	fclose(f);
	snprintf(p, sizeof(p), "%s/etc/nsswitch.conf", root);
	{
		char *text = NULL, line[512];
		size_t tl = 0;
		FILE *in = fopen(p, "r");

		if (in != NULL) {
			while (fgets(line, sizeof(line), in) != NULL) {
				const char *use = strncmp(line, "hosts:", 6) == 0 ?
				    "hosts: files mdns dns\n" : line;
				size_t ul = strlen(use);
				if ((text = realloc(text, tl + ul + 1)) == NULL)
					sa_fail("failed", "out of memory");
				memcpy(text + tl, use, ul);
				tl += ul;
				text[tl] = '\0';
			}
			fclose(in);
			if (text != NULL)
				write_file(p, text, 0644);
			free(text);
		}
	}
	/* Until the kernel reports FreeBSD inside jails (nextbsd-kernel#440),
	 * pkg in a jail would take its ABI from uname -s, NextBSD. */
	{
		char abi[64], osver[16], t[256];

		freebsd_abi(abi, sizeof(abi), osver, sizeof(osver));
		snprintf(p, sizeof(p), "%s/usr/local/etc", root);
		(void)mkdir(p, 0755);
		snprintf(p, sizeof(p), "%s/usr/local/etc/pkg.conf", root);
		snprintf(t, sizeof(t), "# Written by serveradmin(8): this jail is FreeBSD.\n"
		    "ABI = \"%s\";\nOSVERSION = \"%s\";\n", abi, osver);
		write_file(p, t, 0644);
	}
	{
		const char *ex[] = { "install", "-y", "-r", "FreeBSD",
		    "mDNSResponder", "mDNSResponder_nss", NULL };
		if (!jail_pkg(root, ex, &err))
			sa_fail("failed", "installing mDNSResponder in %s: %s",
			    name, err != NULL ? err : "pkg failed");
	}
	(void)name;
}

/* A create that fails part way leaves nothing behind. */
static char creating[64];

static void
undo_create(void)
{
	char ds[400], root[PATH_MAX];
	const char *a[] = { ZFS, "destroy", "-r", ds, NULL };
	struct sa_out o;

	if (creating[0] == '\0')
		return;
	stop_jail(creating);
	jail_ds(creating, ds, sizeof(ds));
	jail_root(creating, root, sizeof(root));
	for (int i = 0; i < 20 && !sa_run(a, &o); i++) {
		sa_out_free(&o);
		sleep(1);
	}
	sa_out_free(&o);
}

static void
jail_list(void)
{
	char **n;
	int c;
	const char *base = base_version();

	j_obj();
	j_key("base");
	if (base != NULL) {
		j_obj();
		j_kstr("version", base);
		j_end();
	} else
		j_null();
	j_key("jails");
	j_arr();
	n = jail_names(&c);
	for (int i = 0; i < c; i++) {
		int jid;
		char rel[64], ds[400];
		bool run = jail_running(n[i], &jid);
		struct sa_out o;
		const char *a[] = { ZFS, "list", "-Hp", "-o", "used", ds, NULL };

		jail_ds(n[i], ds, sizeof(ds));
		prop_get(n[i], "release", rel, sizeof(rel));
		j_obj();
		j_kstr("name", n[i]);
		j_kstr("hostname", n[i]);
		j_kbool("running", run);
		j_kint("jid", run ? jid : 0);
		j_kstr("release", rel);
		j_kbool("autostart", prop_on(n[i], "autostart"));
		j_kbool("raw_sockets", prop_on(n[i], "raw_sockets"));
		j_kbool("sysvipc", prop_on(n[i], "sysvipc"));
		j_kstr("dataset", ds);
		j_kint("used", sa_run(a, &o) ? strtoll(o.out, NULL, 10) : 0);
		sa_out_free(&o);
		if (run) {
			char eb[32], ea[32], addr[64] = "", jb[16];
			const char *ia[] = { JEXEC, jb, IFCONFIG, eb, "inet", NULL };

			snprintf(jb, sizeof(jb), "%d", jid);
			epair_names(n[i], ea, eb, sizeof(ea));
			if (sa_run(ia, &o)) {
				char *p = strstr(o.out, "inet ");
				if (p != NULL) {
					strlcpy(addr, p + 5, sizeof(addr));
					addr[strcspn(addr, " ")] = '\0';
				}
			}
			sa_out_free(&o);
			j_kstr("address", addr);
		} else {
			j_key("address");
			j_null();
		}
		j_end();
		free(n[i]);
	}
	free(n);
	j_aend();
	/* Jails Server didn't make: running ones not under containers. */
	j_key("unmanaged");
	j_arr();
	{
		const char *a[] = { JLS, "-n", "name", "path", NULL };
		struct sa_out o;
		char **l;
		int nl;

		if (sa_run(a, &o)) {
			l = sa_lines(o.out, &nl);
			for (int i = 0; i < nl; i++) {
				char nm[128] = "", pth[PATH_MAX] = "", *p;
				if ((p = strstr(l[i], "name=")) != NULL) {
					strlcpy(nm, p + 5, sizeof(nm));
					nm[strcspn(nm, " ")] = '\0';
				}
				if ((p = strstr(l[i], "path=")) != NULL) {
					strlcpy(pth, p + 5, sizeof(pth));
					pth[strcspn(pth, " ")] = '\0';
				}
				if (strncmp(pth, CONT_MNT "/", strlen(CONT_MNT) + 1) == 0)
					continue;
				j_obj();
				j_kstr("name", nm);
				j_kstr("path", pth);
				j_end();
			}
			free(l);
		}
		sa_out_free(&o);
	}
	j_aend();
	j_end();
	j_flush();
}

static bool
jobs_enabled(void)
{
	const char *a[] = { LAUNCHCTL, "list", "org.nextbsd.jails", NULL };
	struct sa_out o;
	bool on = sa_run(a, &o);

	sa_out_free(&o);
	return (on);
}

void
cmd_jailbase(int argc, char **argv)
{
	const char *v;
	char ifn[32];
	bool conn;

	names();
	if (argc == 1 && strcmp(argv[0], "status") == 0) {
		v = base_version();
		j_obj();
		j_kstr("release", sa_freebsd_release());
		j_kbool("wired", sa_wired(ifn, sizeof(ifn), &conn) && conn);
		j_key("base");
		if (v != NULL) {
			char snap[400];
			const char *a[] = { ZFS, "list", "-H", "-t", "snapshot",
			    "-o", "name", snap, NULL };
			struct sa_out o;

			j_obj();
			j_kstr("version", v);
			snprintf(snap, sizeof(snap), "%s@before-upgrade", ds_base);
			j_kbool("can_revert", sa_run(a, &o));
			sa_out_free(&o);
			j_end();
		} else
			j_null();
		j_kbool("autostart_job", jobs_enabled());
		j_end();
		j_flush();
		return;
	}
	if (argc == 1 && strcmp(argv[0], "check") == 0) {
		if (base_version() == NULL)
			sa_fail("no-base", "the base system isn't set up");
		write_repos();
		{
			const char *ex[] = { "update", "-q", NULL };
			(void)jail_pkg(BASE_MNT, ex, NULL);
		}
		v = upgrade_available();
		j_obj();
		j_kstr("version", base_version());
		j_kstr("available", v);
		j_end();
		j_flush();
		return;
	}
	if (argc == 1 && strcmp(argv[0], "setup") == 0) {
		char *err = NULL, mark[PATH_MAX];

		if (!sa_wired(ifn, sizeof(ifn), &conn) || !conn)
			sa_fail("no-ethernet", "Jails are not supported on this system: "
			    "it has no connected Ethernet");
		if (base_version() != NULL)
			sa_fail("exists", "the base system is already set up");
		if (!ds_exists(ds_jails)) {
			const char *a[] = { ZFS, "create", "-o", "mountpoint=" JAILS_MNT,
			    "-o", "compression=lz4", ds_jails, NULL };
			sa_must("zfs create", a);
		}
		if (!ds_exists(ds_base)) {
			const char *a[] = { ZFS, "create", ds_base, NULL };
			sa_must("zfs create", a);
		}
		if (!ds_exists(ds_cont)) {
			const char *a[] = { ZFS, "create", ds_cont, NULL };
			sa_must("zfs create", a);
		}
		(void)mkdir(PKG_CACHE, 0755);
		snprintf(mark, sizeof(mark), BASE_MNT "/" FOREIGN_MARK);
		write_file(mark, "This is a jail's FreeBSD base system, not NextBSD's "
		    "(serveradmin(8)).\n", 0644);
		/* pkg reads the repository keys inside the root it fills. */
		{
			char keys[PATH_MAX], kd[64];
			const char *a[] = { "/bin/cp", "-R", kd, keys, NULL };

			snprintf(keys, sizeof(keys), BASE_MNT "/usr/share/keys");
			(void)mkdir(BASE_MNT "/usr", 0755);
			(void)mkdir(BASE_MNT "/usr/share", 0755);
			(void)mkdir(keys, 0755);
			snprintf(kd, sizeof(kd), "/usr/share/keys/pkgbase-%d",
			    atoi(sa_freebsd_release()));
			sa_must("cp keys", a);
		}
		write_repos();
		{
			const char *ex[] = { "install", "-y", "-r", "FreeBSD-base",
			    "FreeBSD-set-base-jail", NULL };
			if (!jail_pkg(BASE_MNT, ex, &err))
				sa_fail("failed", "installing FreeBSD's base system: %s",
				    err != NULL ? err : "pkg failed");
		}
		bridge_up();
		{
			const char *a[] = { LAUNCHCTL, "load", "-w", JAILS_JOB, NULL };
			struct sa_out o;
			(void)sa_run(a, &o);
			sa_out_free(&o);
		}
		j_obj();
		j_kstr("version", base_version());
		j_end();
		j_flush();
		return;
	}
	if (argc == 1 && (strcmp(argv[0], "upgrade") == 0 ||
	    strcmp(argv[0], "revert") == 0)) {
		char **n, snap[400], *err = NULL;
		int c;
		bool *was;

		if (base_version() == NULL)
			sa_fail("no-base", "the base system isn't set up");
		snprintf(snap, sizeof(snap), "%s@before-upgrade", ds_base);
		n = jail_names(&c);
		was = calloc((size_t)c + 1, sizeof(*was));
		/* Each jail restarts once, one at a time, around the change. */
		for (int i = 0; i < c; i++)
			if ((was[i] = jail_running(n[i], NULL)))
				stop_jail(n[i]);
		if (argv[0][0] == 'u') {
			const char *d[] = { ZFS, "destroy", snap, NULL };
			const char *s[] = { ZFS, "snapshot", snap, NULL };
			const char *ex[] = { "upgrade", "-y", "-r", "FreeBSD-base", NULL };
			struct sa_out o;

			write_repos();
			(void)sa_run(d, &o);
			sa_out_free(&o);
			sa_must("zfs snapshot", s);
			if (!jail_pkg(BASE_MNT, ex, &err)) {
				for (int i = 0; i < c; i++)
					if (was[i])
						start_jail(n[i]);
				sa_fail("failed", "upgrading the base system: %s",
				    err != NULL ? err : "pkg failed");
			}
		} else {
			const char *r[] = { ZFS, "rollback", "-r", snap, NULL };
			sa_must("zfs rollback", r);
		}
		for (int i = 0; i < c; i++) {
			char rel[64];
			release_of(base_version(), rel, sizeof(rel));
			prop_set(n[i], "release", rel);
			if (was[i])
				start_jail(n[i]);
		}
		j_obj();
		j_kstr("version", base_version());
		j_end();
		j_flush();
		return;
	}
	sa_usage("usage: serveradmin jailbase status | check | setup | upgrade | revert");
}

void
cmd_jail(int argc, char **argv)
{
	char ds[400], root[PATH_MAX];

	names();
	if (argc == 1 && strcmp(argv[0], "list") == 0) {
		jail_list();
		return;
	}
	if (argc == 2 && strcmp(argv[0], "name-check") == 0) {
		const char *why = name_problem(argv[1], true);
		j_obj();
		j_kbool("ok", why[0] == '\0');
		j_kstr("problem", why);
		j_end();
		j_flush();
		return;
	}
	if (argc == 2 && strcmp(argv[0], "create") == 0) {
		const char *why, *base = base_version();
		char rel[64];

		if (base == NULL)
			sa_fail("no-base", "set up the base system first");
		if ((why = name_problem(argv[1], true))[0] != '\0')
			sa_fail("name", "%s", why);
		jail_ds(argv[1], ds, sizeof(ds));
		jail_root(argv[1], root, sizeof(root));
		release_of(base, rel, sizeof(rel));
		strlcpy(creating, argv[1], sizeof(creating));
		sa_on_fail = undo_create;
		{
			char relp[100];
			const char *a[] = { ZFS, "create", "-o", PROP "managed=on",
			    "-o", PROP "autostart=on", "-o", PROP "raw_sockets=on",
			    "-o", PROP "sysvipc=off", "-o", relp, ds, NULL };
			snprintf(relp, sizeof(relp), PROP "release=%s", rel);
			sa_must("zfs create", a);
		}
		for (int i = 0; own_dirs[i] != NULL; i++) {
			char src[PATH_MAX], dst[PATH_MAX];
			const char *a[] = { "/bin/cp", "-a", src, dst, NULL };
			snprintf(src, sizeof(src), BASE_MNT "/%s", own_dirs[i]);
			snprintf(dst, sizeof(dst), "%s/%s", root, own_dirs[i]);
			sa_must("cp", a);
		}
		{
			const char *dirs[] = { "home", "tmp", "dev", "usr", "usr/local", NULL };
			for (int i = 0; dirs[i] != NULL; i++) {
				char p[PATH_MAX];
				snprintf(p, sizeof(p), "%s/%s", root, dirs[i]);
				(void)mkdir(p, 0755);
			}
			snprintf(ds, sizeof(ds), "%s/tmp", root);
			(void)chmod(ds, 01777);
		}
		/* Its packages are FreeBSD's too: mark the root for pkg -r. */
		{
			char mark[PATH_MAX];
			snprintf(mark, sizeof(mark), "%s/" FOREIGN_MARK, root);
			write_file(mark, "A jail's own files (serveradmin(8)).\n", 0644);
		}
		mount_shared(root);
		write_repos();
		prepare_jail(argv[1], root);
		start_jail(argv[1]);
		sa_on_fail = NULL;
		j_obj();
		j_kstr("name", argv[1]);
		j_kstr("hostname", argv[1]);
		j_end();
		j_flush();
		return;
	}
	if (argc == 2 && strcmp(argv[0], "start") == 0 &&
	    strcmp(argv[1], "--all") == 0) {
		char **n;
		int c, started = 0;

		if (base_version() == NULL) {
			ok();
			return;
		}
		n = jail_names(&c);
		for (int i = 0; i < c; i++)
			if (prop_on(n[i], "autostart") && !jail_running(n[i], NULL)) {
				start_jail(n[i]);
				started++;
			}
		j_obj();
		j_kint("started", started);
		j_end();
		j_flush();
		return;
	}
	if (argc == 2 && (strcmp(argv[0], "start") == 0 ||
	    strcmp(argv[0], "stop") == 0 || strcmp(argv[0], "restart") == 0)) {
		must_jail(argv[1]);
		if (strcmp(argv[0], "stop") == 0 || strcmp(argv[0], "restart") == 0)
			stop_jail(argv[1]);
		if (strcmp(argv[0], "start") == 0 || strcmp(argv[0], "restart") == 0)
			start_jail(argv[1]);
		ok();
		return;
	}
	if (argc == 2 && strcmp(argv[0], "delete") == 0) {
		must_jail(argv[1]);
		stop_jail(argv[1]);
		jail_ds(argv[1], ds, sizeof(ds));
		{
			/* A jail that just stopped holds its dataset for a few
			 * seconds while it finishes dying. */
			const char *a[] = { ZFS, "destroy", "-r", ds, NULL };
			struct sa_out o;
			int tries = 0;

			while (!sa_run(a, &o) && tries++ < 20) {
				sa_out_free(&o);
				sleep(1);
			}
			if (tries > 20)
				sa_fail("failed", "zfs destroy: %s", o.err);
			sa_out_free(&o);
		}
		ok();
		return;
	}
	if (argc == 4 && strcmp(argv[0], "security") == 0) {
		bool on;

		must_jail(argv[1]);
		if (strcmp(argv[2], "raw_sockets") != 0 &&
		    strcmp(argv[2], "sysvipc") != 0)
			sa_usage("security takes raw_sockets or sysvipc");
		if (!sa_is_onoff(argv[3], &on))
			sa_usage("security takes on or off");
		prop_set(argv[1], argv[2], on ? "on" : "off");
		j_obj();
		j_kbool("restart_needed", jail_running(argv[1], NULL));
		j_end();
		j_flush();
		return;
	}
	if (argc == 2 && strcmp(argv[0], "autostart") == 0)
		sa_usage("usage: serveradmin jail autostart NAME on|off");
	if (argc == 3 && strcmp(argv[0], "autostart") == 0) {
		bool on;

		must_jail(argv[1]);
		if (!sa_is_onoff(argv[2], &on))
			sa_usage("autostart takes on or off");
		prop_set(argv[1], "autostart", on ? "on" : "off");
		ok();
		return;
	}
	/*
	 * Moving and cloning: send writes a zfs send -p stream of the jail's
	 * dataset to stdout; receive reads one from stdin into a new jail of
	 * the given name. Both computers' bases must be the same FreeBSD
	 * release; Server checks first, and receive checks again.
	 */
	if (argc == 2 && strcmp(argv[0], "send") == 0) {
		char snap[450], *err;
		const char *d[] = { ZFS, "destroy", snap, NULL };
		const char *s[] = { ZFS, "snapshot", snap, NULL };
		const char *a[] = { ZFS, "send", "-p", snap, NULL };
		struct sa_out o;

		must_jail(argv[1]);
		jail_ds(argv[1], ds, sizeof(ds));
		snprintf(snap, sizeof(snap), "%s@serveradmin-send", ds);
		(void)sa_run(d, &o);
		sa_out_free(&o);
		sa_must("zfs snapshot", s);
		if (isatty(STDOUT_FILENO))
			sa_fail("usage", "send writes a stream: pipe it to receive");
		if (!sa_run_stdio(a, &err)) {
			(void)sa_run(d, &o);
			sa_out_free(&o);
			fprintf(stderr, "zfs send: %s\n", err);
			exit(1);
		}
		(void)sa_run(d, &o);
		sa_out_free(&o);
		exit(0);
	}
	if (argc == 4 && strcmp(argv[0], "receive") == 0) {
		const char *why, *base = base_version();
		char rel[64], *err, snap[450];
		bool clone = strcmp(argv[2], "clone") == 0;
		const char *a[] = { ZFS, "receive", "-u", ds, NULL };

		if (!clone && strcmp(argv[2], "move") != 0)
			sa_usage("receive takes move or clone");
		if (base == NULL)
			sa_fail("no-base", "set up the base system first");
		release_of(base, rel, sizeof(rel));
		if (strcmp(rel, argv[3]) != 0)
			sa_fail("release", "this computer's jails run FreeBSD %s, "
			    "and the jail runs %s", rel, argv[3]);
		/* A clone must not be found on the network; a moved jail is
		 * still there until it has started here. */
		if ((why = name_problem(argv[1], clone))[0] != '\0')
			sa_fail("name", "%s", why);
		if (isatty(STDIN_FILENO))
			sa_fail("usage", "receive reads a stream from send");
		jail_ds(argv[1], ds, sizeof(ds));
		if (!sa_run_stdio(a, &err))
			sa_fail("failed", "zfs receive: %s", err);
		snprintf(snap, sizeof(snap), "%s@serveradmin-send", ds);
		{
			const char *d[] = { ZFS, "destroy", snap, NULL };
			const char *m[] = { ZFS, "mount", ds, NULL };
			struct sa_out o;
			(void)sa_run(d, &o);
			sa_out_free(&o);
			sa_must("zfs mount", m);
		}
		jail_root(argv[1], root, sizeof(root));
		if (clone) {
			/* A separate machine: new SSH host keys, a new lease. */
			forget_identity(root);
		}
		start_jail(argv[1]);
		j_obj();
		j_kstr("name", argv[1]);
		j_end();
		j_flush();
		return;
	}
	if (argc == 2 && strcmp(argv[0], "release") == 0) {
		char rel[64];

		must_jail(argv[1]);
		prop_get(argv[1], "release", rel, sizeof(rel));
		j_obj();
		j_kstr("release", rel);
		j_end();
		j_flush();
		return;
	}
	if (argc == 2 && strcmp(argv[0], "console-check") == 0) {
		int jid;

		must_jail(argv[1]);
		j_obj();
		j_kbool("running", jail_running(argv[1], &jid));
		j_kint("jid", jid > 0 ? jid : 0);
		j_end();
		j_flush();
		return;
	}
	sa_usage("usage: serveradmin jail list | create NAME | start NAME|--all | stop NAME | restart NAME | delete NAME | security NAME raw_sockets|sysvipc on|off | autostart NAME on|off | name-check NAME | send NAME | receive NAME move|clone RELEASE | release NAME");
}
