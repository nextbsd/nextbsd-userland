/*
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * sh_time.c — SetTimeZone, SetDate and SetNetworkTime: Date & Time.app's
 * operations (nextbsd/nextbsd#634).
 *
 *   SetTimeZone      tzsetup's way (src/tzsetup/apply.cpp): copy
 *                    /usr/share/zoneinfo/<zone> over /etc/localtime (a
 *                    copy, not a symlink, so a single-user boot with only
 *                    / mounted keeps its zone) and record the name in
 *                    /var/db/zoneinfo. Then post Darwin's
 *                    com.apple.system.timezone, which nothing posted
 *                    before; the Dock's clock and the settings apps reset
 *                    their zone on it.
 *   SetDate          settimeofday(2), which on FreeBSD also writes the
 *                    hardware clock, then com.apple.system.clock_set.
 *   SetNetworkTime   org.nextbsd.ntpd on or off with launchctl load -w or
 *                    unload -w, as tzsetup does, so the choice survives a
 *                    reboot (launchd's overrides database). A new server
 *                    replaces the server and pool lines of /etc/ntp.conf,
 *                    and a running ntpd is restarted to use it.
 *
 * Files are read and replaced, and launchctl is run, with sh_file.c's
 * helpers.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <notify.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "SettingsHelper.h"

#define	ZONEINFO	"/usr/share/zoneinfo"
#define	LOCALTIME	"/etc/localtime"
#define	ZONE_DB		"/var/db/zoneinfo"
#define	NTP_CONF	"/etc/ntp.conf"
#define	LAUNCHCTL	"/bin/launchctl"
#define	NTPD_LABEL	"org.nextbsd.ntpd"
#define	NTPD_PLIST	"/System/Library/LaunchDaemons/org.nextbsd.ntpd.plist"

/* SetDate's range: 2001 (Darwin's reference date) to 2100. */
#define	DATE_MIN	978307200LL
#define	DATE_MAX	4102444800LL

/* Run launchctl with these arguments; true if it exited 0. */
static bool
launchctl(const char *a1, const char *a2, const char *a3)
{
	const char *argv[] = { LAUNCHCTL, a1, a2, a3, NULL };

	return (sh_run(argv));
}

/*
 * A zone name: path components of letters, digits, '_', '-' and '+',
 * none starting with '.', not absolute. It indexes the filesystem, so
 * this comes before the name is ever joined to a path.
 */
static bool
zone_name_ok(const char *zone)
{
	const char *p;
	bool start = true;

	if (zone[0] == '\0' || strlen(zone) > SH_ZONE_MAX)
		return (false);
	for (p = zone; *p != '\0'; p++) {
		if (*p == '/') {
			if (start)
				return (false);	/* "/x", "x//y" */
			start = true;
			continue;
		}
		if (start && (*p == '.' || *p == '-'))
			return (false);
		if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' &&
		    *p != '+')
			return (false);
		start = false;
	}
	return (!start);			/* not "x/" */
}

int
sh_tz_set(const char *zone)
{
	char path[PATH_MAX], line[SH_ZONE_MAX + 2];
	char *data;
	size_t len;

	if (!zone_name_ok(zone))
		return (SH_ERR_INVALID);
	(void)snprintf(path, sizeof(path), "%s/%s", ZONEINFO, zone);
	data = sh_read_file(path, &len);
	if (data == NULL)
		return (SH_ERR_INVALID);	/* no such zone */
	/* A compiled zone (tzfile(5)), not zone.tab or a stray file. */
	if (len < 44 || memcmp(data, "TZif", 4) != 0) {
		free(data);
		return (SH_ERR_INVALID);
	}
	if (!sh_write_file(LOCALTIME, data, len)) {
		free(data);
		return (SH_ERR_FAILED);
	}
	free(data);
	(void)snprintf(line, sizeof(line), "%s\n", zone);
	if (!sh_write_file(ZONE_DB, line, strlen(line)))
		return (SH_ERR_FAILED);
	(void)notify_post("com.apple.system.timezone");
	return (SH_OK);
}

int
sh_date_set(int64_t seconds, int32_t microseconds)
{
	struct timeval tv;

	if (seconds < DATE_MIN || seconds > DATE_MAX || microseconds < 0 ||
	    microseconds > 999999)
		return (SH_ERR_INVALID);
	tv.tv_sec = (time_t)seconds;
	tv.tv_usec = microseconds;
	if (settimeofday(&tv, NULL) == -1) {
		sh_log("settimeofday: %s", strerror(errno));
		return (SH_ERR_FAILED);
	}
	(void)notify_post("com.apple.system.clock_set");
	return (SH_OK);
}

/*
 * A time server as ntp.conf takes it: a host name or an IPv4 or IPv6
 * address. Only those characters, so a server can't add a directive.
 */
static bool
server_ok(const char *server)
{
	const char *p;

	if (!isalnum((unsigned char)server[0]) && server[0] != ':')
		return (false);
	if (strlen(server) > 253)
		return (false);
	for (p = server; *p != '\0'; p++)
		if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-' &&
		    *p != ':')
			return (false);
	return (true);
}

/* Is this ntp.conf line a "server" or "pool" line? */
static bool
server_line(const char *line, size_t len)
{
	while (len > 0 && (*line == ' ' || *line == '\t')) {
		line++;
		len--;
	}
	return ((len > 7 && strncmp(line, "server", 6) == 0 &&
	    isspace((unsigned char)line[6])) ||
	    (len > 5 && strncmp(line, "pool", 4) == 0 &&
	    isspace((unsigned char)line[4])));
}

/*
 * Put `server` in ntp.conf in place of every server and pool line: a
 * "pool" line for an NTP pool name (one name, many servers), else a
 * "server" line. The rest of the file is kept. *changed says whether the
 * file changed.
 */
static int
ntp_conf_set(const char *server, bool *changed)
{
	char entry[SH_NTP_SERVER_MAX + 32];
	char *old, *new, *p, *eol;
	size_t len, used = 0, n;
	bool placed = false;
	int rc = SH_OK;

	*changed = false;
	(void)snprintf(entry, sizeof(entry), "%s %s iburst\n",
	    strstr(server, "pool.ntp.org") != NULL ? "pool" : "server",
	    server);
	old = sh_read_file(NTP_CONF, &len);
	if (old == NULL) {
		old = strdup("");
		len = 0;
		if (old == NULL)
			return (SH_ERR_FAILED);
	}
	new = malloc(len + strlen(entry) + 2);
	if (new == NULL) {
		free(old);
		return (SH_ERR_FAILED);
	}
	for (p = old; *p != '\0'; p = eol) {
		eol = strchr(p, '\n');
		eol = eol != NULL ? eol + 1 : p + strlen(p);
		n = (size_t)(eol - p);
		if (server_line(p, n)) {
			if (!placed) {
				(void)memcpy(new + used, entry, strlen(entry));
				used += strlen(entry);
				placed = true;
			}
			continue;
		}
		(void)memcpy(new + used, p, n);
		used += n;
		if (p[n - 1] != '\n')
			new[used++] = '\n';
	}
	if (!placed) {
		(void)memcpy(new + used, entry, strlen(entry));
		used += strlen(entry);
	}
	if (used != len || memcmp(new, old, len) != 0) {
		if (sh_write_file(NTP_CONF, new, used))
			*changed = true;
		else
			rc = SH_ERR_FAILED;
	}
	free(new);
	free(old);
	return (rc);
}

int
sh_ntp_set(bool on, const char *server)
{
	struct stat st;
	bool changed = false, loaded;
	int rc;

	if (server[0] != '\0' && !server_ok(server))
		return (SH_ERR_INVALID);
	if (stat(NTPD_PLIST, &st) == -1)
		return (SH_ERR_UNSUPPORTED);	/* no ntpd job installed */
	if (server[0] != '\0' && (rc = ntp_conf_set(server, &changed)) != SH_OK)
		return (rc);

	loaded = launchctl("list", NTPD_LABEL, NULL);
	if (on) {
		/* load -w also clears a Disabled override. */
		if (!loaded && !launchctl("load", "-w", NTPD_PLIST))
			return (SH_ERR_FAILED);
		/* A running ntpd reads ntp.conf only when it starts. Stopped,
		 * KeepAlive starts it again (after launchd's 10 s throttle). */
		if (loaded && changed && !launchctl("stop", NTPD_LABEL, NULL))
			return (SH_ERR_FAILED);
	} else {
		if (!launchctl("unload", "-w", NTPD_PLIST))
			return (SH_ERR_FAILED);
	}
	return (SH_OK);
}
