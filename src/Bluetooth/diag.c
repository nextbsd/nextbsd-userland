/*
 * diag.c — controller details, statistics, links, raw HCI commands and the
 * L2CAP echo, for bluetooth(8)'s diagnostics. See diag.h.
 *
 * These replace hccontrol and l2ping, whose BSD-licensed shapes they follow:
 * hccontrol's hci_request() (send, then wait for the matching Command
 * Complete) and its node ioctls, and l2ping's raw L2CAP socket with
 * SIOC_L2CAP_L2CA_PING. Unlike hccontrol, the wait is bounded and runs on a
 * socket of its own, so a slow controller costs the caller time and nothing
 * else.
 */
#include "diag.h"
#include "bluetoothd.h"
#include "hci.h"
#include "hci_compat.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * What the calls below need from a controller, copied under the lock: its
 * HCI node, its address and what bring-up read.
 */
struct diag_target {
	BTNodeName	node;
	char		hci_node[32];
	struct bt_addr	addr;
	bool		have_addr;
	bool		powered;
	uint8_t		features[8];
	uint8_t		hci_version;
	uint16_t	manufacturer;
};

static bt_status_t
target(const char *node, struct diag_target *t)
{
	struct bt_adapter *a;
	bt_status_t st;

	(void)memset(t, 0, sizeof(*t));
	bluetoothd_lock();
	a = bluetoothd_adapter_pick_locked(node);
	if (a == NULL) {
		st = bt_status_no_adapter_e;
	} else if (a->hci_fd < 0 || a->hci_node[0] == '\0') {
		/* Arrived, but the graph and its HCI node are not up yet. */
		st = bt_status_busy_e;
	} else {
		(void)strlcpy(t->node, a->node, sizeof(t->node));
		(void)strlcpy(t->hci_node, a->hci_node, sizeof(t->hci_node));
		t->addr = a->addr;
		t->have_addr = a->have_addr;
		t->powered = a->powered;
		(void)memcpy(t->features, a->features, sizeof(t->features));
		t->hci_version = a->hci_version;
		t->manufacturer = a->manufacturer;
		st = bt_status_success_e;
	}
	bluetoothd_unlock();
	return (st);
}

/*
 * Send `opcode` on `fd` and wait for the Command Complete or Command Status
 * that answers it, skipping every other event. Returns 0 or an errno.
 */
static int
request(int fd, uint16_t opcode, const void *cp, size_t cplen, int *event,
    uint8_t *status, uint8_t *rp, size_t rpmax, size_t *rplen)
{
	uint8_t buf[sizeof(ng_hci_event_pkt_t) + NG_HCI_EVENT_PKT_SIZE];
	uint64_t deadline;

	*event = 0;
	*status = 0;
	*rplen = 0;
	if (hci_send_cmd(fd, opcode, cp, cplen) != 0)
		return (errno);

	deadline = bt_now_ms() + DIAG_HCI_TIMEOUT_MS;
	for (;;) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		struct hci_event ev;
		const uint8_t *p;
		uint16_t op;
		size_t n;
		uint64_t now = bt_now_ms();
		int r;

		if (now >= deadline)
			return (ETIMEDOUT);
		r = poll(&pfd, 1, (int)(deadline - now));
		if (r < 0 && errno != EINTR)
			return (errno);
		if (r <= 0)
			continue;
		while ((r = hci_recv_event(fd, buf, sizeof(buf), &ev)) == 1) {
			if (!hci_cmd_result(&ev, &op, status, &p, &n) ||
			    op != opcode)
				continue;
			*event = ev.code;
			if (n > rpmax)
				n = rpmax;
			if (n > 0 && rp != NULL)
				(void)memcpy(rp, p, n);
			*rplen = n;
			return (0);
		}
		if (r < 0)
			return (errno);
	}
}

bt_status_t
diag_details(const char *node, struct diag_details *out)
{
	struct ng_btsocket_hci_raw_node_buffer b;
	struct diag_target t;
	uint8_t rp[16], hst;
	size_t n;
	bt_status_t st;
	int fd, ev;

	(void)memset(out, 0, sizeof(*out));
	if ((st = target(node, &t)) != bt_status_success_e)
		return (st);
	(void)strlcpy(out->node, t.node, sizeof(out->node));
	out->addr = t.addr;
	out->have_addr = t.have_addr;
	(void)memcpy(out->features, t.features, sizeof(out->features));
	out->hci_version = t.hci_version;
	out->lmp_version = t.hci_version;
	out->manufacturer = t.manufacturer;

	if ((fd = hci_open(t.hci_node)) < 0)
		return (bt_status_success_e);	/* the cached facts only */

	/* Read_Local_Version_Information: the whole version, not just HCI's. */
	if (request(fd, NG_HCI_OPCODE(NG_HCI_OGF_INFO,
	    NG_HCI_OCF_READ_LOCAL_VER), NULL, 0, &ev, &hst, rp, sizeof(rp),
	    &n) == 0 && ev == NG_HCI_EVENT_COMMAND_COMPL && hst == 0 &&
	    n >= 8) {
		out->hci_version = rp[0];
		out->hci_revision = rp[1] | rp[2] << 8;
		out->lmp_version = rp[3];
		out->manufacturer = rp[4] | rp[5] << 8;
		out->lmp_subversion = rp[6] | rp[7] << 8;
	}

	/* ACL and SCO buffers as ng_hci holds them (Read_Buffer_Size). */
	(void)memset(&b, 0, sizeof(b));
	if (ioctl(fd, SIOC_HCI_RAW_NODE_GET_BUFFER, &b) == 0) {
		out->acl_mtu = b.buffer.acl_size;
		out->acl_pkts = b.buffer.acl_pkts;
		out->sco_mtu = b.buffer.sco_size;
		out->sco_pkts = b.buffer.sco_pkts;
	}

	/* LE_Read_Buffer_Size; 0/0 means LE shares the ACL pool. */
	if (HCI_FEAT_LE(t.features) &&
	    request(fd, NG_HCI_OPCODE(NG_HCI_OGF_LE,
	    NG_HCI_OCF_LE_READ_BUFFER_SIZE), NULL, 0, &ev, &hst, rp,
	    sizeof(rp), &n) == 0 && ev == NG_HCI_EVENT_COMMAND_COMPL &&
	    hst == 0 && n >= 3) {
		out->le_mtu = rp[0] | rp[1] << 8;
		out->le_pkts = rp[2];
	}
	(void)close(fd);
	return (bt_status_success_e);
}

bt_status_t
diag_stats(const char *node, struct diag_stats *out)
{
	struct ng_btsocket_hci_raw_node_stat s;
	struct ng_btsocket_hci_raw_node_buffer b;
	struct diag_target t;
	bt_status_t st;
	int fd;

	(void)memset(out, 0, sizeof(*out));
	if ((st = target(node, &t)) != bt_status_success_e)
		return (st);
	if ((fd = hci_open(t.hci_node)) < 0)
		return (bt_status_internal_error_e);
	(void)memset(&s, 0, sizeof(s));
	(void)memset(&b, 0, sizeof(b));
	if (ioctl(fd, SIOC_HCI_RAW_NODE_GET_STAT, &s) != 0 ||
	    ioctl(fd, SIOC_HCI_RAW_NODE_GET_BUFFER, &b) != 0) {
		(void)close(fd);
		return (bt_status_internal_error_e);
	}
	(void)close(fd);
	out->cmd_sent = s.stat.cmd_sent;
	out->events = s.stat.evnt_recv;
	out->acl_recv = s.stat.acl_recv;
	out->acl_sent = s.stat.acl_sent;
	out->sco_recv = s.stat.sco_recv;
	out->sco_sent = s.stat.sco_sent;
	out->bytes_recv = s.stat.bytes_recv;
	out->bytes_sent = s.stat.bytes_sent;
	out->cmd_free = b.buffer.cmd_free;
	out->acl_free = b.buffer.acl_free;
	out->sco_free = b.buffer.sco_free;
	return (bt_status_success_e);
}

bt_status_t
diag_links(const char *node, struct diag_link **out, int *n)
{
	struct ng_btsocket_hci_raw_con_list r;
	struct diag_target t;
	struct diag_link *l;
	bt_status_t st;
	uint32_t i;
	int fd;

	*out = NULL;
	*n = 0;
	if ((st = target(node, &t)) != bt_status_success_e)
		return (st);
	if ((fd = hci_open(t.hci_node)) < 0)
		return (bt_status_internal_error_e);
	(void)memset(&r, 0, sizeof(r));
	r.num_connections = NG_HCI_MAX_CON_NUM;
	r.connections = calloc(NG_HCI_MAX_CON_NUM, sizeof(ng_hci_node_con_ep));
	if (r.connections == NULL ||
	    ioctl(fd, SIOC_HCI_RAW_NODE_GET_CON_LIST, &r) != 0) {
		free(r.connections);
		(void)close(fd);
		return (bt_status_internal_error_e);
	}
	(void)close(fd);

	l = calloc(r.num_connections > 0 ? r.num_connections : 1, sizeof(*l));
	if (l == NULL) {
		free(r.connections);
		return (bt_status_internal_error_e);
	}
	for (i = 0; i < r.num_connections; i++) {
		const ng_hci_node_con_ep *c = &r.connections[i];

		hci_addr_from_wire((const uint8_t *)&c->bdaddr, &l[i].addr);
		if (c->link_type == NG_HCI_LINK_LE_RANDOM)
			l[i].addr.type = BT_ADDR_RANDOM;
		l[i].link_type = c->link_type;
		l[i].handle = c->con_handle;
		l[i].role = c->role;
		l[i].encryption = c->encryption_mode;
		l[i].mode = c->mode;
		l[i].pending = c->pending;
	}
	*out = l;
	*n = (int)r.num_connections;
	free(r.connections);
	return (bt_status_success_e);
}

bt_status_t
diag_hci_command(const char *node, uint16_t opcode, const uint8_t *cp,
    size_t cplen, int *event, uint8_t *hci_status, uint8_t *rp,
    size_t *rplen, int *errnum)
{
	struct diag_target t;
	bt_status_t st;
	int fd;

	*event = 0;
	*hci_status = 0;
	*rplen = 0;
	*errnum = 0;
	if (cplen > NG_HCI_CMD_PKT_SIZE || opcode == 0)
		return (bt_status_invalid_parameter_e);
	if ((st = target(node, &t)) != bt_status_success_e)
		return (st);
	if ((fd = hci_open(t.hci_node)) < 0) {
		*errnum = errno;
		return (bt_status_internal_error_e);
	}
	*errnum = request(fd, opcode, cp, cplen, event, hci_status, rp,
	    BT_HCI_DATA_MAX, rplen);
	(void)close(fd);
	return (*errnum == 0 ? bt_status_success_e : bt_status_internal_error_e);
}

bt_status_t
diag_l2cap_ping(const struct bt_addr *peer, size_t size, uint32_t *rtt_us,
    uint32_t *result, int *errnum)
{
	struct ng_btsocket_l2cap_raw_ping r;
	struct sockaddr_l2cap sa;
	struct diag_target t;
	struct timespec t0, t1;
	uint8_t *data;
	bt_status_t st;
	size_t i;
	int fd;

	*rtt_us = 0;
	*result = 0;
	*errnum = 0;
	if (peer->type != BT_ADDR_PUBLIC)
		return (bt_status_operation_not_supported_e);	/* LE: no echo */
	if (size > NG_L2CAP_MAX_ECHO_SIZE)
		return (bt_status_invalid_parameter_e);
	if ((st = target("", &t)) != bt_status_success_e)
		return (st);
	if (!t.powered)
		return (bt_status_powered_off_e);

	fd = socket(PF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC,
	    BLUETOOTH_PROTO_L2CAP);
	if (fd < 0) {
		*errnum = errno;
		return (bt_status_internal_error_e);
	}

	/* Bound to this controller when its address is known, else any. */
	(void)memset(&sa, 0, sizeof(sa));
	sa.l2cap_len = sizeof(sa);
	sa.l2cap_family = AF_BLUETOOTH;
	sa.l2cap_bdaddr_type = BDADDR_BREDR;
	if (t.have_addr)
		hci_addr_to_wire(&t.addr, (uint8_t *)&sa.l2cap_bdaddr);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0)
		goto fail;
	hci_addr_to_wire(peer, (uint8_t *)&sa.l2cap_bdaddr);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0)
		goto fail;

	/* l2ping's payload is a counter pattern; the peer echoes it back. */
	data = calloc(1, size > 0 ? size : 1);
	if (data == NULL) {
		errno = ENOMEM;
		goto fail;
	}
	for (i = 0; i < size; i++)
		data[i] = (uint8_t)i;
	(void)memset(&r, 0, sizeof(r));
	r.echo_size = (uint32_t)size;
	r.echo_data = data;

	(void)clock_gettime(CLOCK_MONOTONIC, &t0);
	if (ioctl(fd, SIOC_L2CAP_L2CA_PING, &r) != 0) {
		free(data);
		goto fail;
	}
	(void)clock_gettime(CLOCK_MONOTONIC, &t1);
	free(data);
	(void)close(fd);

	*rtt_us = (uint32_t)((t1.tv_sec - t0.tv_sec) * 1000000 +
	    (t1.tv_nsec - t0.tv_nsec) / 1000);
	*result = r.result;
	return (r.result == 0 ? bt_status_success_e :
	    bt_status_not_connected_e);

fail:
	*errnum = errno;
	(void)close(fd);
	/* The kernel's wait for the echo response ran out (msleep). */
	if (*errnum == EWOULDBLOCK)
		*errnum = ETIMEDOUT;
	return (bt_status_internal_error_e);
}
