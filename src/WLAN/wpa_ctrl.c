/*
 * wpa_ctrl.c — minimal wpa_supplicant control-socket client. See wpa_ctrl.h for
 * why wland drives stock wpa_supplicant rather than vendoring or patching it.
 *
 * Deliberately hand-rolled rather than vendoring upstream's wpa_ctrl.c. The
 * protocol is a plain-text request/reply over a UNIX datagram socket, and the
 * subset wland needs is ~150 lines — small enough that a copy of upstream's
 * file (with its own #ifdef thicket for UDP transports, Windows named pipes,
 * and Android sockets) would be more code to carry, not less.
 */
#include "wpa_ctrl.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Where wpa_supplicant listens. This is the FreeBSD base default
 * (ctrl_interface=/var/run/wpa_supplicant in the shipped wpa_supplicant.conf);
 * wland's launchd plist starts wpa_supplicant with an explicit -C of the same
 * path, so the two cannot drift.
 */
#define	WPA_CTRL_DIR	"/var/run/wpa_supplicant"

struct wpa_ctrl {
	int	fd;
	char	local[sizeof(((struct sockaddr_un *)0)->sun_path)];
	bool	attached;
};

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "wland[wpa] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/*
 * Open a datagram socket, bind it to a unique local path (wpa_supplicant sends
 * replies back to the client's bound address, so an unbound socket gets nothing),
 * and connect it to wpa_supplicant's per-interface socket.
 *
 * `tag` distinguishes the command connection from the event connection so their
 * local paths can't collide.
 */
static struct wpa_ctrl *
ctrl_open(const char *ifname, const char *tag)
{
	struct sockaddr_un local, dest;
	struct wpa_ctrl *c;

	c = calloc(1, sizeof(*c));
	if (c == NULL)
		return (NULL);
	c->fd = -1;

	c->fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (c->fd < 0) {
		xlog("socket(AF_UNIX): %s", strerror(errno));
		free(c);
		return (NULL);
	}

	(void)memset(&local, 0, sizeof(local));
	local.sun_family = AF_UNIX;
	(void)snprintf(local.sun_path, sizeof(local.sun_path),
	    "/tmp/wland-%s-%s-%d", tag, ifname, (int)getpid());
	(void)strlcpy(c->local, local.sun_path, sizeof(c->local));
	(void)unlink(c->local);			/* stale from a hard restart */

	if (bind(c->fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
		xlog("bind(%s): %s", c->local, strerror(errno));
		(void)close(c->fd);
		free(c);
		return (NULL);
	}
	/* Only root talks to wpa_supplicant; do not leave a world-writable node. */
	(void)chmod(c->local, 0600);

	(void)memset(&dest, 0, sizeof(dest));
	dest.sun_family = AF_UNIX;
	(void)snprintf(dest.sun_path, sizeof(dest.sun_path), "%s/%s",
	    WPA_CTRL_DIR, ifname);

	if (connect(c->fd, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
		/*
		 * ENOENT / ECONNREFUSED just means wpa_supplicant is not up yet.
		 * The caller retries; this is not worth a log line on every tick.
		 */
		(void)close(c->fd);
		(void)unlink(c->local);
		free(c);
		return (NULL);
	}
	return (c);
}

struct wpa_ctrl *
wpa_ctrl_open(const char *ifname)
{
	return (ctrl_open(ifname, "cmd"));
}

struct wpa_ctrl *
wpa_ctrl_open_event(const char *ifname)
{
	struct wpa_ctrl *c;
	char reply[16];

	c = ctrl_open(ifname, "evt");
	if (c == NULL)
		return (NULL);

	/*
	 * ATTACH turns this connection into an event stream. Without it
	 * wpa_supplicant answers commands but never pushes anything, and wland
	 * would have to poll for association state — which is exactly the
	 * busy-wait the event-driven design exists to avoid.
	 */
	if (wpa_ctrl_request(c, "ATTACH", reply, sizeof(reply), 1000) != 0 ||
	    strncmp(reply, "OK", 2) != 0) {
		xlog("%s: ATTACH failed — no event stream", ifname);
		wpa_ctrl_close(c);
		return (NULL);
	}
	c->attached = true;

	/* Events arrive whenever; never let a read block the event loop. */
	(void)fcntl(c->fd, F_SETFL, O_NONBLOCK);
	return (c);
}

int
wpa_ctrl_fd(const struct wpa_ctrl *c)
{
	return (c != NULL ? c->fd : -1);
}

/*
 * A command as it may appear in the log. "SET_NETWORK <id> <field> <value>" is
 * cut after the field name, because the value can be a credential.
 */
static const char *
cmd_for_log(const char *cmd, char *buf, size_t buf_sz)
{
	char *p;

	if (strncmp(cmd, "SET_NETWORK ", 12) != 0)
		return (cmd);
	(void)strlcpy(buf, cmd, buf_sz);
	p = strchr(buf + 12, ' ');		/* after the id */
	if (p != NULL)
		p = strchr(p + 1, ' ');		/* after the field */
	if (p != NULL)
		*p = '\0';
	return (buf);
}

int
wpa_ctrl_request(struct wpa_ctrl *c, const char *cmd, char *reply,
    size_t reply_sz, int timeout_ms)
{
	struct pollfd pfd;
	char logbuf[64];
	ssize_t n;

	if (c == NULL || cmd == NULL || reply == NULL || reply_sz == 0)
		return (-1);

	if (send(c->fd, cmd, strlen(cmd), 0) < 0) {
		xlog("send(%s): %s", cmd_for_log(cmd, logbuf, sizeof(logbuf)),
		    strerror(errno));
		return (-1);
	}

	for (;;) {
		pfd.fd = c->fd;
		pfd.events = POLLIN;
		n = poll(&pfd, 1, timeout_ms);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return (-1);
		}
		if (n == 0) {
			xlog("timeout waiting for reply to '%s'",
			    cmd_for_log(cmd, logbuf, sizeof(logbuf)));
			return (-1);
		}

		n = recv(c->fd, reply, reply_sz - 1, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return (-1);
		}
		reply[n] = '\0';

		/*
		 * On an ATTACHed connection an unsolicited event can arrive
		 * between our send and the reply. Events are prefixed "<N>";
		 * replies never are. Skip events and keep waiting for the reply.
		 * (wland only issues commands on the un-attached command
		 * connection, so this is belt-and-braces — but getting it wrong
		 * would corrupt a reply in a way that is miserable to debug.)
		 */
		if (n > 0 && reply[0] == '<')
			continue;
		return (0);
	}
}

bool
wpa_ctrl_ok(struct wpa_ctrl *c, const char *cmd)
{
	char reply[32];

	if (wpa_ctrl_request(c, cmd, reply, sizeof(reply), 2000) != 0)
		return (false);
	return (strncmp(reply, "OK", 2) == 0);
}

int
wpa_ctrl_recv_event(struct wpa_ctrl *c, char *buf, size_t buf_sz)
{
	ssize_t n;
	char *p;

	if (c == NULL || buf == NULL || buf_sz == 0)
		return (-1);
	buf[0] = '\0';

	n = recv(c->fd, buf, buf_sz - 1, 0);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return (0);		/* nothing pending */
		return (-1);
	}
	if (n == 0)
		return (0);
	buf[n] = '\0';

	/* Strip the "<N>" priority prefix so callers match on the event name. */
	if (buf[0] == '<') {
		p = strchr(buf, '>');
		if (p != NULL && p[1] != '\0')
			(void)memmove(buf, p + 1, strlen(p + 1) + 1);
	}

	/* Trim a trailing newline — wpa_supplicant is inconsistent about it. */
	p = buf + strlen(buf);
	while (p > buf && (p[-1] == '\n' || p[-1] == '\r'))
		*--p = '\0';

	return (buf[0] != '\0' ? 1 : 0);
}

void
wpa_ctrl_close(struct wpa_ctrl *c)
{
	if (c == NULL)
		return;
	if (c->attached)
		(void)send(c->fd, "DETACH", 6, 0);
	if (c->fd >= 0)
		(void)close(c->fd);
	if (c->local[0] != '\0')
		(void)unlink(c->local);
	free(c);
}
