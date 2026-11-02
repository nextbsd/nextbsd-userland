/*
 * ngstack.c — load the upper Bluetooth kexts and build a controller's netgraph
 * graph. See ngstack.h.
 *
 * Talks to netgraph through libnetgraph's control socket, sending the same
 * generic messages ngctl(8) sends for `mkpeer`, `name`, `connect` and
 * `shutdown`. bluetoothd never runs ngctl. Each call opens its own control
 * socket and closes it: graph changes happen once per controller arrival or
 * departure, so there is nothing to keep open.
 */
#include "ngstack.h"
#include "hci_compat.h"

/*
 * KextManager.h uses the AVAILABLE_MAC_OS_X_* macros and kern_return_t
 * without including them.
 */
#include <mach/mach.h>
#include <AvailabilityMacros.h>
#include <IOKit/kext/KextManager.h>
#include <CoreFoundation/CoreFoundation.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <netgraph.h>
#include <netgraph/ng_message.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Hook names, from ng_ubt.h, ng_hci.h and ng_l2cap.h. */
#define	UBT_HOOK		"hook"
#define	HCI_HOOK_DRV		"drv"
#define	HCI_HOOK_ACL		"acl"
#define	HCI_HOOK_RAW		"raw"
#define	HCI_HOOK_SCO		"sco"
#define	L2CAP_HOOK_HCI		"hci"
#define	L2CAP_HOOK_L2C		"l2c"
#define	L2CAP_HOOK_CTL		"ctl"

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "bluetoothd[netgraph] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static int
load_one(const char *id)
{
	CFStringRef s;
	OSReturn r;

	s = CFStringCreateWithCString(NULL, id, kCFStringEncodingUTF8);
	if (s == NULL)
		return (-1);
	r = KextManagerLoadKextWithIdentifier(s, NULL);
	CFRelease(s);
	if (r != kOSReturnSuccess) {
		xlog("cannot load %s: 0x%x", id, (unsigned)r);
		return (-1);
	}
	return (0);
}

/* True when PF_NETGRAPH and PF_BLUETOOTH raw HCI sockets both exist. */
static bool
stack_present(void)
{
	int cs, s;

	if (NgMkSockNode(NULL, &cs, NULL) < 0)
		return (false);
	(void)close(cs);
	s = socket(PF_BLUETOOTH, SOCK_RAW, BLUETOOTH_PROTO_HCI);
	if (s < 0)
		return (false);
	(void)close(s);
	return (true);
}

int
ngstack_load(void)
{
	/* NgSocket first: NgBtsocket's nodes are reached through it. */
	(void)load_one(NGSTACK_KEXT_SOCKET);
	(void)load_one(NGSTACK_KEXT_L2CAP);
	(void)load_one(NGSTACK_KEXT_BTSOCKET);

	/*
	 * A load error is not the verdict: a kernel with the stack compiled in
	 * has nothing to load. What matters is whether the sockets exist now.
	 */
	if (!stack_present()) {
		xlog("the netgraph Bluetooth stack is not available");
		return (-1);
	}
	return (0);
}

/* One generic netgraph message to `path`. 0, or -1 with errno set. */
static int
ng_msg(int cs, const char *path, int cmd, const void *arg, size_t len)
{
	return (NgSendMsg(cs, path, NGM_GENERIC_COOKIE, cmd, arg, len) < 0 ?
	    -1 : 0);
}

static int
ng_mkpeer(int cs, const char *path, const char *type, const char *ourhook,
    const char *peerhook)
{
	struct ngm_mkpeer m;

	(void)memset(&m, 0, sizeof(m));
	(void)strlcpy(m.type, type, sizeof(m.type));
	(void)strlcpy(m.ourhook, ourhook, sizeof(m.ourhook));
	(void)strlcpy(m.peerhook, peerhook, sizeof(m.peerhook));
	return (ng_msg(cs, path, NGM_MKPEER, &m, sizeof(m)));
}

static int
ng_name(int cs, const char *path, const char *name)
{
	struct ngm_name m;

	(void)memset(&m, 0, sizeof(m));
	(void)strlcpy(m.name, name, sizeof(m.name));
	return (ng_msg(cs, path, NGM_NAME, &m, sizeof(m)));
}

static int
ng_connect(int cs, const char *path, const char *peer, const char *ourhook,
    const char *peerhook)
{
	struct ngm_connect m;

	(void)memset(&m, 0, sizeof(m));
	(void)strlcpy(m.path, peer, sizeof(m.path));
	(void)strlcpy(m.ourhook, ourhook, sizeof(m.ourhook));
	(void)strlcpy(m.peerhook, peerhook, sizeof(m.peerhook));
	return (ng_msg(cs, path, NGM_CONNECT, &m, sizeof(m)));
}

/*
 * Does node `name` exist? NGM_NODEINFO's path is resolved when the message is
 * sent, so a missing node fails the send with ENOENT; the reply, if any, is
 * dropped with the socket.
 */
static bool
ng_exists(int cs, const char *name)
{
	char path[NG_PATHSIZ];

	(void)snprintf(path, sizeof(path), "%s:", name);
	return (ng_msg(cs, path, NGM_NODEINFO, NULL, 0) == 0);
}

int
ngstack_build(const char *node, bool *adopted)
{
	char dev[NG_PATHSIZ], hci[NG_NODESIZ], l2cap[NG_NODESIZ];
	char hcip[NG_PATHSIZ], l2capp[NG_PATHSIZ], path[NG_PATHSIZ];
	char hook[NG_HOOKSIZ];
	const char *step;
	int cs;

	*adopted = false;
	(void)snprintf(dev, sizeof(dev), "%s:", node);
	(void)snprintf(hci, sizeof(hci), "%shci", node);
	(void)snprintf(l2cap, sizeof(l2cap), "%sl2cap", node);
	(void)snprintf(hcip, sizeof(hcip), "%s:", hci);
	(void)snprintf(l2capp, sizeof(l2capp), "%s:", l2cap);

	if (NgMkSockNode(NULL, &cs, NULL) < 0) {
		xlog("%s: no netgraph control socket: %s", node,
		    strerror(errno));
		return (-1);
	}

	if (ng_exists(cs, hci)) {
		/* bluetoothd restarted; the graph outlived it. */
		*adopted = true;
		(void)close(cs);
		xlog("%s: adopting the existing graph (%s)", node, hci);
		return (0);
	}

	/* HCI. */
	step = "mkpeer hci";
	if (ng_mkpeer(cs, dev, NG_HCI_NODE_TYPE, UBT_HOOK,
	    HCI_HOOK_DRV) < 0)
		goto fail;
	step = "name hci";
	(void)snprintf(path, sizeof(path), "%s:%s", node, UBT_HOOK);
	if (ng_name(cs, path, hci) < 0)
		goto fail;

	/* L2CAP. */
	step = "mkpeer l2cap";
	if (ng_mkpeer(cs, hcip, NG_L2CAP_NODE_TYPE, HCI_HOOK_ACL,
	    L2CAP_HOOK_HCI) < 0)
		goto fail;
	step = "name l2cap";
	(void)snprintf(path, sizeof(path), "%s:%s", hci, HCI_HOOK_ACL);
	if (ng_name(cs, path, l2cap) < 0)
		goto fail;

	/* The socket layer. */
	step = "connect btsock_hci_raw";
	(void)snprintf(hook, sizeof(hook), "%sraw", node);
	if (ng_connect(cs, hcip, "btsock_hci_raw:", HCI_HOOK_RAW, hook) < 0)
		goto fail;
	step = "connect btsock_l2c_raw";
	(void)snprintf(hook, sizeof(hook), "%sctl", node);
	if (ng_connect(cs, l2capp, "btsock_l2c_raw:", L2CAP_HOOK_CTL,
	    hook) < 0)
		goto fail;
	step = "connect btsock_l2c";
	(void)snprintf(hook, sizeof(hook), "%sl2c", node);
	if (ng_connect(cs, l2capp, "btsock_l2c:", L2CAP_HOOK_L2C, hook) < 0)
		goto fail;

	/*
	 * SCO: the HFP gateway (#525) opens BLUETOOTH_PROTO_SCO sockets, and
	 * they reach the controller only through this hook (SCO/eSCO support:
	 * nextbsd-kernel-extensions#31). Not fatal: ACL works without it.
	 */
	(void)snprintf(hook, sizeof(hook), "%ssco", node);
	if (ng_connect(cs, hcip, "btsock_sco:", HCI_HOOK_SCO, hook) < 0)
		xlog("%s: connect btsock_sco: %s (SCO audio unavailable)",
		    node, strerror(errno));

	(void)close(cs);
	xlog("%s: graph built (%s, %s)", node, hci, l2cap);
	return (0);

fail:
	xlog("%s: %s: %s", node, step, strerror(errno));
	(void)close(cs);
	/* Leave nothing half-built behind for the next attempt. */
	ngstack_teardown(node);
	return (-1);
}

void
ngstack_teardown(const char *node)
{
	char path[NG_PATHSIZ];
	int cs;

	if (NgMkSockNode(NULL, &cs, NULL) < 0)
		return;
	(void)snprintf(path, sizeof(path), "%shci:", node);
	(void)ng_msg(cs, path, NGM_SHUTDOWN, NULL, 0);
	(void)snprintf(path, sizeof(path), "%sl2cap:", node);
	(void)ng_msg(cs, path, NGM_SHUTDOWN, NULL, 0);
	(void)close(cs);
}
