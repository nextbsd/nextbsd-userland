/*
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * sh_session.c — is the caller in the console user's session?
 *
 * On Darwin that is the caller's audit session (the asid in its audit
 * token) against the console user's. NextBSD's loginwindow doesn't start
 * an audit session, so the session it does keep stands in for it:
 * loginwindow (apps-loginwindow LWSession.m) makes the session leader with
 * setsid(), is the reaper of everything below it, and writes
 * /var/run/loginwindow/<uid>.session every few seconds:
 *
 *	boot <kern.boot_id in hex>
 *	user <name>
 *	uid <uid>
 *	sid <the leader's pid, which is the session id>
 *	pid <pid> <start, microseconds since boot>	(one per member)
 *
 * A process is in the console session if:
 *   - it's in the leader's Unix session (ki_sid), or
 *   - one of its ancestors is (an app Workspace started, which NSTask
 *     put in a session of its own), or
 *   - the record lists it with the same start time (a daemonized member
 *     whose parent is gone, found by loginwindow as the reaper).
 * Only a root-owned record in a root-owned directory nobody else can write
 * counts, as in WindowServer's console check (apps-windowserver dock.c);
 * a record from an earlier boot, or one whose leader has gone or is
 * another process with the same pid, doesn't. The newest record is the
 * console's: loginwindow runs one session at a time.
 *
 * The audit token names the sender's pid, euid and (where the kernel sets
 * it) its pid version; the process is looked up by pid and must still
 * have that euid.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/user.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "SettingsHelper.h"

#define	LW_RUN_DIR	"/var/run/loginwindow"

/* Two readings of one start time differ by this much at most
 * (apps-loginwindow LWSession.m, START_SLOP_US). */
#define	START_SLOP_US	1000000LL

#define	MAX_LISTED	4096
#define	MAX_DEPTH	64

struct record {
	char		boot[2 * 64 + 1];
	char		user[SH_USER_MAX + 1];
	long		uid;
	long		sid;
	size_t		nlisted;
	struct { pid_t pid; long long start; } listed[MAX_LISTED];
};

static int
proc_get(pid_t pid, struct kinfo_proc *out)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };
	size_t len = sizeof(*out);

	if (sysctl(mib, 4, out, &len, NULL, 0) < 0 || len < sizeof(*out))
		return (-1);
	return (0);
}

static int
boot_time(struct timeval *bt)
{
	size_t len = sizeof(*bt);

	return (sysctlbyname("kern.boottime", bt, &len, NULL, 0));
}

static long long
since_boot(const struct kinfo_proc *kp, const struct timeval *bt)
{
	return ((long long)(kp->ki_start.tv_sec - bt->tv_sec) * 1000000LL +
	    (long long)(kp->ki_start.tv_usec - bt->tv_usec));
}

static bool
same_start(long long a, long long b)
{
	return (a - b <= START_SLOP_US && b - a <= START_SLOP_US);
}

static const char *
boot_id(void)
{
	static char text[2 * 64 + 1];
	unsigned char id[64];
	size_t len = sizeof(id), i;

	if (text[0] != '\0')
		return (text);
	if (sysctlbyname("kern.boot_id", id, &len, NULL, 0) != 0 || len == 0)
		return ("");
	for (i = 0; i < len; i++)
		(void)snprintf(text + 2 * i, 3, "%02x", id[i]);
	return (text);
}

static bool
read_record(int dfd, const char *name, struct record *r)
{
	char line[256];
	FILE *f;
	int fd;

	memset(r, 0, sizeof(*r));
	r->uid = r->sid = -1;
	fd = openat(dfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return (false);
	if ((f = fdopen(fd, "r")) == NULL) {
		(void)close(fd);
		return (false);
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		long long b;
		long a;

		if (sscanf(line, "boot %128s", r->boot) == 1)
			continue;
		if (sscanf(line, "user %255s", r->user) == 1)
			continue;
		if (sscanf(line, "uid %ld", &a) == 1) {
			r->uid = a;
			continue;
		}
		if (sscanf(line, "sid %ld", &a) == 1) {
			r->sid = a;
			continue;
		}
		if (sscanf(line, "pid %ld %lld", &a, &b) == 2 &&
		    r->nlisted < MAX_LISTED) {
			r->listed[r->nlisted].pid = (pid_t)a;
			r->listed[r->nlisted].start = b;
			r->nlisted++;
		}
	}
	(void)fclose(f);
	return (r->uid >= 0 && r->sid > 1);
}

/*
 * The newest trustworthy record, checked against this boot and the
 * running leader. Static: one at a time, single-threaded.
 */
static struct record *
console_record(struct sh_console *out)
{
	static struct record rec, best;
	struct kinfo_proc kp;
	struct timeval bt, bt2;
	struct dirent *de;
	struct stat st;
	time_t when = 0;
	bool found = false;
	size_t n, i;
	DIR *d;

	d = opendir(LW_RUN_DIR);
	if (d == NULL)
		return (NULL);
	if (fstat(dirfd(d), &st) < 0 || st.st_uid != 0 ||
	    (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
		sh_log("%s isn't root's alone; no console session", LW_RUN_DIR);
		(void)closedir(d);
		return (NULL);
	}
	while ((de = readdir(d)) != NULL) {
		n = strlen(de->d_name);
		if (n <= 8 || strcmp(de->d_name + n - 8, ".session") != 0)
			continue;
		if (fstatat(dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0 ||
		    !S_ISREG(st.st_mode) || st.st_uid != 0 ||
		    (st.st_mode & (S_IWGRP | S_IWOTH)) != 0)
			continue;
		if (found && st.st_birthtime <= when)
			continue;
		if (!read_record(dirfd(d), de->d_name, &rec))
			continue;
		if (rec.boot[0] == '\0' || strcmp(rec.boot, boot_id()) != 0)
			continue;	/* an earlier boot, or untrustworthy times */
		best = rec;
		when = st.st_birthtime;
		found = true;
	}
	(void)closedir(d);
	if (!found)
		return (NULL);

	/* The leader must be running, and be the process the record means. */
	if (boot_time(&bt) != 0 || proc_get((pid_t)best.sid, &kp) != 0 ||
	    kp.ki_stat == SZOMB || boot_time(&bt2) != 0 ||
	    bt.tv_sec != bt2.tv_sec || bt.tv_usec != bt2.tv_usec)
		return (NULL);
	for (i = 0; i < best.nlisted; i++)
		if (best.listed[i].pid == (pid_t)best.sid)
			break;
	if (i == best.nlisted ||
	    !same_start(best.listed[i].start, since_boot(&kp, &bt)))
		return (NULL);

	if (out != NULL) {
		memset(out, 0, sizeof(*out));
		out->uid = (uid_t)best.uid;
		out->leader = (pid_t)best.sid;
		out->leader_start = best.listed[i].start;
		(void)strlcpy(out->user, best.user, sizeof(out->user));
	}
	return (&best);
}

bool
sh_console_session(struct sh_console *out)
{
	return (console_record(out) != NULL);
}

static bool
in_session(pid_t pid, const struct record *r)
{
	struct kinfo_proc kp;
	struct timeval bt;
	size_t i;
	int depth;

	if (boot_time(&bt) != 0)
		return (false);
	for (depth = 0; depth < MAX_DEPTH && pid > 1; depth++) {
		if (proc_get(pid, &kp) != 0 || kp.ki_stat == SZOMB)
			return (false);
		if (kp.ki_sid == (pid_t)r->sid || kp.ki_pid == (pid_t)r->sid)
			return (true);
		for (i = 0; i < r->nlisted; i++)
			if (r->listed[i].pid == kp.ki_pid &&
			    same_start(r->listed[i].start, since_boot(&kp, &bt)))
				return (true);
		pid = kp.ki_ppid;
	}
	return (false);
}

void
sh_caller_from_token(uid_t euid, pid_t pid, struct sh_caller *caller)
{
	struct kinfo_proc kp;
	struct record *r;

	memset(caller, 0, sizeof(*caller));
	caller->euid = euid;
	caller->pid = pid;
	if (caller->euid == 0) {
		caller->root = true;
		/* Root is in no session of its own; credentials it asks for
		 * aren't needed, but sh_console_session still names one. */
		caller->in_console = sh_console_session(&caller->console);
		return;
	}
	/* The sender may have gone, and its pid been reused. */
	if (caller->pid <= 1 || proc_get(caller->pid, &kp) != 0 ||
	    kp.ki_uid != caller->euid)
		return;
	if ((r = console_record(&caller->console)) == NULL)
		return;
	caller->in_console = in_session(caller->pid, r);
}
