/*
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Bonjour lookups through mDNSResponder (dns_sd), each bounded by a
 * short timeout: is a name taken on the network, what address does a
 * .local name have, and which computers advertise a service.
 */

#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <dns_sd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "serveradmin.h"

#define	LOOKUP_MS	1500

/* Process a ref's results until `*done` or the timeout. */
static void
pump(DNSServiceRef ref, int ms, bool *done)
{
	struct timespec start, now;
	int fd = DNSServiceRefSockFD(ref);

	clock_gettime(CLOCK_MONOTONIC, &start);
	while (!*done) {
		fd_set r;
		struct timeval tv;
		long left;

		clock_gettime(CLOCK_MONOTONIC, &now);
		left = ms - ((now.tv_sec - start.tv_sec) * 1000 +
		    (now.tv_nsec - start.tv_nsec) / 1000000);
		if (left <= 0)
			break;
		FD_ZERO(&r);
		FD_SET(fd, &r);
		tv.tv_sec = left / 1000;
		tv.tv_usec = (left % 1000) * 1000;
		if (select(fd + 1, &r, NULL, NULL, &tv) <= 0)
			break;
		if (DNSServiceProcessResult(ref) != kDNSServiceErr_NoError)
			break;
	}
}

struct addr_ctx {
	char	*addr;
	size_t	 len;
	bool	 done;
};

static void
addr_cb(DNSServiceRef ref, DNSServiceFlags flags, uint32_t ifi,
    DNSServiceErrorType err, const char *host, const struct sockaddr *sa,
    uint32_t ttl, void *ctx)
{
	struct addr_ctx *c = ctx;

	(void)ref; (void)flags; (void)ifi; (void)host; (void)ttl;
	if (err != kDNSServiceErr_NoError || sa == NULL ||
	    sa->sa_family != AF_INET)
		return;
	inet_ntop(AF_INET, &((const struct sockaddr_in *)sa)->sin_addr,
	    c->addr, (socklen_t)c->len);
	c->done = true;
}

bool
sa_mdns_resolve(const char *label, char *addr, size_t len)
{
	DNSServiceRef ref;
	struct addr_ctx c = { addr, len, false };
	char name[128];

	addr[0] = '\0';
	if (!sa_is_label(label))
		return (false);
	snprintf(name, sizeof(name), "%s.local.", label);
	if (DNSServiceGetAddrInfo(&ref, kDNSServiceFlagsTimeout, 0,
	    kDNSServiceProtocol_IPv4, name, addr_cb, &c) !=
	    kDNSServiceErr_NoError)
		return (false);
	pump(ref, LOOKUP_MS, &c.done);
	DNSServiceRefDeallocate(ref);
	return (c.done);
}

bool
sa_mdns_taken(const char *label, char *addr, size_t len)
{
	return (sa_mdns_resolve(label, addr, len));
}

/* ---- browsing ------------------------------------------------------- */
struct browse_ctx {
	char	names[64][64];
	int	n;
	bool	done;
};

static void
browse_cb(DNSServiceRef ref, DNSServiceFlags flags, uint32_t ifi,
    DNSServiceErrorType err, const char *name, const char *type,
    const char *domain, void *ctx)
{
	struct browse_ctx *c = ctx;

	(void)ref; (void)ifi; (void)type; (void)domain;
	if (err != kDNSServiceErr_NoError || !(flags & kDNSServiceFlagsAdd) ||
	    c->n >= 64)
		return;
	for (int i = 0; i < c->n; i++)
		if (strcmp(c->names[i], name) == 0)
			return;
	strlcpy(c->names[c->n++], name, sizeof(c->names[0]));
}

struct resolve_ctx {
	char	host[256];
	bool	done;
};

static void
resolve_cb(DNSServiceRef ref, DNSServiceFlags flags, uint32_t ifi,
    DNSServiceErrorType err, const char *full, const char *host,
    uint16_t port, uint16_t txtlen, const unsigned char *txt, void *ctx)
{
	struct resolve_ctx *c = ctx;

	(void)ref; (void)flags; (void)ifi; (void)full; (void)port;
	(void)txtlen; (void)txt;
	if (err != kDNSServiceErr_NoError)
		return;
	strlcpy(c->host, host, sizeof(c->host));
	c->done = true;
}

/* Emit [{"name":…, "host":…}] for every instance of `type` found. */
void	sa_mdns_browse_json(const char *type, const char *skip_host);
void
sa_mdns_browse_json(const char *type, const char *skip_host)
{
	DNSServiceRef ref;
	struct browse_ctx *c = calloc(1, sizeof(*c));

	j_arr();
	if (c == NULL || DNSServiceBrowse(&ref, 0, 0, type, "local.",
	    browse_cb, c) != kDNSServiceErr_NoError) {
		j_aend();
		free(c);
		return;
	}
	pump(ref, LOOKUP_MS, &c->done);
	DNSServiceRefDeallocate(ref);
	for (int i = 0; i < c->n; i++) {
		DNSServiceRef rr;
		struct resolve_ctx r = { "", false };
		size_t hl;

		if (DNSServiceResolve(&rr, 0, 0, c->names[i], type, "local.",
		    resolve_cb, &r) != kDNSServiceErr_NoError)
			continue;
		pump(rr, LOOKUP_MS, &r.done);
		DNSServiceRefDeallocate(rr);
		if (!r.done)
			continue;
		hl = strlen(r.host);
		if (hl > 0 && r.host[hl - 1] == '.')
			r.host[hl - 1] = '\0';
		if (skip_host != NULL && strcasecmp(r.host, skip_host) == 0)
			continue;
		j_obj();
		j_kstr("name", c->names[i]);
		j_kstr("host", r.host);
		j_end();
	}
	j_aend();
	free(c);
}
