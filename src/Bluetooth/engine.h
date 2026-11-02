/*
 * engine.h — the contract between bluetoothd's policy layer and its two
 * protocol engines, Classic (BR/EDR) and LE.
 *
 * THE TWO-ENGINE RULE
 *
 * The policy layer (controller lifecycle, the device list, pairing prompts,
 * the pairing store, SCDynamicStore publication, the MIG surface) must not know
 * whether a device is BR/EDR or LE. If the Classic engine were written first
 * with Classic-only types (a bare 6-byte address, a 16-byte link key, PIN-only
 * prompts), adding LE later would touch every layer. So the types every layer
 * shares are defined here once:
 *
 *   struct bt_addr         a typed address: BD_ADDR + public/random
 *   struct bt_pair_prompt  one prompt shape for legacy PIN, SSP and LE SMP
 *   struct bt_pair_answer  the answer the client sends back
 *   struct bt_bond         one bond record for both engines' keys, and the
 *                          device metadata that lets a paired device reconnect
 *                          (and a keyboard work before login) without an SDP or
 *                          GATT round trip
 *   struct bt_engine_ops   the function table each engine implements
 *   struct bt_engine_host  what bluetoothd hands an engine: its event sink
 *
 * WLAN precedent: src/WLAN/supplicant.h puts a narrow contract between wland
 * and wpa_supplicant (an opaque engine, an event fd, a pump that returns
 * "state changed", caller-owned caches). This is the same shape, for two
 * engines instead of one.
 *
 * Darwin shape: on macOS bluetoothd is the core (HCI, security, store,
 * Classic) and BTLEServer is the LE engine. Here that seam is a module
 * boundary, not a process boundary, because both engines share one controller,
 * one raw HCI socket and one key store.
 *
 * Pure C, no CoreFoundation: the engines and the policy layer stay plain C;
 * CF is confined to btstore.c, keystore.c, btstate.c and iokit_watch.c.
 */
#ifndef _BLUETOOTHD_ENGINE_H_
#define _BLUETOOTHD_ENGINE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "bluetooth_mig_types.h"	/* bt_status_t, bt_pair_method_t */

/* ------------------------------------------------------------------ */
/* Addresses                                                           */
/* ------------------------------------------------------------------ */

#define	BT_ADDR_LEN	6

/*
 * Address type. Values are the LE HCI Peer_Address_Type / SMP values (0 public,
 * 1 random). A BR/EDR address is always public; which transport a device was
 * seen on is a separate property (BT_TRANSPORT_* in bluetooth_mig_types.h), so
 * a dual-mode device with one public address is one device, not two.
 *
 * The kernel's sockaddr_l2cap folds the two into one field (BDADDR_BREDR,
 * BDADDR_LE_PUBLIC, BDADDR_LE_RANDOM in ng_bluetooth.h); bt_addr_l2cap_type()
 * maps to it.
 */
typedef enum {
	BT_ADDR_PUBLIC	= 0,
	BT_ADDR_RANDOM	= 1
} bt_addr_type_t;

/*
 * A typed device address. b[] is in HCI wire order: b[0] is the LEAST
 * significant octet, exactly as bdaddr_t. Text forms print it most significant
 * first.
 */
struct bt_addr {
	uint8_t		b[BT_ADDR_LEN];
	uint8_t		type;		/* bt_addr_type_t */
};

/*
 * Text forms. One formatter, two styles:
 *
 *   BT_ADDR_FMT_DISPLAY  "AA:BB:CC:DD:EE:FF", "AA:BB:CC:DD:EE:FF/random".
 *                        What bluetooth(8) prints and accepts.
 *   BT_ADDR_FMT_KEY      "aa-bb-cc-dd-ee-ff", "aa-bb-cc-dd-ee-ff-random".
 *                        The pairing store's dictionary key and the last
 *                        component of State:/Bluetooth/Device/<addr>. No '/'
 *                        and no ':' so it is safe as a dynamic-store path
 *                        component; dashes and lower case as IOBluetooth's
 *                        addressString.
 *
 * BT_ADDR_STRLEN fits either, with its NUL.
 */
#define	BT_ADDR_STRLEN	32

typedef enum {
	BT_ADDR_FMT_DISPLAY	= 0,
	BT_ADDR_FMT_KEY		= 1
} bt_addr_fmt_t;

const char	*bt_addr_ntoa(const struct bt_addr *a, bt_addr_fmt_t fmt,
		    char *buf, size_t len);

/*
 * Parse either text form (':' or '-' separators, any case, optional "/random"
 * or "-random" suffix; "/public" is accepted too). Returns false on anything
 * else.
 */
bool	bt_addr_aton(const char *s, struct bt_addr *out);

bool	bt_addr_equal(const struct bt_addr *a, const struct bt_addr *b);

/* sockaddr_l2cap.l2cap_bdaddr_type for `a` on `transport` (BT_TRANSPORT_*). */
uint8_t	bt_addr_l2cap_type(const struct bt_addr *a, int transport);

/* ------------------------------------------------------------------ */
/* Pairing prompts (pull model)                                        */
/* ------------------------------------------------------------------ */

/*
 * Local IO capability, as both HCI IO_Capability_Request_Reply and SMP Pairing
 * Request encode it (SMP adds KeyboardDisplay; HCI maps it to DisplayYesNo).
 */
typedef enum {
	BT_IO_DISPLAY_ONLY		= 0,
	BT_IO_DISPLAY_YES_NO		= 1,
	BT_IO_KEYBOARD_ONLY		= 2,
	BT_IO_NO_INPUT_NO_OUTPUT	= 3,
	BT_IO_KEYBOARD_DISPLAY		= 4
} bt_io_cap_t;

#define	BT_PIN_MAX	16
#define	BT_PASSKEY_MAX	999999

/*
 * A pairing request waiting for the user. An engine raises one through
 * bt_engine_host.event (BT_EV_PAIR_PROMPT); bluetoothd parks it, a client
 * fetches it over MIG (bt_pair_status) and answers it (bt_pair_confirm), and
 * bluetoothd hands the answer back through bt_engine_ops.pair_reply. No client
 * is ever called back: nothing above the Mach line can receive a Mach message.
 *
 *   just works            accept or reject
 *   numeric comparison    show `passkey`; accept if the peer shows the same
 *   passkey entry         the user types the 6 digits the peer displays
 *   passkey display       show `passkey`; the user types it on the peer
 *   PIN                   legacy pairing; `pin_min` is 16 when the peer
 *                         demands a secure (16-digit) PIN, else 1
 *
 * `id` is unique for the daemon's lifetime, so an answer to a prompt that has
 * since expired or been replaced is rejected rather than misapplied.
 */
struct bt_pair_prompt {
	uint32_t		id;
	struct bt_addr		peer;
	int			transport;	/* BT_TRANSPORT_* */
	bt_pair_method_t	method;
	uint32_t		passkey;	/* numeric comparison, display */
	uint8_t			pin_min;	/* PIN only */
	bool			initiated;	/* we started this pairing */
	time_t			expires;	/* the engine fails it after this */
};

struct bt_pair_answer {
	uint32_t		id;		/* bt_pair_prompt.id */
	struct bt_addr		peer;
	bool			accept;
	uint32_t		passkey;	/* passkey entry */
	char			pin[BT_PIN_MAX + 1];	/* PIN */
};

/* ------------------------------------------------------------------ */
/* Bond record                                                         */
/* ------------------------------------------------------------------ */

#define	BT_KEY_LEN		16
#define	BT_HID_DESC_MAX		4096

/*
 * Profiles, as a bitmask in C and as an array of names in the store
 * (PairedDevices/<addr>/Profiles). Append; never renumber. The names are in
 * engine.c's table and are what bluetooth(8) prints.
 */
#define	BT_PROFILE_HID		0x0001	/* "HID":        Classic HID (HIDP) */
#define	BT_PROFILE_HOGP		0x0002	/* "HOGP":       HID over GATT (LE) */
#define	BT_PROFILE_A2DP_SINK	0x0004	/* "A2DPSink":   the peer plays audio */
#define	BT_PROFILE_A2DP_SOURCE	0x0008	/* "A2DPSource": the peer sends audio */
#define	BT_PROFILE_AVRCP	0x0010	/* "AVRCP" */
#define	BT_PROFILE_HFP		0x0020	/* "HFP":        hands-free headset */

const char	*bt_profile_name(uint32_t bit);
uint32_t	bt_profile_bit(const char *name);	/* 0 if unknown */

/* Write the names of `mask` comma-separated into buf ("" when none). */
void		bt_profiles_format(uint32_t mask, char *buf, size_t len);

/* What the bond holds. */
#define	BT_BOND_LINK_KEY	0x01	/* Classic link key */
#define	BT_BOND_LTK		0x02	/* LE long-term key */
#define	BT_BOND_IRK		0x04	/* LE identity resolving key */
#define	BT_BOND_CSRK		0x08	/* LE signature key */
#define	BT_BOND_HID_DESC	0x10	/* cached HID report descriptor */
#define	BT_BOND_PNP_ID		0x20	/* PnP ID (SDP 0x1200 / DIS 0x2A50) */

/* Device ID / PnP ID. vendor_id_source: 1 Bluetooth SIG, 2 USB-IF. */
struct bt_pnp_id {
	uint8_t		vendor_id_source;
	uint16_t	vendor_id;
	uint16_t	product_id;
	uint16_t	version;
};

/*
 * LE long-term key. As central (the host pairing a keyboard) this is the
 * peripheral's key, used to encrypt on reconnect: LE Secure Connections
 * derives one LTK with ediv = rand = 0; legacy pairing distributes it with its
 * EDIV and Rand.
 */
struct bt_le_ltk {
	uint8_t		key[BT_KEY_LEN];
	uint16_t	ediv;
	uint64_t	rand;
	uint8_t		key_size;	/* 7..16 */
	bool		authenticated;	/* MITM-protected */
	bool		secure_conn;	/* LE Secure Connections */
};

/*
 * One bonded device, for both engines. This is the in-memory record; the
 * store splits it on disk:
 *
 *   metadata -> org.nextbsd.Bluetooth.plist       0644  (btstore.c)
 *   secrets  -> org.nextbsd.Bluetooth.keys.plist  0600  (keystore.c)
 *
 * so a future keychain replaces keystore.c and nothing else. `flags` says
 * which optional parts are present. All key bytes are in HCI/SMP wire order.
 * Key bytes are never logged.
 */
struct bt_bond {
	struct bt_addr		addr;		/* identity address */
	int			transports;	/* BT_TRANSPORT_* */
	uint32_t		flags;		/* BT_BOND_* */

	/* -- metadata (0644) -- */
	char			name[BT_NAME_MAX + 1];
	uint32_t		class_of_device;	/* Classic CoD */
	uint16_t		appearance;		/* LE GAP appearance */
	struct bt_pnp_id	pnp;
	uint32_t		profiles;		/* BT_PROFILE_* */
	bool			autoconnect;
	bool			trusted;		/* accept its connections */
	time_t			paired_at;
	time_t			last_connected;
	uint16_t		hid_desc_len;
	uint8_t			hid_desc[BT_HID_DESC_MAX];

	/* -- secrets (0600) -- */
	uint8_t			link_key[BT_KEY_LEN];	/* Classic */
	uint8_t			link_key_type;		/* HCI key type */
	struct bt_le_ltk	ltk;			/* LE */
	uint8_t			irk[BT_KEY_LEN];	/* LE, peer's */
	uint8_t			csrk[BT_KEY_LEN];	/* LE, peer's */
};

/* ------------------------------------------------------------------ */
/* Events an engine raises                                             */
/* ------------------------------------------------------------------ */

typedef enum {
	BT_EV_FOUND		= 1,	/* discovery saw a device */
	BT_EV_PAIR_PROMPT	= 2,	/* park u.prompt for the user */
	BT_EV_PAIRED		= 3,	/* u.bond: save it */
	BT_EV_PAIR_FAILED	= 4,	/* u.status says why */
	BT_EV_CONNECTED		= 5,
	BT_EV_DISCONNECTED	= 6,
	BT_EV_BOND_UPDATED	= 7,	/* u.bond: new keys (e.g. a link key change) */
	BT_EV_POWER_CHANGED	= 8,	/* u.powered */
	BT_EV_DISCOVERY		= 9	/* u.scanning: discovery started/ended */
} bt_event_kind_t;

#define	BT_EIR_MAX	240	/* what the kernel neighbor cache keeps */

/*
 * A device seen during discovery. A dual-mode device seen on both transports
 * merges into one row: `transports` gains the second bit.
 */
struct bt_found {
	struct bt_addr	addr;
	int		transports;		/* BT_TRANSPORT_* */
	char		name[BT_NAME_MAX + 1];
	int8_t		rssi;			/* dBm; 127 when unknown */
	uint32_t	class_of_device;	/* Classic */
	uint16_t	appearance;		/* LE */
	uint16_t	eir_len;
	uint8_t		eir[BT_EIR_MAX];	/* raw EIR / AD */
	time_t		last_seen;
};

struct bt_event {
	bt_event_kind_t		kind;
	struct bt_addr		peer;
	int			transport;	/* BT_TRANSPORT_* */
	union {
		struct bt_found		found;
		struct bt_pair_prompt	prompt;
		const struct bt_bond	*bond;
		bt_status_t		status;
		bool			powered;
		bool			scanning;
	} u;
};

/* ------------------------------------------------------------------ */
/* Engines                                                             */
/* ------------------------------------------------------------------ */

struct bt_engine;	/* opaque, one per (engine, controller) */

/* What bluetoothd tells an engine about the controller it attaches to. */
struct bt_controller_desc {
	const char	*node;		/* netgraph node: "ubt0" */
	const char	*hci_node;	/* "ubt0hci", once the graph is built */
	uint16_t	vendor;		/* USB idVendor */
	uint16_t	product;	/* USB idProduct */
	int		hci_fd;		/* the shared raw HCI socket, or -1 */
	uint8_t		features[8];	/* LMP features, page 0 */
	bool		powered;	/* scans enabled at attach */
};

/*
 * bluetoothd's side. Every call into an engine, and every callback out of
 * one, happens with bluetoothd's state lock held: from the main loop (HCI
 * events, timers) or from a Mach RPC (pair, scan, power). The lock serialises
 * them, so an engine needs no locking of its own. A callback must not publish
 * to the SCDynamicStore itself; it records what changed and the main loop
 * publishes.
 */
struct bt_engine_host {
	void	*ctx;

	/* Deliver an event. Pointers in `ev` are valid for the call only. */
	void	(*event)(void *ctx, const struct bt_engine *e,
		    const struct bt_event *ev);

	/*
	 * Look up the stored bond for `peer` with its keys (the Classic
	 * Link_Key_Request and the LE encryption on reconnect both need this).
	 * Returns false when there is none.
	 */
	bool	(*bond_lookup)(void *ctx, const struct bt_addr *peer,
		    struct bt_bond *out);

	/*
	 * Is a name for `peer` already cached? Discovery skips the Remote Name
	 * Request (a page of its own) for devices whose name is known.
	 */
	bool	(*name_known)(void *ctx, const struct bt_addr *peer);
};

/*
 * The table an engine implements. Every entry may be NULL except name,
 * transport and attach; a NULL entry means bt_status_operation_not_supported_e.
 *
 * Both engines on one controller share one raw HCI socket
 * (bt_controller_desc.hci_fd, opened by the controller bring-up). Events read
 * from it are dispatched by event code to each engine's hci_event; each engine
 * also reports the HCI events it needs in hci_event_mask so the socket's
 * SO_HCI_RAW_FILTER is the union. Sockets an engine owns itself (an LE ATT
 * channel on L2CAP CID 4, a Classic HID control channel) are polled through
 * event_fd and serviced by pump.
 */
struct bt_engine_ops {
	const char	*name;		/* "classic", "le" */
	int		transport;	/* BT_TRANSPORT_* */

	bt_status_t	(*attach)(const struct bt_controller_desc *c,
			    const struct bt_engine_host *host,
			    struct bt_engine **out);
	void		(*detach)(struct bt_engine *e);

	/* HCI events this engine wants, as a bit per event code (0..255). */
	void		(*hci_event_mask)(struct bt_engine *e, uint8_t mask[32]);
	/* One HCI event packet from the shared socket. */
	void		(*hci_event)(struct bt_engine *e, uint8_t code,
			    const uint8_t *params, size_t len);

	/* An fd to poll for the engine's own sockets, or -1; then pump. */
	int		(*event_fd)(struct bt_engine *e);
	bool		(*pump)(struct bt_engine *e);	/* true: state changed */

	bt_status_t	(*set_power)(struct bt_engine *e, bool on);
	bt_status_t	(*discover)(struct bt_engine *e, bool start,
			    int seconds);
	bt_status_t	(*pair)(struct bt_engine *e, const struct bt_addr *peer,
			    bt_io_cap_t io, const char *pin);
	bt_status_t	(*pair_reply)(struct bt_engine *e,
			    const struct bt_pair_answer *a);
	bt_status_t	(*unpair)(struct bt_engine *e,
			    const struct bt_addr *peer);
	bt_status_t	(*connect)(struct bt_engine *e,
			    const struct bt_addr *peer);
	bt_status_t	(*disconnect)(struct bt_engine *e,
			    const struct bt_addr *peer);

	/*
	 * Hand a stored bond to the engine at attach time, before the first
	 * connection, so keys it caches itself (an LE resolving list, a
	 * controller's stored link keys) are in place.
	 */
	bt_status_t	(*bond_import)(struct bt_engine *e,
			    const struct bt_bond *b);

	/*
	 * Timers. next_timer returns the monotonic time (bt_now_ms(), hci.h)
	 * of the engine's next deadline, or 0 for none; the main loop sleeps no
	 * longer than that and then calls timer.
	 */
	uint64_t	(*next_timer)(struct bt_engine *e);
	void		(*timer)(struct bt_engine *e, uint64_t now);
};

/*
 * The engines bluetoothd knows, NULL-terminated, Classic first. A controller
 * attaches each one whose transport it supports.
 */
extern const struct bt_engine_ops	bt_classic_engine;
extern const struct bt_engine_ops	bt_le_engine;
extern const struct bt_engine_ops * const bt_engines[];

#define	BT_ENGINE_MAX	2

#endif /* _BLUETOOTHD_ENGINE_H_ */
