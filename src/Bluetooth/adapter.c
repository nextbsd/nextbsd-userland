/*
 * adapter.c — the controller lifecycle. See adapter.h.
 *
 * The init sequence is rc.d/bluetooth's bluetooth_setup_stack() (its hccontrol
 * calls), with the Secure Simple Pairing and LE steps it never had, sent from
 * here over the raw HCI socket. ng_hci watches the Command Complete events go
 * past and takes the address, features and buffer sizes from them; the LE
 * buffer pool comes from LE_Read_Buffer_Size, which must follow
 * Read_Buffer_Size (nextbsd-kernel-extensions#30).
 */
#include "adapter.h"
#include "a2dp_sink.h"
#include "audio.h"
#include "avrcp.h"
#include "bluetoothd.h"
#include "btstate.h"
#include "btstore.h"
#include "connections.h"
#include "discovery.h"
#include "engine.h"
#include "hci.h"
#include "hci_compat.h"
#include "hid_host.h"
#include "ngstack.h"
#include "pairing.h"
#include "sdp_client.h"
#include "sdp_server.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define	OGF_LC		NG_HCI_OGF_LINK_CONTROL
#define	OGF_HC		NG_HCI_OGF_HC_BASEBAND
#define	OGF_INFO	NG_HCI_OGF_INFO
#define	OGF_LE		NG_HCI_OGF_LE

#define	INIT_TIMEOUT_MS		5000
#define	RESET_TIMEOUT_MS	10000
#define	RESET_TRIES		3

/*
 * Class of Device: Computer (major 1), uncategorised. Service bits are added
 * when a profile that needs them (the A2DP sink) registers.
 */
#define	LOCAL_CLASS_OF_DEVICE	0x000100

/* Service-class bits each profile asks for (adapter_set_cod_services_locked). */
static uint32_t		g_cod_owner[ADAPTER_COD_OWNERS];
static uint32_t		g_cod_services;		/* their union */

enum {
	INIT_RESET = 0,
	INIT_READ_VERSION,
	INIT_READ_BDADDR,
	INIT_READ_FEATURES,
	INIT_READ_BUFFER_SIZE,
	INIT_LE_READ_BUFFER_SIZE,
	INIT_LE_EVENT_MASK,
	INIT_LE_HOST_SUPPORTED,
	INIT_EVENT_MASK,
	INIT_SSP_MODE,
	INIT_INQUIRY_MODE,
	INIT_CLASS,
	INIT_NAME,
	INIT_SCAN_ENABLE,
	INIT_DONE
};

static const char *const init_names[] = {
	"Reset", "Read_Local_Version", "Read_BD_ADDR",
	"Read_Local_Supported_Features", "Read_Buffer_Size",
	"LE_Read_Buffer_Size", "LE_Set_Event_Mask", "Write_LE_Host_Supported",
	"Set_Event_Mask", "Write_Simple_Pairing_Mode", "Write_Inquiry_Mode",
	"Write_Class_Of_Device", "Change_Local_Name", "Write_Scan_Enable"
};

/* The kexts are loaded once, by the first controller. */
static bool	g_stack_loaded;

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "bluetoothd[adapter] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static const char *
state_name(bt_adapter_state_t s)
{
	switch (s) {
	case bt_adapter_arrived_e:		return ("Arrived");
	case bt_adapter_firmware_e:		return ("Firmware");
	case bt_adapter_initializing_e:		return ("Initializing");
	case bt_adapter_powered_on_e:		return ("On");
	case bt_adapter_powered_off_e:		return ("Off");
	default:				return ("Error");
	}
}

static bool
mandatory(int step)
{
	return (step == INIT_RESET || step == INIT_READ_BDADDR ||
	    step == INIT_READ_FEATURES || step == INIT_READ_BUFFER_SIZE);
}

static void
put_le64(uint8_t *p, uint64_t v)
{
	int i;

	for (i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

/* Write_Scan_Enable for the adapter's current policy. */
static int
send_scan_enable(struct bt_adapter *a)
{
	uint8_t scan = HCI_SCAN_NONE;

	if (a->powered) {
		scan = HCI_SCAN_PAGE;
		if (a->discoverable)
			scan |= HCI_SCAN_INQUIRY;
	}
	return (hci_send_cmd(a->hci_fd, HCI_OPCODE(OGF_HC,
	    NG_HCI_OCF_WRITE_SCAN_ENABLE), &scan, sizeof(scan)));
}

/* ---- engines -------------------------------------------------------------- */

static void	host_event(void *ctx, const struct bt_engine *e,
		    const struct bt_event *ev);
static bool	host_bond_lookup(void *ctx, const struct bt_addr *peer,
		    struct bt_bond *out);
static bool	host_name_known(void *ctx, const struct bt_addr *peer);
static void	import_bonds(struct bt_adapter *a);

static void
attach_engines(struct bt_adapter *a)
{
	struct bt_controller_desc desc;
	struct bt_engine_host host;
	int i;

	(void)memset(&desc, 0, sizeof(desc));
	desc.node = a->node;
	desc.hci_node = a->hci_node;
	desc.vendor = a->vendor;
	desc.product = a->product;
	desc.hci_fd = a->hci_fd;
	(void)memcpy(desc.features, a->features, sizeof(desc.features));
	desc.powered = a->powered;

	(void)memset(&host, 0, sizeof(host));
	host.ctx = a;
	host.event = host_event;
	host.bond_lookup = host_bond_lookup;
	host.name_known = host_name_known;

	for (i = 0; i < BT_ENGINE_MAX && bt_engines[i] != NULL; i++) {
		bt_status_t st;

		if (bt_engines[i]->transport == BT_TRANSPORT_LE &&
		    !HCI_FEAT_LE(a->features))
			continue;
		st = bt_engines[i]->attach(&desc, &host, &a->engines[i]);
		if (st == bt_status_success_e)
			xlog("%s: engine %s attached", a->node,
			    bt_engines[i]->name);
		else if (st != bt_status_operation_not_supported_e)
			xlog("%s: engine %s did not attach (status %d)",
			    a->node, bt_engines[i]->name, (int)st);
	}
	import_bonds(a);
}

/*
 * Hand every stored bond to the engines that keep keys of their own (the LE
 * engine's reconnect list and IRKs), so a paired keyboard reconnects before
 * anyone logs in.
 */
static void
import_bonds(struct bt_adapter *a)
{
	struct bt_bond *b;
	int i, j, n;

	b = calloc(1, sizeof(*b));
	if (b == NULL)
		return;
	n = bt_store_paired_count();
	for (i = 0; i < n; i++) {
		if (!bt_store_paired_at(i, b) ||
		    !bt_store_bond_find(&b->addr, true, b))
			continue;
		for (j = 0; j < BT_ENGINE_MAX; j++) {
			if (a->engines[j] != NULL &&
			    bt_engines[j]->bond_import != NULL)
				(void)bt_engines[j]->bond_import(
				    a->engines[j], b);
		}
	}
	explicit_bzero(b, sizeof(*b));
	free(b);
}

static void
detach_engines(struct bt_adapter *a)
{
	int i;

	for (i = 0; i < BT_ENGINE_MAX; i++) {
		if (a->engines[i] != NULL && bt_engines[i]->detach != NULL)
			bt_engines[i]->detach(a->engines[i]);
		a->engines[i] = NULL;
	}
}

/* The engine of `a` for `transport` (BT_TRANSPORT_*), if attached. */
static struct bt_engine *
engine_of(struct bt_adapter *a, int transport,
    const struct bt_engine_ops **ops)
{
	*ops = NULL;
	int i;

	for (i = 0; i < BT_ENGINE_MAX && bt_engines[i] != NULL; i++) {
		if (bt_engines[i]->transport == transport &&
		    a->engines[i] != NULL) {
			*ops = bt_engines[i];
			return (a->engines[i]);
		}
	}
	return (NULL);
}

/* The Classic engine of `a`, if attached. */
static struct bt_engine *
classic_of(struct bt_adapter *a, const struct bt_engine_ops **ops)
{
	return (engine_of(a, BT_TRANSPORT_BREDR, ops));
}

/* ---- init ----------------------------------------------------------------- */

static void
init_failed(struct bt_adapter *a, const char *why)
{
	xlog("%s: bring-up failed: %s", a->node, why);
	a->state = bt_adapter_error_e;
	a->init_opcode = 0;
	a->init_deadline = 0;
	a->dirty = true;
}

static void
init_finish(struct bt_adapter *a)
{
	char addr[BT_ADDR_STRLEN];

	a->init_opcode = 0;
	a->init_deadline = 0;
	if (hci_node_init(a->hci_fd) < 0)
		xlog("%s: NGM_HCI_NODE_INIT: %s", a->node, strerror(errno));
	a->state = a->powered ? bt_adapter_powered_on_e :
	    bt_adapter_powered_off_e;
	attach_engines(a);
	a->dirty = true;
	if (a->powered)
		conn_adapter_up_locked(a);
	xlog("%s: ready: %s \"%s\", %s%s%s, powered %s", a->node,
	    a->have_addr ? bt_addr_ntoa(&a->addr, BT_ADDR_FMT_DISPLAY, addr,
	    sizeof(addr)) : "?", a->name,
	    HCI_FEAT_SSP(a->features) ? "SSP" : "legacy pairing",
	    HCI_FEAT_LE(a->features) ? ", LE" : "",
	    HCI_FEAT_EIR(a->features) ? ", EIR" : "",
	    a->powered ? "on" : "off");
}

/*
 * Send the command for the current step, skipping steps this controller has
 * no use for. Moves to INIT_DONE when there is nothing left.
 */
static void
init_send(struct bt_adapter *a)
{
	uint8_t buf[NG_HCI_UNIT_NAME_SIZE];
	uint16_t op;
	size_t len;
	uint64_t timeout = INIT_TIMEOUT_MS;

	for (;;) {
		len = 0;
		op = 0;
		switch (a->init_step) {
		case INIT_RESET:
			/* A graph that outlived a restart is already up. */
			if (a->adopted) {
				int ns = hci_node_state(a->hci_fd);

				if (ns >= 0 && (ns & NG_HCI_UNIT_INITED) != 0)
					break;
			}
			op = HCI_OPCODE(OGF_HC, NG_HCI_OCF_RESET);
			timeout = RESET_TIMEOUT_MS;
			break;
		case INIT_READ_VERSION:
			op = HCI_OPCODE(OGF_INFO, NG_HCI_OCF_READ_LOCAL_VER);
			break;
		case INIT_READ_BDADDR:
			op = HCI_OPCODE(OGF_INFO, NG_HCI_OCF_READ_BDADDR);
			break;
		case INIT_READ_FEATURES:
			op = HCI_OPCODE(OGF_INFO,
			    NG_HCI_OCF_READ_LOCAL_FEATURES);
			break;
		case INIT_READ_BUFFER_SIZE:
			op = HCI_OPCODE(OGF_INFO, NG_HCI_OCF_READ_BUFFER_SIZE);
			break;
		case INIT_LE_READ_BUFFER_SIZE:
			if (!HCI_FEAT_LE(a->features))
				break;
			op = HCI_OPCODE(OGF_LE,
			    NG_HCI_OCF_LE_READ_BUFFER_SIZE);
			break;
		case INIT_LE_EVENT_MASK:
			if (!HCI_FEAT_LE(a->features))
				break;
			put_le64(buf, HCI_LE_EVENT_MASK_BLUETOOTHD);
			len = 8;
			op = HCI_OPCODE(OGF_LE, NG_HCI_OCF_LE_SET_EVENT_MASK);
			break;
		case INIT_LE_HOST_SUPPORTED:
			if (!HCI_FEAT_LE(a->features))
				break;
			buf[0] = 1;	/* LE supported (host) */
			buf[1] = 0;	/* simultaneous LE and BR/EDR: no */
			len = 2;
			op = HCI_OPCODE(OGF_HC,
			    NG_HCI_OCF_WRITE_LE_HOST_SUPPORTED);
			break;
		case INIT_EVENT_MASK:
			put_le64(buf, HCI_EVENT_MASK_BLUETOOTHD);
			len = 8;
			op = HCI_OPCODE(OGF_HC, NG_HCI_OCF_SET_EVENT_MASK);
			break;
		case INIT_SSP_MODE:
			if (!HCI_FEAT_SSP(a->features))
				break;
			buf[0] = 1;
			len = 1;
			op = HCI_OPCODE(OGF_HC,
			    HCI_OCF_WRITE_SIMPLE_PAIRING_MODE);
			break;
		case INIT_INQUIRY_MODE:
			if (HCI_FEAT_EIR(a->features))
				buf[0] = HCI_INQUIRY_MODE_EXTENDED;
			else if (HCI_FEAT_RSSI_INQUIRY(a->features))
				buf[0] = HCI_INQUIRY_MODE_RSSI;
			else
				break;
			len = 1;
			op = HCI_OPCODE(OGF_HC, HCI_OCF_WRITE_INQUIRY_MODE);
			break;
		case INIT_CLASS:
			buf[0] = (LOCAL_CLASS_OF_DEVICE | g_cod_services) &
			    0xff;
			buf[1] = ((LOCAL_CLASS_OF_DEVICE | g_cod_services) >>
			    8) & 0xff;
			buf[2] = ((LOCAL_CLASS_OF_DEVICE | g_cod_services) >>
			    16) & 0xff;
			len = 3;
			op = HCI_OPCODE(OGF_HC, NG_HCI_OCF_WRITE_UNIT_CLASS);
			break;
		case INIT_NAME:
			if (!bt_state_computer_name(a->name, sizeof(a->name)))
				(void)strlcpy(a->name, "NextBSD",
				    sizeof(a->name));
			(void)memset(buf, 0, sizeof(buf));
			(void)strlcpy((char *)buf, a->name, sizeof(buf));
			len = NG_HCI_UNIT_NAME_SIZE;
			op = HCI_OPCODE(OGF_HC, NG_HCI_OCF_CHANGE_LOCAL_NAME);
			break;
		case INIT_SCAN_ENABLE:
			a->powered = a->want_power;
			a->discoverable = a->powered &&
			    bluetoothd_discoverable_pref_locked();
			op = HCI_OPCODE(OGF_HC, NG_HCI_OCF_WRITE_SCAN_ENABLE);
			if (send_scan_enable(a) < 0) {
				init_failed(a, strerror(errno));
				return;
			}
			a->init_opcode = op;
			a->init_deadline = bt_now_ms() + timeout;
			return;
		default:
			init_finish(a);
			return;
		}
		if (op != 0)
			break;
		a->init_step++;		/* not for this controller */
	}

	if (hci_send_cmd(a->hci_fd, op, len > 0 ? buf : NULL, len) < 0) {
		init_failed(a, strerror(errno));
		return;
	}
	a->init_opcode = op;
	a->init_deadline = bt_now_ms() + timeout;
}

/* The step's Command Complete (or Status) arrived. */
static void
init_result(struct bt_adapter *a, uint8_t status, const uint8_t *rp,
    size_t rplen)
{
	int step = a->init_step;

	a->init_opcode = 0;
	if (status != 0) {
		if (step == INIT_RESET && ++a->init_tries < RESET_TRIES) {
			xlog("%s: Reset failed (0x%02x), retrying", a->node,
			    status);
			init_send(a);
			return;
		}
		if (mandatory(step)) {
			char why[64];

			(void)snprintf(why, sizeof(why), "%s: status 0x%02x",
			    init_names[step], status);
			init_failed(a, why);
			return;
		}
		xlog("%s: %s: status 0x%02x (continuing)", a->node,
		    init_names[step], status);
	} else {
		switch (step) {
		case INIT_READ_VERSION:
			if (rplen >= 7) {
				a->hci_version = rp[0];
				a->manufacturer = (uint16_t)(rp[4] |
				    rp[5] << 8);
			}
			break;
		case INIT_READ_BDADDR:
			if (rplen < BT_ADDR_LEN) {
				init_failed(a, "short Read_BD_ADDR");
				return;
			}
			hci_addr_from_wire(rp, &a->addr);
			a->have_addr = true;
			break;
		case INIT_READ_FEATURES:
			if (rplen >= sizeof(a->features))
				(void)memcpy(a->features, rp,
				    sizeof(a->features));
			break;
		default:
			break;
		}
	}
	a->init_step++;
	init_send(a);
}

void
adapter_bringup(struct bt_adapter *a)
{
	char node[sizeof(a->node)], hci_node[sizeof(a->hci_node)];
	bool adopted = false;
	int fd = -1;
	const char *err = NULL;

	/* The slot is ours: the main loop is the only writer of identity. */
	(void)strlcpy(node, a->node, sizeof(node));
	(void)snprintf(hci_node, sizeof(hci_node), "%shci", node);

	if (!g_stack_loaded) {
		if (ngstack_load() == 0)
			g_stack_loaded = true;
		else
			err = "the upper Bluetooth kexts did not load";
	}
	if (err == NULL && ngstack_build(node, &adopted) < 0)
		err = "netgraph graph build failed";
	/*
	 * NgBtsocket is loaded and this controller's L2CAP node is up: the SDP
	 * server can listen on PSM 1. A no-op once it runs.
	 */
	if (err == NULL && sdp_server_start() < 0)
		xlog("%s: SDP server not started: %s", node, strerror(errno));
	/* The HID host's listeners (PSM 0x11/0x13), likewise. */
	if (err == NULL && hid_host_start() < 0)
		xlog("%s: HID listeners not started", node);
	/* AVRCP's listener and records; the A2DP sink's, if on. */
	if (err == NULL) {
		avrcp_start();
		a2dp_sink_prefs_changed();
	}
	if (err == NULL) {
		fd = hci_open(hci_node);
		if (fd < 0)
			err = "cannot open the raw HCI socket";
	}

	bluetoothd_lock();
	if (!a->active || strcmp(a->node, node) != 0) {
		/* It left while we were building; nothing to keep. */
		bluetoothd_unlock();
		if (fd >= 0)
			(void)close(fd);
		return;
	}
	(void)strlcpy(a->hci_node, hci_node, sizeof(a->hci_node));
	a->hci_fd = fd;
	a->adopted = adopted;
	if (err != NULL) {
		init_failed(a, err);
	} else {
		a->state = bt_adapter_initializing_e;
		a->init_step = INIT_RESET;
		a->init_tries = 0;
		a->dirty = true;
		init_send(a);
	}
	bluetoothd_unlock();
}

void
adapter_teardown_locked(struct bt_adapter *a)
{
	char key[128];

	pairing_drop_node_locked(a->node);
	conn_adapter_down_locked(a);
	detach_engines(a);
	if (a->hci_fd >= 0) {
		(void)close(a->hci_fd);
		a->hci_fd = -1;
	}
	ngstack_teardown(a->node);
	(void)snprintf(key, sizeof(key), "%s%s", BT_STATE_ADAPTER_PREFIX,
	    a->node);
	(void)bt_state_remove(key);
	(void)strlcat(key, "/A2DPSink", sizeof(key));
	(void)bt_state_remove(key);
	bt_state_notify(BT_NOTIFY_ADAPTER);
}

/* ---- events and timers ---------------------------------------------------- */

void
adapter_input_locked(struct bt_adapter *a)
{
	uint8_t buf[sizeof(ng_hci_event_pkt_t) + NG_HCI_EVENT_PKT_SIZE + 1];
	struct hci_event ev;
	int i, n, guard;

	for (guard = 0; guard < 64 && a->hci_fd >= 0; guard++) {
		uint16_t opcode;
		uint8_t status;
		const uint8_t *rp;
		size_t rplen;
		bool result;

		n = hci_recv_event(a->hci_fd, buf, sizeof(buf), &ev);
		if (n <= 0) {
			if (n < 0)
				xlog("%s: HCI socket: %s", a->node,
				    strerror(errno));
			return;
		}
		result = hci_cmd_result(&ev, &opcode, &status, &rp, &rplen);

		if (a->state == bt_adapter_initializing_e) {
			if (result && opcode == a->init_opcode)
				init_result(a, status, rp, rplen);
			continue;	/* engines are not attached yet */
		}

		if (result && status != 0 && opcode == HCI_OPCODE(OGF_HC,
		    NG_HCI_OCF_WRITE_SCAN_ENABLE))
			xlog("%s: Write_Scan_Enable failed: 0x%02x", a->node,
			    status);
		if (ev.code == NG_HCI_EVENT_HARDWARE_ERROR)
			xlog("%s: controller reports a hardware error",
			    a->node);

		for (i = 0; i < BT_ENGINE_MAX; i++) {
			if (a->engines[i] != NULL &&
			    bt_engines[i]->hci_event != NULL)
				bt_engines[i]->hci_event(a->engines[i],
				    ev.code, ev.params, ev.len);
		}
	}
}

static uint64_t
earlier(uint64_t t, uint64_t u)
{
	if (u == 0)
		return (t);
	if (t == 0 || u < t)
		return (u);
	return (t);
}

uint64_t
adapter_next_timer_locked(void)
{
	uint64_t t = conn_next_timer_locked();
	int i, j, n = bluetoothd_adapter_count_locked();

	for (i = 0; i < n; i++) {
		struct bt_adapter *a = bluetoothd_adapter_at_locked(i);

		if (a == NULL)
			continue;
		if (a->state == bt_adapter_initializing_e)
			t = earlier(t, a->init_deadline);
		t = earlier(t, a->discoverable_until);
		for (j = 0; j < BT_ENGINE_MAX; j++) {
			if (a->engines[j] != NULL &&
			    bt_engines[j]->next_timer != NULL)
				t = earlier(t,
				    bt_engines[j]->next_timer(a->engines[j]));
		}
	}
	return (t);
}

void
adapter_timers_locked(uint64_t now)
{
	time_t wall = time(NULL);
	int i, j, n = bluetoothd_adapter_count_locked();

	conn_timers_locked(now);
	for (i = 0; i < n; i++) {
		struct bt_adapter *a = bluetoothd_adapter_at_locked(i);

		if (a == NULL)
			continue;
		if (a->state == bt_adapter_initializing_e &&
		    a->init_deadline != 0 && now >= a->init_deadline) {
			int step = a->init_step;

			if (step == INIT_RESET &&
			    ++a->init_tries < RESET_TRIES) {
				xlog("%s: Reset timed out, retrying", a->node);
				init_send(a);
			} else if (mandatory(step)) {
				char why[64];

				(void)snprintf(why, sizeof(why),
				    "%s timed out", init_names[step]);
				init_failed(a, why);
			} else {
				xlog("%s: %s timed out (continuing)",
				    a->node, step < INIT_DONE ?
				    init_names[step] : "?");
				a->init_step++;
				init_send(a);
			}
		}
		if (a->discoverable_until != 0 &&
		    now >= a->discoverable_until) {
			a->discoverable_until = 0;
			(void)adapter_set_discoverable_locked(a, false, 0);
		}
		for (j = 0; j < BT_ENGINE_MAX; j++) {
			if (a->engines[j] != NULL &&
			    bt_engines[j]->timer != NULL)
				bt_engines[j]->timer(a->engines[j], now);
		}
		disc_prune(a, wall);
	}
}

/* ---- the host side of engine.h -------------------------------------------- */

/*
 * A new link key (engine.h BT_EV_BOND_UPDATED). The engine only raises it for
 * a bonded device or the pairing in progress. A first key creates the
 * PairedDevices entry with what discovery knew about the device.
 */
static void
save_link_key(struct bt_adapter *a, const struct bt_bond *k)
{
	struct bt_bond *b;
	const struct bt_found *f;
	char addr[BT_ADDR_STRLEN];
	bool existed;

	b = calloc(1, sizeof(*b));
	if (b == NULL)
		return;
	existed = bt_store_bond_find(&k->addr, true, b);
	if (!existed) {
		(void)memset(b, 0, sizeof(*b));
		b->addr = k->addr;
		b->addr.type = BT_ADDR_PUBLIC;
		(void)disc_name(a, &k->addr, b->name, sizeof(b->name));
		f = disc_find(a, &k->addr);
		if (f != NULL)
			b->class_of_device = f->class_of_device;
		b->autoconnect = true;
		b->trusted = true;
		b->paired_at = time(NULL);
		pairing_mark_created_locked(&k->addr);
	}
	b->transports |= BT_TRANSPORT_BREDR;
	b->flags |= BT_BOND_LINK_KEY;
	(void)memcpy(b->link_key, k->link_key, sizeof(b->link_key));
	b->link_key_type = k->link_key_type;
	if (bt_store_bond_save(b) != 0)
		xlog("%s: could not store the link key for %s", a->node,
		    bt_addr_ntoa(&k->addr, BT_ADDR_FMT_DISPLAY, addr,
		    sizeof(addr)));
	(void)memset(b->link_key, 0, sizeof(b->link_key));
	free(b);
}

/*
 * LE keys, or what HOGP cached (engine.h BT_EV_BOND_UPDATED from the LE
 * engine): merged into the bond for the identity address in k->addr. `peer`
 * is the address the device was seen and paired at, which may be a private
 * one: the discovery cache knows its name under that.
 */
static void
save_le_bond(struct bt_adapter *a, const struct bt_addr *peer,
    const struct bt_bond *k)
{
	struct bt_bond *b;
	const struct bt_found *f;
	char addr[BT_ADDR_STRLEN];
	bool existed;

	b = calloc(1, sizeof(*b));
	if (b == NULL)
		return;
	existed = bt_store_bond_find(&k->addr, true, b);
	if (!existed) {
		if ((k->flags & (BT_BOND_LTK | BT_BOND_IRK)) == 0) {
			free(b);	/* a cache update for a forgotten bond */
			return;
		}
		(void)memset(b, 0, sizeof(*b));
		b->addr = k->addr;
		if (!disc_name(a, peer, b->name, sizeof(b->name)))
			(void)disc_name(a, &k->addr, b->name,
			    sizeof(b->name));
		f = disc_find(a, peer);
		if (f != NULL)
			b->appearance = f->appearance;
		b->autoconnect = true;
		b->trusted = true;
		b->paired_at = k->paired_at != 0 ? k->paired_at : time(NULL);
	}
	b->transports |= BT_TRANSPORT_LE;
	b->profiles |= k->profiles;
	if ((k->flags & BT_BOND_LTK) != 0)
		b->ltk = k->ltk;
	if ((k->flags & BT_BOND_IRK) != 0)
		(void)memcpy(b->irk, k->irk, sizeof(b->irk));
	if ((k->flags & BT_BOND_CSRK) != 0)
		(void)memcpy(b->csrk, k->csrk, sizeof(b->csrk));
	if ((k->flags & BT_BOND_HID_DESC) != 0 && k->hid_desc_len > 0) {
		b->hid_desc_len = k->hid_desc_len;
		(void)memcpy(b->hid_desc, k->hid_desc, k->hid_desc_len);
	}
	if ((k->flags & BT_BOND_PNP_ID) != 0)
		b->pnp = k->pnp;
	b->flags |= k->flags & (BT_BOND_LTK | BT_BOND_IRK | BT_BOND_CSRK |
	    BT_BOND_HID_DESC | BT_BOND_PNP_ID);
	if (bt_store_bond_save(b) != 0)
		xlog("%s: could not store the LE bond for %s", a->node,
		    bt_addr_ntoa(&k->addr, BT_ADDR_FMT_DISPLAY, addr,
		    sizeof(addr)));
	else if (existed && (k->flags & (BT_BOND_HID_DESC |
	    BT_BOND_PNP_ID)) != 0)
		pairing_device_changed_locked(&k->addr, true);
	explicit_bzero(b, sizeof(*b));
	free(b);
}

static void
host_event(void *ctx, const struct bt_engine *e, const struct bt_event *ev)
{
	struct bt_adapter *a = ctx;
	int i;

	switch (ev->kind) {
	case BT_EV_FOUND:
		disc_found(a, &ev->u.found);
		break;
	case BT_EV_DISCOVERY:
		/* Both engines scan; the controller scans while either does. */
		for (i = 0; i < BT_ENGINE_MAX; i++) {
			if (a->engines[i] != e)
				continue;
			if (ev->u.scanning)
				a->scanning_engines |= (uint8_t)(1u << i);
			else
				a->scanning_engines &= (uint8_t)~(1u << i);
		}
		a->scanning = a->scanning_engines != 0;
		a->dirty = true;
		a->scan_dirty = true;
		break;
	case BT_EV_PAIR_PROMPT:
		pairing_prompt_locked(&ev->u.prompt);
		break;
	case BT_EV_BOND_UPDATED:
		if (ev->u.bond == NULL)
			break;
		if (ev->transport == BT_TRANSPORT_LE)
			save_le_bond(a, &ev->peer, ev->u.bond);
		else if ((ev->u.bond->flags & BT_BOND_LINK_KEY) != 0)
			save_link_key(a, ev->u.bond);
		break;
	case BT_EV_PAIRED:
		(void)pairing_finish_locked(&ev->peer, bt_status_success_e);
		/* An LE device is published under its identity address. */
		pairing_device_changed_locked(ev->u.bond != NULL ?
		    &ev->u.bond->addr : &ev->peer, true);
		/*
		 * Read a Classic device's SDP records (HID descriptor, profiles)
		 * into its bond; LE devices describe themselves over GATT. Its
		 * own thread; it merges under bluetoothd_lock(), so it waits
		 * until this callback's caller lets go. A Classic HID device is
		 * then connected for its first session (connections.c).
		 */
		if (ev->transport != BT_TRANSPORT_LE)
			conn_paired_locked(&ev->peer);
		if (ev->transport != BT_TRANSPORT_LE &&
		    sdp_client_refresh_bond(&ev->peer) != 0)
			xlog("%s: could not start the SDP query", a->node);
		break;
	case BT_EV_CONNECTED:
	case BT_EV_DISCONNECTED:
	case BT_EV_LINK_CHANGED:
		conn_event_locked(a, ev);
		break;
	case BT_EV_PAIR_FAILED:
		if (pairing_finish_locked(&ev->peer, ev->u.status)) {
			/* The half-made bond goes with the failed pairing. */
			(void)bt_store_bond_forget(&ev->peer);
		}
		break;
	default:
		break;
	}
	bluetoothd_wake();
}

static bool
host_bond_lookup(void *ctx, const struct bt_addr *peer, struct bt_bond *out)
{
	struct bt_addr p = *peer;

	(void)ctx;
	/* An LE identity may be a static random address; Classic is public. */
	if (bt_store_bond_find(&p, true, out))
		return (true);
	if (p.type == BT_ADDR_PUBLIC)
		return (false);
	p.type = BT_ADDR_PUBLIC;
	return (bt_store_bond_find(&p, true, out));
}

static bool
host_name_known(void *ctx, const struct bt_addr *peer)
{
	char name[BT_NAME_MAX + 1];

	return (disc_name(ctx, peer, name, sizeof(name)));
}

/* ---- RPCs ------------------------------------------------------------------ */

static bool
ready(const struct bt_adapter *a)
{
	return (a->state == bt_adapter_powered_on_e ||
	    a->state == bt_adapter_powered_off_e);
}

bt_status_t
adapter_set_power_locked(struct bt_adapter *a, bool on)
{
	int i;

	a->want_power = on;
	if (!ready(a))
		return (bt_status_success_e);	/* applied at the end of init */
	if (a->powered == on)
		return (bt_status_success_e);

	if (!on) {
		uint16_t h[NG_HCI_MAX_CON_NUM];
		int n;

		/* Engines first: they cancel inquiry and pairing. */
		for (i = 0; i < BT_ENGINE_MAX; i++) {
			if (a->engines[i] != NULL &&
			    bt_engines[i]->set_power != NULL)
				(void)bt_engines[i]->set_power(a->engines[i],
				    false);
		}
		n = hci_acl_handles(a->hci_fd, h, NG_HCI_MAX_CON_NUM);
		for (i = 0; i < n; i++) {
			ng_hci_discon_cp cp;

			cp.con_handle = htole16(h[i]);
			cp.reason = HCI_ERR_REMOTE_POWER_OFF;
			(void)hci_send_cmd(a->hci_fd, HCI_OPCODE(OGF_LC,
			    NG_HCI_OCF_DISCON), &cp, sizeof(cp));
		}
		a->discoverable = false;
		a->discoverable_until = 0;
		a->powered = false;
		a->scanning = false;
		conn_adapter_down_locked(a);
	} else {
		a->powered = true;
		a->discoverable = bluetoothd_discoverable_pref_locked();
	}
	if (send_scan_enable(a) < 0)
		xlog("%s: Write_Scan_Enable: %s", a->node, strerror(errno));
	if (on) {
		for (i = 0; i < BT_ENGINE_MAX; i++) {
			if (a->engines[i] != NULL &&
			    bt_engines[i]->set_power != NULL)
				(void)bt_engines[i]->set_power(a->engines[i],
				    true);
		}
	}
	a->state = on ? bt_adapter_powered_on_e : bt_adapter_powered_off_e;
	a->dirty = true;
	if (on)
		conn_adapter_up_locked(a);
	xlog("%s: powered %s", a->node, on ? "on" : "off");
	bluetoothd_wake();
	return (bt_status_success_e);
}

bt_status_t
adapter_set_discoverable_locked(struct bt_adapter *a, bool on, int seconds)
{
	if (!ready(a))
		return (bt_status_busy_e);
	if (on && !a->powered)
		return (bt_status_powered_off_e);
	a->discoverable = on;
	a->discoverable_until = (on && seconds > 0) ?
	    bt_now_ms() + (uint64_t)seconds * 1000 : 0;
	if (send_scan_enable(a) < 0)
		return (bt_status_internal_error_e);
	a->dirty = true;
	xlog("%s: discoverable %s%s", a->node, on ? "on" : "off",
	    a->discoverable_until != 0 ? " (timed)" : "");
	bluetoothd_wake();
	return (bt_status_success_e);
}

bt_status_t
adapter_scan_locked(struct bt_adapter *a, int seconds)
{
	bt_status_t st = bt_status_operation_not_supported_e, r;
	bool any = false;
	int i;

	if (!ready(a))
		return (bt_status_busy_e);
	if (!a->powered)
		return (bt_status_powered_off_e);
	if (seconds <= 0)
		seconds = 10;
	if (seconds > 60)
		seconds = 60;
	/* Inquiry and LE scanning together: one list (discovery.c). */
	for (i = 0; i < BT_ENGINE_MAX && bt_engines[i] != NULL; i++) {
		if (a->engines[i] == NULL || bt_engines[i]->discover == NULL)
			continue;
		r = bt_engines[i]->discover(a->engines[i], true, seconds);
		if (r == bt_status_success_e)
			any = true;
		else if (st == bt_status_operation_not_supported_e)
			st = r;
	}
	bluetoothd_wake();
	return (any ? bt_status_success_e : st);
}

/*
 * The controller to pair through: the one whose scan saw the device, else the
 * first powered one.
 */
static struct bt_adapter *
pick_for_peer(const struct bt_addr *peer)
{
	struct bt_adapter *a, *first = NULL;
	int i, n = bluetoothd_adapter_count_locked();

	for (i = 0; i < n; i++) {
		a = bluetoothd_adapter_at_locked(i);
		if (a == NULL || !ready(a))
			continue;
		if (disc_find(a, peer) != NULL)
			return (a);
		if (first == NULL || (!first->powered && a->powered))
			first = a;
	}
	return (first);
}

/*
 * Which engine pairs `peer`, by what scanning saw of it, its address type or
 * an earlier bond: LE for random addresses and LE-only devices, Classic for
 * the rest, dual-mode devices included (Classic HID is preferred over HOGP).
 * The address the user typed carries no type, so a device the LE scan saw
 * at a random address is matched on its six octets and `peer` takes the
 * random type.
 */
static int
pair_transport(struct bt_adapter *a, struct bt_addr *peer)
{
	const struct bt_found *f = disc_find(a, peer);
	struct bt_addr r;
	struct bt_bond *b;
	int t = BT_TRANSPORT_BREDR;

	if (f == NULL && peer->type == BT_ADDR_PUBLIC) {
		r = *peer;
		r.type = BT_ADDR_RANDOM;
		if ((f = disc_find(a, &r)) != NULL)
			*peer = r;
	}
	if (peer->type == BT_ADDR_RANDOM)
		return (BT_TRANSPORT_LE);
	if (f != NULL)
		return ((f->transports & BT_TRANSPORT_BREDR) != 0 ?
		    BT_TRANSPORT_BREDR : BT_TRANSPORT_LE);
	b = calloc(1, sizeof(*b));
	if (b != NULL && bt_store_bond_find(peer, false, b) &&
	    b->transports == BT_TRANSPORT_LE)
		t = BT_TRANSPORT_LE;
	free(b);
	return (t);
}

bt_status_t
adapter_pair_locked(const struct bt_addr *peer, const char *pin)
{
	const struct bt_engine_ops *ops;
	struct bt_adapter *a;
	struct bt_engine *e;
	struct bt_addr p = *peer;
	bt_status_t st;
	int transport;

	a = pick_for_peer(&p);
	if (a == NULL)
		return (bt_status_no_adapter_e);
	if (!a->powered)
		return (bt_status_powered_off_e);
	transport = pair_transport(a, &p);
	e = engine_of(a, transport, &ops);
	if (e == NULL || ops->pair == NULL)
		return (bt_status_operation_not_supported_e);
	if (pairing_active_locked(&p))
		return (bt_status_busy_e);

	pairing_begin_locked(a->node, &p);
	st = ops->pair(e, &p, BT_IO_DISPLAY_YES_NO,
	    (pin != NULL && pin[0] != '\0') ? pin : NULL);
	if (st != bt_status_success_e)
		(void)pairing_finish_locked(&p, st);
	bluetoothd_wake();
	return (st);
}

bt_status_t
adapter_pair_confirm_locked(const struct bt_addr *peer, bool accept,
    uint32_t passkey, const char *pin)
{
	const struct bt_engine_ops *ops;
	struct bt_pair_prompt pr;
	struct bt_pair_answer ans;
	struct bt_adapter *a;
	struct bt_engine *e;
	bt_pair_state_t state;
	bt_status_t result, st;
	char node[32];

	if (!pairing_status_locked(peer, &state, &result, &pr, node,
	    sizeof(node)) || state != bt_pair_state_waiting_e)
		return (bt_status_no_prompt_e);
	a = bluetoothd_adapter_by_node_locked(node);
	if (a == NULL)
		return (bt_status_no_adapter_e);
	e = engine_of(a, pr.transport == BT_TRANSPORT_LE ? BT_TRANSPORT_LE :
	    BT_TRANSPORT_BREDR, &ops);
	if (e == NULL || ops->pair_reply == NULL)
		return (bt_status_operation_not_supported_e);

	(void)memset(&ans, 0, sizeof(ans));
	ans.id = pr.id;
	ans.peer = pr.peer;
	ans.accept = accept;
	ans.passkey = passkey;
	if (pin != NULL)
		(void)strlcpy(ans.pin, pin, sizeof(ans.pin));
	st = ops->pair_reply(e, &ans);
	if (st == bt_status_success_e)
		pairing_answered_locked(peer);
	bluetoothd_wake();
	return (st);
}

bt_status_t
adapter_unpair_locked(const struct bt_addr *peer)
{
	const struct bt_engine_ops *ops;
	struct bt_bond *b;
	struct bt_engine *e;
	struct bt_addr p = *peer;
	bool found;
	int i, transports, n = bluetoothd_adapter_count_locked();

	b = calloc(1, sizeof(*b));
	if (b == NULL)
		return (bt_status_internal_error_e);
	found = bt_store_bond_find(&p, false, b);
	if (!found && p.type == BT_ADDR_PUBLIC) {
		/* An LE device with a static random identity. */
		p.type = BT_ADDR_RANDOM;
		found = bt_store_bond_find(&p, false, b);
	}
	transports = b->transports;
	free(b);
	if (!found)
		return (bt_status_not_paired_e);

	/* A virtual-cable HID device is told before its link goes. */
	hid_host_unplug(&p);
	for (i = 0; i < n; i++) {
		struct bt_adapter *a = bluetoothd_adapter_at_locked(i);

		if (a == NULL || !ready(a))
			continue;
		e = classic_of(a, &ops);
		if (e != NULL && ops->unpair != NULL &&
		    p.type == BT_ADDR_PUBLIC &&
		    (transports & BT_TRANSPORT_LE) != transports)
			(void)ops->unpair(e, &p);
		e = engine_of(a, BT_TRANSPORT_LE, &ops);
		if (e != NULL && ops->unpair != NULL &&
		    (transports & BT_TRANSPORT_LE) != 0)
			(void)ops->unpair(e, &p);
	}
	if (bt_store_bond_forget(&p) != 0)
		return (bt_status_store_error_e);
	audio_bond_updated_locked(&p);		/* its mixer stops */
	pairing_device_changed_locked(&p, false);
	bluetoothd_wake();
	return (bt_status_success_e);
}

/* ---- publication ------------------------------------------------------------ */

void
adapter_publish(void)
{
	struct snap {
		BTNodeName		node;
		char			address[BT_ADDR_STRLEN];
		BTDeviceName		name;
		BTTransportName		transport;
		int			vendor, product;
		bt_adapter_state_t	state;
		bool			powered, discoverable, scanning;
	} *snaps;
	bool adapter_changed = false, scan_changed = false;
	int i, n, k = 0;

	snaps = calloc(BT_MAX_ADAPTERS, sizeof(*snaps));
	if (snaps == NULL)
		return;

	bluetoothd_lock();
	n = bluetoothd_adapter_count_locked();
	for (i = 0; i < n; i++) {
		struct bt_adapter *a = bluetoothd_adapter_at_locked(i);
		struct snap *s;

		if (a == NULL)
			continue;
		if (a->scan_dirty) {
			a->scan_dirty = false;
			scan_changed = true;
		}
		if (!a->dirty)
			continue;
		a->dirty = false;
		s = &snaps[k++];
		(void)strlcpy(s->node, a->node, sizeof(s->node));
		if (a->have_addr)
			(void)bt_addr_ntoa(&a->addr, BT_ADDR_FMT_DISPLAY,
			    s->address, sizeof(s->address));
		(void)strlcpy(s->name, a->name, sizeof(s->name));
		(void)strlcpy(s->transport, a->transport,
		    sizeof(s->transport));
		s->vendor = a->vendor;
		s->product = a->product;
		s->state = a->state;
		s->powered = a->powered;
		s->discoverable = a->discoverable;
		s->scanning = a->scanning;
	}
	bluetoothd_unlock();

	for (i = 0; i < k; i++) {
		struct bt_adapter_summary sum;

		(void)memset(&sum, 0, sizeof(sum));
		sum.node = snaps[i].node;
		sum.address = snaps[i].address;
		sum.name = snaps[i].name;
		sum.transport = snaps[i].transport;
		sum.vendor = snaps[i].vendor;
		sum.product = snaps[i].product;
		sum.state = state_name(snaps[i].state);
		sum.powered = snaps[i].powered;
		sum.discoverable = snaps[i].discoverable;
		sum.connectable = snaps[i].powered;
		sum.scanning = snaps[i].scanning;
		(void)bt_state_publish_adapter(&sum);
		adapter_changed = true;
	}
	free(snaps);

	if (adapter_changed)
		bt_state_notify(BT_NOTIFY_ADAPTER);
	if (scan_changed)
		bt_state_notify(BT_NOTIFY_SCAN);
	pairing_publish();
	conn_publish();
	audio_publish();
	a2dp_sink_publish();
}

struct bt_engine *
adapter_classic_locked(struct bt_adapter *a, const struct bt_engine_ops **ops)
{
	return (classic_of(a, ops));
}

struct bt_engine *
adapter_engine_locked(struct bt_adapter *a, int transport,
    const struct bt_engine_ops **ops)
{
	*ops = NULL;
	return (engine_of(a, transport, ops));
}

void
adapter_set_cod_services_locked(int owner, uint32_t bits)
{
	uint32_t cod, all = 0;
	uint8_t buf[3];
	int i, n = bluetoothd_adapter_count_locked();

	if (owner < 0 || owner >= ADAPTER_COD_OWNERS)
		return;
	g_cod_owner[owner] = bits;
	for (i = 0; i < ADAPTER_COD_OWNERS; i++)
		all |= g_cod_owner[i];
	if (all == g_cod_services)
		return;
	g_cod_services = all;
	bits = all;
	cod = LOCAL_CLASS_OF_DEVICE | bits;
	buf[0] = cod & 0xff;
	buf[1] = (cod >> 8) & 0xff;
	buf[2] = (cod >> 16) & 0xff;
	/* Controllers still initialising pick it up at INIT_CLASS. */
	for (i = 0; i < n; i++) {
		struct bt_adapter *a = bluetoothd_adapter_at_locked(i);

		if (a == NULL || !ready(a) || a->hci_fd < 0)
			continue;
		if (hci_send_cmd(a->hci_fd, HCI_OPCODE(OGF_HC,
		    NG_HCI_OCF_WRITE_UNIT_CLASS), buf, sizeof(buf)) < 0)
			xlog("%s: Write_Class_Of_Device: %s", a->node,
			    strerror(errno));
		else
			xlog("%s: Class of Device %06x", a->node, cod);
	}
}
