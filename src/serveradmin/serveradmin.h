/*
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * serveradmin(8): what Server.app runs on each NextBSD computer, as root
 * over SSH (E42, nextbsd/nextbsd#654). One typed subcommand per request,
 * answers in JSON on stdout. Every change runs fixed command lines with
 * validated arguments; nothing the caller sends is ever run as a command
 * or used as a path.
 */

#ifndef SERVERADMIN_H
#define SERVERADMIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bumped whenever an answer's shape or a subcommand changes in a way an
 * older Server.app couldn't handle. */
#define	SA_PROTOCOL	1

/* ---- JSON output (json.c) ------------------------------------------- */
void	j_obj(void);			/* { */
void	j_end(void);			/* } */
void	j_arr(void);			/* [ */
void	j_aend(void);			/* ] */
void	j_key(const char *);
void	j_str(const char *);
void	j_strn(const char *, size_t);
void	j_int(int64_t);
void	j_dbl(double);
void	j_bool(bool);
void	j_null(void);
/* key + value in one call */
void	j_kstr(const char *, const char *);
void	j_kint(const char *, int64_t);
void	j_kbool(const char *, bool);
void	j_flush(void);			/* write the answer to stdout */
/* Print {"error": msg, "code": code} and exit 1 (or 2 for usage). */
void	sa_fail(const char *code, const char *fmt, ...)
	    __attribute__((noreturn, format(printf, 2, 3)));
#define	sa_usage(...)	sa_fail("usage", __VA_ARGS__)
/* Called once by sa_fail before it exits, to undo a half-made change. */
extern void (*sa_on_fail)(void);

/* ---- running fixed command lines (run.c) ---------------------------- */
struct sa_out {
	char	*out;		/* stdout, NUL-terminated */
	size_t	 outlen;
	char	*err;		/* stderr, NUL-terminated */
	int	 status;	/* exit status, or -1 if it didn't run */
};
/* Run argv[0] (an absolute path) with argv, no shell, stdin from
 * /dev/null, capturing stdout and stderr. Returns true if it exited 0. */
bool	sa_run(const char *const argv[], struct sa_out *);
void	sa_out_free(struct sa_out *);
/* Run it with this process's stdin and stdout (for zfs send/recv
 * streams); stderr is captured into *err. */
bool	sa_run_stdio(const char *const argv[], char **err);
/* Run and fail the request with its stderr if it doesn't exit 0. */
void	sa_must(const char *what, const char *const argv[]);
/* The lines of a buffer, split in place. */
char	**sa_lines(char *buf, int *n);
char	**sa_fields(char *line, const char *sep, int *n);

/* ---- argument checks (check.c) -------------------------------------- */
bool	sa_is_label(const char *);	/* 1-63 [a-z0-9-], not leading '-' */
bool	sa_is_user(const char *);	/* an account name */
bool	sa_is_pool(const char *);	/* a pool name we accept */
bool	sa_is_be(const char *);		/* a boot environment name */
bool	sa_is_onoff(const char *, bool *);
bool	sa_parse_size(const char *, uint64_t *); /* "10G", "500M", bytes */

/* ---- subcommands ---------------------------------------------------- */
void	cmd_status(int, char **);
void	cmd_pair(int, char **);
void	cmd_update(int, char **);
void	cmd_users(int, char **);
void	cmd_quota(int, char **);
void	cmd_directory(int, char **);
void	cmd_pool(int, char **);
void	cmd_disk(int, char **);
void	cmd_jailbase(int, char **);
void	cmd_jail(int, char **);
void	cmd_crashdumps(int, char **);
void	cmd_log(int, char **);

/* Shared facts (status.c). */
const char *sa_main_pool(void);		/* the pool holding / */
bool	sa_wired(char *ifname, size_t len, bool *connected);
bool	sa_ipv4(const char *ifname, char *addr, size_t len);
bool	sa_gateway(char *addr, size_t len);
const char *sa_freebsd_release(void);	/* "15.1-RELEASE" from the kernel */

/* mDNS (mdns.c): does anything on the network answer name.local? */
bool	sa_mdns_taken(const char *label, char *addr, size_t len);
bool	sa_mdns_resolve(const char *label, char *addr, size_t len);

#define	ZFS	"/sbin/zfs"
#define	ZPOOL	"/sbin/zpool"
#define	BECTL	"/sbin/bectl"
#define	JAIL	"/usr/sbin/jail"
#define	JEXEC	"/usr/sbin/jexec"
#define	JLS	"/usr/sbin/jls"
#define	PKG	"/usr/sbin/pkg"
#define	IFCONFIG "/sbin/ifconfig"
#define	MOUNT_NULLFS "/sbin/mount_nullfs"
#define	UMOUNT	"/sbin/umount"
#define	LAUNCHCTL "/bin/launchctl"
#define	DUMPON	"/sbin/dumpon"
#define	SYSRC	"/usr/sbin/sysrc"

#endif
