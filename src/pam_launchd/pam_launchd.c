/*
 * Copyright (c) 2007-2009 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#define _PAM_EXTERN_FUNCTIONS
#define PAM_SM_SESSION
#include <security/pam_modules.h>
#include <security/pam_appl.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vproc.h>
#include <vproc_priv.h>
#include <servers/bootstrap.h>
#include <bootstrap_priv.h>
#include <pwd.h>
#include <sys/syslimits.h>
#ifdef __FreeBSD__
/*
 * NextBSD: OpenPAM declares openpam_get_option() and openpam_log() here;
 * Apple's <security/pam_modules.h> pulled them in itself.
 */
#include <security/openpam.h>
/*
 * NextBSD: a client stub for launchd's get_root_bootstrap routine that waits
 * at most a bounded time for the reply (MIG WaitTime). Generated at build time
 * from pam_launchd_probe.defs; see pam_launchd_probe() below.
 */
#include "pam_launchd_probe.h"

/* How long the liveness probe waits for the per-user launchd, in ms. */
#define PAM_LAUNCHD_PROBE_TIMEOUT_MS 3000
#endif

#define SESSION_TYPE_OPT "launchd_session_type"
#define DEFAULT_SESSION_TYPE VPROCMGR_SESSION_BACKGROUND
#define NULL_SESSION_TYPE "NullSession"

/* An application may modify the default behavior as follows:
 * (1) Choose a specific session type:
 *     pam_putenv(pamh, "launchd_session_type=Aqua");
 * (2) Choose to not start a new session:
 *     pam_putenv(pamh, "launchd_session_type=NullSession");
 * Otherwise, if launchd_session_type is not set, a new session of the
 * default type will be created.
 */

extern vproc_err_t _vproc_post_fork_ping(void);

static mach_port_t
get_root_bootstrap_port(void)
{
	mach_port_t parent_port = MACH_PORT_NULL;
	mach_port_t previous_port = MACH_PORT_NULL;
	do {
		if (previous_port) {
			if (previous_port != bootstrap_port) {
				mach_port_deallocate(mach_task_self(), previous_port);
			}
			previous_port = parent_port;
		} else {
			previous_port = bootstrap_port;
		}
		if (bootstrap_parent(previous_port, &parent_port) != 0) {
			return MACH_PORT_NULL;
		}
	} while (parent_port != previous_port);
	
	return parent_port;
}

#ifdef __FreeBSD__
/*
 * NextBSD: check that the per-user launchd behind puc is alive and answering
 * before this process (and every child it spawns, e.g. the login shell) is
 * moved onto it. PID 1 hands out a per-user port even when the per-user
 * launchd has not started or checked in, and once bootstrap_port points at a
 * port nobody services, every later bootstrap lookup in the session blocks
 * forever. Ask the per-user launchd itself for its root bootstrap (a routine
 * it answers locally, without forwarding to PID 1) and wait at most
 * PAM_LAUNCHD_PROBE_TIMEOUT_MS for the reply. On a timeout the MIG stub
 * destroys the reply port, so a late answer cannot be mistaken for the reply
 * to a later RPC on this thread.
 *
 * Returns 1 if the per-user launchd answered, 0 if not.
 */
static int
pam_launchd_probe(mach_port_t puc, uid_t uid)
{
	mach_port_t rbs = MACH_PORT_NULL;
	kern_return_t kr;

	kr = pam_launchd_probe_get_root_bootstrap(puc,
	    PAM_LAUNCHD_PROBE_TIMEOUT_MS, &rbs);
	if (kr != KERN_SUCCESS) {
		openpam_log(PAM_LOG_ERROR, "The per-user launchd for UID %u did "
		    "not answer within %d ms (0x%x); staying in the root bootstrap.",
		    uid, PAM_LAUNCHD_PROBE_TIMEOUT_MS, kr);
		return 0;
	}
	if (rbs != MACH_PORT_NULL) {
		mach_port_deallocate(mach_task_self(), rbs);
	}
	return 1;
}

/*
 * NextBSD: put this process back on the root bootstrap port root_bs (whose
 * send right the caller holds and hands over) and drop the right on whatever
 * bootstrap_port is now. Used when a step after the switch to the per-user
 * bootstrap fails, so the session carries on exactly as it would without
 * this module.
 */
static void
pam_launchd_restore_root_bootstrap(mach_port_t root_bs)
{
	mach_port_t cur = bootstrap_port;

	task_set_bootstrap_port(mach_task_self(), root_bs);
	bootstrap_port = root_bs;
	if (cur != MACH_PORT_NULL && cur != root_bs) {
		mach_port_deallocate(mach_task_self(), cur);
	}
}
#endif /* __FreeBSD__ */

PAM_EXTERN int
pam_sm_open_session(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
	char buffer[2*PATH_MAX];
	const char* default_session_type = DEFAULT_SESSION_TYPE;
	const char* session_type = pam_getenv(pamh, SESSION_TYPE_OPT);
	const char* username;
	struct passwd *pwd;
	struct passwd pwdbuf;
	uid_t uid, suid;
#ifdef __FreeBSD__
	mach_port_t root_bs;
#endif

	/* Deterine the launchd session type. */
	if (NULL == (default_session_type = openpam_get_option(pamh, SESSION_TYPE_OPT))) {
		openpam_log(PAM_LOG_DEBUG, "No session type specified.");
		default_session_type = DEFAULT_SESSION_TYPE;
	}
	if (NULL == session_type) {
		session_type = default_session_type;
	} else if (0 == strcmp(session_type, NULL_SESSION_TYPE)) {
		openpam_log(PAM_LOG_DEBUG, "Skipping due to NULL session type.");
		return PAM_IGNORE;
	}
	
	/* Get the username (and UID). */
	if (PAM_SUCCESS != pam_get_item(pamh, PAM_USER, (void *)&username) || NULL == username) {
		openpam_log(PAM_LOG_DEBUG, "The username could not be obtained.");
		return PAM_IGNORE;
	}
	if (0 != getpwnam_r(username, &pwdbuf, buffer, sizeof(buffer), &pwd) || NULL == pwd) {
		openpam_log(PAM_LOG_DEBUG, "The pwd for %s could not be obtained.", username);
		return PAM_IGNORE;
	}
	uid = pwd->pw_uid;
	openpam_log(PAM_LOG_DEBUG, "Going to switch to (%s) %u's %s session", username, uid, session_type);
	
	/* If we're running as root, set the root Mach bootstrap as our bootstrap port. If not, we fail. */
	if (geteuid() == 0) {
		mach_port_t rbs = get_root_bootstrap_port();
		if (rbs) {
			mach_port_mod_refs(mach_task_self(), bootstrap_port, MACH_PORT_RIGHT_SEND, -1);
			task_set_bootstrap_port(mach_task_self(), rbs);
			bootstrap_port = rbs;
		}
	} else {
		openpam_log(PAM_LOG_DEBUG, "We are not running as root.");
		return PAM_IGNORE;
	}

	/* We need to set the UID to appease launchd, then lookup the per-user bootstrap. */
	suid = getuid();
	setreuid(0, 0);
	mach_port_t puc = MACH_PORT_NULL;
	kern_return_t kr = bootstrap_look_up_per_user(bootstrap_port, NULL, uid, &puc);
	setreuid(suid, 0);
	/*
	 * NextBSD: test BOOTSTRAP_NOT_PRIVILEGED first. Apple tested it after
	 * "!= BOOTSTRAP_SUCCESS", which already catches it, so its own message
	 * could never be logged.
	 */
	if (BOOTSTRAP_NOT_PRIVILEGED == kr) {
		openpam_log(PAM_LOG_ERROR, "Permission denied to look up per-user bootstrap for UID %u.", uid);
		/* If this happens, bootstrap_port is probably already set appropriately anyway. */
		return PAM_IGNORE;
	} else if (BOOTSTRAP_SUCCESS != kr) {
		openpam_log(PAM_LOG_ERROR, "Could not look up per-user bootstrap for UID %u.", uid);
		return PAM_IGNORE;
	}
#ifdef __FreeBSD__
	/* NextBSD: a successful reply carrying no port is a failure too. */
	if (MACH_PORT_NULL == puc) {
		openpam_log(PAM_LOG_ERROR, "The per-user bootstrap for UID %u is MACH_PORT_NULL.", uid);
		return PAM_IGNORE;
	}
	/* NextBSD: only move onto a per-user launchd that answers in time. */
	if (!pam_launchd_probe(puc, uid)) {
		mach_port_deallocate(mach_task_self(), puc);
		return PAM_IGNORE;
	}
#endif

	/* Set our bootstrap port to be that of the Background session of the per-user launchd. */
#ifdef __FreeBSD__
	/*
	 * NextBSD: keep the send right on the root bootstrap rather than
	 * dropping it, so a failure below can put this process back on it.
	 */
	root_bs = bootstrap_port;
#else
	mach_port_mod_refs(mach_task_self(), bootstrap_port, MACH_PORT_RIGHT_SEND, -1);
#endif
	task_set_bootstrap_port(mach_task_self(), puc);
	bootstrap_port = puc;
	
	/* Now move ourselves into the appropriate session. */
	if (strncmp(session_type, VPROCMGR_SESSION_BACKGROUND, sizeof(VPROCMGR_SESSION_BACKGROUND)) != 0) {
		vproc_err_t verr = NULL;
		if (NULL != (verr = _vprocmgr_switch_to_session(session_type, 0))) {
			openpam_log(PAM_LOG_ERROR, "Unable to switch to %u's %s session (0x%p).", uid, session_type, verr);
#ifdef __FreeBSD__
			/*
			 * NextBSD: never block a login. Go back to the root
			 * bootstrap and let the session continue without the
			 * per-user launchd.
			 */
			pam_launchd_restore_root_bootstrap(root_bs);
			return PAM_IGNORE;
#else
			return PAM_SESSION_ERR;
#endif
		}
	}
	if (NULL != _vproc_post_fork_ping()) {
		openpam_log(PAM_LOG_ERROR, "Calling _vproc_post_fork_ping failed.");
#ifdef __FreeBSD__
		/* NextBSD: as above, fall back rather than fail the session. */
		pam_launchd_restore_root_bootstrap(root_bs);
		return PAM_IGNORE;
#else
		return PAM_SESSION_ERR;
#endif
	}

#ifdef __FreeBSD__
	/* NextBSD: the switch held; drop the root bootstrap right kept above. */
	if (root_bs != MACH_PORT_NULL) {
		mach_port_deallocate(mach_task_self(), root_bs);
	}
#endif
	return PAM_SUCCESS;
}

PAM_EXTERN int
pam_sm_close_session(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
	return PAM_SUCCESS;
}

#ifdef __FreeBSD__
/* NextBSD: OpenPAM's module entry, as every inherited FreeBSD module has. */
PAM_MODULE_ENTRY("pam_launchd");
#endif
