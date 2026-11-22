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

#endif /* _SETTINGSHELPER_H */
