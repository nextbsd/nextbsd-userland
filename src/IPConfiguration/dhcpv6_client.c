/*
 * dhcpv6_client.c — the DHCPv6 client on one interface (#424).
 *
 * See dhcpv6_client.h. The packet handling and retransmit arithmetic are
 * Apple's bootp-268.1 IPConfiguration.bproj/DHCPv6Client.c, with its state
 * and function names, driven from a blocking thread instead of a run loop.
 * The lease is only ever in memory, as Apple's is; the DUID and IAIDs
 * persist (dhcp_duid_iaid.c).
 */
#include "dhcpv6_client.h"

#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "DHCPv6.h"
#include "DHCPv6Options.h"
#include "DNSNameList.h"
#include "dhcp_duid_iaid.h"
#include "dhcpv6_socket.h"

extern volatile sig_atomic_t got_term;

/* How often a wait looks at the stop flag. */
#define	DHCPV6_WAIT_SLICE_MS	500

static const uint16_t	DHCPv6RequestedOptions[] = {
	kDHCPv6OPTION_DNS_SERVERS,
	kDHCPv6OPTION_DOMAIN_LIST
};
#define	DHCPv6RequestedOptionsCount \
    (sizeof(DHCPv6RequestedOptions) / sizeof(DHCPv6RequestedOptions[0]))

struct dhcpv6_client {
	char			ifname[IFNAMSIZ];
	unsigned		ifindex;
	struct dhcpv6_sock	*sock;
	dhcpv6_client_notify_t	notify;
	void			*notify_arg;
	pthread_t		tid;
	volatile sig_atomic_t	stop;
	pthread_mutex_t		lock;		/* saved, saved_len */
	uint8_t			saved[DHCPV6_PACKET_MAX];
	int			saved_len;
	uint32_t		transaction_id;
	int			try;
	double			start_time;
	double			retransmit_time;
};

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

static double
current_time(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
}

static double
random_double_in_range(double bottom, double top)
{
	double r = (double)arc4random() / (double)UINT32_MAX;

	return (bottom + (top - bottom) * r);
}

static uint32_t
get_new_transaction_id(void)
{
	/* the lower 24 bits */
	return (arc4random() & 0x00ffffffU);
}

static double
DHCPv6_RAND(void)
{
	return (random_double_in_range(-0.1, 0.1));
}

static double
DHCPv6SubsequentTimeout(double RTprev, double MRT)
{
	double RT;

	RT = 2 * RTprev + DHCPv6_RAND() * RTprev;
	if (MRT != 0 && RT > MRT)
		RT = MRT + DHCPv6_RAND() * MRT;
	return (RT);
}

static double
DHCPv6InitialTimeout(double IRT)
{
	return (IRT + DHCPv6_RAND() * IRT);
}

static void
DHCPv6ClientClearRetransmit(struct dhcpv6_client *client)
{
	client->try = 0;
}

static double
DHCPv6ClientNextRetransmit(struct dhcpv6_client *client, double IRT, double MRT)
{
	client->try++;
	if (client->try == 1)
		client->retransmit_time = DHCPv6InitialTimeout(IRT);
	else
		client->retransmit_time =
		    DHCPv6SubsequentTimeout(client->retransmit_time, MRT);
	return (client->retransmit_time);
}

static uint16_t
get_elapsed_time(struct dhcpv6_client *client)
{
	uint32_t elapsed;

	if (client->try == 1)
		return (0);
	/* elapsed time is in 1/100ths of a second */
	elapsed = (uint32_t)((current_time() - client->start_time) * 100);
	if (elapsed > 0xffff)
		elapsed = 0xffff;
	return (htons((uint16_t)elapsed));
}

static bool
S_insert_duid(DHCPv6OptionAreaRef oa_p)
{
	CFDataRef data;
	DHCPv6OptionErrorString err;

	data = DHCPDUIDGet();
	if (data == NULL)
		return (false);
	if (!DHCPv6OptionAreaAddOption(oa_p, kDHCPv6OPTION_CLIENTID,
	    (int)CFDataGetLength(data), CFDataGetBytePtr(data), &err)) {
		xlog("failed to add CLIENTID, %s", err.str);
		return (false);
	}
	return (true);
}

static bool
S_duid_matches(DHCPv6OptionListRef options)
{
	CFDataRef data;
	const uint8_t *duid;
	int option_len;

	data = DHCPDUIDGet();
	if (data == NULL)
		return (false);
	duid = DHCPv6OptionListGetOptionDataAndLength(options,
	    kDHCPv6OPTION_CLIENTID, &option_len, NULL);
	return (duid != NULL && CFDataGetLength(data) == option_len &&
	    memcmp(duid, CFDataGetBytePtr(data), (size_t)option_len) == 0);
}

static DHCPv6PacketRef
DHCPv6ClientMakePacket(struct dhcpv6_client *client, int message_type,
    uint8_t *buf, int buf_size, DHCPv6OptionAreaRef oa_p)
{
	DHCPv6OptionErrorString err;
	DHCPv6PacketRef pkt;
	uint16_t elapsed_time;

	pkt = (DHCPv6PacketRef)(void *)buf;
	DHCPv6PacketSetMessageType(pkt, message_type);
	DHCPv6PacketSetTransactionID(pkt, client->transaction_id);
	DHCPv6OptionAreaInit(oa_p, pkt->options,
	    buf_size - DHCPV6_PACKET_HEADER_LENGTH);
	if (!S_insert_duid(oa_p))
		return (NULL);
	if (!DHCPv6OptionAreaAddOptionRequestOption(oa_p,
	    DHCPv6RequestedOptions, DHCPv6RequestedOptionsCount, &err)) {
		xlog("failed to add ORO, %s", err.str);
		return (NULL);
	}
	elapsed_time = get_elapsed_time(client);
	if (!DHCPv6OptionAreaAddOption(oa_p, kDHCPv6OPTION_ELAPSED_TIME,
	    sizeof(elapsed_time), &elapsed_time, &err)) {
		xlog("failed to add ELAPSED_TIME, %s", err.str);
		return (NULL);
	}
	return (pkt);
}

static void
DHCPv6ClientTransmit(struct dhcpv6_client *client, DHCPv6PacketRef pkt,
    DHCPv6OptionAreaRef oa_p, const char *label)
{
	int error;

	error = dhcpv6_socket_transmit(client->sock, pkt,
	    DHCPV6_PACKET_HEADER_LENGTH + DHCPv6OptionAreaGetUsedLength(oa_p));
	switch (error) {
	case 0:
	case ENXIO:
	case ENETDOWN:
	case EADDRNOTAVAIL:
		break;
	default:
		xlog("%s: %s transmit failed: %s", client->ifname, label,
		    strerror(error));
		break;
	}
}

static void
DHCPv6ClientSendInform(struct dhcpv6_client *client)
{
	uint8_t buf[DHCPV6_PACKET_MAX];
	DHCPv6OptionArea oa;
	DHCPv6PacketRef pkt;

	pkt = DHCPv6ClientMakePacket(client, kDHCPv6MessageINFORMATION_REQUEST,
	    buf, sizeof(buf), &oa);
	if (pkt != NULL)
		DHCPv6ClientTransmit(client, pkt, &oa, "Information-Request");
}

static void
DHCPv6ClientSavePacket(struct dhcpv6_client *client, const uint8_t *pkt,
    int pkt_len)
{
	(void)pthread_mutex_lock(&client->lock);
	(void)memcpy(client->saved, pkt, (size_t)pkt_len);
	client->saved_len = pkt_len;
	(void)pthread_mutex_unlock(&client->lock);
}

static void
DHCPv6ClientPostNotification(struct dhcpv6_client *client)
{
	if (client->notify != NULL)
		client->notify(client->notify_arg, client);
}

static bool
client_should_stop(const struct dhcpv6_client *client)
{
	return (got_term || client->stop);
}

/*
 * Wait up to `wait_s` for a Reply matching this client's transaction and DUID
 * and return it as DHCPv6Client_Inform's IFEventID_data_e tests it, or 0.
 */
static int
wait_for_reply(struct dhcpv6_client *client, double wait_s, uint8_t *pkt,
    size_t size)
{
	double deadline = current_time() + wait_s;

	while (!client_should_stop(client)) {
		DHCPv6OptionErrorString err;
		DHCPv6OptionListRef options;
		double left = deadline - current_time();
		unsigned ms;
		bool matches;
		int len;

		if (left <= 0)
			return (0);
		ms = (unsigned)(left * 1000) + 1;
		if (ms > DHCPV6_WAIT_SLICE_MS)
			ms = DHCPV6_WAIT_SLICE_MS;
		len = dhcpv6_socket_receive(client->sock, pkt, size, ms);
		if (len < DHCPV6_PACKET_HEADER_LENGTH)
			continue;
		options = DHCPv6OptionListCreateWithPacket(
		    (DHCPv6PacketRef)(void *)pkt, len, &err);
		if (options == NULL) {
			xlog("%s: options parse failed, %s", client->ifname,
			    err.str);
			continue;
		}
		matches = ((DHCPv6PacketRef)(void *)pkt)->msg_type ==
		    kDHCPv6MessageREPLY &&
		    DHCPv6PacketGetTransactionID((DHCPv6PacketRef)(void *)pkt) ==
		    client->transaction_id && S_duid_matches(options);
		DHCPv6OptionListRelease(&options);
		if (matches)
			return (len);
	}
	return (0);
}

/*
 * DHCPv6Client_Inform: no limit on tries (DHCPv6_INF_MAX_RT only caps the
 * timeout), so with no server this keeps quietly trying until the link goes
 * down and the service stops the client.
 */
static void
DHCPv6Client_Inform(struct dhcpv6_client *client)
{
	uint8_t pkt[DHCPV6_PACKET_MAX];
	int len;

	xlog("%s: DHCPv6 Inform", client->ifname);
	DHCPv6ClientClearRetransmit(client);
	client->transaction_id = get_new_transaction_id();
	(void)wait_for_reply(client,
	    random_double_in_range(0, DHCPv6_INF_MAX_DELAY), pkt, sizeof(pkt));
	while (!client_should_stop(client)) {
		double wait_s;

		if (client->try == 0)
			client->start_time = current_time();
		wait_s = DHCPv6ClientNextRetransmit(client, DHCPv6_INF_TIMEOUT,
		    DHCPv6_INF_MAX_RT);
		DHCPv6ClientSendInform(client);
		len = wait_for_reply(client, wait_s, pkt, sizeof(pkt));
		if (len > 0) {
			xlog("%s: DHCPv6 Reply received (try=%d)",
			    client->ifname, client->try);
			DHCPv6ClientSavePacket(client, pkt, len);
			DHCPv6ClientPostNotification(client);
			return;
		}
	}
}

static void *
dhcpv6_client_main(void *arg)
{
	struct dhcpv6_client *client = arg;

	client->sock = dhcpv6_socket_create(client->ifname, client->ifindex);
	if (client->sock != NULL) {
		DHCPv6Client_Inform(client);
		dhcpv6_socket_release(&client->sock);
	}
	return (NULL);
}

struct dhcpv6_client *
dhcpv6_client_start(const char *ifname, unsigned ifindex,
    dhcpv6_client_notify_t notify, void *arg)
{
	struct dhcpv6_client *client;
	int rc;

	client = calloc(1, sizeof(*client));
	if (client == NULL)
		return (NULL);
	(void)strlcpy(client->ifname, ifname, sizeof(client->ifname));
	client->ifindex = ifindex;
	client->notify = notify;
	client->notify_arg = arg;
	(void)pthread_mutex_init(&client->lock, NULL);
	rc = pthread_create(&client->tid, NULL, dhcpv6_client_main, client);
	if (rc != 0) {
		xlog("%s: pthread_create failed: %s", ifname, strerror(rc));
		(void)pthread_mutex_destroy(&client->lock);
		free(client);
		return (NULL);
	}
	return (client);
}

void
dhcpv6_client_stop(struct dhcpv6_client **client_p)
{
	struct dhcpv6_client *client = *client_p;

	if (client == NULL)
		return;
	*client_p = NULL;
	client->stop = 1;
	(void)pthread_join(client->tid, NULL);
	(void)pthread_mutex_destroy(&client->lock);
	free(client);
}

bool
dhcpv6_client_copy_packet(struct dhcpv6_client *client, void *buf, size_t size,
    size_t *len)
{
	bool ok = false;

	(void)pthread_mutex_lock(&client->lock);
	if (client->saved_len > 0 && (size_t)client->saved_len <= size) {
		(void)memcpy(buf, client->saved, (size_t)client->saved_len);
		*len = (size_t)client->saved_len;
		ok = true;
	}
	(void)pthread_mutex_unlock(&client->lock);
	return (ok);
}

bool
dhcpv6_client_copy_dns(struct dhcpv6_client *client, struct ipconfig_dns *out)
{
	uint8_t pkt[DHCPV6_PACKET_MAX];
	DHCPv6OptionErrorString err;
	DHCPv6OptionListRef options;
	const uint8_t *servers, *search;
	size_t len = 0;
	int servers_len, search_len, i, servers_count;

	(void)memset(out, 0, sizeof(*out));
	if (!dhcpv6_client_copy_packet(client, pkt, sizeof(pkt), &len))
		return (false);
	options = DHCPv6OptionListCreateWithPacket(
	    (DHCPv6PacketRef)(void *)pkt, (int)len, &err);
	if (options == NULL)
		return (false);
	servers = DHCPv6OptionListGetOptionDataAndLength(options,
	    kDHCPv6OPTION_DNS_SERVERS, &servers_len, NULL);
	servers_count = (servers != NULL) ?
	    servers_len / (int)sizeof(struct in6_addr) : 0;
	for (i = 0; i < servers_count &&
	    out->server_count < IPCFG_DNS_MAX_SERVERS; i++) {
		if (inet_ntop(AF_INET6,
		    servers + (size_t)i * sizeof(struct in6_addr),
		    out->servers[out->server_count], IPCFG_DNS_ADDR_LEN) != NULL)
			out->server_count++;
	}
	if (out->server_count > 0) {
		/* the DNS search list, if present */
		search = DHCPv6OptionListGetOptionDataAndLength(options,
		    kDHCPv6OPTION_DOMAIN_LIST, &search_len, NULL);
		if (search != NULL) {
			const char **list;
			int list_count;

			list = DNSNameListCreate(search, search_len,
			    &list_count);
			if (list != NULL) {
				for (i = 0; i < list_count &&
				    out->search_count < IPCFG_DNS_MAX_SEARCH;
				    i++)
					(void)strlcpy(
					    out->search[out->search_count++],
					    list[i], IPCFG_DNS_NAME_LEN);
				out->has_search = out->search_count > 0;
				free(list);
			}
		}
	}
	DHCPv6OptionListRelease(&options);
	return (out->server_count > 0);
}
