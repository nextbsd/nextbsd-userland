/*
 * dhcpv6_client.c — the DHCPv6 client on one interface (#424, #576).
 *
 * See dhcpv6_client.h. The packet handling, retransmit arithmetic and lease
 * rules are Apple's bootp-268.1 IPConfiguration.bproj/DHCPv6Client.c, with
 * its state and function names. Apple drives each state from run-loop
 * events; here a state is a function on the client's own blocking thread,
 * and returns the next state. Waits end early for a stop, a restart or a
 * duplicate report.
 *
 * Like Apple's, the lease and packet are only in memory; the DUID and IAIDs
 * persist (dhcp_duid_iaid.c). A client lasts as long as its service, so a
 * lease that has to outlive a link bounce for Confirm is held in a small
 * table here until the client on the next link-up takes it back.
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
#include "apply_lease_v6.h"
#include "bound_state.h"	/* BOUND_MAX_IF */
#include "dhcp_duid_iaid.h"
#include "dhcpv6_socket.h"
#include "sc_link_watch.h"

extern volatile sig_atomic_t got_term;

/* How often a wait looks at the stop flag. */
#define	DHCPV6_WAIT_SLICE_MS	500

/* Apple's dhcp.h. */
#define	DHCP_INFINITE_LEASE	((uint32_t)0xffffffff)

/* Apple's S_get_prefix_length(): nothing on-link says otherwise. */
#define	DHCPV6_PREFIX_LENGTH	128

typedef enum {
	kDHCPv6ClientStateNone = 0,
	kDHCPv6ClientStateSolicit,
	kDHCPv6ClientStateRequest,
	kDHCPv6ClientStateBound,
	kDHCPv6ClientStateRenew,
	kDHCPv6ClientStateRebind,
	kDHCPv6ClientStateConfirm,
	kDHCPv6ClientStateRelease,
	kDHCPv6ClientStateUnbound,
	kDHCPv6ClientStateDecline,
	kDHCPv6ClientStateInform,
	kDHCPv6ClientStateExit		/* not Apple's: leave the thread */
} DHCPv6ClientState;

typedef struct {
	double		start;
	/* these times are all relative to start */
	uint32_t	t1;
	uint32_t	t2;
	uint32_t	valid_lifetime;
	uint32_t	preferred_lifetime;
	bool		valid;
} lease_info_t;

static const uint16_t	DHCPv6RequestedOptions[] = {
	kDHCPv6OPTION_DNS_SERVERS,
	kDHCPv6OPTION_DOMAIN_LIST
};
#define	DHCPv6RequestedOptionsCount \
    (sizeof(DHCPv6RequestedOptions) / sizeof(DHCPv6RequestedOptions[0]))

struct dhcpv6_client {
	char			ifname[IFNAMSIZ];
	unsigned		ifindex;
	bool			allocate;
	struct dhcpv6_sock	*sock;
	dhcpv6_client_notify_t	notify;
	void			*notify_arg;
	pthread_t		tid;
	volatile sig_atomic_t	stop;
	volatile sig_atomic_t	restart;
	volatile sig_atomic_t	duplicated;

	DHCPv6ClientState	cstate;
	uint32_t		transaction_id;
	int			try;
	double			start_time;
	double			retransmit_time;
	lease_info_t		lease;

	/* Fixed before the thread starts, or under `lock` (a restart). */
	struct ra_onlink_prefix	onlink[RA_MAX_ONLINK];
	unsigned		onlink_count;

	pthread_mutex_t		lock;	/* the fields below, for other threads */
	uint8_t			saved[DHCPV6_PACKET_MAX];
	int			saved_len;
	bool			saved_verified;
	struct in6_addr		our_ip;	/* :: if none installed */
	int			our_prefix_length;
	bool			addr_ready;	/* past DAD */

	/* Into saved: the client's thread only. */
	const uint8_t		*server_id;
	DHCPv6OptionIAADDRRef	ia_addr;
	DHCPv6OptionIA_NARef	ia_na;
};

/* Leases kept over a link bounce; see the top of the file. */
static pthread_mutex_t	g_retained_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
	char		ifname[IFNAMSIZ];
	uint8_t		saved[DHCPV6_PACKET_MAX];
	int		saved_len;
	lease_info_t	lease;
} g_retained[BOUND_MAX_IF];

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

/* The sanity checks on an IA_NA option, and its first usable IAADDR. */
static DHCPv6OptionIA_NARef
get_ia_na_addr(struct dhcpv6_client *client, int msg_type,
    DHCPv6OptionListRef options, DHCPv6OptionIAADDRRef *ret_ia_addr)
{
	DHCPv6OptionErrorString err;
	DHCPv6OptionIA_NARef ia_na;
	DHCPv6OptionListRef ia_na_options;
	int option_len, start_index;
	uint32_t preferred_lifetime, t1, t2, valid_lifetime;

	(void)msg_type;
	*ret_ia_addr = NULL;
	ia_na = (DHCPv6OptionIA_NARef)(uintptr_t)
	    DHCPv6OptionListGetOptionDataAndLength(options,
	    kDHCPv6OPTION_IA_NA, &option_len, NULL);
	if (ia_na == NULL || option_len <= DHCPv6OptionIA_NA_MIN_LENGTH)
		return (NULL);	/* no IA_NA option */
	t1 = DHCPv6OptionIA_NAGetT1(ia_na);
	t2 = DHCPv6OptionIA_NAGetT2(ia_na);
	if (t1 != 0 && t2 != 0 && t1 > t2)
		return (NULL);	/* server is confused */
	option_len -= DHCPv6OptionIA_NA_MIN_LENGTH;
	ia_na_options = DHCPv6OptionListCreate(ia_na->options, option_len,
	    &err);
	if (ia_na_options == NULL)
		return (NULL);	/* no options */
	/* find the first ia_addr with non-zero lifetime */
	for (start_index = 0; ; start_index++) {
		DHCPv6OptionIAADDRRef ia_addr;

		ia_addr = (DHCPv6OptionIAADDRRef)(uintptr_t)
		    DHCPv6OptionListGetOptionDataAndLength(ia_na_options,
		    kDHCPv6OPTION_IAADDR, &option_len, &start_index);
		if (ia_addr == NULL ||
		    option_len < DHCPv6OptionIAADDR_MIN_LENGTH)
			break;	/* missing/invalid IAADDR option */
		valid_lifetime = DHCPv6OptionIAADDRGetValidLifetime(ia_addr);
		preferred_lifetime =
		    DHCPv6OptionIAADDRGetPreferredLifetime(ia_addr);
		if (valid_lifetime == 0) {
			continue;	/* lifetime is 0, skipping */
		} else if (preferred_lifetime > valid_lifetime) {
			break;		/* server is confused */
		}
		*ret_ia_addr = ia_addr;
		break;
	}
	DHCPv6OptionListRelease(&ia_na_options);

	/* if we didn't find a suitable IAADDR, then ignore the IA_NA */
	if (*ret_ia_addr == NULL)
		ia_na = NULL;
	(void)client;
	return (ia_na);
}

static uint8_t
get_preference_value_from_options(DHCPv6OptionListRef options)
{
	DHCPv6OptionPREFERENCERef pref;
	int option_len;

	pref = (DHCPv6OptionPREFERENCERef)(uintptr_t)
	    DHCPv6OptionListGetOptionDataAndLength(options,
	    kDHCPv6OPTION_PREFERENCE, &option_len, NULL);
	if (pref != NULL && option_len >= DHCPv6OptionPREFERENCE_MIN_LENGTH)
		return (pref->value);
	return (kDHCPv6OptionPREFERENCEMinValue);
}

#define	OUR_IA_NA_SIZE	(DHCPv6OptionIA_NA_MIN_LENGTH + \
	(int)DHCPV6_OPTION_HEADER_SIZE + DHCPv6OptionIAADDR_MIN_LENGTH)

static bool
add_ia_na_option(struct dhcpv6_client *client, DHCPv6OptionAreaRef oa_p,
    DHCPv6OptionErrorStringRef err_p)
{
	uint8_t buf[OUR_IA_NA_SIZE];
	DHCPv6OptionIA_NARef ia_na_p;
	DHCPv6OptionRef option;
	DHCPv6OptionIAADDRRef ia_addr_p;

	ia_na_p = (DHCPv6OptionIA_NARef)(void *)buf;
	DHCPv6OptionIA_NASetIAID(ia_na_p, DHCPIAIDGet(client->ifname));
	DHCPv6OptionIA_NASetT1(ia_na_p, 0);
	DHCPv6OptionIA_NASetT2(ia_na_p, 0);
	option = (DHCPv6OptionRef)(void *)(buf + DHCPv6OptionIA_NA_MIN_LENGTH);
	DHCPv6OptionSetCode(option, kDHCPv6OPTION_IAADDR);
	DHCPv6OptionSetLength(option, DHCPv6OptionIAADDR_MIN_LENGTH);
	ia_addr_p = (DHCPv6OptionIAADDRRef)(void *)
	    (((uint8_t *)option) + DHCPV6_OPTION_HEADER_SIZE);
	DHCPv6OptionIAADDRSetAddress(ia_addr_p,
	    DHCPv6OptionIAADDRGetAddress(client->ia_addr));
	DHCPv6OptionIAADDRSetPreferredLifetime(ia_addr_p, 0);
	DHCPv6OptionIAADDRSetValidLifetime(ia_addr_p, 0);
	return (DHCPv6OptionAreaAddOption(oa_p, kDHCPv6OPTION_IA_NA,
	    OUR_IA_NA_SIZE, ia_na_p, err_p));
}

/*
 * option_data_get_length(): an option's data is preceded by its length, the
 * last two bytes of its header.
 */
static int
option_data_get_length(const uint8_t *option_data)
{
	return ((int)net_uint16_get(option_data - sizeof(uint16_t)));
}

/* Apple's S_get_prefix_length(), from the RA's on-link prefixes. */
static int
S_get_prefix_length(const struct dhcpv6_client *client,
    const struct in6_addr *addr)
{
	unsigned i;

	for (i = 0; i < client->onlink_count; i++) {
		const struct ra_onlink_prefix *p = &client->onlink[i];
		int bytes = p->prefix_len / 8, bits = p->prefix_len % 8;

		if (p->prefix_len > 128 || p->prefix_len == 0)
			continue;
		if (memcmp(addr, &p->prefix, (size_t)bytes) != 0)
			continue;
		if (bits != 0 && ((addr->s6_addr[bytes] ^
		    p->prefix.s6_addr[bytes]) & (0xff << (8 - bits))) != 0)
			continue;
		return (p->prefix_len);
	}
	return (DHCPV6_PREFIX_LENGTH);
}

static void
DHCPv6ClientSetState(struct dhcpv6_client *client, DHCPv6ClientState cstate)
{
	static const char *const names[] = {
		"None", "Solicit", "Request", "Bound", "Renew", "Rebind",
		"Confirm", "Release", "Unbound", "Decline", "Inform", "Exit"
	};

	client->cstate = cstate;
	xlog("%s: DHCPv6 %s", client->ifname, names[cstate]);
}

static void
DHCPv6ClientPostNotification(struct dhcpv6_client *client)
{
	if (client->notify != NULL)
		client->notify(client->notify_arg, client);
}

/* Remove the leased address from the interface. */
static void
DHCPv6ClientRemoveAddress(struct dhcpv6_client *client, const char *label)
{
	struct in6_addr addr;

	(void)pthread_mutex_lock(&client->lock);
	addr = client->our_ip;
	(void)memset(&client->our_ip, 0, sizeof(client->our_ip));
	client->our_prefix_length = 0;
	client->addr_ready = false;
	(void)pthread_mutex_unlock(&client->lock);

	if (IN6_IS_ADDR_UNSPECIFIED(&addr))
		return;
	{
		char a[INET6_ADDRSTRLEN];

		(void)inet_ntop(AF_INET6, &addr, a, sizeof(a));
		xlog("%s: %s: removing %s", client->ifname, label, a);
	}
	(void)remove_v6_address(client->ifname, &addr);
}

/* DHCPv6ClientClearPacket(). */
static void
DHCPv6ClientClearPacket(struct dhcpv6_client *client)
{
	(void)pthread_mutex_lock(&client->lock);
	if (client->saved_len != 0) {
		(void)memset(&client->lease, 0, sizeof(client->lease));
		client->saved_len = 0;
		client->server_id = NULL;
		client->ia_na = NULL;
		client->ia_addr = NULL;
		client->saved_verified = false;
	}
	(void)pthread_mutex_unlock(&client->lock);
}

/*
 * Re-derive the pointers into `saved`. Caller holds no lock; `saved` only
 * changes on the client's thread.
 */
static void
DHCPv6ClientParseSaved(struct dhcpv6_client *client)
{
	DHCPv6OptionErrorString err;
	DHCPv6OptionListRef options;
	int option_len;

	client->server_id = NULL;
	client->ia_na = NULL;
	client->ia_addr = NULL;
	options = DHCPv6OptionListCreateWithPacket(
	    (DHCPv6PacketRef)(void *)client->saved, client->saved_len, &err);
	if (options == NULL)
		return;
	client->server_id = DHCPv6OptionListGetOptionDataAndLength(options,
	    kDHCPv6OPTION_SERVERID, &option_len, NULL);
	client->ia_na = get_ia_na_addr(client,
	    ((DHCPv6PacketRef)(void *)client->saved)->msg_type, options,
	    &client->ia_addr);
	DHCPv6OptionListRelease(&options);
}

/* DHCPv6ClientSavePacket(): the packet, then the lease it describes. */
static void
DHCPv6ClientSavePacket(struct dhcpv6_client *client, const uint8_t *pkt,
    int pkt_len)
{
	lease_info_t *lease_p = &client->lease;
	uint32_t preferred_lifetime, t1, t2, valid_lifetime;

	DHCPv6ClientClearPacket(client);
	(void)pthread_mutex_lock(&client->lock);
	(void)memcpy(client->saved, pkt, (size_t)pkt_len);
	client->saved_len = pkt_len;
	(void)pthread_mutex_unlock(&client->lock);
	DHCPv6ClientParseSaved(client);
	if (client->ia_na != NULL) {
		t1 = DHCPv6OptionIA_NAGetT1(client->ia_na);
		t2 = DHCPv6OptionIA_NAGetT2(client->ia_na);
		valid_lifetime =
		    DHCPv6OptionIAADDRGetValidLifetime(client->ia_addr);
		preferred_lifetime =
		    DHCPv6OptionIAADDRGetPreferredLifetime(client->ia_addr);
		if (preferred_lifetime == 0)
			preferred_lifetime = valid_lifetime;
		if (t1 == 0 || t2 == 0) {
			if (preferred_lifetime == DHCP_INFINITE_LEASE) {
				t1 = t2 = 0;
			} else {
				t1 = (uint32_t)(preferred_lifetime * 0.5);
				t2 = (uint32_t)(preferred_lifetime * 0.8);
			}
		} else if (t1 == DHCP_INFINITE_LEASE ||
		    t2 == DHCP_INFINITE_LEASE) {
			t1 = t2 = 0;
			preferred_lifetime = DHCP_INFINITE_LEASE;
			valid_lifetime = DHCP_INFINITE_LEASE;
		}
		lease_p->start = current_time();
		if (valid_lifetime == DHCP_INFINITE_LEASE) {
			lease_p->t1 = lease_p->t2 = 0;
			preferred_lifetime = DHCP_INFINITE_LEASE;
		} else {
			lease_p->t1 = t1;
			lease_p->t2 = t2;
		}
		lease_p->preferred_lifetime = preferred_lifetime;
		lease_p->valid_lifetime = valid_lifetime;
	}
	(void)pthread_mutex_lock(&client->lock);
	client->saved_verified = true;
	(void)pthread_mutex_unlock(&client->lock);
}

static bool
DHCPv6ClientLeaseStillValid(struct dhcpv6_client *client)
{
	lease_info_t *lease_p = &client->lease;

	if (!lease_p->valid)
		return (false);
	if (lease_p->valid_lifetime == DHCP_INFINITE_LEASE)
		return (true);
	if (current_time() - lease_p->start >= lease_p->valid_lifetime) {
		/* expired */
		DHCPv6ClientClearPacket(client);
		lease_p->valid = false;
	}
	return (lease_p->valid);
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

	if (client->sock == NULL)
		client->sock = dhcpv6_socket_create(client->ifname,
		    client->ifindex);
	if (client->sock == NULL)
		return;
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
DHCPv6ClientSendSolicit(struct dhcpv6_client *client)
{
	uint8_t buf[DHCPV6_PACKET_MAX];
	DHCPv6OptionErrorString err;
	DHCPv6OptionIA_NA ia_na;
	DHCPv6OptionArea oa;
	DHCPv6PacketRef pkt;

	pkt = DHCPv6ClientMakePacket(client, kDHCPv6MessageSOLICIT, buf,
	    sizeof(buf), &oa);
	if (pkt == NULL)
		return;
	DHCPv6OptionIA_NASetIAID(&ia_na, DHCPIAIDGet(client->ifname));
	DHCPv6OptionIA_NASetT1(&ia_na, 0);
	DHCPv6OptionIA_NASetT2(&ia_na, 0);
	if (!DHCPv6OptionAreaAddOption(&oa, kDHCPv6OPTION_IA_NA,
	    DHCPv6OptionIA_NA_MIN_LENGTH, &ia_na, &err)) {
		xlog("failed to add IA_NA, %s", err.str);
		return;
	}
	DHCPv6ClientTransmit(client, pkt, &oa, "Solicit");
}

/* DHCPv6ClientSendPacket(): the messages built from the saved reply. */
static void
DHCPv6ClientSendPacket(struct dhcpv6_client *client)
{
	uint8_t buf[DHCPV6_PACKET_MAX];
	DHCPv6OptionErrorString err;
	DHCPv6OptionArea oa;
	DHCPv6PacketRef pkt;
	int message_type;

	if (client->ia_na == NULL || client->server_id == NULL) {
		xlog("%s: SendPacket given NULLs", client->ifname);
		return;
	}
	switch (client->cstate) {
	case kDHCPv6ClientStateRequest:
		message_type = kDHCPv6MessageREQUEST;
		break;
	case kDHCPv6ClientStateRenew:
		message_type = kDHCPv6MessageRENEW;
		break;
	case kDHCPv6ClientStateRebind:
		message_type = kDHCPv6MessageREBIND;
		break;
	case kDHCPv6ClientStateRelease:
		message_type = kDHCPv6MessageRELEASE;
		break;
	case kDHCPv6ClientStateConfirm:
		message_type = kDHCPv6MessageCONFIRM;
		break;
	case kDHCPv6ClientStateDecline:
		message_type = kDHCPv6MessageDECLINE;
		break;
	default:
		return;
	}
	pkt = DHCPv6ClientMakePacket(client, message_type, buf, sizeof(buf),
	    &oa);
	if (pkt == NULL)
		return;
	if (message_type != kDHCPv6MessageREBIND &&
	    message_type != kDHCPv6MessageCONFIRM) {
		if (!DHCPv6OptionAreaAddOption(&oa, kDHCPv6OPTION_SERVERID,
		    option_data_get_length(client->server_id),
		    client->server_id, &err)) {
			xlog("failed to add SERVERID, %s", err.str);
			return;
		}
	}
	if (!add_ia_na_option(client, &oa, &err)) {
		xlog("failed to add IA_NA, %s", err.str);
		return;
	}
	DHCPv6ClientTransmit(client, pkt, &oa, "message");
}

static bool
client_interrupted(const struct dhcpv6_client *client)
{
	return (got_term || client->stop || client->restart ||
	    client->duplicated);
}

/* What a wait that ended early means. */
static DHCPv6ClientState
DHCPv6ClientInitialState(struct dhcpv6_client *client)
{
	if (!client->allocate)
		return (kDHCPv6ClientStateInform);
	if (DHCPv6ClientLeaseStillValid(client))
		return (kDHCPv6ClientStateConfirm);
	return (kDHCPv6ClientStateSolicit);
}

static DHCPv6ClientState
DHCPv6ClientNextEvent(struct dhcpv6_client *client)
{
	if (got_term || client->stop)
		return (kDHCPv6ClientStateExit);
	if (client->duplicated) {
		client->duplicated = 0;
		return (kDHCPv6ClientStateDecline);
	}
	client->restart = 0;
	return (DHCPv6ClientInitialState(client));
}

/*
 * Wait up to `wait_s` for a message of `msg_type` matching this client's
 * transaction and DUID. Returns its length, 0 on timeout, -1 if interrupted.
 */
static int
wait_for_message(struct dhcpv6_client *client, double wait_s, int msg_type,
    uint8_t *pkt, size_t size)
{
	double deadline = current_time() + wait_s;

	if (client->sock == NULL)
		client->sock = dhcpv6_socket_create(client->ifname,
		    client->ifindex);
	for (;;) {
		DHCPv6OptionErrorString err;
		DHCPv6OptionListRef options;
		double left;
		unsigned ms;
		bool matches;
		int len;

		if (client_interrupted(client))
			return (-1);
		left = deadline - current_time();
		if (left <= 0)
			return (0);
		ms = (unsigned)(left * 1000) + 1;
		if (ms > DHCPV6_WAIT_SLICE_MS)
			ms = DHCPV6_WAIT_SLICE_MS;
		if (client->sock == NULL) {
			struct timespec ts = { 0, (long)ms * 1000000L };

			(void)nanosleep(&ts, NULL);
			continue;
		}
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
		matches = ((DHCPv6PacketRef)(void *)pkt)->msg_type == msg_type &&
		    DHCPv6PacketGetTransactionID((DHCPv6PacketRef)(void *)pkt) ==
		    client->transaction_id && S_duid_matches(options);
		DHCPv6OptionListRelease(&options);
		if (matches)
			return (len);
	}
}

/* Let the DHCPv6 socket go during a long wait, as Apple's receive-disable. */
static void
client_release_socket(struct dhcpv6_client *client)
{
	dhcpv6_socket_release(&client->sock);
}

/* Wait out `wait_s` (forever if negative). False if interrupted. */
static bool
client_idle(struct dhcpv6_client *client, double wait_s)
{
	double deadline = current_time() + wait_s;

	client_release_socket(client);
	while (!client_interrupted(client)) {
		struct timespec ts = { 0, 200 * 1000000L };

		if (wait_s >= 0 && current_time() >= deadline)
			return (true);
		(void)nanosleep(&ts, NULL);
	}
	return (false);
}

/* Is a Reply's server identifier present and valid? */
static bool
reply_server_ok(DHCPv6OptionListRef options)
{
	const uint8_t *server_id;
	int option_len;

	server_id = DHCPv6OptionListGetOptionDataAndLength(options,
	    kDHCPv6OPTION_SERVERID, &option_len, NULL);
	return (server_id != NULL && DHCPDUIDIsValid(
	    (DHCPDUIDRef)(uintptr_t)server_id, option_len));
}

/*
 * The Solicit and Request states' checks on an answer: a valid server
 * identifier, no NoAddrsAvail ("must ignore it"), and a usable IA_NA.
 */
static bool
answer_ok(struct dhcpv6_client *client, DHCPv6OptionListRef options,
    int msg_type)
{
	const DHCPv6OptionSTATUS_CODE *status_code;
	DHCPv6OptionIAADDRRef ia_addr;
	int option_len;

	if (!reply_server_ok(options))
		return (false);	/* missing/invalid DUID */
	status_code = (const DHCPv6OptionSTATUS_CODE *)
	    DHCPv6OptionListGetOptionDataAndLength(options,
	    kDHCPv6OPTION_STATUS_CODE, &option_len, NULL);
	if (status_code != NULL) {
		if (option_len < DHCPv6OptionSTATUS_CODE_MIN_LENGTH)
			return (false);	/* too short */
		if (DHCPv6OptionSTATUS_CODEGetCode(
		    (DHCPv6OptionSTATUS_CODERef)(uintptr_t)status_code) ==
		    kDHCPv6StatusCodeNoAddrsAvail)
			return (false);	/* must ignore it */
	}
	return (get_ia_na_addr(client, msg_type, options, &ia_addr) != NULL);
}

/*
 * The Stateless reply. No limit on tries (DHCPv6_INF_MAX_RT only caps the
 * timeout), so with no server this keeps quietly trying until the link goes
 * down and the service stops the client.
 */
static DHCPv6ClientState
DHCPv6Client_Inform(struct dhcpv6_client *client)
{
	uint8_t pkt[DHCPV6_PACKET_MAX];
	int len;

	DHCPv6ClientSetState(client, kDHCPv6ClientStateInform);
	DHCPv6ClientClearPacket(client);
	DHCPv6ClientClearRetransmit(client);
	client->transaction_id = get_new_transaction_id();
	len = wait_for_message(client,
	    random_double_in_range(0, DHCPv6_INF_MAX_DELAY),
	    kDHCPv6MessageREPLY, pkt, sizeof(pkt));
	for (;;) {
		double wait_s;

		if (len < 0)
			return (DHCPv6ClientNextEvent(client));
		if (client->try == 0)
			client->start_time = current_time();
		wait_s = DHCPv6ClientNextRetransmit(client, DHCPv6_INF_TIMEOUT,
		    DHCPv6_INF_MAX_RT);
		DHCPv6ClientSendInform(client);
		len = wait_for_message(client, wait_s, kDHCPv6MessageREPLY, pkt,
		    sizeof(pkt));
		if (len > 0)
			break;
	}
	xlog("%s: DHCPv6 Reply received (try=%d)", client->ifname, client->try);
	DHCPv6ClientSavePacket(client, pkt, len);
	DHCPv6ClientPostNotification(client);
	/* Nothing more until a restart or a stop. */
	(void)client_idle(client, -1);
	return (DHCPv6ClientNextEvent(client));
}

static DHCPv6ClientState
DHCPv6Client_Solicit(struct dhcpv6_client *client)
{
	uint8_t pkt[DHCPV6_PACKET_MAX];
	int len;

	DHCPv6ClientSetState(client, kDHCPv6ClientStateSolicit);
	DHCPv6ClientClearRetransmit(client);
	DHCPv6ClientClearPacket(client);
	client->transaction_id = get_new_transaction_id();
	len = wait_for_message(client,
	    random_double_in_range(0, DHCPv6_SOL_MAX_DELAY),
	    kDHCPv6MessageADVERTISE, pkt, sizeof(pkt));
	for (;;) {
		double deadline;

		if (len < 0)
			return (DHCPv6ClientNextEvent(client));
		if (client->try == 0)
			client->start_time = current_time();
		/* we've got a packet */
		if (client->saved_len != 0)
			return (kDHCPv6ClientStateRequest);
		deadline = current_time() + DHCPv6ClientNextRetransmit(client,
		    DHCPv6_SOL_TIMEOUT, DHCPv6_SOL_MAX_RT);
		DHCPv6ClientSendSolicit(client);
		/*
		 * Collect Advertise messages until the timer: keep the best,
		 * and go straight to Request on the highest preference.
		 */
		for (;;) {
			DHCPv6OptionErrorString err;
			DHCPv6OptionListRef options, saved_options;
			uint8_t pref, saved_pref;
			double left = deadline - current_time();

			if (left <= 0) {
				len = 0;
				break;
			}
			len = wait_for_message(client, left,
			    kDHCPv6MessageADVERTISE, pkt, sizeof(pkt));
			if (len <= 0)
				break;
			options = DHCPv6OptionListCreateWithPacket(
			    (DHCPv6PacketRef)(void *)pkt, len, &err);
			if (options == NULL)
				continue;
			if (!answer_ok(client, options,
			    kDHCPv6MessageADVERTISE)) {
				DHCPv6OptionListRelease(&options);
				continue;
			}
			/* check for a server preference value */
			pref = get_preference_value_from_options(options);
			DHCPv6OptionListRelease(&options);
			if (client->saved_len != 0) {
				saved_options = DHCPv6OptionListCreateWithPacket(
				    (DHCPv6PacketRef)(void *)client->saved,
				    client->saved_len, &err);
				if (saved_options != NULL) {
					saved_pref =
					    get_preference_value_from_options(
					    saved_options);
					DHCPv6OptionListRelease(&saved_options);
					if (saved_pref >= pref)
						continue;
				}
			}
			DHCPv6ClientSavePacket(client, pkt, len);
			if (pref == kDHCPv6OptionPREFERENCEMaxValue)
				return (kDHCPv6ClientStateRequest);
		}
	}
}

static DHCPv6ClientState
DHCPv6Client_Request(struct dhcpv6_client *client)
{
	uint8_t pkt[DHCPV6_PACKET_MAX];
	double deadline;
	int len;

	DHCPv6ClientSetState(client, kDHCPv6ClientStateRequest);
	DHCPv6ClientClearRetransmit(client);
	client->transaction_id = get_new_transaction_id();
	for (;;) {
		if (client->try >= DHCPv6_REQ_MAX_RC)
			return (kDHCPv6ClientStateSolicit);	/* back */
		deadline = current_time() + DHCPv6ClientNextRetransmit(client,
		    DHCPv6_REQ_TIMEOUT, DHCPv6_REQ_MAX_RT);
		DHCPv6ClientSendPacket(client);
		for (;;) {
			DHCPv6OptionErrorString err;
			DHCPv6OptionListRef options;
			double left = deadline - current_time();
			bool ok;

			if (left <= 0)
				break;
			len = wait_for_message(client, left,
			    kDHCPv6MessageREPLY, pkt, sizeof(pkt));
			if (len < 0)
				return (DHCPv6ClientNextEvent(client));
			if (len == 0)
				break;
			options = DHCPv6OptionListCreateWithPacket(
			    (DHCPv6PacketRef)(void *)pkt, len, &err);
			if (options == NULL)
				continue;
			ok = answer_ok(client, options, kDHCPv6MessageREPLY);
			DHCPv6OptionListRelease(&options);
			if (!ok)
				continue;
			DHCPv6ClientSavePacket(client, pkt, len);
			return (kDHCPv6ClientStateBound);
		}
	}
}

/*
 * After the address is ready: wait for T1 and go to Renew, or idle if the
 * lease is infinite (Apple's Bound and HandleAddressChanged, past DAD).
 */
static DHCPv6ClientState
client_wait_t1(struct dhcpv6_client *client)
{
	if (client->lease.valid_lifetime != DHCP_INFINITE_LEASE) {
		double since = current_time() - client->lease.start;
		double t1 = client->lease.t1;

		t1 = (since < t1) ? t1 - since : 10; /* wakeup in 10 seconds */
		if (client_idle(client, t1))
			return (kDHCPv6ClientStateRenew);
	} else {
		(void)client_idle(client, -1);
	}
	return (DHCPv6ClientNextEvent(client));
}

static DHCPv6ClientState
DHCPv6Client_Bound(struct dhcpv6_client *client)
{
	struct in6_addr our_ip, old_ip;
	uint32_t preferred_lifetime, valid_lifetime;
	double time_since_start = 0;
	int prefix_length;
	bool same_address = false, have_old;
	char a[INET6_ADDRSTRLEN];

	(void)memcpy(&our_ip, DHCPv6OptionIAADDRGetAddress(client->ia_addr),
	    sizeof(our_ip));
	DHCPv6ClientSetState(client, kDHCPv6ClientStateBound);
	client->lease.valid = true;
	(void)pthread_mutex_lock(&client->lock);
	client->saved_verified = true;
	old_ip = client->our_ip;
	(void)pthread_mutex_unlock(&client->lock);

	valid_lifetime = client->lease.valid_lifetime;
	preferred_lifetime = client->lease.preferred_lifetime;
	if (valid_lifetime != DHCP_INFINITE_LEASE) {
		time_since_start = current_time() - client->lease.start;
		if (time_since_start >= client->lease.valid_lifetime)
			return (kDHCPv6ClientStateUnbound);	/* expired */
		/* reduce the time left by the amount that's elapsed already */
		valid_lifetime -= (uint32_t)time_since_start;
		if (time_since_start < preferred_lifetime)
			preferred_lifetime -= (uint32_t)time_since_start;
		else
			preferred_lifetime = 0;
	}

	/* if the address has changed, remove the old first */
	have_old = !IN6_IS_ADDR_UNSPECIFIED(&old_ip);
	if (have_old) {
		if (IN6_ARE_ADDR_EQUAL(&old_ip, &our_ip))
			same_address = true;
		else
			DHCPv6ClientRemoveAddress(client, "Bound");
	}
	prefix_length = S_get_prefix_length(client, &our_ip);
	(void)inet_ntop(AF_INET6, &our_ip, a, sizeof(a));
	xlog("%s: DHCPv6 setting %s/%d valid %u preferred %u", client->ifname, a,
	    prefix_length, valid_lifetime, preferred_lifetime);
	if (install_v6_dhcp_address(client->ifname, &our_ip,
	    (uint8_t)prefix_length, valid_lifetime, preferred_lifetime) != 0) {
		/* Apple sets no timer here either; a restart retries. */
		(void)client_idle(client, -1);
		return (DHCPv6ClientNextEvent(client));
	}
	if (same_address) {
		DHCPv6ClientPostNotification(client);
		return (client_wait_t1(client));
	}

	(void)pthread_mutex_lock(&client->lock);
	client->our_ip = our_ip;
	client->our_prefix_length = prefix_length;
	client->addr_ready = false;
	(void)pthread_mutex_unlock(&client->lock);

	/* Hold the publish for DAD, as for a SLAAC address. */
	if (sc_v6_dad_wait(client->ifname, &our_ip,
	    v6_dad_budget_ms(client->ifname)) == SC_V6_DAD_DUPLICATED) {
		xlog("%s: DHCPv6: %s is a duplicate", client->ifname, a);
		return (kDHCPv6ClientStateDecline);
	}
	(void)pthread_mutex_lock(&client->lock);
	client->addr_ready = true;
	(void)pthread_mutex_unlock(&client->lock);
	DHCPv6ClientPostNotification(client);
	return (client_wait_t1(client));
}

static DHCPv6ClientState
DHCPv6Client_Unbound(struct dhcpv6_client *client)
{
	DHCPv6ClientSetState(client, kDHCPv6ClientStateUnbound);
	DHCPv6ClientRemoveAddress(client, "Unbound");
	client->lease.valid = false;
	DHCPv6ClientClearPacket(client);
	DHCPv6ClientPostNotification(client);
	return (kDHCPv6ClientStateSolicit);
}

static DHCPv6ClientState
DHCPv6Client_Decline(struct dhcpv6_client *client)
{
	uint8_t pkt[DHCPV6_PACKET_MAX];
	int len;

	DHCPv6ClientSetState(client, kDHCPv6ClientStateDecline);
	client->duplicated = 0;
	DHCPv6ClientRemoveAddress(client, "Decline");
	client->lease.valid = false;
	(void)pthread_mutex_lock(&client->lock);
	client->saved_verified = false;
	(void)pthread_mutex_unlock(&client->lock);
	DHCPv6ClientPostNotification(client);
	DHCPv6ClientClearRetransmit(client);
	client->transaction_id = get_new_transaction_id();
	for (;;) {
		DHCPv6OptionErrorString err;
		DHCPv6OptionListRef options;
		double deadline;
		bool ok;

		if (client->try >= DHCPv6_DEC_MAX_RC)
			return (kDHCPv6ClientStateSolicit);	/* back */
		deadline = current_time() + DHCPv6ClientNextRetransmit(client,
		    DHCPv6_DEC_TIMEOUT, 0);
		DHCPv6ClientSendPacket(client);
		for (;;) {
			double left = deadline - current_time();

			if (left <= 0)
				break;
			len = wait_for_message(client, left,
			    kDHCPv6MessageREPLY, pkt, sizeof(pkt));
			if (len < 0)
				return (DHCPv6ClientNextEvent(client));
			if (len == 0)
				break;
			options = DHCPv6OptionListCreateWithPacket(
			    (DHCPv6PacketRef)(void *)pkt, len, &err);
			if (options == NULL)
				continue;
			ok = reply_server_ok(options);
			DHCPv6OptionListRelease(&options);
			if (ok)
				return (kDHCPv6ClientStateSolicit);
		}
	}
}

static DHCPv6ClientState
DHCPv6Client_RenewRebind(struct dhcpv6_client *client)
{
	uint8_t pkt[DHCPV6_PACKET_MAX];

	DHCPv6ClientSetState(client, kDHCPv6ClientStateRenew);
	DHCPv6ClientClearRetransmit(client);
	client->start_time = current_time();
	client->transaction_id = get_new_transaction_id();
	for (;;) {
		double deadline, time_since_start, wait_time, now = current_time();

		time_since_start = now - client->lease.start;
		if (time_since_start >= client->lease.valid_lifetime)
			return (kDHCPv6ClientStateUnbound);	/* expired */

		if (time_since_start < client->lease.t2) {
			double time_until_t2;

			/* Renew (before T2) */
			wait_time = DHCPv6ClientNextRetransmit(client,
			    DHCPv6_REN_TIMEOUT, DHCPv6_REN_MAX_RT);
			time_until_t2 = client->lease.t2 - time_since_start;
			if (wait_time > time_until_t2)
				wait_time = time_until_t2;
		} else {
			double time_until_expiration;

			/* Rebind (T2 or later) */
			if (client->cstate != kDHCPv6ClientStateRebind) {
				/* switch to Rebind */
				DHCPv6ClientSetState(client,
				    kDHCPv6ClientStateRebind);
				DHCPv6ClientClearRetransmit(client);
			}
			wait_time = DHCPv6ClientNextRetransmit(client,
			    DHCPv6_REB_TIMEOUT, DHCPv6_REB_MAX_RT);
			time_until_expiration = client->lease.valid_lifetime -
			    time_since_start;
			if (wait_time > time_until_expiration)
				wait_time = time_until_expiration;
		}
		deadline = now + wait_time;
		DHCPv6ClientSendPacket(client);
		for (;;) {
			DHCPv6OptionErrorString err;
			DHCPv6OptionListRef options;
			const DHCPv6OptionSTATUS_CODE *status_code;
			DHCPv6OptionIAADDRRef ia_addr;
			double left = deadline - current_time();
			int len, option_len;

			if (left <= 0)
				break;
			len = wait_for_message(client, left,
			    kDHCPv6MessageREPLY, pkt, sizeof(pkt));
			if (len < 0)
				return (DHCPv6ClientNextEvent(client));
			if (len == 0)
				break;
			options = DHCPv6OptionListCreateWithPacket(
			    (DHCPv6PacketRef)(void *)pkt, len, &err);
			if (options == NULL)
				continue;
			if (!reply_server_ok(options)) {
				DHCPv6OptionListRelease(&options);
				continue;	/* missing/invalid DUID */
			}
			status_code = (const DHCPv6OptionSTATUS_CODE *)
			    DHCPv6OptionListGetOptionDataAndLength(options,
			    kDHCPv6OPTION_STATUS_CODE, &option_len, NULL);
			if (status_code != NULL) {
				if (option_len <
				    DHCPv6OptionSTATUS_CODE_MIN_LENGTH) {
					DHCPv6OptionListRelease(&options);
					continue;	/* too short */
				}
				if (DHCPv6OptionSTATUS_CODEGetCode(
				    (DHCPv6OptionSTATUS_CODERef)(uintptr_t)
				    status_code) != kDHCPv6StatusCodeSuccess) {
					DHCPv6OptionListRelease(&options);
					return (kDHCPv6ClientStateUnbound);
				}
			}
			if (get_ia_na_addr(client, kDHCPv6MessageREPLY, options,
			    &ia_addr) == NULL) {
				DHCPv6OptionListRelease(&options);
				return (kDHCPv6ClientStateUnbound);
			}
			DHCPv6OptionListRelease(&options);
			DHCPv6ClientSavePacket(client, pkt, len);
			return (kDHCPv6ClientStateBound);
		}
	}
}

static DHCPv6ClientState
DHCPv6Client_Confirm(struct dhcpv6_client *client)
{
	uint8_t pkt[DHCPV6_PACKET_MAX];
	int len;

	DHCPv6ClientSetState(client, kDHCPv6ClientStateConfirm);
	DHCPv6ClientClearRetransmit(client);
	(void)pthread_mutex_lock(&client->lock);
	client->saved_verified = false;
	(void)pthread_mutex_unlock(&client->lock);
	client->transaction_id = get_new_transaction_id();
	len = wait_for_message(client,
	    random_double_in_range(0, DHCPv6_CNF_MAX_DELAY),
	    kDHCPv6MessageREPLY, pkt, sizeof(pkt));
	for (;;) {
		double deadline;

		if (len < 0)
			return (DHCPv6ClientNextEvent(client));
		if (client->try == 0) {
			client->start_time = current_time();
		} else if (current_time() - client->start_time >=
		    DHCPv6_CNF_MAX_RD) {
			if (DHCPv6ClientLeaseStillValid(client))
				return (kDHCPv6ClientStateBound);
			return (kDHCPv6ClientStateSolicit);
		}
		deadline = current_time() + DHCPv6ClientNextRetransmit(client,
		    DHCPv6_CNF_TIMEOUT, DHCPv6_CNF_MAX_RT);
		DHCPv6ClientSendPacket(client);
		for (;;) {
			DHCPv6OptionErrorString err;
			DHCPv6OptionListRef options;
			const DHCPv6OptionSTATUS_CODE *status_code;
			double left = deadline - current_time();
			int option_len;

			if (left <= 0) {
				len = 0;
				break;
			}
			len = wait_for_message(client, left,
			    kDHCPv6MessageREPLY, pkt, sizeof(pkt));
			if (len <= 0)
				break;
			options = DHCPv6OptionListCreateWithPacket(
			    (DHCPv6PacketRef)(void *)pkt, len, &err);
			if (options == NULL)
				continue;
			if (!reply_server_ok(options)) {
				DHCPv6OptionListRelease(&options);
				continue;	/* missing/invalid DUID */
			}
			status_code = (const DHCPv6OptionSTATUS_CODE *)
			    DHCPv6OptionListGetOptionDataAndLength(options,
			    kDHCPv6OPTION_STATUS_CODE, &option_len, NULL);
			if (status_code == NULL ||
			    option_len < DHCPv6OptionSTATUS_CODE_MIN_LENGTH) {
				DHCPv6OptionListRelease(&options);
				continue;
			}
			if (DHCPv6OptionSTATUS_CODEGetCode(
			    (DHCPv6OptionSTATUS_CODERef)(uintptr_t)status_code)
			    != kDHCPv6StatusCodeSuccess) {
				DHCPv6OptionListRelease(&options);
				return (kDHCPv6ClientStateUnbound);
			}
			DHCPv6OptionListRelease(&options);
			return (kDHCPv6ClientStateBound);
		}
	}
}

/* A lease kept from before a link bounce on this interface, if any. */
static void
retained_restore(struct dhcpv6_client *client)
{
	int i;

	(void)pthread_mutex_lock(&g_retained_lock);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (strcmp(g_retained[i].ifname, client->ifname) != 0)
			continue;
		(void)memcpy(client->saved, g_retained[i].saved,
		    (size_t)g_retained[i].saved_len);
		client->saved_len = g_retained[i].saved_len;
		client->lease = g_retained[i].lease;
		g_retained[i].ifname[0] = '\0';
		break;
	}
	(void)pthread_mutex_unlock(&g_retained_lock);
	if (client->saved_len != 0)
		DHCPv6ClientParseSaved(client);
}

static void
retained_save(struct dhcpv6_client *client)
{
	int i, slot = -1;

	(void)pthread_mutex_lock(&g_retained_lock);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (strcmp(g_retained[i].ifname, client->ifname) == 0) {
			slot = i;
			break;
		}
		if (slot < 0 && g_retained[i].ifname[0] == '\0')
			slot = i;
	}
	if (slot >= 0) {
		(void)strlcpy(g_retained[slot].ifname, client->ifname,
		    sizeof(g_retained[slot].ifname));
		(void)memcpy(g_retained[slot].saved, client->saved,
		    (size_t)client->saved_len);
		g_retained[slot].saved_len = client->saved_len;
		g_retained[slot].lease = client->lease;
	}
	(void)pthread_mutex_unlock(&g_retained_lock);
}

static void
retained_forget(const char *ifname)
{
	int i;

	(void)pthread_mutex_lock(&g_retained_lock);
	for (i = 0; i < BOUND_MAX_IF; i++) {
		if (strcmp(g_retained[i].ifname, ifname) == 0)
			g_retained[i].ifname[0] = '\0';
	}
	(void)pthread_mutex_unlock(&g_retained_lock);
}

static void *
dhcpv6_client_main(void *arg)
{
	struct dhcpv6_client *client = arg;
	DHCPv6ClientState cstate;

	if (client->allocate)
		retained_restore(client);
	else
		retained_forget(client->ifname);
	cstate = DHCPv6ClientInitialState(client);
	while (cstate != kDHCPv6ClientStateExit) {
		switch (cstate) {
		case kDHCPv6ClientStateInform:
			cstate = DHCPv6Client_Inform(client);
			break;
		case kDHCPv6ClientStateSolicit:
			cstate = DHCPv6Client_Solicit(client);
			break;
		case kDHCPv6ClientStateRequest:
			cstate = DHCPv6Client_Request(client);
			break;
		case kDHCPv6ClientStateBound:
			cstate = DHCPv6Client_Bound(client);
			break;
		case kDHCPv6ClientStateRenew:
		case kDHCPv6ClientStateRebind:
			cstate = DHCPv6Client_RenewRebind(client);
			break;
		case kDHCPv6ClientStateConfirm:
			cstate = DHCPv6Client_Confirm(client);
			break;
		case kDHCPv6ClientStateUnbound:
			cstate = DHCPv6Client_Unbound(client);
			break;
		case kDHCPv6ClientStateDecline:
			cstate = DHCPv6Client_Decline(client);
			break;
		default:
			cstate = kDHCPv6ClientStateExit;
			break;
		}
	}
	client_release_socket(client);
	return (NULL);
}

static void
client_set_onlink(struct dhcpv6_client *client, const struct ra_info *ra)
{
	unsigned i;

	client->onlink_count = ra->onlink_count;
	for (i = 0; i < ra->onlink_count; i++)
		client->onlink[i] = ra->onlink[i];
}

struct dhcpv6_client *
dhcpv6_client_start(const char *ifname, unsigned ifindex, bool allocate,
    const struct ra_info *ra, dhcpv6_client_notify_t notify, void *arg)
{
	struct dhcpv6_client *client;
	int rc;

	client = calloc(1, sizeof(*client));
	if (client == NULL)
		return (NULL);
	(void)strlcpy(client->ifname, ifname, sizeof(client->ifname));
	client->ifindex = ifindex;
	client->allocate = allocate;
	client->notify = notify;
	client->notify_arg = arg;
	client_set_onlink(client, ra);
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

bool
dhcpv6_client_is_stateful(const struct dhcpv6_client *client)
{
	return (client->allocate);
}

void
dhcpv6_client_restart(struct dhcpv6_client *client, const struct ra_info *ra)
{
	(void)pthread_mutex_lock(&client->lock);
	client_set_onlink(client, ra);
	(void)pthread_mutex_unlock(&client->lock);
	client->restart = 1;
}

void
dhcpv6_client_stop(struct dhcpv6_client **client_p, dhcpv6_stop_t how)
{
	struct dhcpv6_client *client = *client_p;

	if (client == NULL)
		return;
	*client_p = NULL;
	client->stop = 1;
	(void)pthread_join(client->tid, NULL);

	/* The thread has gone; this one owns the client's state now. */
	switch (how) {
	case DHCPV6_STOP_LINK:
		DHCPv6ClientRemoveAddress(client, "Stop");
		if (client->allocate && DHCPv6ClientLeaseStillValid(client))
			retained_save(client);
		break;
	case DHCPV6_STOP_RELEASE:
		retained_forget(client->ifname);
		if (client->allocate && DHCPv6ClientLeaseStillValid(client)) {
			DHCPv6ClientSetState(client, kDHCPv6ClientStateRelease);
			DHCPv6ClientRemoveAddress(client, "Release");
			DHCPv6ClientClearRetransmit(client);
			client->transaction_id = get_new_transaction_id();
			DHCPv6ClientSendPacket(client);
			/*
			 * We're supposed to wait for a Reply, but as on
			 * Apple's a stop has to be synchronous.
			 */
			client_release_socket(client);
		} else {
			DHCPv6ClientRemoveAddress(client, "Stop");
		}
		break;
	case DHCPV6_STOP_KEEP:
		break;
	}
	(void)pthread_mutex_destroy(&client->lock);
	free(client);
}

bool
dhcpv6_client_copy_packet(struct dhcpv6_client *client, void *buf, size_t size,
    size_t *len)
{
	bool ok = false;

	(void)pthread_mutex_lock(&client->lock);
	if (client->saved_len > 0 && client->saved_verified &&
	    (size_t)client->saved_len <= size) {
		(void)memcpy(buf, client->saved, (size_t)client->saved_len);
		*len = (size_t)client->saved_len;
		ok = true;
	}
	(void)pthread_mutex_unlock(&client->lock);
	return (ok);
}

bool
dhcpv6_client_copy_address(struct dhcpv6_client *client, struct in6_addr *addr,
    uint8_t *prefix_len)
{
	bool ok;

	(void)pthread_mutex_lock(&client->lock);
	ok = client->addr_ready;
	if (ok) {
		*addr = client->our_ip;
		*prefix_len = (uint8_t)client->our_prefix_length;
	}
	(void)pthread_mutex_unlock(&client->lock);
	return (ok);
}

bool
dhcpv6_client_on_duplicated(struct dhcpv6_client *client,
    const struct in6_addr *addr)
{
	bool ours;

	(void)pthread_mutex_lock(&client->lock);
	ours = !IN6_IS_ADDR_UNSPECIFIED(&client->our_ip) &&
	    IN6_ARE_ADDR_EQUAL(&client->our_ip, addr);
	(void)pthread_mutex_unlock(&client->lock);
	if (ours)
		client->duplicated = 1;
	return (ours);
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
