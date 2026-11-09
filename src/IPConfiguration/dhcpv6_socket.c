/*
 * dhcpv6_socket.c — the DHCPv6 client socket (#424).
 *
 * See dhcpv6_socket.h. Apple's bootp-268.1
 * IPConfiguration.bproj/DHCPv6Socket.c, in the local thread style: its
 * run-loop read callback is a reader thread here, and each client takes
 * packets from a small queue instead of being called back.
 */
#include "dhcpv6_socket.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>

#include <net/if.h>
#include <netinet/in.h>

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "DHCPv6.h"

#define	DHCPV6_SOCK_QUEUE	8
#define	DHCPV6_SOCK_PKT_MAX	1500

struct queued_pkt {
	int	len;
	uint8_t	data[DHCPV6_SOCK_PKT_MAX];
};

struct dhcpv6_sock {
	struct dhcpv6_sock	*next;
	char			ifname[IFNAMSIZ];
	unsigned		ifindex;
	pthread_cond_t		cond;
	struct queued_pkt	queue[DHCPV6_SOCK_QUEUE];
	unsigned		head;
	unsigned		count;
};

static pthread_mutex_t		g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct dhcpv6_sock	*g_socks;
static int			g_fd = -1;
static bool			g_reader_running;

static const struct in6_addr	dhcpv6_all_servers_and_relay_agents =
    All_DHCP_Relay_Agents_and_Servers_INIT;

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[dhcpv6] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static int
open_dhcpv6_socket(void)
{
	struct sockaddr_in6 me;
	struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
	int opt = 1, sockfd;

	sockfd = socket(AF_INET6, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		xlog("socket failed: %s", strerror(errno));
		return (-1);
	}
	(void)memset(&me, 0, sizeof(me));
	me.sin6_family = AF_INET6;
	me.sin6_len = sizeof(me);
	me.sin6_port = htons(DHCPV6_CLIENT_PORT);
	if (bind(sockfd, (struct sockaddr *)&me, sizeof(me)) != 0) {
		xlog("bind failed: %s", strerror(errno));
		goto failed;
	}
	/* ask for packet info */
	if (setsockopt(sockfd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &opt,
	    sizeof(opt)) != 0) {
		xlog("setsockopt(IPV6_RECVPKTINFO) failed: %s",
		    strerror(errno));
		goto failed;
	}
	/* A 1s wake-up, so the reader notices the last client going. */
	(void)setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	return (sockfd);

failed:
	(void)close(sockfd);
	return (-1);
}

/* DHCPv6SocketDemux(): hand the packet to the handle for its interface. */
static void
demux(int if_index, const uint8_t *pkt, int pkt_len)
{
	struct dhcpv6_sock *s;

	if (pkt_len < DHCPV6_PACKET_HEADER_LENGTH)
		return;
	for (s = g_socks; s != NULL; s = s->next) {
		struct queued_pkt *q;

		if (s->ifindex != (unsigned)if_index)
			continue;
		if (s->count == DHCPV6_SOCK_QUEUE) {
			/* Full: the oldest goes. */
			s->head = (s->head + 1) % DHCPV6_SOCK_QUEUE;
			s->count--;
		}
		q = &s->queue[(s->head + s->count) % DHCPV6_SOCK_QUEUE];
		q->len = pkt_len;
		(void)memcpy(q->data, pkt, (size_t)pkt_len);
		s->count++;
		(void)pthread_cond_signal(&s->cond);
	}
}

/* DHCPv6SocketRead(), looped. */
static void *
reader_main(void *arg)
{
	uint8_t buf[DHCPV6_SOCK_PKT_MAX];
	char cmsgbuf[CMSG_SPACE(sizeof(struct in6_pktinfo))];

	(void)arg;
	for (;;) {
		struct sockaddr_in6 from;
		struct iovec iov;
		struct msghdr mhdr;
		struct cmsghdr *cm;
		struct in6_pktinfo pi;
		bool have_pi = false;
		ssize_t n;
		int fd;

		(void)pthread_mutex_lock(&g_lock);
		fd = g_fd;
		if (g_socks == NULL) {
			if (g_fd >= 0)
				(void)close(g_fd);
			g_fd = -1;
			g_reader_running = false;
			(void)pthread_mutex_unlock(&g_lock);
			return (NULL);
		}
		(void)pthread_mutex_unlock(&g_lock);

		iov.iov_base = buf;
		iov.iov_len = sizeof(buf);
		(void)memset(&mhdr, 0, sizeof(mhdr));
		mhdr.msg_name = &from;
		mhdr.msg_namelen = sizeof(from);
		mhdr.msg_iov = &iov;
		mhdr.msg_iovlen = 1;
		mhdr.msg_control = cmsgbuf;
		mhdr.msg_controllen = sizeof(cmsgbuf);

		n = recvmsg(fd, &mhdr, 0);
		if (n < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK &&
			    errno != EINTR) {
				xlog("recvmsg failed: %s", strerror(errno));
				(void)sleep(1);
			}
			continue;
		}
		for (cm = CMSG_FIRSTHDR(&mhdr); cm != NULL;
		    cm = CMSG_NXTHDR(&mhdr, cm)) {
			if (cm->cmsg_level != IPPROTO_IPV6 ||
			    cm->cmsg_type != IPV6_PKTINFO ||
			    cm->cmsg_len < CMSG_LEN(sizeof(pi)))
				continue;
			(void)memcpy(&pi, CMSG_DATA(cm), sizeof(pi));
			have_pi = true;
		}
		if (!have_pi) {
			xlog("missing IPV6_PKTINFO");
			continue;
		}
		(void)pthread_mutex_lock(&g_lock);
		demux((int)pi.ipi6_ifindex, buf, (int)n);
		(void)pthread_mutex_unlock(&g_lock);
	}
}

/* Open the socket and start the reader if need be. Caller holds g_lock. */
static void
ensure_open_locked(void)
{
	pthread_t tid;
	pthread_attr_t attr;
	int rc;

	if (g_fd >= 0)
		return;
	g_fd = open_dhcpv6_socket();
	if (g_fd < 0 || g_reader_running)
		return;
	(void)pthread_attr_init(&attr);
	(void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	rc = pthread_create(&tid, &attr, reader_main, NULL);
	(void)pthread_attr_destroy(&attr);
	if (rc != 0) {
		xlog("pthread_create failed: %s", strerror(rc));
		(void)close(g_fd);
		g_fd = -1;
		return;
	}
	g_reader_running = true;
}

struct dhcpv6_sock *
dhcpv6_socket_create(const char *ifname, unsigned ifindex)
{
	struct dhcpv6_sock *s;
	pthread_condattr_t ca;

	s = calloc(1, sizeof(*s));
	if (s == NULL)
		return (NULL);
	(void)strlcpy(s->ifname, ifname, sizeof(s->ifname));
	s->ifindex = ifindex;
	(void)pthread_condattr_init(&ca);
	(void)pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	(void)pthread_cond_init(&s->cond, &ca);
	(void)pthread_condattr_destroy(&ca);

	(void)pthread_mutex_lock(&g_lock);
	s->next = g_socks;
	g_socks = s;
	ensure_open_locked();
	(void)pthread_mutex_unlock(&g_lock);
	return (s);
}

void
dhcpv6_socket_release(struct dhcpv6_sock **sock)
{
	struct dhcpv6_sock *s = *sock, **p;

	if (s == NULL)
		return;
	*sock = NULL;
	(void)pthread_mutex_lock(&g_lock);
	for (p = &g_socks; *p != NULL; p = &(*p)->next) {
		if (*p == s) {
			*p = s->next;
			break;
		}
	}
	(void)pthread_mutex_unlock(&g_lock);
	(void)pthread_cond_destroy(&s->cond);
	free(s);
}

/* S_send_packet(). */
int
dhcpv6_socket_transmit(struct dhcpv6_sock *sock, const void *pkt, int len)
{
	char cmsgbuf[CMSG_SPACE(sizeof(struct in6_pktinfo))];
	struct sockaddr_in6 dst;
	struct iovec iov;
	struct msghdr mhdr;
	struct cmsghdr *cm;
	struct in6_pktinfo pi;
	ssize_t n;
	int fd;

	(void)pthread_mutex_lock(&g_lock);
	ensure_open_locked();
	fd = g_fd;
	(void)pthread_mutex_unlock(&g_lock);
	if (fd < 0)
		return (EADDRNOTAVAIL);

	(void)memset(&dst, 0, sizeof(dst));
	dst.sin6_family = AF_INET6;
	dst.sin6_len = sizeof(dst);
	dst.sin6_addr = dhcpv6_all_servers_and_relay_agents;
	dst.sin6_port = htons(DHCPV6_SERVER_PORT);
	/* Apple's stack takes the zone from the pktinfo; here it's required. */
	dst.sin6_scope_id = sock->ifindex;

	iov.iov_base = (void *)(uintptr_t)pkt;
	iov.iov_len = (size_t)len;
	(void)memset(&mhdr, 0, sizeof(mhdr));
	mhdr.msg_name = &dst;
	mhdr.msg_namelen = sizeof(dst);
	mhdr.msg_iov = &iov;
	mhdr.msg_iovlen = 1;
	mhdr.msg_control = cmsgbuf;
	mhdr.msg_controllen = sizeof(cmsgbuf);

	/* specify the outgoing interface */
	(void)memset(cmsgbuf, 0, sizeof(cmsgbuf));
	cm = CMSG_FIRSTHDR(&mhdr);
	cm->cmsg_level = IPPROTO_IPV6;
	cm->cmsg_type = IPV6_PKTINFO;
	cm->cmsg_len = CMSG_LEN(sizeof(pi));
	(void)memset(&pi, 0, sizeof(pi));
	pi.ipi6_ifindex = sock->ifindex;
	(void)memcpy(CMSG_DATA(cm), &pi, sizeof(pi));

	n = sendmsg(fd, &mhdr, 0);
	return (n == (ssize_t)len ? 0 : errno);
}

int
dhcpv6_socket_receive(struct dhcpv6_sock *sock, void *buf, size_t size,
    unsigned timeout_ms)
{
	struct timespec deadline;
	int len = 0;

	(void)clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += timeout_ms / 1000;
	deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_nsec -= 1000000000L;
		deadline.tv_sec++;
	}

	(void)pthread_mutex_lock(&g_lock);
	while (sock->count == 0) {
		if (pthread_cond_timedwait(&sock->cond, &g_lock, &deadline) ==
		    ETIMEDOUT)
			break;
	}
	if (sock->count > 0) {
		const struct queued_pkt *q = &sock->queue[sock->head];

		len = q->len < (int)size ? q->len : (int)size;
		(void)memcpy(buf, q->data, (size_t)len);
		sock->head = (sock->head + 1) % DHCPV6_SOCK_QUEUE;
		sock->count--;
	}
	(void)pthread_mutex_unlock(&g_lock);
	return (len);
}
