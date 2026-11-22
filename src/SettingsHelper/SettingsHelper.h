/*
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * SettingsHelper.h — the helper's internal interfaces.
 */
#ifndef _SETTINGSHELPER_H
#define _SETTINGSHELPER_H

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "settingshelper_types.h"

/* ---- logging (SettingsHelper.c) ---------------------------------------- */

void	sh_log(const char *fmt, ...) __printflike(1, 2);

/* Seconds on a clock a step of the wall clock doesn't move. */
time_t	sh_now(void);

/* ---- the caller (sh_session.c) ----------------------------------------- */

/*
 * The console session: loginwindow's newest session record, checked
 * against the running processes. `leader` and `leader_start` (its start,
 * microseconds since boot) name it for this boot.
 */
struct sh_console {
	uid_t		uid;
	pid_t		leader;
	long long	leader_start;
	char		user[SH_USER_MAX + 1];
};

/*
 * Who sent a message, from its audit token. `root` callers pass every
 * check. Otherwise `in_console` says whether the sender is in the console
 * session, which then fills `console`.
 */
struct sh_caller {
	pid_t		pid;
	uid_t		euid;
	bool		root;
	bool		in_console;
	struct sh_console console;
};

/*
 * Fill *caller from an audit token's euid and pid (sh_mig.c takes them
 * out: no Mach headers here, whose boolean_t clashes with <sys/user.h>'s).
 * Never fails; see in_console.
 */
void	sh_caller_from_token(uid_t euid, pid_t pid, struct sh_caller *caller);

/* The console session, if there is one. */
bool	sh_console_session(struct sh_console *out);

/* ---- accounts (sh_auth.c) ---------------------------------------------- */

/* Is `uid` a member of group `group` (by name)? */
bool	sh_user_in_group(uid_t uid, const char *group);

/* Does the account have a password? (An unknown account: true.) */
bool	sh_user_has_password(const char *user);

/*
 * Check a name and password with PAM service "authorization". On success
 * *uid is the account's. The password is not kept.
 */
bool	sh_pam_check(const char *user, const char *password, uid_t *uid);

/* ---- rights (sh_rights.c) ---------------------------------------------- */

/* A right's rule, resolved from SettingsRights.plist. */
struct sh_rule {
	int		kind;		/* SH_RULE_* */
	char		group[64];	/* for IS_ADMIN and AUTH_ADMIN */
	int		timeout;	/* seconds; 0: good for one operation */
};

/* Look up `right`. False: no such right (or a broken rules file). */
bool	sh_rule_for_right(const char *right, struct sh_rule *rule);

/*
 * May this caller use `right` now, without a prompt? SH_OK, or
 * SH_ERR_NOT_CONSOLE, SH_ERR_DENIED, SH_ERR_AUTH_NEEDED,
 * SH_ERR_UNKNOWN_RIGHT. *seconds_left (if not NULL) says how much longer
 * a credential that satisfies it lasts. `consume`: this is an operation,
 * so a one-time (timeout 0) credential is used up.
 */
int	sh_rights_check(const struct sh_caller *caller, const char *right,
	    bool consume, int *seconds_left);

/* sh_authorize: prove `right` with a name and password. */
int	sh_rights_authorize(const struct sh_caller *caller, const char *right,
	    const char *user, const char *password, int *seconds_left);

/* Forget every credential of the caller's session. */
void	sh_rights_forget(const struct sh_caller *caller);

/* Drop expired credentials; true if any are left. */
bool	sh_rights_live(void);

/* ---- files and programs (sh_file.c) ------------------------------------ */

/* Run argv[0] (a full path) with argv, stdin and stdout on /dev/null;
 * true if it exited 0. */
bool	sh_run(const char *const argv[]);

/* Read a whole (small, regular) file. *len gets its size; NULL on
 * failure. The caller frees it. */
char	*sh_read_file(const char *path, size_t *len);

/* Replace `path` with `len` bytes, mode 0644, through a temporary. */
bool	sh_write_file(const char *path, const char *data, size_t len);

/* ---- operations -------------------------------------------------------- */

/* sh_pm.c: one power source's Energy Saver settings. */
int	sh_pm_set(int source, const int32_t values[SH_PM_VALUES]);

/* sh_time.c: the time zone, the clock, and network time. */
int	sh_tz_set(const char *zone);
int	sh_date_set(int64_t seconds, int32_t microseconds);
int	sh_ntp_set(bool on, const char *server);

/* sh_users.c: accounts in the local directory (libds). */
int	sh_user_create(const char *name, const char *full_name,
	    const char *password, bool admin);
int	sh_user_delete(const char *name, bool keep_home);
int	sh_user_set_password(const char *name, const char *password);
int	sh_user_set_admin(const char *name, bool admin);
bool	sh_user_is_passwordless(const char *name);

/* sh_login.c: the login window's settings. */
int	sh_login_set_auto_login(const char *user);
int	sh_login_forget_user(const char *user);
int	sh_login_set_options(bool show_name_fields, bool show_power_buttons);

/* sh_cups.c: CUPSAdmin, a printer added (SH_CUPS_ADD) or removed. */
int	sh_printer_admin(int op, const char *printer, const char *uri,
	    const char *info, const char *location);

/* sh_net.c: a network service's settings, and the computer's name. */
struct sh_net_service {
	SHNetServiceID	service_id;
	int		ipv4_method;	/* SH_NET_V4_* */
	SHNetAddress	ipv4_address;
	SHNetAddress	ipv4_mask;
	SHNetAddress	ipv4_router;
	int		ipv6_method;	/* SH_NET_V6_* */
	SHNetAddress	ipv6_address;
	int		ipv6_prefix;
	SHNetAddress	ipv6_router;
	SHNetList	dns_servers;	/* one per line */
	SHNetList	search_domains;
};

int	sh_net_commit(const struct sh_net_service *service);
int	sh_computer_name_set(const char *computer, const char *local);

/* sh_locale.c: the system's language, login.conf's default class. */
int	sh_locale_set(const char *locale);

/* sh_update.c: Update.app's jobs, SH_UPDATE_JOB_*. */
struct sh_update_status {
	int		job;
	int		state;
	bool		pending;	/* pkg upgrade has something to do */
	SHUpdateVersion	available;
	int64_t		size;
	int64_t		done;
	int64_t		total;
	SHUpdateText	line;
	bool		changed;
	int		restart;
	SHUpdateVersion	restart_version;	/* the update installed */
};

int	sh_update_start(int job);
int	sh_update_cancel(void);
void	sh_update_status(struct sh_update_status *status);
int	sh_update_restart(void);
/* Collect a finished job; is one still running? */
void	sh_update_reap(void);
bool	sh_update_busy(void);
/* A job runs, or this process installed an update that waits for its
 * restart: don't exit idle. */
bool	sh_update_live(void);

#endif /* _SETTINGSHELPER_H */
