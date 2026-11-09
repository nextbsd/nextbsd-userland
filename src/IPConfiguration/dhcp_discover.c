/*
 * dhcp_discover.c — DHCPv4 client front door (iter 3).
 *
 * iter 2 shipped a one-shot DISCOVER/OFFER probe. iter 3 grows it
 * into the full RFC 2131 INIT → SELECTING → REQUESTING → BOUND
 * exchange: brings the iface up, opens BPF, sends DHCPDISCOVER,
 * receives DHCPOFFER, sends DHCPREQUEST referencing the offer's
 * server-id + yiaddr, receives DHCPACK, parses lease options
 * (subnet, router, lease time, DNS) into struct dhcp_lease.
 *
 * Both transmits use Darwin's ladder (1, 2, 4, then 8 seconds, #426).
 * The first DISCOVER on QEMU SLIRP is always answered in well under
 * 100ms; the ladder exists for a lossy or server-less network.
 *
 * BPF reads are signal-aware: SIGTERM/SIGHUP between reads or
 * during a blocking BIOCSRTIMEOUT wait short-circuits the loop
 * (the daemon's global `got_term` flag is checked between every
 * read and on EINTR). Fixes iter 2's 10-second signal deafness.
 *
 * NOT vendored from Apple's IPConfiguration.bproj. The option parser
 * here is its own small one for the options a lease carries; Apple's
 * bootplib/dhcp_options.c (vendored/) serves ipconfig getoption and
 * getpacket from the raw packet saved below (#430).
 */
#include "dhcp_discover.h"
#include "arp_probe.h"
#include "dhcp_packet.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#include <arpa/inet.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DHCP_CLIENT_PORT	68
#define DHCP_SERVER_PORT	67

/*
 * build_dhcp_msg()'s msgtype for a BOOTREQUEST (no message type at all), and
 * what parse_dhcp_reply() takes as want_msgtype for the two exchanges that
 * accept more than one kind of reply (#569).
 */
#define	DHCP_MTYPE_NONE		0
#define	DHCP_WANT_BOOTP		DHCP_MTYPE_NONE
#define	DHCP_WANT_INFORM	0xfe

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[dhcp] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/* RFC 1071 one's-complement Internet checksum. */
static uint16_t
in_cksum(const void *buf, size_t len)
{
	const uint8_t *p = buf;
	uint32_t sum = 0;

	while (len > 1) {
		sum += (uint32_t)((p[0] << 8) | p[1]);
		p += 2;
		len -= 2;
	}
	if (len == 1)
		sum += (uint32_t)(p[0] << 8);
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ((uint16_t)~sum);
}

int
dhcp_pick_interface(char *ifname_out, size_t ifname_sz)
{
	struct ifaddrs *ifa, *p;
	int found = 0;

	if (getifaddrs(&ifa) != 0)
		return (-1);
	for (p = ifa; p != NULL; p = p->ifa_next) {
		const struct sockaddr_dl *dl;

		if (p->ifa_addr == NULL ||
		    p->ifa_addr->sa_family != AF_LINK)
			continue;
		if ((p->ifa_flags & IFF_LOOPBACK) != 0)
			continue;
		dl = (const struct sockaddr_dl *)(void *)p->ifa_addr;
		if (dl->sdl_type != 6)	/* IFT_ETHER */
			continue;
		(void)strlcpy(ifname_out, p->ifa_name, ifname_sz);
		found = 1;
		break;
	}
	freeifaddrs(ifa);
	return (found ? 0 : -1);
}

/*
 * One getifaddrs pass for ifname's AF_LINK address. Returns 0 with mac
 * filled, else -1 with errno saying why: the getifaddrs errno, ENXIO for
 * no AF_LINK record, EADDRNOTAVAIL for a record whose sdl_alen is not 6
 * (*alen_out holds it).
 */
static int
read_interface_mac(const char *ifname, uint8_t mac[6], int *alen_out)
{
	struct ifaddrs *ifa, *p;
	int ok = -1, err = ENXIO;

	if (getifaddrs(&ifa) != 0)
		return (-1);
	for (p = ifa; p != NULL; p = p->ifa_next) {
		const struct sockaddr_dl *dl;

		if (p->ifa_addr == NULL ||
		    p->ifa_addr->sa_family != AF_LINK)
			continue;
		if (strcmp(p->ifa_name, ifname) != 0)
			continue;
		dl = (const struct sockaddr_dl *)(void *)p->ifa_addr;
		*alen_out = dl->sdl_alen;
		if (dl->sdl_alen != 6) {
			err = EADDRNOTAVAIL;
			break;
		}
		(void)memcpy(mac, LLADDR(dl), 6);
		ok = 0;
		break;
	}
	freeifaddrs(ifa);
	if (ok != 0)
		errno = err;
	return (ok);
}

/*
 * The interface's Ethernet address, retried for up to ~2 s (#408). The
 * kernel's if_attach() makes the ifnet visible, and posts
 * KEV_DL_IF_ATTACHED, before ether_ifattach() fills in the lladdr, so a
 * worker started on configd's Link{Active} can look in that window. The
 * reason is logged when it first fails and if it never clears; errno is
 * left as read_interface_mac() set it for the caller's own log line.
 */
#define	MAC_RETRY_MS		100
#define	MAC_RETRY_TRIES		20

static int
get_interface_mac(const char *ifname, uint8_t mac[6])
{
	struct timespec ts = { 0, MAC_RETRY_MS * 1000000L };
	const char *more;
	int alen = 0, err = 0, i;

	for (i = 0; i < MAC_RETRY_TRIES; i++) {
		if (read_interface_mac(ifname, mac, &alen) == 0) {
			if (i > 0)
				xlog("get_interface_mac(%s): lladdr visible "
				    "after %d ms", ifname, i * MAC_RETRY_MS);
			return (0);
		}
		err = errno;
		more = (i == 0 && !got_term) ? ", retrying" : "";
		if (i == 0 || i == MAC_RETRY_TRIES - 1 || got_term) {
			if (err == ENXIO)
				xlog("get_interface_mac(%s): no AF_LINK "
				    "record%s", ifname, more);
			else if (err == EADDRNOTAVAIL)
				xlog("get_interface_mac(%s): sdl_alen %d "
				    "!= 6%s", ifname, alen, more);
			else
				xlog("get_interface_mac(%s): getifaddrs: "
				    "%s%s", ifname, strerror(err), more);
		}
		if (got_term)
			break;
		if (i < MAC_RETRY_TRIES - 1)
			(void)nanosleep(&ts, NULL);
	}
	errno = err;
	return (-1);
}

static int
bring_interface_up(const char *ifname)
{
	struct ifreq ifr;
	int sock, rc = -1, i;
	unsigned before, after = 0;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		xlog("bring_interface_up(%s): socket: %s", ifname,
		    strerror(errno));
		return (-1);
	}
	(void)memset(&ifr, 0, sizeof(ifr));
	(void)strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(sock, SIOCGIFFLAGS, &ifr) != 0) {
		xlog("SIOCGIFFLAGS(%s) failed: %s", ifname, strerror(errno));
		(void)close(sock);
		return (-1);
	}
	before = (unsigned)(uint16_t)ifr.ifr_flags;
	if ((ifr.ifr_flags & IFF_UP) == 0) {
		ifr.ifr_flags |= IFF_UP;
		rc = ioctl(sock, SIOCSIFFLAGS, &ifr);
		if (rc != 0) {
			xlog("SIOCSIFFLAGS(%s, +IFF_UP) failed: %s",
			    ifname, strerror(errno));
			(void)close(sock);
			return (rc);
		}
	} else {
		rc = 0;
	}
	for (i = 0; i < 30; i++) {
		(void)memset(&ifr, 0, sizeof(ifr));
		(void)strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
		if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
			after = (unsigned)(uint16_t)ifr.ifr_flags;
			if ((after & IFF_UP) != 0)
				break;
		}
		(void)usleep(100000);
	}
	xlog("bring_interface_up(%s) flags=0x%x -> 0x%x (waited %d/30)",
	    ifname, before, after, i);
	(void)close(sock);
	return (rc);
}

static int
bpf_open(const char *ifname)
{
	struct ifreq ifr;
	int fd, i;
	u_int one = 1;
	struct timeval tv;

	fd = open("/dev/bpf", O_RDWR);
	if (fd < 0 && (errno == ENOENT || errno == ENXIO)) {
		for (i = 0; i < 256; i++) {
			char path[32];

			(void)snprintf(path, sizeof(path), "/dev/bpf%d", i);
			fd = open(path, O_RDWR);
			if (fd >= 0)
				break;
			if (errno != EBUSY)
				break;
		}
	}
	if (fd < 0)
		return (-1);

	(void)memset(&ifr, 0, sizeof(ifr));
	(void)strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(fd, BIOCSETIF, &ifr) != 0) {
		(void)close(fd);
		return (-1);
	}
	(void)ioctl(fd, BIOCIMMEDIATE, &one);
	(void)ioctl(fd, BIOCSHDRCMPLT, &one);
	/* BPF read timeout: 1s. Each timeout returns to the caller so
	 * the wait loop can rotate through got_term + retransmit
	 * deadline checks. */
	tv.tv_sec = 1;
	tv.tv_usec = 0;
	(void)ioctl(fd, BIOCSRTIMEOUT, &tv);
	return (fd);
}

/*
 * Build a DHCPDISCOVER or DHCPREQUEST frame into `buf` (≥ 1024
 * bytes). For DISCOVER, requested_ip / server_id / ciaddr are all
 * NULL. For a SELECTING-state REQUEST, requested_ip + server_id
 * come from the OFFER we just received and ciaddr is NULL. For a
 * RENEWING/REBINDING REQUEST (iter 4), requested_ip + server_id are
 * NULL but ciaddr is set to the bound address — RFC 2131 §4.3.2
 * encoding for the post-BOUND renewal request. Returns the total
 * Ethernet frame length.
 */
static size_t
build_dhcp_msg(uint8_t *buf, const uint8_t mac[6], uint32_t xid,
    uint8_t msgtype, const struct in_addr *requested_ip,
    const struct in_addr *server_id, const struct in_addr *ciaddr)
{
	struct ether_header *eh;
	struct ip *ip;
	struct udphdr *udp;
	struct dhcp *dh;
	const uint8_t cookie[4] = DHCP_MAGIC_COOKIE;
	uint8_t *opt;
	size_t udp_len, ip_len, frame_len;
	uint8_t pseudo[12];

	(void)memset(buf, 0, 1024);

	eh = (struct ether_header *)(void *)buf;
	(void)memset(eh->ether_dhost, 0xff, 6);
	(void)memcpy(eh->ether_shost, mac, 6);
	eh->ether_type = htons(ETHERTYPE_IP);

	ip = (struct ip *)(void *)(buf + sizeof(*eh));
	ip->ip_v = 4;
	ip->ip_hl = 5;
	ip->ip_ttl = 64;
	ip->ip_p = IPPROTO_UDP;
	ip->ip_src.s_addr = htonl(INADDR_ANY);
	ip->ip_dst.s_addr = htonl(INADDR_BROADCAST);

	udp = (struct udphdr *)(void *)(buf + sizeof(*eh) + sizeof(*ip));
	udp->uh_sport = htons(DHCP_CLIENT_PORT);
	udp->uh_dport = htons(DHCP_SERVER_PORT);
	udp->uh_sum = 0;

	dh = (struct dhcp *)(void *)(buf + sizeof(*eh) + sizeof(*ip) +
	    sizeof(*udp));
	dh->dp_op = BOOTREQUEST;
	dh->dp_htype = HTYPE_ETHERNET;
	dh->dp_hlen = 6;
	dh->dp_xid = htonl(xid);
	/* make_dhcp_request(..., must_broadcast FALSE) for INFORM and BOOTP. */
	if (msgtype != DHCP_MTYPE_NONE && msgtype != DHCP_MTYPE_INFORM)
		dh->dp_flags = htons(DHCP_FLAG_BROADCAST);
	if (ciaddr != NULL)
		dh->dp_ciaddr = *ciaddr;
	(void)memcpy(dh->dp_chaddr, mac, 6);

	opt = (uint8_t *)dh + sizeof(*dh);
	(void)memcpy(opt, cookie, 4);
	opt += 4;

	/* make_bootp_request(): the cookie and nothing else. */
	if (msgtype == DHCP_MTYPE_NONE) {
		*opt++ = DHCP_OPT_END;
		goto pad;
	}

	/* 53 message type */
	*opt++ = DHCP_OPT_MESSAGE_TYPE;
	*opt++ = 1;
	*opt++ = msgtype;

	/* 61 client id = htype + mac */
	*opt++ = DHCP_OPT_CLIENT_ID;
	*opt++ = 7;
	*opt++ = HTYPE_ETHERNET;
	(void)memcpy(opt, mac, 6);
	opt += 6;

	/* REQUEST + DECLINE: option 50 (requested ip) + 54 (server id).
	 * RFC 2131 §4.3.2 requires these on the SELECTING-state REQUEST
	 * so the server can disambiguate which offer the client took.
	 * RFC 2131 §3.1.5 requires them on DHCPDECLINE so the server
	 * knows which offer the client is rejecting (so it can mark the
	 * address as in-use and not re-offer it). */
	if (msgtype == DHCP_MTYPE_REQUEST || msgtype == DHCP_MTYPE_DECLINE) {
		if (requested_ip != NULL) {
			*opt++ = DHCP_OPT_REQUESTED_IP;
			*opt++ = 4;
			(void)memcpy(opt, &requested_ip->s_addr, 4);
			opt += 4;
		}
		if (server_id != NULL) {
			*opt++ = DHCP_OPT_SERVER_ID;
			*opt++ = 4;
			(void)memcpy(opt, &server_id->s_addr, 4);
			opt += 4;
		}
	}

	/* 12 host name — advertise our hostname so the DHCP server (the
	 * home router, typically) registers it in LAN DNS. Without this the
	 * router has no name to map, so neither the bare hostname nor
	 * <hostname>.<router-domain> (e.g. .home.local) resolves. RFC 2132
	 * §3.14: the option carries the client's name; we send the short
	 * (label-only) form, which is what routers register. Sent on
	 * DISCOVER and REQUEST (incl. renew); skipped on DECLINE/RELEASE.
	 * gethostname(3) returns the synthesised name launchd PID-1 set at
	 * early init (see hostnamed / freebsd_synthesize_hostname). */
	if (msgtype == DHCP_MTYPE_DISCOVER || msgtype == DHCP_MTYPE_REQUEST ||
	    msgtype == DHCP_MTYPE_INFORM) {
		char hn[256];

		if (gethostname(hn, sizeof(hn)) == 0 && hn[0] != '\0') {
			char *dot = strchr(hn, '.');
			size_t hlen;

			if (dot != NULL)
				*dot = '\0';	/* strip domain — label only */
			hlen = strlen(hn);
			if (hlen > 0 && hlen <= 255) {
				*opt++ = DHCP_OPT_HOST_NAME;
				*opt++ = (uint8_t)hlen;
				(void)memcpy(opt, hn, hlen);
				opt += hlen;
			}
		}
	}

	/* 55 parameter request list — what we want in the OFFER/ACK. */
	*opt++ = DHCP_OPT_PARAM_REQUEST;
	*opt++ = 7;
	*opt++ = DHCP_OPT_SUBNET_MASK;
	*opt++ = DHCP_OPT_ROUTER;
	*opt++ = DHCP_OPT_DNS_SERVER;
	*opt++ = DHCP_OPT_DOMAIN_NAME;
	*opt++ = DHCP_OPT_DOMAIN_SEARCH;
	*opt++ = DHCP_OPT_LEASE_TIME;
	*opt++ = DHCP_OPT_SERVER_ID;
	*opt++ = DHCP_OPT_END;

pad:
	/* Pad to RFC-2131 minimum 576 IP bytes. */
	frame_len = (size_t)(opt - buf);
	while (frame_len < sizeof(*eh) + DHCP_PACKET_MIN)
		buf[frame_len++] = DHCP_OPT_PAD;

	udp_len = frame_len - sizeof(*eh) - sizeof(*ip);
	ip_len = frame_len - sizeof(*eh);
	ip->ip_len = htons((u_short)ip_len);
	udp->uh_ulen = htons((u_short)udp_len);

	ip->ip_sum = 0;
	ip->ip_sum = htons(in_cksum(ip, sizeof(*ip)));

	(void)memset(pseudo, 0, sizeof(pseudo));
	(void)memcpy(pseudo, &ip->ip_src.s_addr, 4);
	(void)memcpy(pseudo + 4, &ip->ip_dst.s_addr, 4);
	pseudo[9] = IPPROTO_UDP;
	pseudo[10] = (uint8_t)(udp_len >> 8);
	pseudo[11] = (uint8_t)(udp_len & 0xff);
	{
		uint8_t tmp[2048];

		if (udp_len <= sizeof(tmp) - sizeof(pseudo)) {
			(void)memcpy(tmp, pseudo, sizeof(pseudo));
			(void)memcpy(tmp + sizeof(pseudo), udp, udp_len);
			udp->uh_sum = htons(in_cksum(tmp,
			    sizeof(pseudo) + udp_len));
			if (udp->uh_sum == 0)
				udp->uh_sum = 0xffff;
		}
	}
	return (frame_len);
}

/*
 * Find DHCP option `code` in the options blob (after the cookie).
 * Returns pointer + length, or NULL on absence.
 */
static const uint8_t *
dhcp_option(const uint8_t *opts, size_t opts_len, uint8_t code,
    uint8_t *len_out)
{
	size_t i = 4;	/* skip the magic cookie */

	while (i + 1 < opts_len) {
		uint8_t c = opts[i], l;

		if (c == DHCP_OPT_PAD) {
			i++;
			continue;
		}
		if (c == DHCP_OPT_END)
			break;
		l = opts[i + 1];
		if (i + 2 + l > opts_len)
			break;
		if (c == code) {
			*len_out = l;
			return (&opts[i + 2]);
		}
		i += 2 + l;
	}
	return (NULL);
}

/*
 * Every occurrence of option `code`, concatenated into `out` (Apple's
 * dhcpol_option_copy, RFC 3396). Returns the length, or -1 when the
 * option is absent. Data past `out_max` is cut off, which the name-list
 * decoder then rejects as truncated.
 */
static int
dhcp_option_copy(const uint8_t *opts, size_t opts_len, uint8_t code,
    uint8_t *out, size_t out_max)
{
	size_t i = 4, used = 0;
	bool found = false;

	while (i + 1 < opts_len) {
		uint8_t c = opts[i], l;

		if (c == DHCP_OPT_PAD) {
			i++;
			continue;
		}
		if (c == DHCP_OPT_END)
			break;
		l = opts[i + 1];
		if (i + 2 + l > opts_len)
			break;
		if (c == code) {
			size_t n = l;

			if (n > out_max - used)
				n = out_max - used;
			(void)memcpy(out + used, &opts[i + 2], n);
			used += n;
			found = true;
		}
		i += 2 + l;
	}
	return (found ? (int)used : -1);
}

/*
 * A name from the network ends up in /etc/resolv.conf, which is live
 * here, and in a CFString. Keep only printable ASCII with no white space.
 */
static bool
dns_name_ok(const char *s)
{
	if (*s == '\0')
		return (false);
	for (; *s != '\0'; s++) {
		unsigned char c = (unsigned char)*s;

		if (c <= 0x20 || c >= 0x7f)
			return (false);
	}
	return (true);
}

static void
lease_add_search(struct dhcp_lease *lease, const char *name)
{
	if (lease->search_count < DHCP_LEASE_MAX_SEARCH && dns_name_ok(name))
		(void)strlcpy(lease->search[lease->search_count++], name,
		    DHCP_LEASE_MAX_DOMAIN);
}

/*
 * Option 119: Apple's DNSNameListCreate (bootplib/DNSNameList.c), the
 * RFC 1035 compressed name list, into lease->search. The same checks: a
 * pointer must not be truncated, must point before the last completed
 * name and at the start of a label; a label must fit and be at most 63
 * octets. Any error discards the whole list, as Apple's returns NULL.
 */
#define	DNS_PTR_BYTE_MASK	0xc0
#define	DNS_LABEL_MAX		63
#define	DHCP_SEARCH_BUF_MAX	1500

static void
parse_search_list(const uint8_t *buf, size_t size, struct dhcp_lease *lease)
{
	bool is_label[DHCP_SEARCH_BUF_MAX];
	char name[DHCP_LEASE_MAX_DOMAIN];
	size_t last_completed = 0, left = size, name_len = 0;
	size_t read_head = 0, scan = 0;

	lease->search_count = 0;
	if (size == 0 || size > sizeof(is_label))
		return;
	(void)memset(is_label, 0, sizeof(is_label));

	while (read_head < size) {
		uint8_t len = buf[read_head];

		if ((len & DNS_PTR_BYTE_MASK) == DNS_PTR_BYTE_MASK) {
			size_t ptr;

			if (read_head >= scan) {
				if (left < 2)
					goto failed;
				scan += 2;
				left -= 2;
			}
			if (read_head + 1 >= size)
				goto failed;
			ptr = (size_t)(((len & ~DNS_PTR_BYTE_MASK) << 8) |
			    buf[read_head + 1]);
			if (ptr >= read_head || last_completed == 0 ||
			    ptr >= last_completed || !is_label[ptr])
				goto failed;
			read_head = ptr;
			continue;
		}
		if (read_head >= scan) {
			if (len > DNS_LABEL_MAX || left < (size_t)len + 1)
				goto failed;
			scan += (size_t)len + 1;
			left -= (size_t)len + 1;
		}
		if (len == 0) {
			name[name_len] = '\0';
			lease_add_search(lease, name);
			name_len = 0;
			last_completed = scan;
			read_head = scan;
			continue;
		}
		is_label[read_head] = true;
		/* room for the label, a separating dot and the NUL */
		if (name_len + (size_t)len + 2 > sizeof(name))
			goto failed;
		if (name_len > 0)
			name[name_len++] = '.';
		(void)memcpy(name + name_len, buf + read_head + 1, len);
		name_len += len;
		read_head += (size_t)len + 1;
	}
	return;

failed:
	lease->search_count = 0;
}

/*
 * Option 15: Apple's process_domain_name() (sysconfig.c). With option
 * 119 present only the first token counts; otherwise one token is the
 * domain and several are a search list.
 */
static void
parse_domain_name(const uint8_t *v, size_t len, bool search_present,
    struct dhcp_lease *lease)
{
	char tokens[DHCP_LEASE_MAX_SEARCH][DHCP_LEASE_MAX_DOMAIN];
	unsigned count = 0, i;
	size_t pos, start = 0;
	bool in_token = false;

	for (pos = 0; pos <= len; pos++) {
		uint8_t ch = (pos < len) ? v[pos] : 0;

		if (ch == 0 || isspace(ch)) {
			if (in_token) {
				size_t n = pos - start;

				in_token = false;
				if (count < DHCP_LEASE_MAX_SEARCH &&
				    n < DHCP_LEASE_MAX_DOMAIN) {
					(void)memcpy(tokens[count], v + start,
					    n);
					tokens[count][n] = '\0';
					if (dns_name_ok(tokens[count]))
						count++;
				}
				if (search_present || ch == 0)
					break;
			}
		} else if (!in_token) {
			in_token = true;
			start = pos;
		}
	}
	if (count == 1) {
		(void)strlcpy(lease->domain_name, tokens[0],
		    sizeof(lease->domain_name));
	} else if (count > 1) {
		for (i = 0; i < count; i++)
			lease_add_search(lease, tokens[i]);
	}
}

/*
 * Keep the DHCP payload of the reply just accepted, as sent (Apple's
 * bcopy() into saved.pkt, dhcp.c). The UDP length bounds it, since a short
 * frame is padded on the wire; a payload longer than the buffer is cut
 * off, and then the option parser rejects what remains.
 */
static void
save_packet(const uint8_t *frame, size_t frame_len, const struct udphdr *udp,
    const struct dhcp *dh, struct saved_pkt *saved)
{
	size_t off = (size_t)((const uint8_t *)dh - frame);
	size_t ulen = ntohs(udp->uh_ulen);
	size_t len;

	saved->pkt_size = 0;
	if (off >= frame_len)
		return;
	len = frame_len - off;
	if (ulen >= sizeof(*udp) && ulen - sizeof(*udp) < len)
		len = ulen - sizeof(*udp);
	if (len > sizeof(saved->pkt))
		len = sizeof(saved->pkt);
	(void)memcpy(saved->pkt, dh, len);
	saved->pkt_size = (unsigned)len;
}

/*
 * Parse a BPF-delivered frame as a DHCP reply matching want_xid +
 * want_msgtype. On match: fills `lease` with everything we care
 * about (addr / netmask / router / server / lease_time / DNS),
 * `saved` (may be NULL) with the raw packet, and returns 1. Returns 0
 * on a non-match (skip; caller continues the read loop).
 *
 * want_msgtype may also be DHCP_WANT_BOOTP (any reply with a usable yiaddr,
 * as bootp_request() takes) or DHCP_WANT_INFORM (a DHCPACK or a reply with no
 * message type, as inform_request() takes). Those two need no RFC 2132 cookie
 * and leave lease->netmask zero if the reply has no mask. `chaddr` (NULL: not
 * checked) must match the reply's client hardware address.
 */
static int
parse_dhcp_reply(const uint8_t *frame, size_t frame_len,
    uint32_t want_xid, uint8_t want_msgtype, struct dhcp_lease *lease,
    struct saved_pkt *saved, const uint8_t *chaddr)
{
	const struct ether_header *eh;
	const struct ip *ip;
	const struct udphdr *udp;
	const struct dhcp *dh;
	const uint8_t *opts;
	const uint8_t cookie[4] = DHCP_MAGIC_COOKIE;
	const uint8_t *v;
	size_t opts_len;
	uint8_t l = 0;
	bool lenient = want_msgtype == DHCP_WANT_BOOTP ||
	    want_msgtype == DHCP_WANT_INFORM;

	if (frame_len < sizeof(*eh) + sizeof(*ip) + sizeof(*udp) +
	    sizeof(*dh) + 4)
		return (0);
	eh = (const struct ether_header *)(const void *)frame;
	if (ntohs(eh->ether_type) != ETHERTYPE_IP)
		return (0);
	ip = (const struct ip *)(const void *)(frame + sizeof(*eh));
	if (ip->ip_v != 4 || ip->ip_p != IPPROTO_UDP)
		return (0);
	udp = (const struct udphdr *)(const void *)
	    (frame + sizeof(*eh) + (ip->ip_hl * 4));
	if (ntohs(udp->uh_dport) != DHCP_CLIENT_PORT)
		return (0);
	dh = (const struct dhcp *)(const void *)
	    ((const uint8_t *)udp + sizeof(*udp));
	if (dh->dp_op != BOOTREPLY)
		return (0);
	if (ntohl(dh->dp_xid) != want_xid)
		return (0);
	if (chaddr != NULL && memcmp(dh->dp_chaddr, chaddr, 6) != 0)
		return (0);

	opts = (const uint8_t *)dh + sizeof(*dh);
	opts_len = (size_t)(frame + frame_len - opts);
	if (opts_len < 4 || memcmp(opts, cookie, 4) != 0) {
		if (!lenient)
			return (0);
		opts_len = 0;	/* a BOOTP reply with no options */
	}

	/* Message type */
	v = dhcp_option(opts, opts_len, DHCP_OPT_MESSAGE_TYPE, &l);
	if (want_msgtype == DHCP_WANT_BOOTP) {
		if (dh->dp_yiaddr.s_addr == htonl(INADDR_ANY) ||
		    dh->dp_yiaddr.s_addr == htonl(INADDR_BROADCAST))
			return (0);
	} else if (want_msgtype == DHCP_WANT_INFORM) {
		if (v != NULL && (l != 1 || *v != DHCP_MTYPE_ACK))
			return (0);
	} else if (v == NULL || l != 1 || *v != want_msgtype) {
		return (0);
	}

	(void)memset(lease, 0, sizeof(*lease));
	lease->addr.s_addr = dh->dp_yiaddr.s_addr;

	/* 54 server id */
	v = dhcp_option(opts, opts_len, DHCP_OPT_SERVER_ID, &l);
	if (v != NULL && l == 4)
		(void)memcpy(&lease->server.s_addr, v, 4);

	/* 1 subnet mask (default /8 if absent — RFC silly fallback) */
	v = dhcp_option(opts, opts_len, DHCP_OPT_SUBNET_MASK, &l);
	if (v != NULL && l == 4)
		(void)memcpy(&lease->netmask.s_addr, v, 4);
	else if (!lenient)
		lease->netmask.s_addr = htonl(0xff000000);

	/* 3 router (first only) */
	v = dhcp_option(opts, opts_len, DHCP_OPT_ROUTER, &l);
	if (v != NULL && l >= 4)
		(void)memcpy(&lease->router.s_addr, v, 4);

	/* 51 lease time */
	v = dhcp_option(opts, opts_len, DHCP_OPT_LEASE_TIME, &l);
	if (v != NULL && l == 4) {
		uint32_t be;

		(void)memcpy(&be, v, 4);
		lease->lease_time = ntohl(be);
	}

	/* 6 DNS — list of IPv4 addrs, up to DHCP_LEASE_MAX_DNS */
	v = dhcp_option(opts, opts_len, DHCP_OPT_DNS_SERVER, &l);
	if (v != NULL && l >= 4 && (l % 4) == 0) {
		unsigned n = l / 4;

		if (n > DHCP_LEASE_MAX_DNS)
			n = DHCP_LEASE_MAX_DNS;
		for (unsigned i = 0; i < n; i++)
			(void)memcpy(&lease->dns[i].s_addr, v + i * 4, 4);
		lease->dns_count = n;
	}

	/* 12 host name — server-supplied client name. Per RFC 2132 §3.14
	 * this is a plain string (NOT NUL-terminated on the wire). Truncate
	 * any over-length value to DHCP_LEASE_MAX_HOSTNAME - 1 so we keep
	 * room for a terminator; sc_publish_dhcp converts to CFString.
	 * Issue #88. */
	v = dhcp_option(opts, opts_len, DHCP_OPT_HOST_NAME, &l);
	if (v != NULL && l > 0) {
		unsigned n = (l < DHCP_LEASE_MAX_HOSTNAME - 1)
		    ? (unsigned)l : (DHCP_LEASE_MAX_HOSTNAME - 1);
		(void)memcpy(lease->host_name, v, n);
		lease->host_name[n] = '\0';
		lease->host_name_len = n;
	}

	/*
	 * 15 domain name, 119 domain search (#425). 119 may span several
	 * option instances (RFC 3396); 15 is read knowing whether 119 exists.
	 */
	{
		uint8_t sbuf[DHCP_SEARCH_BUF_MAX];
		int slen;

		slen = dhcp_option_copy(opts, opts_len, DHCP_OPT_DOMAIN_SEARCH,
		    sbuf, sizeof(sbuf));
		v = dhcp_option(opts, opts_len, DHCP_OPT_DOMAIN_NAME, &l);
		if (v != NULL && l > 0)
			parse_domain_name(v, l, slen >= 0, lease);
		if (slen > 0)
			parse_search_list(sbuf, (size_t)slen, lease);
	}

	if (saved != NULL)
		save_packet(frame, frame_len, udp, dh, saved);
	return (1);
}

/*
 * Wait up to `deadline` (CLOCK_MONOTONIC absolute) for a DHCP
 * reply matching want_xid + want_msgtype. Returns 1 on match, 0
 * on timeout, -1 on shutdown signal, stop request or fatal read
 * error.
 *
 * The BPF descriptor has a 1s BIOCSRTIMEOUT so the wait wakes
 * every second to re-check got_term, `*stop` (NULL: none) and the
 * deadline. `saved` (may be NULL) gets the matching packet. `chaddr` is
 * parse_dhcp_reply()'s.
 */
static int
recv_dhcp_reply(int bpf_fd, uint32_t want_xid, uint8_t want_msgtype,
    struct timespec *deadline, struct dhcp_lease *lease,
    struct saved_pkt *saved, volatile sig_atomic_t *stop,
    const uint8_t *chaddr)
{
	uint8_t rxbuf[4096];

	while (!got_term && !(stop != NULL && *stop)) {
		struct timespec now;
		ssize_t n;
		uint8_t *p, *end;

		(void)clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec > deadline->tv_sec ||
		    (now.tv_sec == deadline->tv_sec &&
		     now.tv_nsec >= deadline->tv_nsec))
			return (0);

		n = read(bpf_fd, rxbuf, sizeof(rxbuf));
		if (n < 0) {
			if (errno == EINTR && got_term)
				return (-1);
			if (errno == EAGAIN || errno == EINTR)
				continue;
			xlog("read(bpf) failed: %s", strerror(errno));
			return (-1);
		}
		if (n == 0)
			continue;	/* BPF read timeout */

		p = rxbuf;
		end = rxbuf + n;
		while (p + sizeof(struct bpf_hdr) <= end) {
			struct bpf_hdr bh;
			uint8_t *pkt;
			size_t caplen;

			(void)memcpy(&bh, p, sizeof(bh));
			pkt = p + bh.bh_hdrlen;
			caplen = bh.bh_caplen;
			if (pkt + caplen > end)
				break;
			if (parse_dhcp_reply(pkt, caplen, want_xid,
			    want_msgtype, lease, saved, chaddr))
				return (1);
			p += BPF_WORDALIGN(bh.bh_hdrlen + caplen);
		}
	}
	return (-1);
}

/*
 * Send a DHCP message, handling em(4)/iflib's ~100ms post-IFF_UP
 * window where if_transmit returns ENETDOWN even after IFF_UP +
 * IFF_DRV_RUNNING are both set. iter 2 audited this in detail.
 */
static int
send_dhcp(int bpf_fd, const uint8_t *frame, size_t frame_len)
{
	int try;
	ssize_t n = -1;

	for (try = 0; try < 10 && !got_term; try++) {
		n = write(bpf_fd, frame, frame_len);
		if (n == (ssize_t)frame_len)
			return (0);
		if (errno != ENETDOWN)
			break;
		(void)usleep(100000);
	}
	/* On SIGTERM the caller reports the shutdown, not a write error. */
	if (!got_term)
		xlog("write(bpf, %zu) failed after %d tries: %s",
		    frame_len, try + 1, strerror(errno));
	return (-1);
}

/* RFC 2131 retransmit ladder: 4s, 8s, 16s with ±1s jitter. Renewal only. */
static const int retry_seconds[] = { 4, 8, 16 };
#define DHCP_MAX_RETRIES	(int)(sizeof(retry_seconds) / sizeof(retry_seconds[0]))

/*
 * Darwin's INIT and SELECT ladder, Info.plist InitialRetryTimeSeconds,
 * MaximumRetryTimeSeconds, RetryCount and DHCPSelectRetryCount (#426): 1, 2,
 * 4, then 8 s, with up to a second of fuzz, and RetryCount+1 DISCOVERs before
 * dhcp.c decides no server is answering.
 */
#define	DHCP_INITIAL_WAIT_SECS		1
#define	DHCP_MAX_WAIT_SECS		8
#define	DHCP_RETRY_COUNT		9
#define	DHCP_SELECT_RETRY_COUNT		3

static void
deadline_in(struct timespec *out, int seconds)
{
	long jitter_ns;

	(void)clock_gettime(CLOCK_MONOTONIC, out);
	/* ±1s jitter — RFC 2131 §4.1 recommendation. */
	jitter_ns = (long)random() % 2000000000L - 1000000000L;
	out->tv_sec += seconds;
	out->tv_nsec += jitter_ns;
	while (out->tv_nsec < 0) {
		out->tv_nsec += 1000000000L;
		out->tv_sec--;
	}
	while (out->tv_nsec >= 1000000000L) {
		out->tv_nsec -= 1000000000L;
		out->tv_sec++;
	}
}

/* `seconds` from now plus up to a second, dhcp.c's random_range() fuzz. */
static void
deadline_after(struct timespec *out, int seconds)
{
	(void)clock_gettime(CLOCK_MONOTONIC, out);
	out->tv_sec += seconds;
	out->tv_nsec += (long)random() % 1000000000L;
	if (out->tv_nsec >= 1000000000L) {
		out->tv_nsec -= 1000000000L;
		out->tv_sec++;
	}
}

int
dhcp_lease_acquire(const char *ifname, struct dhcp_lease *lease_out,
    struct saved_pkt *saved, volatile sig_atomic_t *stop, dhcp_try_cb on_try,
    void *on_try_arg)
{
	uint8_t mac[6];
	uint8_t frame[1024];
	uint32_t xid;
	int bpf_fd = -1, try, wait_secs;
	size_t frame_len;
	struct timespec deadline;
	struct dhcp_lease offer;

	/*
	 * A failure while the daemon or the worker is being stopped is the
	 * stop, not a bind failure: every give-up below takes `shutdown`
	 * then, and the caller checks for a stop before it logs
	 * IPCFG-BOUND-FAIL (#417).
	 */
	if (get_interface_mac(ifname, mac) != 0) {
		if (got_term || (stop != NULL && *stop))
			goto shutdown;
		xlog("get_interface_mac(%s) failed: %s",
		    ifname, strerror(errno));
		return (DHCP_ACQUIRE_ERROR);
	}
	xlog("iface %s mac %02x:%02x:%02x:%02x:%02x:%02x",
	    ifname, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	if (bring_interface_up(ifname) != 0) {
		if (got_term || (stop != NULL && *stop))
			goto shutdown;
		return (DHCP_ACQUIRE_ERROR);
	}

	bpf_fd = bpf_open(ifname);
	if (bpf_fd < 0) {
		xlog("bpf_open(%s) failed: %s", ifname, strerror(errno));
		return (DHCP_ACQUIRE_ERROR);
	}

	{
		struct timespec ts;

		(void)clock_gettime(CLOCK_REALTIME, &ts);
		xid = (uint32_t)ts.tv_nsec ^ (uint32_t)ts.tv_sec ^
		    (uint32_t)(mac[5] | (mac[4] << 8));
		srandom((unsigned)xid);
	}

	/* ---- SELECTING: send DISCOVER until an OFFER arrives ---- */
	(void)memset(&offer, 0, sizeof(offer));
	wait_secs = DHCP_INITIAL_WAIT_SECS;
	for (try = 1; try <= DHCP_RETRY_COUNT + 1; try++) {
		int r;

		if (got_term || (stop != NULL && *stop))
			goto shutdown;
		frame_len = build_dhcp_msg(frame, mac, xid,
		    DHCP_MTYPE_DISCOVER, NULL, NULL, NULL);
		if (send_dhcp(bpf_fd, frame, frame_len) != 0) {
			if (got_term || (stop != NULL && *stop))
				goto shutdown;
			(void)close(bpf_fd);
			return (DHCP_ACQUIRE_ERROR);
		}
		xlog("sent DHCPDISCOVER (xid=0x%08x, try=%d/%d)", xid,
		    try, DHCP_RETRY_COUNT + 1);

		/*
		 * The deadline comes first: what on_try does eats into this
		 * wait, not the ladder, and the BPF descriptor holds what
		 * arrives meanwhile.
		 */
		deadline_after(&deadline, wait_secs);
		if (on_try != NULL)
			on_try(try, on_try_arg);
		r = recv_dhcp_reply(bpf_fd, xid, DHCP_MTYPE_OFFER,
		    &deadline, &offer, NULL, stop, NULL);
		if (r == 1)
			break;
		if (r < 0)
			goto shutdown;
		wait_secs *= 2;
		if (wait_secs > DHCP_MAX_WAIT_SECS)
			wait_secs = DHCP_MAX_WAIT_SECS;
	}
	if (try > DHCP_RETRY_COUNT + 1) {
		xlog("no DHCPOFFER after %d tries (iface=%s xid=0x%08x)",
		    DHCP_RETRY_COUNT + 1, ifname, xid);
		(void)close(bpf_fd);
		return (DHCP_ACQUIRE_NO_SERVER);
	}
	{
		char y[INET_ADDRSTRLEN], s[INET_ADDRSTRLEN];

		(void)inet_ntop(AF_INET, &offer.addr, y, sizeof(y));
		(void)inet_ntop(AF_INET, &offer.server, s, sizeof(s));
		xlog("OFFER yiaddr=%s server=%s", y, s);
	}

	/* ---- RFC 5227 ARP probe on the offered address ----
	 *
	 * Defends against the DHCP server handing out an address that
	 * another host on the segment is already using (a misconfigured
	 * static client, a leaked lease, etc.). On conflict we DECLINE
	 * the offer and the caller goes back to INIT after RFC 2131
	 * §3.1.5's ten seconds, as dhcp_decline() does. CI's SLIRP gateway
	 * can't actually replicate a conflict, so the conflict branch is
	 * exercised only by the unit/logic path. */
	{
		int pr = arp_probe(ifname, offer.addr);

		if (pr == 1) {
			frame_len = build_dhcp_msg(frame, mac, xid,
			    DHCP_MTYPE_DECLINE, &offer.addr, &offer.server,
			    NULL);
			if (send_dhcp(bpf_fd, frame, frame_len) != 0)
				xlog("DHCPDECLINE send failed");
			else
				xlog("sent DHCPDECLINE (xid=0x%08x)", xid);
			(void)close(bpf_fd);
			return (DHCP_ACQUIRE_DECLINED);
		}
		if (pr == 0)
			xlog("IPCFG-ARP-OK");
		/* pr < 0: arp_probe failed (BPF open / send error). Treat
		 * as no-conflict so a transient probe issue doesn't
		 * deadlock the lease; the address is still verified by
		 * DHCPACK below. No marker emitted on this path — the
		 * boot test fails fast if it never sees ARP-OK. */
	}

	/* ---- REQUESTING: send REQUEST until an ACK arrives ---- */
	wait_secs = DHCP_INITIAL_WAIT_SECS;
	for (try = 1; try <= DHCP_SELECT_RETRY_COUNT + 1; try++) {
		int r;

		if (got_term || (stop != NULL && *stop))
			goto shutdown;
		frame_len = build_dhcp_msg(frame, mac, xid,
		    DHCP_MTYPE_REQUEST, &offer.addr, &offer.server, NULL);
		if (send_dhcp(bpf_fd, frame, frame_len) != 0) {
			if (got_term || (stop != NULL && *stop))
				goto shutdown;
			(void)close(bpf_fd);
			return (DHCP_ACQUIRE_ERROR);
		}
		xlog("sent DHCPREQUEST (xid=0x%08x, try=%d/%d)", xid,
		    try, DHCP_SELECT_RETRY_COUNT + 1);

		deadline_after(&deadline, wait_secs);
		r = recv_dhcp_reply(bpf_fd, xid, DHCP_MTYPE_ACK,
		    &deadline, lease_out, saved, stop, NULL);
		if (r == 1) {
			(void)close(bpf_fd);
			return (DHCP_ACQUIRE_OK);
		}
		if (r < 0)
			goto shutdown;
		wait_secs *= 2;
		if (wait_secs > DHCP_MAX_WAIT_SECS)
			wait_secs = DHCP_MAX_WAIT_SECS;
	}
	xlog("no DHCPACK after %d tries (iface=%s xid=0x%08x)",
	    DHCP_SELECT_RETRY_COUNT + 1, ifname, xid);
	(void)close(bpf_fd);
	return (DHCP_ACQUIRE_NO_ACK);

shutdown:
	if (bpf_fd >= 0)
		(void)close(bpf_fd);
	if (got_term) {
		/*
		 * Not a failure: the daemon is exiting (launchctl unload,
		 * shutdown). Its stderr log outlives it, so a marker here
		 * would read as a bind failure of the next instance (#417).
		 */
		xlog("%s: DHCP exchange interrupted by signal %d", ifname,
		    (int)got_term);
		return (DHCP_ACQUIRE_ERROR);
	}
	if (stop != NULL && *stop) {
		/* Not a failure: the service was stopped (#410). */
		xlog("%s: DHCP exchange stopped", ifname);
		return (DHCP_ACQUIRE_ERROR);
	}
	/* recv_dhcp_reply's read(bpf) failed; it logged why. */
	return (DHCP_ACQUIRE_ERROR);
}

/*
 * The exchange dhcp_inform() and dhcp_bootp() share: send the request down
 * the INIT ladder until a matching reply. A stop or a local failure ends it.
 * Returns a DHCP_ACQUIRE_ code.
 */
static int
single_exchange(const char *ifname, uint8_t msgtype, uint8_t want,
    const struct in_addr *ciaddr, struct dhcp_lease *out,
    struct saved_pkt *saved, volatile sig_atomic_t *stop)
{
	uint8_t mac[6];
	uint8_t frame[1024];
	uint32_t xid;
	struct timespec deadline;
	size_t frame_len;
	int bpf_fd, try, wait_secs, rc = DHCP_ACQUIRE_NO_SERVER;

	if (get_interface_mac(ifname, mac) != 0 ||
	    bring_interface_up(ifname) != 0)
		return (DHCP_ACQUIRE_ERROR);
	bpf_fd = bpf_open(ifname);
	if (bpf_fd < 0) {
		xlog("bpf_open(%s) failed: %s", ifname, strerror(errno));
		return (DHCP_ACQUIRE_ERROR);
	}
	xid = arc4random();
	frame_len = build_dhcp_msg(frame, mac, xid, msgtype, NULL, NULL,
	    ciaddr);

	wait_secs = DHCP_INITIAL_WAIT_SECS;
	for (try = 1; try <= DHCP_RETRY_COUNT + 1; try++) {
		int r;

		if (got_term || (stop != NULL && *stop) ||
		    send_dhcp(bpf_fd, frame, frame_len) != 0) {
			rc = DHCP_ACQUIRE_ERROR;
			break;
		}
		xlog("sent %s (xid=0x%08x, try=%d/%d)",
		    msgtype == DHCP_MTYPE_INFORM ? "DHCPINFORM" : "BOOTREQUEST",
		    xid, try, DHCP_RETRY_COUNT + 1);
		deadline_after(&deadline, wait_secs);
		r = recv_dhcp_reply(bpf_fd, xid, want, &deadline, out, saved,
		    stop, mac);
		if (r != 0) {
			rc = r == 1 ? DHCP_ACQUIRE_OK : DHCP_ACQUIRE_ERROR;
			break;
		}
		wait_secs *= 2;
		if (wait_secs > DHCP_MAX_WAIT_SECS)
			wait_secs = DHCP_MAX_WAIT_SECS;
	}
	(void)close(bpf_fd);
	return (rc);
}

int
dhcp_inform(const char *ifname, struct in_addr ciaddr, struct dhcp_lease *out,
    struct saved_pkt *saved, volatile sig_atomic_t *stop)
{
	return (single_exchange(ifname, DHCP_MTYPE_INFORM, DHCP_WANT_INFORM,
	    &ciaddr, out, saved, stop));
}

int
dhcp_bootp(const char *ifname, struct dhcp_lease *out,
    struct saved_pkt *saved, volatile sig_atomic_t *stop)
{
	return (single_exchange(ifname, DHCP_MTYPE_NONE, DHCP_WANT_BOOTP,
	    NULL, out, saved, stop));
}

int
dhcp_decline(const char *ifname, const struct dhcp_lease *lease)
{
	uint8_t mac[6];
	uint8_t frame[1024];
	uint32_t xid;
	int bpf_fd, rc;
	size_t frame_len;
	struct timespec ts;

	if (get_interface_mac(ifname, mac) != 0)
		return (-1);
	bpf_fd = bpf_open(ifname);
	if (bpf_fd < 0) {
		xlog("decline: bpf_open(%s) failed: %s", ifname,
		    strerror(errno));
		return (-1);
	}
	(void)clock_gettime(CLOCK_REALTIME, &ts);
	xid = (uint32_t)ts.tv_nsec ^ (uint32_t)ts.tv_sec ^
	    (uint32_t)(mac[5] | (mac[4] << 8));
	frame_len = build_dhcp_msg(frame, mac, xid, DHCP_MTYPE_DECLINE,
	    &lease->addr, &lease->server, NULL);
	rc = send_dhcp(bpf_fd, frame, frame_len);
	if (rc != 0)
		xlog("DHCPDECLINE send failed");
	else
		xlog("sent DHCPDECLINE (xid=0x%08x)", xid);
	(void)close(bpf_fd);
	return (rc);
}

int
dhcp_renew(const char *ifname, const struct dhcp_lease *existing,
    struct dhcp_lease *new_lease, struct saved_pkt *saved)
{
	uint8_t mac[6];
	uint8_t frame[1024];
	uint32_t xid;
	int bpf_fd, try;
	size_t frame_len;
	struct timespec deadline;

	if (get_interface_mac(ifname, mac) != 0)
		return (-1);

	/* Interface is already up (we have a lease on it) — no
	 * bring_interface_up call needed. Open a fresh BPF descriptor
	 * just for this exchange so we don't have to plumb the fd
	 * across the lease loop. */
	bpf_fd = bpf_open(ifname);
	if (bpf_fd < 0) {
		xlog("renew: bpf_open(%s) failed: %s", ifname,
		    strerror(errno));
		return (-1);
	}

	{
		struct timespec ts;

		(void)clock_gettime(CLOCK_REALTIME, &ts);
		xid = (uint32_t)ts.tv_nsec ^ (uint32_t)ts.tv_sec ^
		    (uint32_t)(mac[5] | (mac[4] << 8));
	}

	for (try = 0; try < DHCP_MAX_RETRIES; try++) {
		int r;

		if (got_term)
			break;
		frame_len = build_dhcp_msg(frame, mac, xid,
		    DHCP_MTYPE_REQUEST, NULL, NULL, &existing->addr);
		if (send_dhcp(bpf_fd, frame, frame_len) != 0) {
			(void)close(bpf_fd);
			return (-1);
		}
		xlog("renew: sent DHCPREQUEST (xid=0x%08x, try=%d/%d)",
		    xid, try + 1, DHCP_MAX_RETRIES);

		deadline_in(&deadline, retry_seconds[try]);
		r = recv_dhcp_reply(bpf_fd, xid, DHCP_MTYPE_ACK,
		    &deadline, new_lease, saved, NULL, NULL);
		if (r == 1) {
			(void)close(bpf_fd);
			return (0);
		}
		if (r < 0)
			break;
	}
	xlog("renew: no DHCPACK after %d retries (iface=%s xid=0x%08x)",
	    DHCP_MAX_RETRIES, ifname, xid);
	(void)close(bpf_fd);
	return (-1);
}
