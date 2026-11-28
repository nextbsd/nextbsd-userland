/*
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * sshd-mdns-register: Remote Login's Bonjour advertisements, for exactly
 * as long as sshd runs. /usr/libexec/sshd-keygen-wrapper starts it in the
 * background and then execs sshd, so sshd becomes its parent; it exits
 * when sshd does, and the advertisements go with it. No daemon of its
 * own, no launchd job: it lives and dies with Remote Login.
 *
 *   _ssh._tcp            port 22
 *   _nextbsd-admin._tcp  port 22, TXT model= version= protocol=
 *                        (how Server.app finds NextBSD computers, E42,
 *                        nextbsd/nextbsd-userland#693)
 *
 * Registering any service also makes mDNSResponder publish the host's
 * <name>.local address record.
 */

#include <sys/types.h>
#include <sys/event.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <arpa/inet.h>
#include <dns_sd.h>
#include <kenv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#define	SYSTEM_VERSION	"/System/Library/CoreServices/SystemVersion.plist"
/* serveradmin(8)'s protocol (src/serveradmin/serveradmin.h). */
#define	SA_PROTOCOL	"1"

static void
plist_string(const char *key, char *val, size_t len)
{
	char want[128], line[512], *p, *q;
	FILE *f;
	int next = 0;

	val[0] = '\0';
	if ((f = fopen(SYSTEM_VERSION, "r")) == NULL)
		return;
	snprintf(want, sizeof(want), "<key>%s</key>", key);
	while (fgets(line, sizeof(line), f) != NULL) {
		if (next && (p = strstr(line, "<string>")) != NULL) {
			p += 8;
			if ((q = strstr(p, "</string>")) != NULL)
				*q = '\0';
			strlcpy(val, p, len);
			break;
		}
		next = strstr(line, want) != NULL;
	}
	fclose(f);
}

static void
txt_add(TXTRecordRef *t, const char *k, const char *v)
{
	size_t n = strlen(v);

	if (n > 200)
		n = 200;
	(void)TXTRecordSetValue(t, k, (uint8_t)n, v);
}

static void
registered(DNSServiceRef ref, DNSServiceFlags flags, DNSServiceErrorType err,
    const char *name, const char *type, const char *domain, void *ctx)
{
	(void)ref; (void)flags; (void)domain; (void)ctx;
	if (err != kDNSServiceErr_NoError)
		syslog(LOG_WARNING, "%s %s: registration failed (%d)", name, type, err);
}

int
main(void)
{
	DNSServiceRef ssh = NULL, admin = NULL;
	TXTRecordRef txt;
	char model[256] = "", version[64], buf[64];
	size_t n = sizeof(model);
	pid_t parent = getppid();
	struct kevent ev;
	int kq;

	openlog("sshd-mdns-register", LOG_PID, LOG_DAEMON);
	if (kenv(KENV_GET, "smbios.system.product", model, sizeof(model)) <= 0 ||
	    strcmp(model, "None") == 0) {
		n = sizeof(model);
		(void)sysctlbyname("hw.model", model, &n, NULL, 0);
	}
	plist_string("ProductVersion", version, sizeof(version));
	plist_string("ProductBuildVersion", buf, sizeof(buf));
	if (buf[0] != '\0') {
		strlcat(version, "-", sizeof(version));
		strlcat(version, buf, sizeof(version));
	}

	/* mDNSResponder may still be starting at boot: retry a while. */
	for (int i = 0; i < 30; i++) {
		if (ssh == NULL && DNSServiceRegister(&ssh, 0, 0, NULL, "_ssh._tcp",
		    NULL, NULL, htons(22), 0, NULL, registered, NULL) !=
		    kDNSServiceErr_NoError)
			ssh = NULL;
		if (admin == NULL) {
			TXTRecordCreate(&txt, 0, NULL);
			txt_add(&txt, "model", model);
			txt_add(&txt, "version", version);
			txt_add(&txt, "protocol", SA_PROTOCOL);
			if (DNSServiceRegister(&admin, 0, 0, NULL,
			    "_nextbsd-admin._tcp", NULL, NULL, htons(22),
			    TXTRecordGetLength(&txt), TXTRecordGetBytesPtr(&txt),
			    registered, NULL) != kDNSServiceErr_NoError)
				admin = NULL;
			TXTRecordDeallocate(&txt);
		}
		if (ssh != NULL && admin != NULL)
			break;
		sleep(2);
	}

	/* Until sshd (our parent, after the wrapper's exec) exits. */
	if ((kq = kqueue()) == -1)
		return (1);
	EV_SET(&ev, parent, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, NULL);
	if (kevent(kq, &ev, 1, NULL, 0, NULL) == -1)
		return (0);	/* already gone */
	for (;;) {
		struct kevent out[2];
		int fds[2] = { ssh ? DNSServiceRefSockFD(ssh) : -1,
		    admin ? DNSServiceRefSockFD(admin) : -1 };
		struct timespec tick = { 5, 0 };
		int got;

		for (int i = 0; i < 2; i++)
			if (fds[i] != -1) {
				EV_SET(&ev, fds[i], EVFILT_READ, EV_ADD, 0, 0, NULL);
				(void)kevent(kq, &ev, 1, NULL, 0, NULL);
			}
		got = kevent(kq, NULL, 0, out, 2, &tick);
		for (int i = 0; i < got; i++) {
			if (out[i].filter == EVFILT_PROC)
				goto done;
			if (ssh != NULL && (int)out[i].ident == fds[0])
				(void)DNSServiceProcessResult(ssh);
			if (admin != NULL && (int)out[i].ident == fds[1])
				(void)DNSServiceProcessResult(admin);
		}
		if (getppid() != parent)
			break;
	}
done:
	if (ssh != NULL)
		DNSServiceRefDeallocate(ssh);
	if (admin != NULL)
		DNSServiceRefDeallocate(admin);
	return (0);
}
