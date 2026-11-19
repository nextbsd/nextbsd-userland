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
 * Every file is replaced by writing a temporary next to it and renaming
 * it over, so a reader never sees half a file. launchctl is run with
 * fork and exec, never a shell; its arguments are ours.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <notify.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "SettingsHelper.h"

#define	ZONEINFO	"/usr/share/zoneinfo"
#define	LOCALTIME	"/etc/localtime"
#define	ZONE_DB		"/var/db/zoneinfo"
#define	NTP_CONF	"/etc/ntp.conf"
#define	LAUNCHCTL	"/bin/launchctl"
#define	NTPD_LABEL	"org.nextbsd.ntpd"
#define	NTPD_PLIST	"/System/Library/LaunchDaemons/org.nextbsd.ntpd.plist"

#define	MAX_FILE	(1024 * 1024)	/* a zone file is a few KB */

/* SetDate's range: 2001 (Darwin's reference date) to 2100. */
#define	DATE_MIN	978307200LL
#define	DATE_MAX	4102444800LL

/* Run launchctl with these arguments; true if it exited 0. */
static bool
launchctl(const char *a1, const char *a2, const char *a3)
{
	const char *argv[] = { LAUNCHCTL, a1, a2, a3, NULL };
	pid_t pid;
	int st;

	pid = fork();
	if (pid == -1)
		return (false);
	if (pid == 0) {
		int fd = open("/dev/null", O_RDWR);

		if (fd != -1) {
			(void)dup2(fd, STDIN_FILENO);
			(void)dup2(fd, STDOUT_FILENO);
			if (fd > STDERR_FILENO)
				(void)close(fd);
		}
		execv(LAUNCHCTL, (char *const *)argv);
		_exit(127);
	}
	while (waitpid(pid, &st, 0) == -1)
		if (errno != EINTR)
			return (false);
	return (WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

/* Read a whole (small) file. *len gets its size; NULL on failure. */
static char *
read_file(const char *path, size_t *len)
{
	struct stat st;
	char *buf;
	ssize_t n;
	size_t got = 0;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		return (NULL);
	if (fstat(fd, &st) == -1 || !S_ISREG(st.st_mode) ||
	    st.st_size > MAX_FILE) {
		(void)close(fd);
		return (NULL);
	}
	buf = malloc((size_t)st.st_size + 1);
	if (buf == NULL) {
		(void)close(fd);
		return (NULL);
	}
	while (got < (size_t)st.st_size) {
		n = read(fd, buf + got, (size_t)st.st_size - got);
		if (n == -1 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	(void)close(fd);
	buf[got] = '\0';
	*len = got;
	return (buf);
}

/* Replace `path` with `len` bytes, mode 0644, through a temporary. */
static bool
write_file(const char *path, const char *data, size_t len)
{
	char tmp[PATH_MAX];
	ssize_t n;
	size_t done = 0;
	int fd;

	(void)snprintf(tmp, sizeof(tmp), "%s.SettingsHelper", path);
	(void)unlink(tmp);
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (fd == -1) {
		sh_log("%s: %s", tmp, strerror(errno));
		return (false);
	}
	while (done < len) {
		n = write(fd, data + done, len - done);
		if (n == -1 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		done += (size_t)n;
	}
	if (done != len || fchmod(fd, 0644) == -1 || fsync(fd) == -1) {
		sh_log("%s: %s", tmp, strerror(errno));
		(void)close(fd);
		(void)unlink(tmp);
		return (false);
	}
	(void)close(fd);
	if (rename(tmp, path) == -1) {
		sh_log("%s: %s", path, strerror(errno));
		(void)unlink(tmp);
		return (false);
	}
	return (true);
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
	data = read_file(path, &len);
	if (data == NULL)
		return (SH_ERR_INVALID);	/* no such zone */
	/* A compiled zone (tzfile(5)), not zone.tab or a stray file. */
	if (len < 44 || memcmp(data, "TZif", 4) != 0) {
		free(data);
		return (SH_ERR_INVALID);
	}
	if (!write_file(LOCALTIME, data, len)) {
		free(data);
		return (SH_ERR_FAILED);
	}
	free(data);
	(void)snprintf(line, sizeof(line), "%s\n", zone);
	if (!write_file(ZONE_DB, line, strlen(line)))
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
	old = read_file(NTP_CONF, &len);
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
		if (write_file(NTP_CONF, new, used))
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
