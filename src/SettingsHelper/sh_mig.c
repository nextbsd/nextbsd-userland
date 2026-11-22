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
#define	RIGHT_DATETIME	"org.nextbsd.settings.datetime"
#define	RIGHT_USERS	"org.nextbsd.settings.users"
#define	RIGHT_OWN_PASSWORD "org.nextbsd.settings.users.own-password"
#define	RIGHT_PRINTERS	"org.nextbsd.settings.printers"
#define	RIGHT_NETWORK	"org.nextbsd.settings.network"

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

/* The Date & Time operations' common start: may this caller change it? */
static bool
datetime_allowed(audit_token_t audit, const char *op, struct sh_caller *caller,
    int *status)
{
	CALLER(audit, caller);
	*status = sh_rights_check(caller, RIGHT_DATETIME, true, NULL);
	if (*status != SH_OK) {
		sh_log("pid %d: %s refused (%d)", (int)caller->pid, op, *status);
		return (false);
	}
	return (true);
}

kern_return_t
_sh_set_time_zone(mach_port_t server, SHAuthForm auth, SHZoneName zone_in,
    int *status, audit_token_t audit)
{
	struct sh_caller caller;
	SHZoneName zone;

	(void)server;
	(void)auth;
	WIRE_STR(zone, zone_in);
	if (!datetime_allowed(audit, "SetTimeZone", &caller, status))
		return (KERN_SUCCESS);
	*status = sh_tz_set(zone);
	sh_log("pid %d: SetTimeZone \"%s\": %d", (int)caller.pid, zone,
	    *status);
	return (KERN_SUCCESS);
}

kern_return_t
_sh_set_date(mach_port_t server, SHAuthForm auth, int64_t seconds,
    int microseconds, int *status, audit_token_t audit)
{
	struct sh_caller caller;

	(void)server;
	(void)auth;
	if (!datetime_allowed(audit, "SetDate", &caller, status))
		return (KERN_SUCCESS);
	*status = sh_date_set(seconds, microseconds);
	sh_log("pid %d: SetDate %lld: %d", (int)caller.pid, (long long)seconds,
	    *status);
	return (KERN_SUCCESS);
}

kern_return_t
_sh_set_network_time(mach_port_t server, SHAuthForm auth, boolean_t enabled,
    SHNTPServer server_in, int *status, audit_token_t audit)
{
	struct sh_caller caller;
	SHNTPServer ntp_server;

	(void)server;
	(void)auth;
	WIRE_STR(ntp_server, server_in);
	if (!datetime_allowed(audit, "SetNetworkTime", &caller, status))
		return (KERN_SUCCESS);
	*status = sh_ntp_set(enabled != FALSE, ntp_server);
	sh_log("pid %d: SetNetworkTime %s \"%s\": %d", (int)caller.pid,
	    enabled ? "on" : "off", ntp_server, *status);
	return (KERN_SUCCESS);
}

/* The Users & Login operations' common start, as datetime_allowed. */
static bool
users_allowed(audit_token_t audit, const char *op, struct sh_caller *caller,
    int *status)
{
	CALLER(audit, caller);
	*status = sh_rights_check(caller, RIGHT_USERS, true, NULL);
	if (*status != SH_OK) {
		sh_log("pid %d: %s refused (%d)", (int)caller->pid, op, *status);
		return (false);
	}
	return (true);
}

kern_return_t
_sh_create_user(mach_port_t server, SHAuthForm auth, SHUserName user_in,
    SHFullName full_name_in, SHPassword password_in, boolean_t admin,
    int *status, audit_token_t audit)
{
	struct sh_caller caller;
	SHUserName user;
	SHFullName full_name;
	SHPassword password;

	(void)server;
	(void)auth;
	WIRE_STR(user, user_in);
	WIRE_STR(full_name, full_name_in);
	WIRE_STR(password, password_in);
	explicit_bzero(password_in, sizeof(SHPassword));
	if (users_allowed(audit, "CreateUser", &caller, status)) {
		*status = sh_user_create(user, full_name, password,
		    admin != FALSE);
		sh_log("pid %d: CreateUser \"%s\"%s: %d", (int)caller.pid,
		    user, admin ? " (admin)" : "", *status);
	}
	explicit_bzero(password, sizeof(password));
	return (KERN_SUCCESS);
}

kern_return_t
_sh_delete_user(mach_port_t server, SHAuthForm auth, SHUserName user_in,
    boolean_t keep_home, int *status, audit_token_t audit)
{
	struct sh_caller caller;
	SHUserName user;

	(void)server;
	(void)auth;
	WIRE_STR(user, user_in);
	if (!users_allowed(audit, "DeleteUser", &caller, status))
		return (KERN_SUCCESS);
	*status = sh_user_delete(user, keep_home != FALSE);
	sh_log("pid %d: DeleteUser \"%s\"%s: %d", (int)caller.pid, user,
	    keep_home ? " (home kept)" : "", *status);
	return (KERN_SUCCESS);
}

/*
 * The users right, or the console user setting their own password with
 * the own-password right (authenticate-session-owner: they gave their
 * old password to sh_authorize).
 */
kern_return_t
_sh_set_password(mach_port_t server, SHAuthForm auth, SHUserName user_in,
    SHPassword password_in, int *status, audit_token_t audit)
{
	struct sh_caller caller;
	SHUserName user;
	SHPassword password;

	(void)server;
	(void)auth;
	WIRE_STR(user, user_in);
	WIRE_STR(password, password_in);
	explicit_bzero(password_in, sizeof(SHPassword));
	CALLER(audit, &caller);
	*status = sh_rights_check(&caller, RIGHT_USERS, true, NULL);
	if (*status != SH_OK && caller.in_console &&
	    strcmp(user, caller.console.user) == 0)
		*status = sh_rights_check(&caller, RIGHT_OWN_PASSWORD, true,
		    NULL);
	if (*status != SH_OK)
		sh_log("pid %d: SetPassword refused (%d)", (int)caller.pid,
		    *status);
	else {
		*status = sh_user_set_password(user, password);
		sh_log("pid %d: SetPassword \"%s\": %d", (int)caller.pid,
		    user, *status);
	}
	explicit_bzero(password, sizeof(password));
	return (KERN_SUCCESS);
}

kern_return_t
_sh_set_admin(mach_port_t server, SHAuthForm auth, SHUserName user_in,
    boolean_t admin, int *status, audit_token_t audit)
{
	struct sh_caller caller;
	SHUserName user;

	(void)server;
	(void)auth;
	WIRE_STR(user, user_in);
	if (!users_allowed(audit, "SetAdmin", &caller, status))
		return (KERN_SUCCESS);
	*status = sh_user_set_admin(user, admin != FALSE);
	sh_log("pid %d: SetAdmin \"%s\" %s: %d", (int)caller.pid, user,
	    admin ? "on" : "off", *status);
	return (KERN_SUCCESS);
}

kern_return_t
_sh_set_auto_login(mach_port_t server, SHAuthForm auth, SHUserName user_in,
    int *status, audit_token_t audit)
{
	struct sh_caller caller;
	SHUserName user;

	(void)server;
	(void)auth;
	WIRE_STR(user, user_in);
	if (!users_allowed(audit, "SetAutoLogin", &caller, status))
		return (KERN_SUCCESS);
	*status = sh_login_set_auto_login(user);
	sh_log("pid %d: SetAutoLogin \"%s\": %d", (int)caller.pid, user,
	    *status);
	return (KERN_SUCCESS);
}

kern_return_t
_sh_set_login_window_options(mach_port_t server, SHAuthForm auth,
    boolean_t show_name_fields, boolean_t show_power_buttons, int *status,
    audit_token_t audit)
{
	struct sh_caller caller;

	(void)server;
	(void)auth;
	if (!users_allowed(audit, "SetLoginWindowOptions", &caller, status))
		return (KERN_SUCCESS);
	*status = sh_login_set_options(show_name_fields != FALSE,
	    show_power_buttons != FALSE);
	sh_log("pid %d: SetLoginWindowOptions %s, %s: %d", (int)caller.pid,
	    show_name_fields ? "name and password" : "list of users",
	    show_power_buttons ? "power buttons" : "no power buttons",
	    *status);
	return (KERN_SUCCESS);
}

kern_return_t
_sh_cups_admin(mach_port_t server, SHAuthForm auth, int op,
    SHPrinterName printer_in, SHDeviceURI uri_in, SHPrinterText info_in,
    SHPrinterText location_in, int *status, audit_token_t audit)
{
	struct sh_caller caller;
	SHPrinterName printer;
	SHDeviceURI uri;
	SHPrinterText info, location;

	(void)server;
	(void)auth;
	WIRE_STR(printer, printer_in);
	WIRE_STR(uri, uri_in);
	WIRE_STR(info, info_in);
	WIRE_STR(location, location_in);
	CALLER(audit, &caller);
	*status = sh_rights_check(&caller, RIGHT_PRINTERS, true, NULL);
	if (*status != SH_OK) {
		sh_log("pid %d: CUPSAdmin refused (%d)", (int)caller.pid,
		    *status);
		return (KERN_SUCCESS);
	}
	*status = sh_printer_admin(op, printer, uri, info, location);
	sh_log("pid %d: CUPSAdmin %s \"%s\" %s: %d", (int)caller.pid,
	    op == SH_CUPS_ADD ? "add" : op == SH_CUPS_DELETE ? "delete" : "?",
	    printer, op == SH_CUPS_ADD ? uri : "", *status);
	return (KERN_SUCCESS);
}

/* Network.app's operations' common start. */
static bool
network_allowed(audit_token_t audit, const char *op, struct sh_caller *caller,
    int *status)
{
	CALLER(audit, caller);
	*status = sh_rights_check(caller, RIGHT_NETWORK, true, NULL);
	if (*status != SH_OK) {
		sh_log("pid %d: %s refused (%d)", (int)caller->pid, op, *status);
		return (false);
	}
	return (true);
}

kern_return_t
_sh_commit_network_preferences(mach_port_t server, SHAuthForm auth,
    SHNetServiceID service_id, int ipv4_method, SHNetAddress ipv4_address,
    SHNetAddress ipv4_mask, SHNetAddress ipv4_router, int ipv6_method,
    SHNetAddress ipv6_address, int ipv6_prefix, SHNetAddress ipv6_router,
    SHNetList dns_servers, SHNetList search_domains, int *status,
    audit_token_t audit)
{
	struct sh_caller caller;
	struct sh_net_service s;

	(void)server;
	(void)auth;
	(void)memset(&s, 0, sizeof(s));
	WIRE_STR(s.service_id, service_id);
	s.ipv4_method = ipv4_method;
	WIRE_STR(s.ipv4_address, ipv4_address);
	WIRE_STR(s.ipv4_mask, ipv4_mask);
	WIRE_STR(s.ipv4_router, ipv4_router);
	s.ipv6_method = ipv6_method;
	WIRE_STR(s.ipv6_address, ipv6_address);
	s.ipv6_prefix = ipv6_prefix;
	WIRE_STR(s.ipv6_router, ipv6_router);
	WIRE_STR(s.dns_servers, dns_servers);
	WIRE_STR(s.search_domains, search_domains);
	if (!network_allowed(audit, "CommitNetworkPreferences", &caller,
	    status))
		return (KERN_SUCCESS);
	*status = sh_net_commit(&s);
	sh_log("pid %d: CommitNetworkPreferences %s IPv4 %d IPv6 %d: %d",
	    (int)caller.pid, s.service_id, ipv4_method, ipv6_method, *status);
	return (KERN_SUCCESS);
}

kern_return_t
_sh_set_computer_name(mach_port_t server, SHAuthForm auth,
    SHComputerName computer_in, SHLocalHostName local_in, int *status,
    audit_token_t audit)
{
	struct sh_caller caller;
	SHComputerName computer;
	SHLocalHostName local;

	(void)server;
	(void)auth;
	WIRE_STR(computer, computer_in);
	WIRE_STR(local, local_in);
	if (!network_allowed(audit, "SetComputerName", &caller, status))
		return (KERN_SUCCESS);
	*status = sh_computer_name_set(computer, local);
	sh_log("pid %d: SetComputerName \"%s\" \"%s\": %d", (int)caller.pid,
	    computer, local, *status);
	return (KERN_SUCCESS);
}
