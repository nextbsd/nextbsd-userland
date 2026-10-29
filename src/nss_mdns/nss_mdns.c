/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The NextBSD Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * nss_mdns: the `mdns` source for the hosts database in nsswitch.conf(5).
 * Single-label names under `local.` (<host>.local) and reverse lookups of
 * IPv4 link-local addresses (169.254/16) are answered by mDNSResponder
 * through the dns_sd(3) client API; every other name or address returns
 * NS_NOTFOUND at once, so the next source (dns) answers it
 * (nextbsd/nextbsd-userland#461, #435).
 *
 * This is the NextBSD counterpart of Libinfo's `mdns` module
 * (lookup.subproj/mdns_module.c), which macOS's libc uses for the same
 * lookups. NextBSD's libc reaches modules through nsdispatch(3) instead of
 * Libinfo's si_mod_t table, so the module is an NSS one, but the lookup
 * follows Libinfo's:
 *
 *   - A and AAAA are asked in parallel on one shared dns_sd connection,
 *     with kDNSServiceFlagsReturnIntermediates (negative answers end a
 *     query early) and kDNSServiceFlagsTimeout (mDNSResponder ends a query
 *     nobody answers);
 *   - once one family has answered, the wait for the other is cut to
 *     twice the time taken so far plus a short extra (Libinfo's
 *     SHORT_AAAA_EXTRA, or MEDIUM_AAAA_EXTRA for an answer from cache);
 *   - a link-local IPv6 answer keeps the interface it came in on, as the
 *     sockaddr_in6 scope id for getaddrinfo(3).
 *
 * Unlike Libinfo, each lookup opens and closes its own connection, so the
 * module keeps no state and needs no locks or fork handlers.
 *
 * Method conventions follow libc's `files` source (lib/libc/net/
 * gethostbyht.c, getaddrinfo.c): a hostent goes into the caller's buffer,
 * and a buffer that is too small sets *errnop = ERANGE and returns
 * NS_RETURN so the caller retries with a larger one.
 */

#include <sys/param.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <dns_sd.h>
#include <errno.h>
#include <netdb.h>
#include <nsswitch.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* Libinfo: RES_MAXRETRANS + 5; mDNSResponder's kDNSServiceFlagsTimeout
 * normally ends an unanswered query long before this. */
#define	MDNS_TOTAL_MS		35000
/* Libinfo's SHORT_AAAA_EXTRA and MEDIUM_AAAA_EXTRA, and its "answer came
 * from cache" threshold. */
#define	MDNS_SHORT_EXTRA_MS	2000
#define	MDNS_MEDIUM_EXTRA_MS	5000
#define	MDNS_CACHE_MS		20

#define	MDNS_MAXADDRS		16

struct lookup;

struct query {
	struct lookup		*lk;
	DNSServiceRef		 sd;
	uint16_t		 type;		/* A, AAAA or PTR */
	size_t			 alen;		/* address length for A/AAAA */
	DNSServiceErrorType	 error;		/* set by an error callback */
	bool			 answered;
	int			 naddrs;
	struct {
		uint8_t		 a[16];
		uint32_t	 ifindex;
	}			 addrs[MDNS_MAXADDRS];
	char			 name[kDNSServiceMaxDomainName];
};

struct lookup {
	DNSServiceRef		 conn;
	bool			 more;		/* kDNSServiceFlagsMoreComing */
	int			 nq;
	struct query		 q[2];
};

static int64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Copy a DNS name, without its trailing dot. */
static void
copy_name(char *dst, size_t dstlen, const char *src)
{
	size_t n;

	n = strlcpy(dst, src, dstlen);
	if (n >= dstlen)
		n = dstlen - 1;
	if (n > 1 && dst[n - 1] == '.')
		dst[n - 1] = '\0';
}

/* Convert an uncompressed wire-format name (PTR rdata) to text. */
static bool
wire_to_name(const uint8_t *p, size_t len, char *out, size_t outlen)
{
	size_t i = 0, o = 0;

	while (i < len) {
		uint8_t l = p[i++];

		if (l == 0)
			break;
		if (l > 63 || i + l > len || o + l + 2 > outlen)
			return (false);
		memcpy(out + o, p + i, l);
		o += l;
		out[o++] = '.';
		i += l;
	}
	if (o == 0)
		return (false);
	out[o - 1] = '\0';
	return (true);
}

static void DNSSD_API
query_reply(DNSServiceRef sd __unused, DNSServiceFlags flags,
    uint32_t ifindex, DNSServiceErrorType error, const char *fullname,
    uint16_t rrtype, uint16_t rrclass __unused, uint16_t rdlen,
    const void *rdata, uint32_t ttl __unused, void *ctx)
{
	struct query *q = ctx;
	int i;

	/* MoreComing is per connection: its absence means nothing more is
	 * queued for any of the queries sharing it (Libinfo does the same). */
	q->lk->more = (flags & kDNSServiceFlagsMoreComing) != 0;

	/* kDNSServiceErr_NoSuchRecord (a negative answer) or
	 * kDNSServiceErr_Timeout both end this query. */
	if (error != kDNSServiceErr_NoError) {
		q->error = error;
		return;
	}
	if ((flags & kDNSServiceFlagsAdd) == 0 || rrtype != q->type)
		return;

	if (q->type == kDNSServiceType_PTR) {
		if (!q->answered &&
		    wire_to_name(rdata, rdlen, q->name, sizeof(q->name)))
			q->answered = true;
		return;
	}

	if (rdlen != q->alen || q->naddrs == MDNS_MAXADDRS)
		return;
	for (i = 0; i < q->naddrs; i++)
		if (memcmp(q->addrs[i].a, rdata, q->alen) == 0)
			return;
	if (q->naddrs == 0)
		copy_name(q->name, sizeof(q->name), fullname);
	memcpy(q->addrs[q->naddrs].a, rdata, q->alen);
	q->addrs[q->naddrs].ifindex = ifindex;
	q->naddrs++;
	q->answered = true;
}

static bool
query_done(const struct query *q)
{
	return (q->error != kDNSServiceErr_NoError ||
	    (q->answered && !q->lk->more));
}

/*
 * Ask mDNSResponder for `qname` with one query per entry in `types` and
 * wait for the answers. Returns -1 when mDNSResponder can't be reached,
 * 0 otherwise (the answers, possibly none, are in lk->q).
 */
static int
lookup_run(struct lookup *lk, const char *qname, const uint16_t *types,
    int ntypes)
{
	const DNSServiceFlags qflags = kDNSServiceFlagsShareConnection |
	    kDNSServiceFlagsReturnIntermediates | kDNSServiceFlagsTimeout;
	struct pollfd pfd;
	int64_t start, deadline, now, elapsed, extra;
	bool all, some, shortened = false;
	int i, n, rc = 0;

	memset(lk, 0, sizeof(*lk));
	if (DNSServiceCreateConnection(&lk->conn) != kDNSServiceErr_NoError)
		return (-1);

	for (i = 0; i < ntypes; i++) {
		struct query *q = &lk->q[i];

		q->lk = lk;
		q->type = types[i];
		q->alen = types[i] == kDNSServiceType_A ? 4 : 16;
		q->sd = lk->conn;
		if (DNSServiceQueryRecord(&q->sd, qflags,
		    kDNSServiceInterfaceIndexAny, qname, types[i],
		    kDNSServiceClass_IN, query_reply, q) !=
		    kDNSServiceErr_NoError) {
			rc = -1;
			goto out;
		}
		lk->nq++;
	}

	pfd.fd = DNSServiceRefSockFD(lk->conn);
	pfd.events = POLLIN;
	start = now_ms();
	deadline = start + MDNS_TOTAL_MS;
	for (;;) {
		now = now_ms();
		if (now >= deadline)
			break;
		n = poll(&pfd, 1, (int)(deadline - now));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		if (DNSServiceProcessResult(lk->conn) !=
		    kDNSServiceErr_NoError)
			break;

		all = true;
		some = false;
		for (i = 0; i < lk->nq; i++) {
			if (query_done(&lk->q[i]))
				some = true;
			else
				all = false;
		}
		if (all)
			break;
		/* One family answered: wait 2N + extra for the other. */
		if (some && !shortened) {
			shortened = true;
			now = now_ms();
			elapsed = now - start;
			extra = elapsed <= MDNS_CACHE_MS ?
			    MDNS_MEDIUM_EXTRA_MS : MDNS_SHORT_EXTRA_MS;
			if (now + 2 * elapsed + extra < deadline)
				deadline = now + 2 * elapsed + extra;
		}
	}
out:
	/* Deallocating the shared connection ends every query on it. */
	DNSServiceRefDeallocate(lk->conn);
	return (rc);
}

/*
 * `name` is <host>.local (or <host>.local.): exactly one label before
 * `local`, which is what Bonjour host names are.
 *
 * A deeper name such as mac-studio.home.local belongs to a unicast DNS zone
 * that happens to be called local -- routers commonly hand out home.local or
 * lan.local -- and mDNS never answers it. Asking anyway made every lookup of
 * such a name wait out the mDNS timeout before dns was tried: 10 s per
 * forward lookup, which sshd's UseDNS check paid twice on every login from a
 * machine with one of those names. On macOS mDNSResponder serves both
 * multicast and unicast DNS, so the same name is answered there by the
 * unicast server at once; here unicast is the `dns` source after this one,
 * so the name is passed straight to it.
 */
static bool
is_local_name(const char *name)
{
	size_t len;

	if (name == NULL)
		return (false);
	len = strlen(name);
	if (len > 0 && name[len - 1] == '.')
		len--;
	if (len <= 6 || name[len - 6] != '.' ||
	    strncasecmp(name + len - 5, "local", 5) != 0)
		return (false);
	return (memchr(name, '.', len - 6) == NULL);
}

/*
 * Lay a hostent out in the caller's buffer: the alias and address pointer
 * arrays (aligned), the addresses, then the name. Returns ERANGE if it
 * does not fit.
 */
static int
fill_hostent(struct hostent *hp, char *buf, size_t buflen, const char *name,
    int af, size_t alen, const uint8_t *const *addrs, int naddrs)
{
	size_t pad, need;
	char **aliases, **list, *p;
	int i;

	pad = (sizeof(char *) - ((uintptr_t)buf % sizeof(char *))) %
	    sizeof(char *);
	need = pad + (size_t)(naddrs + 2) * sizeof(char *) +
	    (size_t)naddrs * alen + strlen(name) + 1;
	if (need > buflen)
		return (ERANGE);

	p = buf + pad;
	aliases = (char **)(void *)p;
	aliases[0] = NULL;
	list = aliases + 1;
	p = (char *)(list + naddrs + 1);
	for (i = 0; i < naddrs; i++) {
		memcpy(p, addrs[i], alen);
		list[i] = p;
		p += alen;
	}
	list[naddrs] = NULL;
	strcpy(p, name);

	hp->h_name = p;
	hp->h_aliases = aliases;
	hp->h_addrtype = af;
	hp->h_length = (int)alen;
	hp->h_addr_list = list;
	return (0);
}

/* hosts "gethostbyname2_r": name, af, hp, buf, buflen, errnop, h_errnop */
static int
mdns_gethostbyname2_r(void *rv, void *mdata __unused, va_list ap)
{
	const char *name;
	int af, *errnop, *h_errnop, i, error;
	struct hostent *hp;
	char *buf;
	size_t buflen;
	uint16_t type;
	struct lookup lk;
	struct query *q;
	const uint8_t *addrs[MDNS_MAXADDRS];

	name = va_arg(ap, const char *);
	af = va_arg(ap, int);
	hp = va_arg(ap, struct hostent *);
	buf = va_arg(ap, char *);
	buflen = va_arg(ap, size_t);
	errnop = va_arg(ap, int *);
	h_errnop = va_arg(ap, int *);

	*(struct hostent **)rv = NULL;
	if (!is_local_name(name) || (af != AF_INET && af != AF_INET6)) {
		*h_errnop = HOST_NOT_FOUND;
		return (NS_NOTFOUND);
	}

	type = af == AF_INET ? kDNSServiceType_A : kDNSServiceType_AAAA;
	if (lookup_run(&lk, name, &type, 1) != 0) {
		*h_errnop = NO_RECOVERY;
		return (NS_UNAVAIL);
	}
	q = &lk.q[0];
	if (q->naddrs == 0) {
		*h_errnop = HOST_NOT_FOUND;
		return (NS_NOTFOUND);
	}

	for (i = 0; i < q->naddrs; i++)
		addrs[i] = q->addrs[i].a;
	error = fill_hostent(hp, buf, buflen, q->name, af, q->alen, addrs,
	    q->naddrs);
	if (error != 0) {
		*errnop = error;
		*h_errnop = NETDB_INTERNAL;
		return (NS_RETURN);
	}
	*(struct hostent **)rv = hp;
	return (NS_SUCCESS);
}

/*
 * hosts "gethostbyaddr_r": addr, len, af, hp, buf, buflen, errnop, h_errnop
 *
 * Only IPv4 link-local addresses are asked over mDNS (254.169.in-addr.arpa,
 * the zone mDNSResponder answers for its own link-local address). libc
 * rejects IPv6 link-local addresses before any source is asked.
 */
static int
mdns_gethostbyaddr_r(void *rv, void *mdata __unused, va_list ap)
{
	const uint8_t *addr;
	socklen_t len;
	int af, *errnop, *h_errnop, error;
	struct hostent *hp;
	char *buf, qname[64];
	size_t buflen;
	uint16_t type = kDNSServiceType_PTR;
	struct lookup lk;

	addr = va_arg(ap, const void *);
	len = va_arg(ap, socklen_t);
	af = va_arg(ap, int);
	hp = va_arg(ap, struct hostent *);
	buf = va_arg(ap, char *);
	buflen = va_arg(ap, size_t);
	errnop = va_arg(ap, int *);
	h_errnop = va_arg(ap, int *);

	*(struct hostent **)rv = NULL;
	if (af != AF_INET || len != 4 || addr[0] != 169 || addr[1] != 254) {
		*h_errnop = HOST_NOT_FOUND;
		return (NS_NOTFOUND);
	}

	snprintf(qname, sizeof(qname), "%u.%u.%u.%u.in-addr.arpa.",
	    addr[3], addr[2], addr[1], addr[0]);
	if (lookup_run(&lk, qname, &type, 1) != 0) {
		*h_errnop = NO_RECOVERY;
		return (NS_UNAVAIL);
	}
	if (!lk.q[0].answered) {
		*h_errnop = HOST_NOT_FOUND;
		return (NS_NOTFOUND);
	}

	error = fill_hostent(hp, buf, buflen, lk.q[0].name, af, len, &addr, 1);
	if (error != 0) {
		*errnop = error;
		*h_errnop = NETDB_INTERNAL;
		return (NS_RETURN);
	}
	*(struct hostent **)rv = hp;
	return (NS_SUCCESS);
}

/* hosts "getaddrinfo": name, pai; libc fills in the ports afterwards. */
static int
mdns_getaddrinfo(void *rv, void *mdata __unused, va_list ap)
{
	const char *name;
	const struct addrinfo *pai;
	struct addrinfo *head = NULL, **tail = &head, *ai;
	uint16_t types[2];
	struct lookup lk;
	int i, j, ntypes = 0;

	name = va_arg(ap, const char *);
	pai = va_arg(ap, const struct addrinfo *);

	*(struct addrinfo **)rv = NULL;
	if (!is_local_name(name))
		return (NS_NOTFOUND);
	switch (pai->ai_family) {
	case AF_UNSPEC:
		types[ntypes++] = kDNSServiceType_AAAA;
		types[ntypes++] = kDNSServiceType_A;
		break;
	case AF_INET6:
		types[ntypes++] = kDNSServiceType_AAAA;
		break;
	case AF_INET:
		types[ntypes++] = kDNSServiceType_A;
		break;
	default:
		return (NS_NOTFOUND);
	}

	if (lookup_run(&lk, name, types, ntypes) != 0)
		return (NS_UNAVAIL);

	for (i = 0; i < lk.nq; i++) {
		const struct query *q = &lk.q[i];

		for (j = 0; j < q->naddrs; j++) {
			size_t salen = q->type == kDNSServiceType_A ?
			    sizeof(struct sockaddr_in) :
			    sizeof(struct sockaddr_in6);

			ai = calloc(1, sizeof(*ai) + salen);
			if (ai == NULL)
				goto nomem;
			ai->ai_flags = pai->ai_flags;
			ai->ai_socktype = pai->ai_socktype;
			ai->ai_protocol = pai->ai_protocol;
			ai->ai_addrlen = (socklen_t)salen;
			ai->ai_addr = (struct sockaddr *)(void *)(ai + 1);
			if (q->type == kDNSServiceType_A) {
				struct sockaddr_in *sin =
				    (struct sockaddr_in *)(void *)ai->ai_addr;

				ai->ai_family = AF_INET;
				sin->sin_len = sizeof(*sin);
				sin->sin_family = AF_INET;
				memcpy(&sin->sin_addr, q->addrs[j].a, 4);
			} else {
				struct sockaddr_in6 *sin6 =
				    (struct sockaddr_in6 *)(void *)ai->ai_addr;

				ai->ai_family = AF_INET6;
				sin6->sin6_len = sizeof(*sin6);
				sin6->sin6_family = AF_INET6;
				memcpy(&sin6->sin6_addr, q->addrs[j].a, 16);
				if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr))
					sin6->sin6_scope_id =
					    q->addrs[j].ifindex;
			}
			if (head == NULL &&
			    (pai->ai_flags & AI_CANONNAME) != 0 &&
			    (ai->ai_canonname = strdup(q->name)) == NULL) {
				free(ai);
				goto nomem;
			}
			*tail = ai;
			tail = &ai->ai_next;
		}
	}

	if (head == NULL)
		return (NS_NOTFOUND);
	*(struct addrinfo **)rv = head;
	return (NS_SUCCESS);

nomem:
	freeaddrinfo(head);
	return (NS_UNAVAIL);
}

static ns_mtab methods[] = {
	{ NSDB_HOSTS, "gethostbyname2_r", mdns_gethostbyname2_r, NULL },
	{ NSDB_HOSTS, "gethostbyaddr_r", mdns_gethostbyaddr_r, NULL },
	{ NSDB_HOSTS, "getaddrinfo", mdns_getaddrinfo, NULL },
};

ns_mtab	*nss_module_register(const char *, unsigned int *,
	    nss_module_unregister_fn *);

ns_mtab *
nss_module_register(const char *source __unused, unsigned int *mtabsize,
    nss_module_unregister_fn *unregister)
{
	*mtabsize = nitems(methods);
	*unregister = NULL;
	return (methods);
}
