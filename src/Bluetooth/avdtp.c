/*
 * avdtp.c — AVDTP signalling messages. See avdtp.h.
 */
#include "avdtp.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>

int
avdtp_recv(int fd, struct avdtp_msg *m)
{
	uint8_t buf[AVDTP_MSG_MAX + 3];
	ssize_t n;
	uint8_t pkt, label;
	size_t hdr;

	n = recv(fd, buf, sizeof(buf), 0);
	if (n < 0)
		return ((errno == EAGAIN || errno == EINTR) ? 0 : -1);
	if (n == 0)
		return (-1);
	if (n < 1)
		return (0);
	label = buf[0] >> 4;
	pkt = (buf[0] >> 2) & 3;

	switch (pkt) {
	case AVDTP_PKT_SINGLE:
		if (n < 2)
			return (0);
		m->frags_left = 0;
		m->label = label;
		m->type = buf[0] & 3;
		m->signal = buf[1] & 0x3f;
		hdr = 2;
		m->len = 0;
		break;
	case AVDTP_PKT_START:
		if (n < 3)
			return (0);
		m->label = label;
		m->type = buf[0] & 3;
		m->frags_left = buf[1] - 1;	/* NOSP counts this one */
		m->signal = buf[2] & 0x3f;
		hdr = 3;
		m->len = 0;
		break;
	default:
		/* A continue or end packet of the message being built. */
		if (m->frags_left <= 0 || label != m->label)
			return (0);
		m->frags_left--;
		hdr = 1;
		break;
	}
	if ((size_t)n - hdr > sizeof(m->params) - m->len) {
		m->frags_left = 0;
		m->len = 0;
		return (0);	/* too big for us: drop it */
	}
	(void)memcpy(m->params + m->len, buf + hdr, (size_t)n - hdr);
	m->len += (size_t)n - hdr;
	if (pkt != AVDTP_PKT_SINGLE && m->frags_left > 0)
		return (0);
	return (1);
}

int
avdtp_send(int fd, uint8_t label, uint8_t type, uint8_t signal,
    const void *params, size_t len)
{
	struct iovec iov[2];
	uint8_t hdr[2];
	ssize_t n;

	hdr[0] = (uint8_t)((label << 4) | (AVDTP_PKT_SINGLE << 2) |
	    (type & 3));
	hdr[1] = signal & 0x3f;
	iov[0].iov_base = hdr;
	iov[0].iov_len = sizeof(hdr);
	iov[1].iov_base = (void *)(uintptr_t)params;
	iov[1].iov_len = len;
	n = writev(fd, iov, len > 0 ? 2 : 1);
	return (n == (ssize_t)(sizeof(hdr) + len) ? 0 : -1);
}

int
avdtp_reject(int fd, const struct avdtp_msg *m, const uint8_t *err,
    size_t len)
{
	/* A signal we do not know gets a General Reject (AVDTP 1.3, 8.18). */
	if (m->signal == 0 || m->signal > AVDTP_DELAY_REPORT)
		return (avdtp_send(fd, m->label, AVDTP_MSG_GENERAL_REJECT,
		    m->signal, NULL, 0));
	return (avdtp_send(fd, m->label, AVDTP_MSG_REJECT, m->signal, err,
	    len));
}

const uint8_t *
avdtp_rtp_payload(const uint8_t *pkt, size_t len, size_t *plen)
{
	size_t hdr = 12;

	if (len < hdr || (pkt[0] >> 6) != 2)
		return (NULL);			/* not RTP version 2 */
	hdr += (size_t)(pkt[0] & 0x0f) * 4;	/* CSRCs */
	if (len < hdr)
		return (NULL);
	if ((pkt[0] & 0x10) != 0) {		/* header extension */
		if (len < hdr + 4)
			return (NULL);
		hdr += 4 + (size_t)((pkt[hdr + 2] << 8) | pkt[hdr + 3]) * 4;
		if (len < hdr)
			return (NULL);
	}
	if ((pkt[0] & 0x20) != 0 && len > hdr) {	/* padding */
		size_t pad = pkt[len - 1];

		if (pad > len - hdr)
			return (NULL);
		len -= pad;
	}
	*plen = len - hdr;
	return (pkt + hdr);
}
