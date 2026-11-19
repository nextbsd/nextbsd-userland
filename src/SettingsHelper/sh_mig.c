/*
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * sh_mig.c — settingshelper.defs' routines.
 *
 * Every routine returns KERN_SUCCESS once it has an answer, and the
 * answer is `status`. Each first finds out who sent the message from its
 * audit token (sh_session.c). Strings from the wire are copied and
 * NUL-terminated before use; the password's copies are wiped.
 */
#include <string.h>

#include "SettingsHelper.h"
#include "settingshelperServer.h"	/* MIG: the routines' prototypes */

#define	RIGHT_ENERGY	"org.nextbsd.settings.energy"

/* Darwin's audit token layout, as audit_token_to_au32 reads it. */
#define	CALLER(tok, c)							\
	sh_caller_from_token((uid_t)(tok).val[1], (pid_t)(tok).val[5], (c))

#define	WIRE_STR(dst, src) do {						\
	(void)memcpy((dst), (src), sizeof(dst));			\
	(dst)[sizeof(dst) - 1] = '\0';					\
} while (0)

kern_return_t
_sh_right_info(mach_port_t server, SHRightName right_in, int *rule,
    int *timeout, boolean_t *authorized, int *seconds_left,
    SHUserName console_user, boolean_t *user_is_admin,
    boolean_t *user_has_password, int *status, audit_token_t audit)
{
	struct sh_caller caller;
	struct sh_rule r;
	SHRightName right;
	int rc;

	(void)server;
	WIRE_STR(right, right_in);
	*rule = SH_RULE_DENY;
	*timeout = 0;
	*authorized = FALSE;
	*seconds_left = 0;
	(void)memset(console_user, 0, sizeof(SHUserName));
	*user_is_admin = FALSE;
	*user_has_password = TRUE;

	CALLER(audit, &caller);
	if (!caller.root && !caller.in_console) {
		*status = SH_ERR_NOT_CONSOLE;
		return (KERN_SUCCESS);
	}
	if (!sh_rule_for_right(right, &r)) {
		*status = SH_ERR_UNKNOWN_RIGHT;
		return (KERN_SUCCESS);
	}
	*rule = r.kind;
	*timeout = r.timeout;
	rc = sh_rights_check(&caller, right, false, seconds_left);
	*authorized = rc == SH_OK;
	if (caller.in_console) {
		(void)strlcpy(console_user, caller.console.user,
		    sizeof(SHUserName));
		*user_is_admin = sh_user_in_group(caller.console.uid,
		    r.group[0] != '\0' ? r.group : "admin");
		*user_has_password = sh_user_has_password(caller.console.user);
	}
	*status = SH_OK;
	return (KERN_SUCCESS);
}

kern_return_t
_sh_authorize(mach_port_t server, SHRightName right_in, SHUserName user_in,
    SHPassword password_in, int *seconds_left, int *status,
    audit_token_t audit)
{
	struct sh_caller caller;
	SHRightName right;
	SHUserName user;
	SHPassword password;

	(void)server;
	WIRE_STR(right, right_in);
	WIRE_STR(user, user_in);
	WIRE_STR(password, password_in);
	explicit_bzero(password_in, sizeof(SHPassword));

	CALLER(audit, &caller);
	*status = sh_rights_authorize(&caller, right, user, password,
	    seconds_left);
	explicit_bzero(password, sizeof(password));
	return (KERN_SUCCESS);
}

kern_return_t
_sh_deauthorize(mach_port_t server, int *status, audit_token_t audit)
{
	struct sh_caller caller;

	(void)server;
	CALLER(audit, &caller);
	if (!caller.root && !caller.in_console) {
		*status = SH_ERR_NOT_CONSOLE;
		return (KERN_SUCCESS);
	}
	sh_rights_forget(&caller);
	*status = SH_OK;
	return (KERN_SUCCESS);
}

kern_return_t
_sh_set_pm_preferences(mach_port_t server, SHAuthForm auth, int source,
    SHPMValues values, int *status, audit_token_t audit)
{
	struct sh_caller caller;

	(void)server;
	(void)auth;		/* all zeroes until an authd; see the .defs */
	CALLER(audit, &caller);
	*status = sh_rights_check(&caller, RIGHT_ENERGY, true, NULL);
	if (*status != SH_OK) {
		sh_log("pid %d: SetPMPreferences refused (%d)", (int)caller.pid,
		    *status);
		return (KERN_SUCCESS);
	}
	*status = sh_pm_set(source, values);
	sh_log("pid %d: SetPMPreferences source %d: %d", (int)caller.pid,
	    source, *status);
	return (KERN_SUCCESS);
}
