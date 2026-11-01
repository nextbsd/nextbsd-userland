/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 NextBSD contributors
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * swapd — NextBSD's dynamic pager (nextbsd/nextbsd-userland#178).
 *
 * Grows and reclaims swap files in /private/var/vm as memory pressure comes
 * and goes. Modelled on Darwin's dynamic_pager (system_cmds-597.1.1, OS X
 * 10.9 / xnu-2422); no code is copied from it.
 *
 *   Kernel interface   macx_triggers() registers a receive right; the kernel
 *                      sends it one default_pager_space_alert (msgh_id 2295,
 *                      default_pager_alerts.defs) per registration
 *                      (nextbsd-kernel#218):
 *                        HI_WAT_ALERT  free swap fell below vm.nswap_lowat
 *                        LO_WAT_ALERT  free swap rose above vm.nswap_hiwat
 *                      As dynamic_pager does, swapd registers again after
 *                      every alert. A HI_WAT_ALERT registration made while
 *                      swap is already almost full (including no swap at
 *                      all) is alerted at once, so swapfile0 is created at
 *                      boot when there is no other swap, as dynamic_pager's
 *                      paging_setup() does.
 *
 *   Thresholds         By default swapd passes 0 for both byte thresholds,
 *                      which leaves the kernel's own: about 10% / 25% of
 *                      the configured swap free, rescaled at every swapon /
 *                      swapoff. Passing a value would fix them, as -H/-L do.
 *
 *   Swap files         /private/var/vm/swapfile<N>, preallocated (never
 *                      sparse; the extent-map swap pager refuses holes,
 *                      nextbsd-kernel#214) and activated with swapon(2) —
 *                      macx_swapon() is still a stub. Sizes follow xnu's
 *                      256 MiB -> 1 GiB range on dynamic_pager's ladder: the
 *                      first two files the same size, then doubling, never
 *                      larger than physical memory. UFS only: on any other
 *                      filesystem (ZFS: swapon gives ENOTBLK) swapd logs
 *                      and exits 0.
 *
 *   Growing            On HI_WAT_ALERT: activate the spare file if there is
 *                      one, else create the next file. Refused, with a
 *                      retry later, when:
 *                        - the file would take more than 1/8 of the free
 *                          space on the volume (dynamic_pager's limit; it
 *                          also keeps swapd off small disks, CI images
 *                          included)
 *                        - total swap would pass vm.swap_maxpages / 2, the
 *                          swap metadata zone's recommended limit, unless
 *                          kern.maxswzone was raised
 *                        - the volume is full (ENOSPC: retry in 15 s)
 *                      After a grow swapd preallocates one spare file, so
 *                      the next grow is a swapon(2), not a write of up to
 *                      1 GiB while memory is short.
 *
 *   Reclaiming         While more than one file is active, swapd checks free
 *                      swap every minute (LO_WAT_ALERT makes it look at
 *                      once). The newest file is removed only if free swap
 *                      would stay above the high-water mark after removing
 *                      it (dynamic_pager's low_water rule) and that has been
 *                      true for 5 minutes. swapoff(2) is called without
 *                      SWAPOFF_FORCE; ENOMEM keeps the file for a later try.
 *                      The removed file becomes the spare. swapfile0 (the
 *                      oldest active file) is never removed.
 *
 *   Boot               dynamic_pager empties /private/var/vm at start; swapd
 *                      keeps swapfile0 and activates it, and removes the
 *                      other files. A file still pinned for swap (swapd
 *                      restarted by launchd) cannot be unlinked (ETXTBSY) or
 *                      swapped on twice (EBUSY), and is adopted as active.
 *                      NextBSD has no rc.d, so swapd also runs `swapon -a`
 *                      once when /etc/fstab lists swap.
 *
 * swapd mlockall()s itself so that handling an alert never waits on the
 * pager it is trying to feed.
 */

#include <sys/param.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <vm/vm_param.h>	/* struct xswdev */

#include <errno.h>
#include <fcntl.h>
#include <fstab.h>
#include <limits.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mach/mach.h>
#include <mach/mach_traps.h>		/* macx_triggers() */
#include <default_pager/default_pager_types.h>

#include "default_pager_alertsServer.h"	/* MIG: default_pager_alerts_server() */

#define	DEFAULT_FILEROOT	"/private/var/vm/swapfile"

#define	MIB			(1024ULL * 1024)
#define	MIN_FILE_SIZE		(256 * MIB)	/* xnu: MIN_SWAP_FILE_SIZE */
#define	MAX_FILE_SIZE		(1024 * MIB)	/* xnu: MAX_SWAP_FILE_SIZE */
#define	FLOOR_FILE_SIZE		(64 * MIB)	/* dynamic_pager: MINIMUM_SIZE */
#define	DISK_FRACTION		8		/* at most 1/8 of free space */
#define	MAX_FILES		64

#define	RETRY_ENOSPC		15	/* s: the volume filled under us */
#define	RETRY_NOROOM		30	/* s: dynamic_pager's "need more space" */
#define	RETRY_CAP		60	/* s: at the swap size limit */
#define	RECLAIM_POLL		60	/* s: free-swap check while grown */
#define	RECLAIM_QUIET		300	/* s: must stay reclaimable this long */
#define	SPARE_DELAY		2	/* s: after a grow, before the spare */

#define	MSG_BUFSZ		1024

extern char **environ;

/* F_SKIP: the name is taken by something that is not a swap file. */
enum fstate { F_NONE, F_ACTIVE, F_SPARE, F_SKIP };

static struct {
	enum fstate	state;
	off_t		size;
} files[MAX_FILES];

static char		fileroot[PATH_MAX - 8];
static char		swapdir[PATH_MAX];
static off_t		fixed_size;		/* -S: fixed file size */
static int		hi_water_bytes;		/* -H: 0 = kernel's */
static int		lo_water_bytes;		/* -L: 0 = kernel's */
static mach_port_t	alert_port = MACH_PORT_NULL;
static long		pagesz;

/* Deadlines in monotonic seconds; 0 = not scheduled. */
static time_t		rearm_hi_at;
static time_t		spare_at;
static time_t		reclaim_at;
static time_t		reclaimable_since;

enum note { NOTE_NONE, NOTE_NOROOM, NOTE_CAP, NOTE_MAXFILES };
static enum note	last_note;

static void
xlog(const char *fmt, ...)
{
	char ts[32];
	time_t now;
	struct tm tm;
	va_list ap;

	now = time(NULL);
	localtime_r(&now, &tm);
	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
	(void)fprintf(stderr, "%s swapd[%d]: ", ts, (int)getpid());
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static time_t
now_s(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec);
}

static void
file_path(int idx, char *buf, size_t len)
{
	(void)snprintf(buf, len, "%s%d", fileroot, idx);
}

static int
sysctl_u64(const char *name, uint64_t *out)
{
	union { uint32_t u32; uint64_t u64; } v;
	size_t len = sizeof(v);

	memset(&v, 0, sizeof(v));
	if (sysctlbyname(name, &v, &len, NULL, 0) == -1)
		return (-1);
	if (len == sizeof(uint32_t))
		*out = v.u32;
	else if (len == sizeof(uint64_t))
		*out = v.u64;
	else
		return (-1);
	return (0);
}

/* Configured and used swap in pages, over every swap device (as libkvm). */
static int
swap_stats(uint64_t *total, uint64_t *used)
{
	int mib[CTL_MAXNAME];
	size_t miblen = nitems(mib) - 1, len;
	struct xswdev xsw;
	int n;

	*total = *used = 0;
	if (sysctlnametomib("vm.swap_info", mib, &miblen) == -1)
		return (-1);
	for (n = 0; ; n++) {
		mib[miblen] = n;
		len = sizeof(xsw);
		if (sysctl(mib, (u_int)miblen + 1, &xsw, &len, NULL, 0) == -1) {
			if (errno == ENOENT)
				break;
			return (-1);
		}
		*total += (uint64_t)xsw.xsw_nblks;
		*used += (uint64_t)xsw.xsw_used;
	}
	return (0);
}

/*
 * The most swap, in pages, swapd will configure: half of vm.swap_maxpages,
 * the limit swapon's own "exceeds maximum recommended amount" warning uses,
 * or all of it once kern.maxswzone has been raised. 0 = unknown, no cap.
 */
static uint64_t
swap_cap_pages(void)
{
	uint64_t maxpages, maxswzone;

	if (sysctl_u64("vm.swap_maxpages", &maxpages) == -1 || maxpages == 0)
		return (0);
	if (sysctl_u64("kern.maxswzone", &maxswzone) == 0 && maxswzone != 0)
		return (maxpages);
	return (maxpages / 2);
}

static bool
within_swap_cap(off_t size)
{
	uint64_t cap, total, used;

	cap = swap_cap_pages();
	if (cap == 0 || swap_stats(&total, &used) == -1)
		return (true);
	return (total + (uint64_t)size / (uint64_t)pagesz <= cap);
}

/* dynamic_pager's ladder: two files of the smallest size, then doubling. */
static off_t
ladder_size(int idx)
{
	uint64_t size, physmem;

	if (fixed_size != 0)
		return (fixed_size);
	size = MIN_FILE_SIZE;
	if (idx >= 2)
		size <<= (idx - 1 < 2 ? idx - 1 : 2);
	if (size > MAX_FILE_SIZE)
		size = MAX_FILE_SIZE;
	if (sysctl_u64("hw.physmem", &physmem) == 0 && physmem != 0 &&
	    size > physmem)
		size = physmem & ~(MIB - 1);
	if (size < FLOOR_FILE_SIZE)
		size = FLOOR_FILE_SIZE;
	return ((off_t)size);
}

/* At most 1/8 of the free space on the volume, as dynamic_pager. */
static bool
disk_allows(off_t size)
{
	struct statfs sfs;
	uint64_t avail;

	if (statfs(swapdir, &sfs) == -1)
		return (false);
	if (sfs.f_bavail <= 0)
		return (false);
	avail = (uint64_t)sfs.f_bavail * sfs.f_bsize;
	return ((uint64_t)size <= avail / DISK_FRACTION);
}

static void
note(enum note which, const char *fmt, ...)
{
	va_list ap;
	char buf[256];

	if (last_note == which)
		return;
	last_note = which;
	va_start(ap, fmt);
	(void)vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	xlog("%s", buf);
}

/*
 * Create path, preallocated to size: every block allocated, so swapon(2)'s
 * extent walk finds no hole. posix_fallocate(2) does it on UFS; a plain
 * zero fill is the fallback. Returns 0 or an errno value, and leaves no file
 * behind on failure.
 */
static int
create_file(const char *path, off_t size)
{
	static char zero[MIB];
	off_t off;
	ssize_t n;
	int fd, error;

	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
	    S_IRUSR | S_IWUSR);
	if (fd == -1)
		return (errno);
	error = posix_fallocate(fd, 0, size);
	if (error == EINVAL || error == EOPNOTSUPP || error == ENODEV) {
		error = 0;
		for (off = 0; off < size && error == 0; off += n) {
			n = write(fd, zero, (size_t)MIN((off_t)sizeof(zero),
			    size - off));
			if (n <= 0)
				error = n == 0 ? EIO : errno;
		}
	}
	if (error == 0 && fsync(fd) == -1)
		error = errno;
	(void)close(fd);
	if (error != 0)
		(void)unlink(path);
	return (error);
}

static void
not_ufs_exit(const char *why)
{
	xlog("%s: swap files need UFS; swapd has nothing to do and exits",
	    why);
	exit(EXIT_SUCCESS);
}

/* swapon(2) on file idx. Returns 0 or an errno value. */
static int
activate(int idx)
{
	char path[PATH_MAX];
	struct stat st;

	file_path(idx, path, sizeof(path));
	if (swapon(path) == -1)
		return (errno);
	if (stat(path, &st) == 0)
		files[idx].size = st.st_size;
	files[idx].state = F_ACTIVE;
	xlog("SWAPD-GROW: %s active (%jd MiB)", path,
	    (intmax_t)(files[idx].size / (off_t)MIB));
	return (0);
}

/* Lowest index in state s, or -1. */
static int
find(enum fstate s)
{
	int i;

	for (i = 0; i < MAX_FILES; i++)
		if (files[i].state == s)
			return (i);
	return (-1);
}

static int
nactive(void)
{
	int i, n = 0;

	for (i = 0; i < MAX_FILES; i++)
		if (files[i].state == F_ACTIVE)
			n++;
	return (n);
}

/*
 * Add one swap file. Returns 0 on success, otherwise the seconds to wait
 * before asking the kernel again.
 */
static int
grow(void)
{
	char path[PATH_MAX];
	off_t size;
	int idx, error;

	idx = find(F_SPARE);
	if (idx >= 0) {
		if (!within_swap_cap(files[idx].size)) {
			note(NOTE_CAP, "at the swap size limit "
			    "(vm.swap_maxpages / 2; raise kern.maxswzone for more)");
			return (RETRY_CAP);
		}
		file_path(idx, path, sizeof(path));
		if ((error = activate(idx)) == 0) {
			last_note = NOTE_NONE;
			return (0);
		}
		xlog("swapon %s: %s; removing it", path, strerror(error));
		(void)unlink(path);
		files[idx].state = F_NONE;
		if (error == ENOTBLK)
			not_ufs_exit(path);
		/* fall through and create a fresh one */
	}

	idx = find(F_NONE);
	if (idx < 0) {
		note(NOTE_MAXFILES, "all %d swap files in use", MAX_FILES);
		return (RETRY_CAP);
	}
	size = ladder_size(idx);
	if (!within_swap_cap(size)) {
		note(NOTE_CAP, "at the swap size limit "
		    "(vm.swap_maxpages / 2; raise kern.maxswzone for more)");
		return (RETRY_CAP);
	}
	if (!disk_allows(size)) {
		note(NOTE_NOROOM, "need more free space on %s to add a "
		    "%jd MiB swap file (at most 1/%d of free space is used)",
		    swapdir, (intmax_t)(size / (off_t)MIB), DISK_FRACTION);
		return (RETRY_NOROOM);
	}
	file_path(idx, path, sizeof(path));
	if ((error = create_file(path, size)) != 0) {
		xlog("cannot create %s: %s", path, strerror(error));
		return (error == ENOSPC ? RETRY_ENOSPC : RETRY_NOROOM);
	}
	files[idx].size = size;
	if ((error = activate(idx)) != 0) {
		xlog("swapon %s: %s", path, strerror(error));
		(void)unlink(path);
		files[idx].state = F_NONE;
		if (error == ENOTBLK)
			not_ufs_exit(path);
		return (RETRY_NOROOM);
	}
	last_note = NOTE_NONE;
	return (0);
}

/* Preallocate one spare file, so the next grow is only a swapon(2). */
static void
make_spare(void)
{
	char path[PATH_MAX];
	off_t size;
	int idx, error;

	if (find(F_SPARE) >= 0 || (idx = find(F_NONE)) < 0)
		return;
	size = ladder_size(idx);
	if (!within_swap_cap(size) || !disk_allows(size))
		return;
	file_path(idx, path, sizeof(path));
	if ((error = create_file(path, size)) != 0) {
		xlog("cannot create spare %s: %s", path, strerror(error));
		return;
	}
	files[idx].state = F_SPARE;
	files[idx].size = size;
	xlog("SWAPD-SPARE: %s preallocated (%jd MiB)", path,
	    (intmax_t)(size / (off_t)MIB));
}

/*
 * dynamic_pager's low-water rule: the newest file may go only if free swap
 * stays above the high-water mark (vm.nswap_hiwat, where LO_WAT_ALERT
 * fires) once it is gone, so removing it cannot itself bring on a
 * HI_WAT_ALERT. With the kernel's scaled thresholds the mark shrinks with
 * the configured swap, so it is recomputed for the smaller total.
 */
static bool
may_shrink(int idx)
{
	uint64_t total, used, fpages, total2, hiwat, lowat, hiwat2;

	if (swap_stats(&total, &used) == -1 ||
	    sysctl_u64("vm.nswap_hiwat", &hiwat) == -1 ||
	    sysctl_u64("vm.nswap_lowat", &lowat) == -1)
		return (false);
	fpages = (uint64_t)files[idx].size / (uint64_t)pagesz;
	if (fpages >= total || used >= total - fpages)
		return (false);
	if (total - used <= hiwat)
		return (false);
	total2 = total - fpages;
	hiwat2 = hiwat;
	if (hiwat == MAX(512, total / 4) && lowat == MAX(128, total / 10))
		hiwat2 = MAX(512, total2 / 4);	/* the kernel's auto scaling */
	return (total2 - used > hiwat2);
}

/* Highest active index above the oldest active file, or -1. */
static int
newest_removable(void)
{
	int i, first = find(F_ACTIVE);

	if (first < 0)
		return (-1);
	for (i = MAX_FILES - 1; i > first; i--)
		if (files[i].state == F_ACTIVE)
			return (i);
	return (-1);
}

static void
reclaim_check(time_t now)
{
	char path[PATH_MAX], spath[PATH_MAX];
	int idx, spare;

	reclaim_at = 0;
	if ((idx = newest_removable()) < 0) {
		reclaimable_since = 0;
		return;
	}
	reclaim_at = now + RECLAIM_POLL;
	if (!may_shrink(idx)) {
		reclaimable_since = 0;
		return;
	}
	if (reclaimable_since == 0)
		reclaimable_since = now;
	if (now - reclaimable_since < RECLAIM_QUIET)
		return;

	file_path(idx, path, sizeof(path));
	if (swapoff(path, 0) == -1) {
		/* ENOMEM: the pages on it would not fit; try again later. */
		xlog("swapoff %s: %s; keeping it", path, strerror(errno));
		reclaimable_since = 0;
		return;
	}
	reclaimable_since = now;	/* next file needs its own quiet time */
	xlog("SWAPD-SHRINK: %s released (%jd MiB)", path,
	    (intmax_t)(files[idx].size / (off_t)MIB));

	/* Keep the released file as the spare; drop any other spare. */
	while ((spare = find(F_SPARE)) >= 0) {
		file_path(spare, spath, sizeof(spath));
		(void)unlink(spath);
		files[spare].state = F_NONE;
	}
	files[idx].state = F_SPARE;
}

static void
arm(int flags)
{
	kern_return_t kr;

	kr = macx_triggers(hi_water_bytes, lo_water_bytes, flags, alert_port);
	if (kr == ENOSYS) {
		xlog("this kernel has no macx_triggers trap; swapd exits");
		exit(EXIT_SUCCESS);
	}
	if (kr != 0)
		xlog("macx_triggers(0x%x): %s", flags, strerror(kr));
}

/*
 * MIG server routine for default_pager_alerts.defs (a simpleroutine: no
 * reply). The kernel's registration is used up by this alert.
 */
kern_return_t
default_pager_space_alert(mach_port_t port, int flags)
{
	time_t now = now_s();
	int retry;

	(void)port;
	if (flags & HI_WAT_ALERT) {
		reclaimable_since = 0;
		retry = grow();
		if (retry == 0) {
			rearm_hi_at = 0;
			arm(HI_WAT_ALERT);
			spare_at = now + SPARE_DELAY;
			if (reclaim_at == 0)
				reclaim_at = now + RECLAIM_POLL;
		} else
			rearm_hi_at = now + retry;
	}
	if (flags & LO_WAT_ALERT) {
		arm(LO_WAT_ALERT);
		reclaim_check(now);	/* starts the quiet period */
	}
	return (KERN_SUCCESS);
}

/* NextBSD has no rc.d: swap listed in /etc/fstab is turned on here. */
static void
fstab_swapon(void)
{
	struct fstab *fs;
	bool any = false;
	char *argv[] = { "swapon", "-a", NULL };
	pid_t pid;
	int status, error;

	if (setfsent() == 0)
		return;
	while ((fs = getfsent()) != NULL)
		if (strcmp(fs->fs_type, FSTAB_SW) == 0)
			any = true;
	endfsent();
	if (!any)
		return;
	error = posix_spawn(&pid, "/sbin/swapon", NULL, NULL, argv, environ);
	if (error != 0) {
		xlog("cannot run swapon -a: %s", strerror(error));
		return;
	}
	while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
		;
	xlog("swapon -a for /etc/fstab: exit %d",
	    WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

/*
 * Boot: keep swapfile0 and turn it on; remove the rest, as dynamic_pager
 * empties the directory. Files still pinned for swap (swapd restarted) are
 * adopted: unlink(2) gives ETXTBSY and swapon(2) EBUSY.
 */
static void
adopt_files(void)
{
	char path[PATH_MAX];
	struct stat st;
	int i, error;

	for (i = 0; i < MAX_FILES; i++) {
		file_path(i, path, sizeof(path));
		if (lstat(path, &st) == -1)
			continue;
		if (!S_ISREG(st.st_mode)) {
			xlog("%s is not a regular file; ignoring it", path);
			files[i].state = F_SKIP;	/* never reuse the name */
			continue;
		}
		files[i].size = st.st_size;
		if (i > 0) {
			if (unlink(path) == 0)
				continue;
			if (errno == ETXTBSY) {
				files[i].state = F_ACTIVE;
				xlog("%s already active; adopted", path);
			} else
				xlog("cannot remove %s: %s", path,
				    strerror(errno));
			continue;
		}
		if (swapon(path) == 0) {
			files[i].state = F_ACTIVE;
			xlog("SWAPD-GROW: %s active (%jd MiB)", path,
			    (intmax_t)(st.st_size / (off_t)MIB));
			continue;
		}
		error = errno;
		if (error == EBUSY) {
			files[i].state = F_ACTIVE;
			xlog("%s already active; adopted", path);
			continue;
		}
		xlog("swapon %s: %s; removing it", path, strerror(error));
		(void)unlink(path);
		if (error == ENOTBLK)
			not_ufs_exit(path);
	}
}

static void
usage(void)
{
	(void)fprintf(stderr, "usage: swapd [-F filename] [-S file size] "
	    "[-H high water alert trigger] [-L low water alert trigger] "
	    "[-P priority]\n");
	exit(EXIT_FAILURE);
}

static long long
parse_bytes(const char *s, long long max)
{
	char *end;
	long long v;

	errno = 0;
	v = strtoll(s, &end, 10);
	if (errno != 0 || *end != '\0' || v < 0 || v > max)
		usage();
	return (v);
}

int
main(int argc, char **argv)
{
	union {
		mach_msg_header_t	hdr;
		char			buf[MSG_BUFSZ];
	} req, rep;
	struct statfs sfs;
	mach_msg_return_t mr;
	time_t now, next;
	mach_msg_timeout_t timeout;
	char *slash;
	int ch;

	(void)strlcpy(fileroot, DEFAULT_FILEROOT, sizeof(fileroot));
	while ((ch = getopt(argc, argv, "F:S:H:L:P:")) != -1) {
		switch (ch) {
		case 'F':
			if (strlcpy(fileroot, optarg, sizeof(fileroot)) >=
			    sizeof(fileroot) || optarg[0] != '/')
				usage();
			break;
		case 'S':
			fixed_size = (off_t)parse_bytes(optarg, INT_MAX * 4096LL);
			if (fixed_size < (off_t)FLOOR_FILE_SIZE)
				usage();
			break;
		case 'H':
			hi_water_bytes = (int)parse_bytes(optarg, INT_MAX);
			break;
		case 'L':
			lo_water_bytes = (int)parse_bytes(optarg, INT_MAX);
			break;
		case 'P':	/* dynamic_pager: accepted, unimplemented */
			break;
		default:
			usage();
		}
	}
	if (lo_water_bytes != 0 && hi_water_bytes > lo_water_bytes) {
		(void)fprintf(stderr, "swapd: the low water trigger must be "
		    "larger than the high water trigger\n");
		exit(EXIT_FAILURE);
	}
	if (geteuid() != 0) {
		(void)fprintf(stderr, "swapd: must be run as root\n");
		exit(EXIT_FAILURE);
	}
	pagesz = sysconf(_SC_PAGESIZE);

	(void)strlcpy(swapdir, fileroot, sizeof(swapdir));
	if ((slash = strrchr(swapdir, '/')) != NULL && slash != swapdir)
		*slash = '\0';

	/* Never wait on the pager while feeding it. */
	if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1)
		xlog("mlockall: %s", strerror(errno));

	fstab_swapon();

	if (statfs(swapdir, &sfs) == -1) {
		if (mkdir(swapdir, 0755) == -1 || statfs(swapdir, &sfs) == -1) {
			xlog("cannot create %s: %s", swapdir, strerror(errno));
			exit(EXIT_FAILURE);
		}
		(void)chown(swapdir, 0, 0);
	}
	if (strcmp(sfs.f_fstypename, "ufs") != 0) {
		xlog("%s is on %s", swapdir, sfs.f_fstypename);
		not_ufs_exit(swapdir);
	}

	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
	    &alert_port) != KERN_SUCCESS) {
		xlog("cannot allocate the alert port");
		exit(EXIT_FAILURE);
	}

	adopt_files();
	xlog("SWAPD-START: %s<N>, %d file(s) active", fileroot, nactive());

	/* The kernel alerts HI at once if swap is already almost full. */
	arm(HI_WAT_ALERT | LO_WAT_ALERT);
	now = now_s();
	spare_at = now + SPARE_DELAY;
	reclaim_at = now + RECLAIM_POLL;

	for (;;) {
		now = now_s();
		if (rearm_hi_at != 0 && now >= rearm_hi_at) {
			rearm_hi_at = 0;
			arm(HI_WAT_ALERT);
		}
		if (spare_at != 0 && now >= spare_at) {
			spare_at = 0;
			if (nactive() > 0)
				make_spare();
		}
		if (reclaim_at != 0 && now >= reclaim_at)
			reclaim_check(now);

		next = 0;
		if (rearm_hi_at != 0)
			next = rearm_hi_at;
		if (spare_at != 0 && (next == 0 || spare_at < next))
			next = spare_at;
		if (reclaim_at != 0 && (next == 0 || reclaim_at < next))
			next = reclaim_at;
		timeout = 0;
		if (next != 0)
			timeout = (mach_msg_timeout_t)
			    (next > now ? (next - now) * 1000 : 1);

		memset(&req, 0, sizeof(req));
		mr = mach_msg(&req.hdr,
		    MACH_RCV_MSG | (next != 0 ? MACH_RCV_TIMEOUT : 0),
		    0, sizeof(req), alert_port, timeout, MACH_PORT_NULL);
		if (mr == MACH_RCV_TIMED_OUT)
			continue;
		if (mr != MACH_MSG_SUCCESS) {
			xlog("receive on the alert port failed: 0x%x",
			    (unsigned)mr);
			exit(EXIT_FAILURE);
		}
		memset(&rep, 0, sizeof(rep));
		if (!default_pager_alerts_server(&req.hdr, &rep.hdr)) {
			xlog("ignoring message id %d", req.hdr.msgh_id);
			mach_msg_destroy(&req.hdr);
		}
	}
	/* NOTREACHED */
}
