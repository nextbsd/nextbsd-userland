/*
 * vm_stat -- show virtual memory statistics, Darwin format, NextBSD backend.
 *
 * Darwin's vm_stat (system_cmds) reads host_statistics64(HOST_VM_INFO64).
 * NextBSD's VM is FreeBSD's, so the numbers come from the vm.stats.*
 * sysctls instead. Output keeps Darwin's header, row names and the
 * `vm_stat [[-c count] interval]` interval mode.
 *
 * Row mapping (Darwin row -> FreeBSD sysctl):
 *
 *   Page size (header)     hw.pagesize
 *   Pages free             vm.stats.vm.v_free_count
 *   Pages active           vm.stats.vm.v_active_count
 *   Pages inactive         vm.stats.vm.v_inactive_count
 *   Pages speculative      0   no speculative read-ahead queue in FreeBSD
 *   Pages throttled        0   no throttled queue; the laundry queue
 *                              (v_laundry_count) holds dirty pages awaiting
 *                              pageout and is not the same thing
 *   Pages wired down       vm.stats.vm.v_wire_count
 *   Pages purgeable        0   FreeBSD has no purgeable (volatile) objects
 *   "Translation faults"   vm.stats.vm.v_vm_faults
 *   Pages copy-on-write    vm.stats.vm.v_cow_faults
 *   Pages zero filled      vm.stats.vm.v_zfod
 *   Pageins                vm.stats.vm.v_vnodepgsin
 *   Pageouts               vm.stats.vm.v_vnodepgsout
 *   Swapins                vm.stats.vm.v_swappgsin
 *   Swapouts               vm.stats.vm.v_swappgsout
 *
 * Choice for rows without an equivalent: the three queue rows Darwin has
 * always printed (speculative, throttled, purgeable) print 0 so scripts that
 * parse them by name keep working. Rows Darwin added with the compressor
 * (10.9+: reactivated, purged, file-backed, anonymous, stored in / occupied
 * by compressor, (de)compressions) are omitted, exactly as vm_stat on
 * pre-compressor Darwin releases omitted them.
 */

#include <sys/types.h>
#include <sys/sysctl.h>

#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct vmstat {
	uint64_t free, active, inactive, wired;
	uint64_t faults, cow, zfod;
	uint64_t pageins, pageouts, swapins, swapouts;
};

/* vm.stats counters are u_int or uint64_t depending on the field. */
static uint64_t
sysctl_u64(const char *name)
{
	union { uint32_t u32; uint64_t u64; } v;
	size_t len = sizeof(v);

	memset(&v, 0, sizeof(v));
	if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
		return (0);
	return (len == sizeof(uint32_t) ? v.u32 : v.u64);
}

static void
get_stats(struct vmstat *s)
{
	s->free = sysctl_u64("vm.stats.vm.v_free_count");
	s->active = sysctl_u64("vm.stats.vm.v_active_count");
	s->inactive = sysctl_u64("vm.stats.vm.v_inactive_count");
	s->wired = sysctl_u64("vm.stats.vm.v_wire_count");
	s->faults = sysctl_u64("vm.stats.vm.v_vm_faults");
	s->cow = sysctl_u64("vm.stats.vm.v_cow_faults");
	s->zfod = sysctl_u64("vm.stats.vm.v_zfod");
	s->pageins = sysctl_u64("vm.stats.vm.v_vnodepgsin");
	s->pageouts = sysctl_u64("vm.stats.vm.v_vnodepgsout");
	s->swapins = sysctl_u64("vm.stats.vm.v_swappgsin");
	s->swapouts = sysctl_u64("vm.stats.vm.v_swappgsout");
}

static unsigned long
page_size(void)
{
	uint64_t ps = sysctl_u64("hw.pagesize");

	return (ps != 0 ? (unsigned long)ps : (unsigned long)getpagesize());
}

static void
pstat(const char *str, uint64_t n)
{
	printf("%-38s %16ju.\n", str, (uintmax_t)n);
}

static void
snapshot(void)
{
	struct vmstat s;

	get_stats(&s);
	printf("Mach Virtual Memory Statistics: (page size of %lu bytes)\n",
	    page_size());
	pstat("Pages free:", s.free);
	pstat("Pages active:", s.active);
	pstat("Pages inactive:", s.inactive);
	pstat("Pages speculative:", 0);
	pstat("Pages throttled:", 0);
	pstat("Pages wired down:", s.wired);
	pstat("Pages purgeable:", 0);
	pstat("\"Translation faults\":", s.faults);
	pstat("Pages copy-on-write:", s.cow);
	pstat("Pages zero filled:", s.zfod);
	pstat("Pageins:", s.pageins);
	pstat("Pageouts:", s.pageouts);
	pstat("Swapins:", s.swapins);
	pstat("Swapouts:", s.swapouts);
}

static void
banner(void)
{
	printf("Mach Virtual Memory Statistics: (page size of %lu bytes)\n",
	    page_size());
	printf("%8s %8s %8s %8s %8s %8s %8s %10s %8s %8s %8s %8s %8s %8s\n",
	    "free", "active", "specul", "inactive", "throttle", "wired",
	    "prgable", "faults", "copy", "0fill", "pageins", "pageout",
	    "swapins", "swapouts");
}

static void
interval_mode(unsigned int delay, int count)
{
	struct vmstat last, s;
	int line = 0;

	memset(&last, 0, sizeof(last));
	for (;;) {
		if (line++ % 20 == 0)
			banner();
		get_stats(&s);
		/* Darwin: queue sizes are absolute, event counters are deltas
		 * (the first line shows totals since boot). */
		printf("%8ju %8ju %8ju %8ju %8ju %8ju %8ju %10ju %8ju %8ju "
		    "%8ju %8ju %8ju %8ju\n",
		    (uintmax_t)s.free, (uintmax_t)s.active, (uintmax_t)0,
		    (uintmax_t)s.inactive, (uintmax_t)0, (uintmax_t)s.wired,
		    (uintmax_t)0,
		    (uintmax_t)(s.faults - last.faults),
		    (uintmax_t)(s.cow - last.cow),
		    (uintmax_t)(s.zfod - last.zfod),
		    (uintmax_t)(s.pageins - last.pageins),
		    (uintmax_t)(s.pageouts - last.pageouts),
		    (uintmax_t)(s.swapins - last.swapins),
		    (uintmax_t)(s.swapouts - last.swapouts));
		fflush(stdout);
		last = s;
		if (count > 0 && --count == 0)
			break;
		sleep(delay);
	}
}

static void
usage(void)
{
	fprintf(stderr, "usage: %s [[-c count] interval]\n", getprogname());
	exit(1);
}

int
main(int argc, char *argv[])
{
	int ch, count = 0;
	char *ep;
	long v;

	while ((ch = getopt(argc, argv, "c:")) != -1) {
		switch (ch) {
		case 'c':
			v = strtol(optarg, &ep, 10);
			if (*optarg == '\0' || *ep != '\0' || v <= 0)
				usage();
			count = (int)v;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc == 0) {
		if (count != 0)
			usage();
		snapshot();
		return (0);
	}
	if (argc != 1)
		usage();
	v = strtol(argv[0], &ep, 10);
	if (*argv[0] == '\0' || *ep != '\0' || v <= 0)
		usage();
	interval_mode((unsigned int)v, count);
	return (0);
}
