/*
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * SettingsHelper — the settings apps' privileged helper
 * (nextbsd/nextbsd#628).
 *
 * launchd starts it as root, on demand, when a settings app first looks
 * up org.nextbsd.SettingsHelper (org.nextbsd.SettingsHelper.plist has
 * MachServices and no RunAtLoad). It checks in, answers settingshelper.defs
 * messages one at a time, and exits once it has been idle for IDLE_SECONDS
 * and holds no live credential, so an unlock lasts its full timeout.
 * launchd keeps the service port meanwhile and starts it again at the
 * next message.
 *
 * Messages are received with the audit trailer, which MIG hands each
 * routine as its ServerAuditToken. The request buffer, which may hold a
 * password, is wiped after every message.
 */
#include <sys/types.h>
#include <sys/stat.h>

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "SettingsHelper.h"
#include "settingshelperServer.h"	/* MIG: settingshelper_server() */

#define	RPC_BUFSZ	8192
#define	IDLE_SECONDS	30

#define	RCV_TRAILER							\
	(MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |		\
	 MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT))

static volatile sig_atomic_t got_term;

void
sh_log(const char *fmt, ...)
{
	char when[32];
	struct tm tm;
	time_t now = time(NULL);
	va_list ap;

	(void)localtime_r(&now, &tm);
	(void)strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
	(void)fprintf(stderr, "%s SettingsHelper[%d] ", when, (int)getpid());
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

time_t
sh_now(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec);
}

static void
on_term(int sig)
{
	(void)sig;
	got_term = 1;
}

int
main(void)
{
	mach_port_t sp = MACH_PORT_NULL;
	kern_return_t kr;
	time_t last;

	/* launchd starts jobs with umask 0; the preference files the
	 * operations write must not come out world-writable. */
	(void)umask(022);
	(void)signal(SIGTERM, on_term);
	(void)signal(SIGPIPE, SIG_IGN);
	if (geteuid() != 0) {
		sh_log("must run as root");
		return (1);
	}
	kr = bootstrap_check_in(bootstrap_port, SH_SERVICE_NAME, &sp);
	if (kr != KERN_SUCCESS) {
		sh_log("bootstrap_check_in(%s) failed: 0x%x", SH_SERVICE_NAME,
		    (unsigned)kr);
		return (1);
	}
	sh_log("checked in as %s", SH_SERVICE_NAME);

	last = sh_now();
	while (!got_term) {
		union {
			mach_msg_header_t	hdr;
			char			buf[RPC_BUFSZ];
		} req, rep;
		mach_msg_return_t mr;

		(void)memset(&req, 0, sizeof(req));
		mr = mach_msg(&req.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT |
		    RCV_TRAILER, 0, sizeof(req), sp, 1000, MACH_PORT_NULL);
		if (mr == MACH_RCV_TIMED_OUT) {
			if (sh_now() - last >= IDLE_SECONDS && !sh_rights_live())
				break;
			continue;
		}
		if (mr != MACH_MSG_SUCCESS) {
			sh_log("receive failed: 0x%x", (unsigned)mr);
			break;
		}
		last = sh_now();

		(void)memset(&rep, 0, sizeof(rep));
		if (!settingshelper_server(&req.hdr, &rep.hdr))
			sh_log("ignoring message id %d", req.hdr.msgh_id);
		explicit_bzero(&req, sizeof(req));
		if (rep.hdr.msgh_remote_port != MACH_PORT_NULL) {
			mr = mach_msg(&rep.hdr, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
			    rep.hdr.msgh_size, 0, MACH_PORT_NULL, 200,
			    MACH_PORT_NULL);
			if (mr != MACH_MSG_SUCCESS)
				sh_log("reply send failed: 0x%x", (unsigned)mr);
		}
	}
	sh_log("idle; exiting");
	return (0);
}
