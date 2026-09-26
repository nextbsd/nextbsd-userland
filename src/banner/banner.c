/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Joseph Maloney
 */

/*
 * nextbsd-fetch -- the login banner (nextbsd/nextbsd-userland#298, E18 U16).
 *
 * Prints a cube and a handful of system lines, the way a fetch tool does.
 * Run from /etc/zprofile at login, and by hand whenever anyone wants it.
 *
 * WHY THIS IS NOT THE MOTD
 *
 * login(1) copies /etc/motd to the terminal and nothing executes, so a motd
 * cannot report memory or uptime. Nor could it carry the task list (#285):
 * login prints the motd and THEN execs the shell that runs this, which would
 * put the list above the banner rather than below it, and one static file
 * would tell an installed machine to install. So the list lives here too,
 * under the banner, and only on live media.
 *
 * WHY IT IS OURS RATHER THAN NEOFETCH
 *
 * neofetch has been archived since 2024 and is a shell script that can only
 * emit text; fastfetch is maintained but would be a pkg dependency in base.
 * This reads /etc/os-release and sysctl directly and links nothing.
 *
 * THE ONE GATE
 *
 * It prints unless Gershwin's login window is installed. Where that exists,
 * something else asks who you are, and a console that has already greeted
 * you answered first. Same check autologin-user makes, so "Gershwin is
 * installed" has one definition.
 *
 * Whether the machine is live media is NOT a gate on the banner -- an
 * installed machine wants the version, the memory and the uptime just as
 * much. It decides two things, from one statfs: `storage` reads "live media"
 * on a unionfs root and names the real device otherwise, and the task list
 * follows the banner only when it does.
 */

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <limits.h>
#include <sys/time.h>

#include <err.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef nitems
#define nitems(x)	(sizeof((x)) / sizeof((x)[0]))
#endif

#include "cube.h"

#ifndef BANNER_LOGINWINDOW
#define BANNER_LOGINWINDOW \
	"/System/Library/LaunchDaemons/io.github.gershwin-desktop.loginwindow.plist"
#endif
#ifndef BANNER_OS_RELEASE
#define BANNER_OS_RELEASE	"/etc/os-release"
#endif
#ifndef BANNER_ROOT
#define BANNER_ROOT		"/"
#endif
#ifndef BANNER_LIVE_FSTYPE
#define BANNER_LIVE_FSTYPE	"unionfs"
#endif

#define GAP	6		/* columns between the cube and the text */
#define RULE	32		/* columns of the rule under the heading */

/* ------------------------------------------------------------------ colour */

enum depth { D_NONE, D_16, D_TRUE };

/*
 * What the terminal can actually do. COLORTERM is what modern terminals set
 * and is the only reliable signal for 24-bit; vt sets neither, so the console
 * lands on 16 and the cube collapses to three greys, which is the most it can
 * show anyway.
 */
static enum depth
depth_of(void)
{
	const char *t;

	if (!isatty(STDOUT_FILENO))
		return (D_NONE);
	if ((t = getenv("COLORTERM")) != NULL &&
	    (strstr(t, "truecolor") != NULL || strstr(t, "24bit") != NULL))
		return (D_TRUE);
	if ((t = getenv("TERM")) == NULL || strcmp(t, "dumb") == 0)
		return (D_NONE);
	return (D_16);
}

/* The 16-colour stand-ins for the three faces, brightest first. */
static const char *const face16[] = { "\033[1;37m", "\033[0;37m", "\033[1;30m" };

static void
set_rgb(enum depth d, unsigned char face, unsigned char r, unsigned char g,
    unsigned char b)
{
	switch (d) {
	case D_TRUE: (void)printf("\033[38;2;%u;%u;%um", r, g, b); break;
	case D_16:   (void)fputs(face16[face], stdout); break;
	case D_NONE: break;
	}
}

static void
reset(enum depth d)
{
	if (d != D_NONE)
		(void)fputs("\033[0m", stdout);
}

/*
 * The three tones the text is set in, from the 16 colours every terminal
 * here has: the heading and the commands bright, the descriptions mid, the
 * keys, the rules and the numbers dim. The values are left as the terminal
 * draws them. RGB is for the cube.
 */
#define BRIGHT	"\033[1;97m"
#define MID	"\033[0;37m"
#define DIM	"\033[0;90m"

static void
tone(enum depth d, const char *t)
{
	if (d != D_NONE)
		(void)fputs(t, stdout);
}

/* ------------------------------------------------------------- the readings */

/*
 * PRETTY_NAME from /etc/os-release: "NextBSD <IMG_DATE>". build.sh writes it
 * from the same variable as /bin/nextbsd-version, so the two agree by
 * construction and this needs no fork at every login.
 */
static void
os_name(char *out, size_t len)
{
	char line[256], *p, *q;
	FILE *f;

	(void)strlcpy(out, "NextBSD", len);
	if ((f = fopen(BANNER_OS_RELEASE, "r")) == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, "PRETTY_NAME=", 12) != 0)
			continue;
		p = line + 12;
		if (*p == '"')
			p++;
		if ((q = strpbrk(p, "\"\n")) != NULL)
			*q = '\0';
		if (*p != '\0')
			(void)strlcpy(out, p, len);
		break;
	}
	(void)fclose(f);
}

static void
shell_name(char *out, size_t len)
{
	const char *sh = getenv("SHELL"), *base, *ver;

	if (sh == NULL || *sh == '\0')
		sh = "sh";
	base = (base = strrchr(sh, '/')) != NULL ? base + 1 : sh;
	/*
	 * zsh does not export its version, so /etc/zprofile passes it. Run by
	 * hand outside that, the name alone is printed rather than a guess.
	 */
	if ((ver = getenv("ZSH_VERSION")) != NULL && *ver != '\0' &&
	    strcmp(base, "zsh") == 0)
		(void)snprintf(out, len, "%s %s", base, ver);
	else
		(void)strlcpy(out, base, len);
}

static void
human(uint64_t bytes, char *out, size_t len)
{
	static const char *const u[] = { "B", "KiB", "MiB", "GiB", "TiB" };
	double v = (double)bytes;
	size_t i = 0;

	while (v >= 1024.0 && i < nitems(u) - 1) {
		v /= 1024.0;
		i++;
	}
	(void)snprintf(out, len, v < 10.0 && i > 1 ? "%.1f %s" : "%.0f %s",
	    v, u[i]);
}

static void
memory(char *out, size_t len)
{
	uint64_t phys = 0, freeb = 0;
	char a[32], b[32];
	size_t sz;

	sz = sizeof(phys);
	if (sysctlbyname("hw.physmem", &phys, &sz, NULL, 0) == -1 || phys == 0) {
		(void)strlcpy(out, "unknown", len);
		return;
	}
#ifdef __FreeBSD__
	{
		uint32_t pages = 0;
		int pagesize = getpagesize();

		sz = sizeof(pages);
		if (sysctlbyname("vm.stats.vm.v_free_count", &pages, &sz,
		    NULL, 0) == 0)
			freeb = (uint64_t)pages * (uint64_t)pagesize;
		sz = sizeof(pages);
		if (sysctlbyname("vm.stats.vm.v_inactive_count", &pages, &sz,
		    NULL, 0) == 0)
			freeb += (uint64_t)pages * (uint64_t)pagesize;
	}
#endif
	/*
	 * Without a free-page count there is no used figure, and printing
	 * "3.5 GiB / 3.5 GiB" would state a full machine rather than an
	 * unknown one. Report the total alone instead.
	 */
	human(phys, b, sizeof(b));
	if (freeb == 0 || freeb > phys) {
		(void)strlcpy(out, b, len);
		return;
	}
	human(phys - freeb, a, sizeof(a));
	(void)snprintf(out, len, "%s / %s", a, b);
}

static void
uptime(char *out, size_t len)
{
	struct timeval bt;
	size_t sz = sizeof(bt);
	time_t up;
	long d, h, m;

	if (sysctlbyname("kern.boottime", &bt, &sz, NULL, 0) == -1) {
		(void)strlcpy(out, "unknown", len);
		return;
	}
	up = time(NULL) - bt.tv_sec;
	if (up < 0)
		up = 0;
	d = (long)(up / 86400);
	h = (long)((up % 86400) / 3600);
	m = (long)((up % 3600) / 60);
	if (d > 0)
		(void)snprintf(out, len, "%ld day%s, %ld:%02ld", d,
		    d == 1 ? "" : "s", h, m);
	else if (h > 0)
		(void)snprintf(out, len, "%ld:%02ld", h, m);
	else
		(void)snprintf(out, len, "%ld min%s", m, m == 1 ? "" : "s");
}

/*
 * "live media" on a unionfs root -- the ISO pivots onto /cow tmpfs over /rofs
 * uzip -- and the device with its usage otherwise. Returns whether it is live
 * media: the one statfs answers that for the task list too, so the line and
 * the list cannot disagree.
 */
static bool
storage(char *out, size_t len)
{
	struct statfs fs;
	char used[32], tot[32];
	const char *dev;
	uint64_t t, u;

	if (statfs(BANNER_ROOT, &fs) == -1) {
		(void)strlcpy(out, "unknown", len);
		return (false);
	}
	if (strcmp(fs.f_fstypename, BANNER_LIVE_FSTYPE) == 0) {
		(void)strlcpy(out, "live media", len);
		return (true);
	}
	dev = (dev = strrchr(fs.f_mntfromname, '/')) != NULL ?
	    dev + 1 : fs.f_mntfromname;
	t = (uint64_t)fs.f_blocks * (uint64_t)fs.f_bsize;
	u = (uint64_t)(fs.f_blocks - fs.f_bfree) * (uint64_t)fs.f_bsize;
	human(u, used, sizeof(used));
	human(t, tot, sizeof(tot));
	(void)snprintf(out, len, "%s  %s / %s", dev, used, tot);
	return (false);
}

/* -------------------------------------------------------------- the layout */

struct field { const char *key, *val; };

/* The 16-step greyscale strip, the thing that makes a fetch look like one. */
static const unsigned char swatch[16][3] = {
	{0x26,0x29,0x2e},{0x33,0x38,0x3e},{0x41,0x47,0x4e},{0x50,0x56,0x5e},
	{0x5f,0x66,0x6f},{0x6f,0x76,0x80},{0x80,0x88,0x91},{0x91,0x99,0xa3},
	{0xa3,0xab,0xb4},{0xb4,0xbc,0xc5},{0xc5,0xcc,0xd4},{0xd5,0xdb,0xe2},
	{0xe3,0xe8,0xed},{0xee,0xf2,0xf6},{0xf7,0xf9,0xfb},{0xff,0xff,0xff}
};

static void
print_swatch(enum depth d)
{
	size_t i;

	for (i = 0; i < nitems(swatch); i++) {
		if (d == D_TRUE)
			(void)printf("\033[38;2;%u;%u;%um██",
			    swatch[i][0], swatch[i][1], swatch[i][2]);
		else if (d == D_16)
			(void)printf("%s██",
			    i < 6 ? "\033[1;30m" : i < 12 ? "\033[0;37m" :
			    "\033[1;37m");
		else
			(void)fputs("  ", stdout);
	}
	reset(d);
}

/* One cube row, returning how many columns it actually drew. */
static int
print_cube_row(enum depth d, int row)
{
	const struct cube_run *runs = cube_rows[row];
	int i, n = cube_nruns[row], drawn = 0, j;

	for (i = 0; i < n; i++) {
		if (runs[i].ch == NULL) {
			for (j = 0; j < runs[i].len; j++)
				(void)fputc(' ', stdout);
		} else {
			set_rgb(d, runs[i].face, runs[i].r, runs[i].g,
			    runs[i].b);
			for (j = 0; j < runs[i].len; j++)
				(void)fputs(runs[i].ch, stdout);
		}
		drawn += runs[i].len;
	}
	reset(d);
	return (drawn);
}

/* ---------------------------------------------------------------- the tasks */

/*
 * What to do first, under the banner on live media only (#285). An installed
 * machine has done all four, and nothing here is worth its screen space then.
 *
 * passwd alone runs without sudo: it is setuid and changes your own account.
 * The other three write /etc, bring up an interface, or partition a disk.
 */
static const struct task { const char *what, *how; } tasks[] = {
	{ "set a password for admin", "passwd" },
	{ "set the time zone",        "sudo tzsetup" },
	{ "connect to wireless",      "sudo wlan" },
	{ "install NextBSD to disk",  "sudo nextbsd-installer" },
};

static void
print_tasks(enum depth d)
{
	size_t i;

	/* A rule the width of the banner, the way the design draws one. */
	(void)fputc('\n', stdout);
	tone(d, DIM);
	for (i = 0; i < CUBE_COLS + GAP + RULE; i++)
		(void)fputs("─", stdout);
	reset(d);
	(void)fputs("\n\n", stdout);

	tone(d, DIM);
	(void)fputs("  Running from live media. Nothing is written to disk.",
	    stdout);
	reset(d);
	(void)fputs("\n\n", stdout);

	for (i = 0; i < nitems(tasks); i++) {
		tone(d, DIM);
		(void)printf("  %02zu   ", i + 1);
		tone(d, MID);
		(void)printf("%-28s", tasks[i].what);
		tone(d, BRIGHT);
		(void)fputs(tasks[i].how, stdout);
		reset(d);
		(void)fputc('\n', stdout);
	}
}

static void
print_banner(enum depth d)
{
	char os[128], sh[64], mem[64], up[64], st[64];
	struct field f[8];
	int row, nf = 0, pad, i;
	size_t nrows;
	bool live;

	os_name(os, sizeof(os));
	shell_name(sh, sizeof(sh));
	memory(mem, sizeof(mem));
	uptime(up, sizeof(up));
	live = storage(st, sizeof(st));

	f[nf++] = (struct field){ NULL, NULL };		/* the heading */
	f[nf++] = (struct field){ "", NULL };		/* the rule */
	f[nf++] = (struct field){ NULL, NULL };		/* a blank */
	f[nf++] = (struct field){ "kernel",  os };
	f[nf++] = (struct field){ "shell",   sh };
	f[nf++] = (struct field){ "memory",  mem };
	f[nf++] = (struct field){ "uptime",  up };
	f[nf++] = (struct field){ "storage", st };

	/* Centre the block against the cube rather than pinning it to the top. */
	nrows = CUBE_ROWS;
	pad = ((int)nrows - (nf + 2)) / 2;
	if (pad < 0)
		pad = 0;

	for (row = 0; row < (int)nrows; row++) {
		int drawn = print_cube_row(d, row);
		int idx = row - pad;

		if (idx < 0 || idx >= nf + 2) {
			(void)fputc('\n', stdout);
			continue;
		}
		for (i = drawn; i < CUBE_COLS + GAP; i++)
			(void)fputc(' ', stdout);

		if (idx == nf + 1) {			/* the strip */
			print_swatch(d);
		} else if (idx == 0) {
			tone(d, BRIGHT);
			(void)fputs("Welcome to NextBSD!", stdout);
			reset(d);
		} else if (idx == 1) {
			tone(d, DIM);
			for (i = 0; i < RULE; i++)
				(void)fputs("─", stdout);
			reset(d);
		} else if (idx >= 3 && idx < nf && f[idx].key != NULL) {
			tone(d, DIM);
			(void)printf("%-9s", f[idx].key);
			reset(d);
			(void)fputs(f[idx].val, stdout);
		}
		(void)fputc('\n', stdout);
	}

	if (live)
		print_tasks(d);
}

int
main(int argc, char *argv[])
{
	enum depth d;
	bool force = false;
	int ch;

	while ((ch = getopt(argc, argv, "f")) != -1) {
		switch (ch) {
		case 'f':
			force = true;	/* print even where a desktop exists */
			break;
		default:
			(void)fprintf(stderr, "usage: nextbsd-fetch [-f]\n");
			return (2);
		}
	}
	if (argc != optind) {
		(void)fprintf(stderr, "usage: nextbsd-fetch [-f]\n");
		return (2);
	}

	/*
	 * The gate. Silent and successful, because this runs from a login
	 * shell: anything printed here on a desktop machine is noise before
	 * a login window appears.
	 */
	if (!force && access(BANNER_LOGINWINDOW, F_OK) == 0)
		return (0);

	/*
	 * ~/.hushlogin silences this too.
	 *
	 * #298 assumed the banner would need an opt-out of its own, since
	 * login(1) checks that file for the motd and never tells anyone else.
	 * But someone who creates it is asking for a quiet login, not for a
	 * quiet motd specifically, and a second file to learn would be a worse
	 * answer than honouring the one that already means this.
	 */
	if (!force) {
		const char *home = getenv("HOME");
		char path[PATH_MAX];

		if (home != NULL && *home != '\0') {
			(void)snprintf(path, sizeof(path), "%s/.hushlogin",
			    home);
			if (access(path, F_OK) == 0)
				return (0);
		}
	}

	d = depth_of();
	print_banner(d);
	return (0);
}
