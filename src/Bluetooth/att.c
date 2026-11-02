/*
 * att.c — the ATT bearer on L2CAP CID 4 (Core v5.4 Vol 3 Part F). See le.h.
 *
 * Client side: requests are queued and go out one at a time (§3.3.2: one
 * outstanding request per bearer); each has a 30 s transaction timeout
 * (§3.3.3), after which the bearer is dead and the link is dropped.
 * Notifications and indications go to HOGP; indications are confirmed.
 *
 * Server side: bluetoothd has no GATT database. Peers do look (a keyboard may
 * read the central's Generic Access name, or ask for the MTU), so every
 * request gets the answer a server with no attributes gives: an MTU response,
 * "attribute not found" for the discovery and read-by-type requests, and
 * "request not supported" for the rest. Commands are ignored, as §3.4.9 asks.
 *
 * When the link goes, queued requests are freed without calling their done
 * functions: the link's owners (HOGP, discovery) are torn down with it.
 */
#include "le.h"
#include "hci.h"
#include "hci_compat.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint16_t
get16(const uint8_t *p)
{
	return ((uint16_t)(p[0] | p[1] << 8));
}

static void
put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

void
att_init(struct att_bearer *b)
{
	(void)memset(b, 0, sizeof(*b));
	b->fd = -1;
	b->mtu = LE_ATT_MTU_DEFAULT;
}

static int
att_send(struct le_conn *c, const uint8_t *pdu, size_t len)
{
	for (;;) {
		if (send(c->att.fd, pdu, len, 0) >= 0)
			return (0);
		if (errno != EINTR)
			return (-1);
	}
}

/* Put the head of the queue on the air, if nothing is. */
static void
att_kick(struct le_conn *c)
{
	struct att_bearer *b = &c->att;
	struct att_req *r;

	while (!b->sent && b->head != NULL && b->fd >= 0) {
		r = b->head;
		if (att_send(c, r->pdu, r->len) == 0) {
			b->sent = true;
			b->deadline = bt_now_ms() + LE_ATT_TIMEOUT_MS;
			return;
		}
		/* The socket refused it; the link is on its way out. */
		le_log(c->e, "ATT send failed: %s", strerror(errno));
		b->head = r->next;
		if (b->head == NULL)
			b->tail = NULL;
		free(r);
	}
}

int
att_request(struct le_conn *c, const uint8_t *pdu, size_t len,
    att_done_fn done, void *arg)
{
	struct att_bearer *b = &c->att;
	struct att_req *r;

	if (b->fd < 0 || len == 0 || len > sizeof(r->pdu) ||
	    (len > b->mtu && pdu[0] != ATT_MTU_REQ))
		return (-1);
	r = calloc(1, sizeof(*r));
	if (r == NULL)
		return (-1);
	(void)memcpy(r->pdu, pdu, len);
	r->len = len;
	r->rsp = (uint8_t)(pdu[0] + 1);	/* every request's response */
	r->done = done;
	r->arg = arg;
	if (b->tail != NULL)
		b->tail->next = r;
	else
		b->head = r;
	b->tail = r;
	att_kick(c);
	return (0);
}

int
att_command(struct le_conn *c, const uint8_t *pdu, size_t len)
{
	if (c->att.fd < 0 || len == 0 || len > c->att.mtu)
		return (-1);
	return (att_send(c, pdu, len));
}

/* The request on the air was answered (err 0: pdu is the response). */
static void
att_complete(struct le_conn *c, uint8_t err, const uint8_t *pdu, size_t len)
{
	struct att_bearer *b = &c->att;
	struct att_req *r = b->head;

	b->head = r->next;
	if (b->head == NULL)
		b->tail = NULL;
	b->sent = false;
	b->deadline = 0;
	if (r->done != NULL)
		r->done(c, r->arg, err, pdu, len);
	free(r);
	att_kick(c);
}

/* A request from the peer: answer as a server with no attributes. */
static void
att_server(struct le_conn *c, const uint8_t *pdu, size_t len)
{
	uint8_t rsp[5];
	uint16_t handle = 0;
	uint8_t ecode = ATT_ECODE_REQ_NOT_SUPP;

	switch (pdu[0]) {
	case ATT_MTU_REQ: {
		uint16_t want;

		if (len < 3)
			return;
		want = get16(pdu + 1);
		if (want < LE_ATT_MTU_DEFAULT)
			want = LE_ATT_MTU_DEFAULT;
		c->att.mtu = want < LE_ATT_MTU_LOCAL ? want : LE_ATT_MTU_LOCAL;
		rsp[0] = ATT_MTU_RSP;
		put16(rsp + 1, LE_ATT_MTU_LOCAL);
		(void)att_send(c, rsp, 3);
		return;
	}
	case ATT_FIND_INFO_REQ:
	case ATT_FIND_BY_TYPE_REQ:
	case ATT_READ_BY_TYPE_REQ:
	case ATT_READ_BY_GROUP_REQ:
		if (len >= 3)
			handle = get16(pdu + 1);
		ecode = ATT_ECODE_ATTR_NOT_FOUND;
		break;
	case ATT_READ_REQ:
	case ATT_READ_BLOB_REQ:
	case ATT_WRITE_REQ:
		if (len >= 3)
			handle = get16(pdu + 1);
		ecode = ATT_ECODE_INVALID_HANDLE;
		break;
	default:
		break;
	}
	rsp[0] = ATT_ERROR_RSP;
	rsp[1] = pdu[0];
	put16(rsp + 2, handle);
	rsp[4] = ecode;
	(void)att_send(c, rsp, sizeof(rsp));
}

static void
att_pdu(struct le_conn *c, const uint8_t *pdu, size_t len)
{
	struct att_bearer *b = &c->att;
	uint8_t op = pdu[0];

	switch (op) {
	case ATT_ERROR_RSP:
		if (len >= 5 && b->sent && b->head->pdu[0] == pdu[1])
			att_complete(c, pdu[4] != 0 ? pdu[4] :
			    ATT_ECODE_REQ_NOT_SUPP, NULL, 0);
		return;
	case ATT_HANDLE_NOTIFY:
		if (len >= 3)
			hogp_notify(c, get16(pdu + 1), pdu + 3, len - 3);
		return;
	case ATT_HANDLE_IND: {
		uint8_t conf = ATT_HANDLE_CONF;

		if (len < 3)
			return;
		(void)att_send(c, &conf, 1);
		hogp_notify(c, get16(pdu + 1), pdu + 3, len - 3);
		return;
	}
	case ATT_HANDLE_CONF:
		return;		/* we never indicate */
	default:
		break;
	}

	if (b->sent && op == b->head->rsp) {
		att_complete(c, 0, pdu, len);
		return;
	}
	if ((op & ATT_OP_COMMAND) != 0)
		return;		/* commands get no answer, even an error */
	if ((op & 1) == 0)
		att_server(c, pdu, len);	/* requests are even */
	/* An odd opcode nobody asked for: a stray response; drop it. */
}

void
att_input(struct le_conn *c)
{
	uint8_t buf[LE_ATT_MTU_LOCAL + 64];
	ssize_t n;
	int guard;

	for (guard = 0; guard < 64 && c->att.fd >= 0 &&
	    c->state != LC_FREE; guard++) {
		n = recv(c->att.fd, buf, sizeof(buf), MSG_DONTWAIT);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			le_conn_drop(c, strerror(errno));
			return;
		}
		if (n == 0) {
			le_conn_drop(c, "the link closed");
			return;
		}
		att_pdu(c, buf, (size_t)n);
	}
}

void
att_close(struct le_conn *c)
{
	struct att_bearer *b = &c->att;
	struct att_req *r;

	while ((r = b->head) != NULL) {
		b->head = r->next;
		free(r);
	}
	b->tail = NULL;
	b->sent = false;
	b->deadline = 0;
	if (b->fd >= 0) {
		le_unwatch(c->e, b->fd);
		(void)close(b->fd);
		b->fd = -1;
	}
}

uint64_t
att_next_timer(struct le_conn *c)
{
	return (c->att.sent ? c->att.deadline : 0);
}

void
att_timer(struct le_conn *c, uint64_t now)
{
	if (c->att.sent && now >= c->att.deadline)
		le_conn_drop(c, "ATT transaction timeout");
}
