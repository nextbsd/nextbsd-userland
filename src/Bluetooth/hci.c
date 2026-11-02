/*
 * hci.c — bluetoothd's raw HCI socket. See hci.h.
 *
 * The socket set-up is hccontrol's socket_open() (usr.sbin/bluetooth/
 * hccontrol/hccontrol.c): bind and connect to the node, then a filter. The
 * difference is that bluetoothd never blocks on it: hccontrol's hci_request()
 * sends and then select()s for the matching Command Complete, which would
 * stall the daemon for a slow controller; here the reply is just another event
 * the main loop reads.
 */
#include "hci.h"
#include "hci_compat.h"

#include <sys/ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint64_t
bt_now_ms(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

int
hci_open(const char *hci_node)
{
	struct ng_btsocket_hci_raw_filter filter;
	struct sockaddr_hci addr;
	int fd, i;

	fd = socket(PF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
	    BLUETOOTH_PROTO_HCI);
	if (fd < 0)
		return (-1);

	(void)memset(&addr, 0, sizeof(addr));
	addr.hci_len = sizeof(addr);
	addr.hci_family = AF_BLUETOOTH;
	(void)strlcpy(addr.hci_node, hci_node, sizeof(addr.hci_node));
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		goto fail;

	/*
	 * Events only, all of them that the filter can name (codes 1..64).
	 * The engines pick what they want out of the stream.
	 */
	(void)memset(&filter, 0, sizeof(filter));
	bit_set(filter.packet_mask, NG_HCI_EVENT_PKT - 1);
	for (i = 0; i < NG_HCI_EVENT_MASK_SIZE * 8; i++)
		bit_set(filter.event_mask, i);
	if (setsockopt(fd, SOL_HCI_RAW, SO_HCI_RAW_FILTER, &filter,
	    sizeof(filter)) < 0)
		goto fail;
	return (fd);

fail:
	i = errno;
	(void)close(fd);
	errno = i;
	return (-1);
}

int
hci_send_cmd(int fd, uint16_t opcode, const void *params, size_t len)
{
	uint8_t buf[sizeof(ng_hci_cmd_pkt_t) + NG_HCI_CMD_PKT_SIZE];
	ng_hci_cmd_pkt_t *c = (ng_hci_cmd_pkt_t *)buf;

	if (len > NG_HCI_CMD_PKT_SIZE) {
		errno = EMSGSIZE;
		return (-1);
	}
	c->type = NG_HCI_CMD_PKT;
	c->opcode = htole16(opcode);
	c->length = (uint8_t)len;
	if (len > 0)
		(void)memcpy(buf + sizeof(*c), params, len);

	for (;;) {
		if (send(fd, buf, sizeof(*c) + len, 0) >= 0)
			return (0);
		if (errno != EINTR)
			return (-1);
	}
}

int
hci_recv_event(int fd, uint8_t *buf, size_t buflen, struct hci_event *ev)
{
	ng_hci_event_pkt_t *e = (ng_hci_event_pkt_t *)buf;
	ssize_t n;

	for (;;) {
		n = recv(fd, buf, buflen, 0);
		if (n >= 0)
			break;
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return (0);
		return (-1);
	}
	if ((size_t)n < sizeof(*e) || e->type != NG_HCI_EVENT_PKT ||
	    (size_t)n < sizeof(*e) + e->length)
		return (0);	/* not an event, or truncated: skip it */
	ev->code = e->event;
	ev->len = e->length;
	ev->params = buf + sizeof(*e);
	return (1);
}

bool
hci_cmd_result(const struct hci_event *ev, uint16_t *opcode, uint8_t *status,
    const uint8_t **rp, size_t *rplen)
{
	*rp = NULL;
	*rplen = 0;

	if (ev->code == NG_HCI_EVENT_COMMAND_COMPL) {
		const ng_hci_command_compl_ep *cc;

		if (ev->len < sizeof(*cc))
			return (false);
		cc = (const ng_hci_command_compl_ep *)ev->params;
		*opcode = le16toh(cc->opcode);
		if (ev->len > sizeof(*cc)) {
			*status = ev->params[sizeof(*cc)];
			*rp = ev->params + sizeof(*cc) + 1;
			*rplen = ev->len - sizeof(*cc) - 1;
		} else
			*status = 0;	/* a command with no return parameters */
		return (*opcode != 0);	/* opcode 0: a credit update only */
	}
	if (ev->code == NG_HCI_EVENT_COMMAND_STATUS) {
		const ng_hci_command_status_ep *cs;

		if (ev->len < sizeof(*cs))
			return (false);
		cs = (const ng_hci_command_status_ep *)ev->params;
		*opcode = le16toh(cs->opcode);
		*status = cs->status;
		return (*opcode != 0);
	}
	return (false);
}

int
hci_node_init(int fd)
{
	return (ioctl(fd, SIOC_HCI_RAW_NODE_INIT) < 0 ? -1 : 0);
}

int
hci_node_state(int fd)
{
	struct ng_btsocket_hci_raw_node_state r;

	(void)memset(&r, 0, sizeof(r));
	if (ioctl(fd, SIOC_HCI_RAW_NODE_GET_STATE, &r) < 0)
		return (-1);
	return ((int)r.state);
}

/* ng_hci's connection list: one ioctl, NG_HCI_MAX_CON_NUM entries at most. */
static int
con_list(int fd, ng_hci_node_con_ep **out)
{
	struct ng_btsocket_hci_raw_con_list r;

	*out = NULL;
	(void)memset(&r, 0, sizeof(r));
	r.num_connections = NG_HCI_MAX_CON_NUM;
	r.connections = calloc(NG_HCI_MAX_CON_NUM, sizeof(ng_hci_node_con_ep));
	if (r.connections == NULL)
		return (-1);
	if (ioctl(fd, SIOC_HCI_RAW_NODE_GET_CON_LIST, &r) < 0) {
		free(r.connections);
		return (-1);
	}
	*out = r.connections;
	return ((int)r.num_connections);
}

int
hci_acl_handle(int fd, const struct bt_addr *peer)
{
	ng_hci_node_con_ep *c;
	uint8_t wire[BT_ADDR_LEN];
	int i, n, h = -1;

	n = con_list(fd, &c);
	if (n < 0)
		return (-1);
	hci_addr_to_wire(peer, wire);
	for (i = 0; i < n; i++) {
		if (c[i].link_type == NG_HCI_LINK_ACL &&
		    memcmp(&c[i].bdaddr, wire, BT_ADDR_LEN) == 0) {
			h = c[i].con_handle;
			break;
		}
	}
	free(c);
	return (h);
}

int
hci_acl_handles(int fd, uint16_t *handles, int max)
{
	ng_hci_node_con_ep *c;
	int i, n, k = 0;

	n = con_list(fd, &c);
	if (n < 0)
		return (0);
	for (i = 0; i < n && k < max; i++) {
		if (c[i].link_type == NG_HCI_LINK_ACL)
			handles[k++] = c[i].con_handle;
	}
	free(c);
	return (k);
}

void
hci_addr_from_wire(const uint8_t wire[BT_ADDR_LEN], struct bt_addr *a)
{
	(void)memcpy(a->b, wire, BT_ADDR_LEN);
	a->type = BT_ADDR_PUBLIC;
}

void
hci_addr_to_wire(const struct bt_addr *a, uint8_t wire[BT_ADDR_LEN])
{
	(void)memcpy(wire, a->b, BT_ADDR_LEN);
}
