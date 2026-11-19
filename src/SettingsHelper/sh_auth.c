/*
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * sh_auth.c — accounts and passwords.
 *
 * Passwords are checked with PAM, service "authorization"
 * (/etc/pam.d/authorization), as Darwin's authd does. The helper hands
 * the password to PAM's conversation and wipes its copy; it never writes
 * it anywhere.
 */
#include <sys/types.h>

#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <security/pam_appl.h>

#include "SettingsHelper.h"

#define	SH_PAM_SERVICE	"authorization"
#define	SH_MAX_GROUPS	1024

bool
sh_user_in_group(uid_t uid, const char *group)
{
	gid_t groups[SH_MAX_GROUPS];
	int ngroups = SH_MAX_GROUPS, i;
	struct passwd *pw;
	struct group *gr;

	if (group == NULL || group[0] == '\0')
		return (false);
	if ((pw = getpwuid(uid)) == NULL || (gr = getgrnam(group)) == NULL)
		return (false);
	if (pw->pw_gid == gr->gr_gid)
		return (true);
	if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups) != 0)
		ngroups = SH_MAX_GROUPS;	/* truncated: look at what came */
	for (i = 0; i < ngroups; i++)
		if (groups[i] == gr->gr_gid)
			return (true);
	return (false);
}

/*
 * Root reads the real password field (through directory services or
 * master.passwd). An empty one means the account has no password, which
 * pam_unix's nullok accepts with an empty password.
 */
bool
sh_user_has_password(const char *user)
{
	struct passwd *pw;

	if (user == NULL || (pw = getpwnam(user)) == NULL)
		return (true);
	return (pw->pw_passwd == NULL || pw->pw_passwd[0] != '\0');
}

struct conv_data {
	const char	*password;
};

static int
conv(int n, const struct pam_message **msg, struct pam_response **resp,
    void *data)
{
	struct conv_data *cd = data;
	struct pam_response *r;
	int i;

	if (n <= 0 || n > PAM_MAX_NUM_MSG)
		return (PAM_CONV_ERR);
	if ((r = calloc((size_t)n, sizeof(*r))) == NULL)
		return (PAM_BUF_ERR);
	for (i = 0; i < n; i++) {
		switch (msg[i]->msg_style) {
		case PAM_PROMPT_ECHO_OFF:
			if ((r[i].resp = strdup(cd->password)) == NULL)
				goto fail;
			break;
		case PAM_PROMPT_ECHO_ON:
			/* Nothing here asks for anything but a password. */
			goto fail;
		case PAM_ERROR_MSG:
		case PAM_TEXT_INFO:
			break;
		default:
			goto fail;
		}
	}
	*resp = r;
	return (PAM_SUCCESS);
fail:
	for (i = 0; i < n; i++) {
		if (r[i].resp != NULL) {
			explicit_bzero(r[i].resp, strlen(r[i].resp));
			free(r[i].resp);
		}
	}
	free(r);
	return (PAM_CONV_ERR);
}

bool
sh_pam_check(const char *user, const char *password, uid_t *uid)
{
	struct conv_data cd = { password };
	struct pam_conv pc = { conv, &cd };
	pam_handle_t *pamh = NULL;
	struct passwd *pw;
	int rc;

	if (user == NULL || user[0] == '\0' || (pw = getpwnam(user)) == NULL)
		return (false);
	*uid = pw->pw_uid;
	rc = pam_start(SH_PAM_SERVICE, user, &pc, &pamh);
	if (rc != PAM_SUCCESS) {
		sh_log("pam_start(%s): %d", SH_PAM_SERVICE, rc);
		return (false);
	}
	rc = pam_authenticate(pamh, 0);
	if (rc == PAM_SUCCESS)
		rc = pam_acct_mgmt(pamh, 0);
	(void)pam_end(pamh, rc);
	return (rc == PAM_SUCCESS);
}
