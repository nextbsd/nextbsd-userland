/*
 * le.h — the LE engine's private state, shared by its parts:
 *
 *   le_engine.c   GAP over the shared raw HCI socket: scanning, connecting,
 *                 encryption, the LTK responder, reconnecting bonded devices,
 *                 and the engine.h ops
 *   att.c         the ATT bearer on L2CAP CID 4: one outstanding request,
 *                 the 30 s transaction timeout, notifications, and the
 *                 "no GATT server here" answers to the peer's requests
 *   gatt.c        the GATT client procedures over it: service, characteristic
 *                 and descriptor discovery, long reads, CCCD writes
 *   smp.c         the Security Manager on CID 6, as initiator: legacy pairing
 *                 and LE Secure Connections, key distribution
 *   le_crypto.c   the SMP crypto toolbox (e, c1, s1, ah, f4, f5, f6, g2) and
 *                 P-256 ECDH, over libcrypto
 *   hogp.c        HID over GATT into IOHIDUserDevice (#217)
 *
 * Nothing outside the engine includes this; the policy layer sees only
 * engine.h, where struct bt_engine is opaque. classic.h defines the Classic
 * engine's struct bt_engine the same way; no file includes both.
 *
 * Kernel interface. The kernel (ng_hci, ng_l2cap, ng_btsocket_l2cap) owns the
 * LE link: a SOCK_SEQPACKET L2CAP socket connected with l2cap_bdaddr_type
 * BDADDR_LE_PUBLIC or BDADDR_LE_RANDOM and l2cap_cid 4 (ATT) makes ng_hci
 * issue LE_Create_Connection; a second socket with l2cap_cid 6 (SMP) shares
 * that link. Each recv() is one ATT or SMP PDU. Everything else LE (scanning,
 * LE_Start_Encryption, the LTK Request reply, LE_Create_Connection_Cancel) is
 * an HCI command bluetoothd sends on the raw socket. The kernel answers LE
 * connection parameter requests itself (nextbsd-kernel-extensions#30).
 *
 * Byte order. Everything kept here is in wire order, least significant octet
 * first, as the PDUs and HCI carry it: addresses, keys, random numbers and
 * public key coordinates. le_crypto.c turns that into the most-significant-
 * first form AES and P-256 want, and back.
 *
 * Every function expects bluetoothd_lock() held, as all engine code does
 * (engine.h). The only other thread is the IOHIDUserDevice queue in hogp.c,
 * which takes the lock itself.
 */
#ifndef _BLUETOOTHD_LE_H_
#define _BLUETOOTHD_LE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "engine.h"

/* ---- limits ---------------------------------------------------------------- */

#define	LE_CONN_MAX		8	/* LE links per controller */
#define	LE_BOND_MAX		32	/* bonded LE devices per controller */

#define	LE_ATT_MTU_DEFAULT	23	/* the LE minimum, before MTU exchange */
#define	LE_ATT_MTU_LOCAL	185	/* what we ask for */
#define	LE_ATT_PDU_MAX		LE_ATT_MTU_LOCAL

#define	LE_GATT_SVC_MAX		32
#define	LE_GATT_CHR_MAX		96

#define	LE_SMP_PDU_MAX		65	/* Pairing Public Key: 1 + 64 */

/* ---- timers (milliseconds) --------------------------------------------------- */

#define	LE_CONNECT_MS		15000	/* LE_Create_Connection, then cancel */
#define	LE_ATT_TIMEOUT_MS	30000	/* ATT transaction timeout (Vol 3 F 3.3.3) */
#define	LE_SMP_TIMEOUT_MS	30000	/* SMP timeout (Vol 3 H 3.4) */
#define	LE_USER_TIMEOUT_MS	60000	/* a prompt waits this long */
#define	LE_PAIR_TIMEOUT_MS	90000	/* the whole pairing */
#define	LE_RETRY_MS		5000	/* a bonded device that failed to connect */

/* ---- ATT (Vol 3 Part F) ------------------------------------------------------- */

#define	ATT_ERROR_RSP		0x01
#define	ATT_MTU_REQ		0x02
#define	ATT_MTU_RSP		0x03
#define	ATT_FIND_INFO_REQ	0x04
#define	ATT_FIND_INFO_RSP	0x05
#define	ATT_FIND_BY_TYPE_REQ	0x06
#define	ATT_READ_BY_TYPE_REQ	0x08
#define	ATT_READ_BY_TYPE_RSP	0x09
#define	ATT_READ_REQ		0x0a
#define	ATT_READ_RSP		0x0b
#define	ATT_READ_BLOB_REQ	0x0c
#define	ATT_READ_BLOB_RSP	0x0d
#define	ATT_READ_BY_GROUP_REQ	0x10
#define	ATT_READ_BY_GROUP_RSP	0x11
#define	ATT_WRITE_REQ		0x12
#define	ATT_WRITE_RSP		0x13
#define	ATT_HANDLE_NOTIFY	0x1b
#define	ATT_HANDLE_IND		0x1d
#define	ATT_HANDLE_CONF		0x1e
#define	ATT_WRITE_CMD		0x52

#define	ATT_OP_COMMAND		0x40	/* no response expected */

/* Error codes. */
#define	ATT_ECODE_INVALID_HANDLE	0x01
#define	ATT_ECODE_READ_NOT_PERM		0x02
#define	ATT_ECODE_REQ_NOT_SUPP		0x06
#define	ATT_ECODE_ATTR_NOT_FOUND	0x0a
#define	ATT_ECODE_ATTR_NOT_LONG		0x0b
#define	ATT_ECODE_INSUFF_ENC		0x0f
/* Not on the air: how a request ended without an answer. */
#define	ATT_ECODE_TIMEOUT		0xfe
#define	ATT_ECODE_LINK_LOST		0xff

/* ---- GATT UUIDs (16-bit) ------------------------------------------------------- */

#define	GATT_PRIMARY_SERVICE	0x2800
#define	GATT_CHARACTERISTIC	0x2803
#define	GATT_CCCD		0x2902
#define	GATT_REPORT_REF		0x2908

#define	GATT_SVC_DEVICE_INFO	0x180a
#define	GATT_SVC_BATTERY	0x180f
#define	GATT_SVC_HID		0x1812

#define	GATT_CHR_BATTERY_LEVEL	0x2a19
#define	GATT_CHR_PNP_ID		0x2a50
#define	GATT_CHR_HID_INFO	0x2a4a
#define	GATT_CHR_REPORT_MAP	0x2a4b
#define	GATT_CHR_HID_CONTROL	0x2a4c
#define	GATT_CHR_REPORT		0x2a4d
#define	GATT_CHR_PROTOCOL_MODE	0x2a4e

/* Characteristic properties. */
#define	GATT_PROP_READ		0x02
#define	GATT_PROP_WRITE_NR	0x04
#define	GATT_PROP_WRITE		0x08
#define	GATT_PROP_NOTIFY	0x10
#define	GATT_PROP_INDICATE	0x20

/* ---- AD types (Core Supplement Part A) ------------------------------------------ */

#define	AD_FLAGS		0x01
#define	AD_UUID16_SOME		0x02
#define	AD_UUID16_ALL		0x03
#define	AD_NAME_SHORT		0x08
#define	AD_NAME_COMPLETE	0x09
#define	AD_APPEARANCE		0x19

/* ---- state --------------------------------------------------------------------- */

struct le_conn;

/*
 * The end of one ATT request. `err` is 0 with the response PDU (opcode
 * included), an ATT error code from an Error Response (pdu NULL), or
 * ATT_ECODE_TIMEOUT / ATT_ECODE_LINK_LOST.
 */
typedef void	(*att_done_fn)(struct le_conn *c, void *arg, uint8_t err,
		    const uint8_t *pdu, size_t len);

struct att_req {
	struct att_req	*next;
	uint8_t		pdu[LE_ATT_PDU_MAX];
	size_t		len;
	uint8_t		rsp;		/* the opcode that answers it */
	att_done_fn	done;
	void		*arg;
};

struct att_bearer {
	int		fd;		/* L2CAP CID 4 socket, or -1 */
	uint16_t	mtu;		/* negotiated */
	struct att_req	*head;		/* head is on the air when `sent` */
	struct att_req	*tail;
	bool		sent;
	uint64_t	deadline;	/* of the request on the air */
};

/* One characteristic, with the descriptors HOGP cares about. */
struct gatt_chr {
	int		svc;		/* index into gatt_db.svc */
	uint16_t	svc_uuid;	/* its service's 16-bit UUID, or 0 */
	uint16_t	decl;		/* declaration handle */
	uint16_t	value;		/* value handle */
	uint16_t	end;		/* last handle of the characteristic */
	uint16_t	uuid;		/* 16-bit UUID, 0 for a 128-bit one */
	uint8_t		props;
	uint16_t	cccd;		/* 0x2902 handle, 0 if none */
	uint16_t	report_ref;	/* 0x2908 handle, 0 if none */
};

struct gatt_svc {
	uint16_t	start;
	uint16_t	end;
	uint16_t	uuid;		/* 16-bit UUID, 0 for a 128-bit one */
};

typedef void	(*gatt_done_fn)(struct le_conn *c, bool ok);

/* A long read's end: the whole value, or an ATT error. */
typedef void	(*gatt_read_fn)(struct le_conn *c, void *arg, uint8_t err,
		    const uint8_t *val, size_t len);

#define	GATT_VALUE_MAX		512	/* the longest attribute value */

struct gatt_db {
	struct gatt_svc	svc[LE_GATT_SVC_MAX];
	int		nsvc;
	struct gatt_chr	chr[LE_GATT_CHR_MAX];
	int		nchr;

	/* The discovery in progress. */
	int		phase;
	int		idx;
	uint16_t	next;
	gatt_done_fn	done;

	/* The long read in progress (one at a time). */
	bool		lr_active;
	uint16_t	lr_handle;
	uint8_t		lr_buf[GATT_VALUE_MAX];
	size_t		lr_len;
	gatt_read_fn	lr_done;
	void		*lr_arg;
};


/* SMP: where the pairing has got to. */
typedef enum {
	SMP_IDLE = 0,
	SMP_W4_RESPONSE,	/* Pairing Request sent */
	SMP_W4_USER,		/* a prompt is parked; the protocol waits */
	SMP_W4_CONFIRM,		/* our confirm (or nothing) sent */
	SMP_W4_RANDOM,		/* our random sent */
	SMP_W4_PUBKEY,		/* SC: our public key sent */
	SMP_W4_DHKEY_CHECK,	/* SC: our Ea sent */
	SMP_W4_ENCRYPTION,	/* LE_Start_Encryption sent */
	SMP_W4_KEYS,		/* encrypted; the peer distributes keys */
	SMP_DONE
} smp_stage_t;

struct smp_state {
	smp_stage_t		stage;
	bool			sc;		/* LE Secure Connections */
	bt_pair_method_t	method;
	uint8_t			key_size;
	uint8_t			preq[7];	/* our Pairing Request */
	uint8_t			pres[7];	/* their Pairing Response */
	uint32_t		passkey;
	bool			have_passkey;
	int			round;		/* SC passkey: 0..19 */

	uint8_t			tk[16];		/* legacy */
	uint8_t			lrand[16];	/* Mrand / Na(i) */
	uint8_t			rrand[16];	/* Srand / Nb(i) */
	uint8_t			rconfirm[16];	/* Sconfirm / Cb(i) */
	bool			have_rconfirm;

	void			*ecdh;		/* EVP_PKEY *, SC */
	uint8_t			lpk[64];	/* our public key, X || Y */
	uint8_t			rpk[64];	/* theirs */
	uint8_t			dhkey[32];
	uint8_t			mackey[16];
	uint8_t			ltk[16];	/* SC LTK, or the STK */
	uint8_t			rcheck[16];	/* Eb, if it came early */
	bool			have_rcheck;

	/* Keys the peer distributes. */
	uint8_t			expect;		/* SMP_DIST_* still to come */
	struct bt_le_ltk	peer_ltk;
	bool			have_ltk;
	uint8_t			irk[16];
	bool			have_irk;
	struct bt_addr		identity;
	bool			have_identity;
	uint8_t			csrk[16];
	bool			have_csrk;

	/* The prompt parked with the policy layer, if any. */
	bt_pair_method_t	waiting;	/* bt_pair_none_e: none */
	uint32_t		prompt_id;
	uint64_t		user_deadline;

	uint64_t		deadline;	/* SMP timeout */
};

/* What one LE link is doing. */
typedef enum {
	LC_FREE = 0,
	LC_CONNECTING,		/* ATT socket connect() in flight */
	LC_LINKED,		/* connected; SMP socket connecting */
	LC_ENCRYPTING,		/* LE_Start_Encryption with a stored LTK */
	LC_PAIRING,		/* SMP running */
	LC_READY		/* encrypted (or unencrypted by choice) */
} le_conn_state_t;

struct hogp;

struct le_conn {
	le_conn_state_t		state;
	uint32_t		gen;		/* tells a reused slot's events apart */
	struct bt_engine	*e;
	struct bt_addr		addr;		/* as on the air (maybe an RPA) */
	struct bt_addr		identity;	/* the bond's address */
	int			bond;		/* index into e->bonds, or -1 */
	int			handle;		/* HCI handle, -1 until known */
	bool			encrypted;
	bool			pairing;	/* a bt_pair runs on this link */
	bool			pair_created;	/* the link exists for it */
	uint64_t		connect_deadline;
	uint64_t		pair_deadline;

	struct att_bearer	att;
	int			smp_fd;		/* L2CAP CID 6 socket, or -1 */
	bool			smp_connected;
	struct smp_state	smp;

	struct gatt_db		db;
	struct hogp		*hogp;
	uint16_t		battery_handle;	/* Battery Level value */
	int			battery;	/* percent, -1 unknown */
};

/* A bonded LE device, as the engine needs it for reconnecting. */
struct le_bond {
	struct bt_addr		identity;
	bool			has_irk;
	uint8_t			irk[16];
	bool			autoconnect;
	uint64_t		retry_at;	/* not before; 0: now */
};

typedef enum {
	LE_SCAN_OFF = 0,
	LE_SCAN_PASSIVE,	/* background: bonded devices advertising */
	LE_SCAN_ACTIVE		/* bt_scan: names, appearance, services */
} le_scan_t;

struct bt_engine {
	struct bt_engine_host	host;
	char			node[32];
	char			hci_node[32];
	int			fd;		/* the shared raw HCI socket */
	struct bt_addr		local;		/* our public address */
	bool			powered;
	int			kq;		/* the engine's sockets */

	struct le_conn		conns[LE_CONN_MAX];

	struct le_bond		bonds[LE_BOND_MAX];
	int			nbonds;

	le_scan_t		scan;		/* what the controller does */
	uint8_t			scan_q[8];	/* Set_Scan_Enable values sent */
	int			scan_qn;
	uint64_t		rescan_at;	/* retry a refused scan */
	bool			user_scan;
	uint64_t		user_scan_until;
	bool			reconcile;	/* re-decide scanning soon */
};

/* ---- le_engine.c --------------------------------------------------------------- */

void	le_log(struct bt_engine *e, const char *fmt, ...)
	    __attribute__((__format__(__printf__, 2, 3)));
void	le_raise(struct bt_engine *e, const struct bt_event *ev);

/* Have the engine's kqueue report `fd` (readable, or one-shot writable). */
void	le_watch(struct bt_engine *e, int fd, struct le_conn *c, bool write);
void	le_unwatch(struct bt_engine *e, int fd);

/* Send an HCI command on the shared socket. */
int	le_hci_cmd(struct bt_engine *e, uint16_t ocf, const void *p,
	    size_t len);

/* LE_Start_Encryption on the link. */
int	le_start_encryption(struct le_conn *c, const uint8_t ltk[16],
	    uint16_t ediv, uint64_t rand);

/* Drop the link (HCI Disconnect) and forget it. */
void	le_conn_drop(struct le_conn *c, const char *why);

/* The peer sent an SMP Security Request. */
void	le_security_request(struct le_conn *c, uint8_t auth);

/* SMP finished, one way or the other. */
void	le_pair_done(struct le_conn *c, bt_status_t st);

/* A device's IRK resolves `rpa`? */
bool	le_rpa_match(const uint8_t irk[16], const struct bt_addr *rpa);

/* Prompt ids for the LE engine, unique for the daemon's lifetime. */
uint32_t	le_prompt_id(void);

/* ---- att.c ------------------------------------------------------------------------ */

void	att_init(struct att_bearer *b);
/* Queue a request; it goes out when the one before it is answered. */
int	att_request(struct le_conn *c, const uint8_t *pdu, size_t len,
	    att_done_fn done, void *arg);
/* A command (no response): sent at once. */
int	att_command(struct le_conn *c, const uint8_t *pdu, size_t len);
/* The socket is readable: read and dispatch every PDU waiting. */
void	att_input(struct le_conn *c);
/* Fail every queued request (link lost) and close the socket. */
void	att_close(struct le_conn *c);
uint64_t	att_next_timer(struct le_conn *c);
void	att_timer(struct le_conn *c, uint64_t now);

/* ---- gatt.c ------------------------------------------------------------------------ */

/* Exchange MTU, then discover the services and characteristics HOGP needs. */
void	gatt_discover(struct le_conn *c, gatt_done_fn done);
/* The first characteristic in service `svc` with UUID `uuid`, after `from`. */
struct gatt_chr	*gatt_find(struct le_conn *c, uint16_t svc, uint16_t uuid,
		    struct gatt_chr *from);
/* Read a value of any length (Read, then Read Blob). */
int	gatt_read_long(struct le_conn *c, uint16_t handle, gatt_read_fn done,
	    void *arg);
int	gatt_read(struct le_conn *c, uint16_t handle, att_done_fn done,
	    void *arg);
int	gatt_write(struct le_conn *c, uint16_t handle, const uint8_t *v,
	    size_t len, att_done_fn done, void *arg);
int	gatt_write_cmd(struct le_conn *c, uint16_t handle, const uint8_t *v,
	    size_t len);
/* Enable notifications (or indications) through the CCCD. */
int	gatt_subscribe(struct le_conn *c, struct gatt_chr *ch,
	    att_done_fn done, void *arg);

/* ---- smp.c ------------------------------------------------------------------------- */

#define	SMP_DIST_ENC		0x01
#define	SMP_DIST_ID		0x02
#define	SMP_DIST_SIGN		0x04

/* Reset, freeing what the last pairing held (the link must be zeroed once). */
void		smp_init(struct le_conn *c);
/* Start pairing as initiator (the SMP socket is connected). */
bt_status_t	smp_start(struct le_conn *c);
/* One SMP PDU arrived / the socket is readable. */
void		smp_input(struct le_conn *c);
bt_status_t	smp_reply(struct le_conn *c, const struct bt_pair_answer *a);
/* The link's encryption changed (status 0: on). */
void		smp_encrypted(struct le_conn *c, uint8_t status);
/* Fail the pairing, telling the peer if it is still there. */
void		smp_abort(struct le_conn *c, bt_status_t st);
uint64_t	smp_next_timer(struct le_conn *c);
void		smp_timer(struct le_conn *c, uint64_t now);
/* Build the bond the finished pairing produced. */
void		smp_bond(struct le_conn *c, struct bt_bond *b);

/* ---- le_crypto.c ------------------------------------------------------------------- */

/* All 16-byte values in wire order (least significant octet first). */
int	le_e(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
int	le_c1(const uint8_t k[16], const uint8_t r[16], const uint8_t preq[7],
	    const uint8_t pres[7], uint8_t iat, const uint8_t ia[6],
	    uint8_t rat, const uint8_t ra[6], uint8_t out[16]);
int	le_s1(const uint8_t k[16], const uint8_t r1[16], const uint8_t r2[16],
	    uint8_t out[16]);
int	le_ah(const uint8_t irk[16], const uint8_t r[3], uint8_t out[3]);
int	le_f4(const uint8_t u[32], const uint8_t v[32], const uint8_t x[16],
	    uint8_t z, uint8_t out[16]);
int	le_f5(const uint8_t w[32], const uint8_t n1[16], const uint8_t n2[16],
	    const uint8_t a1[7], const uint8_t a2[7], uint8_t mackey[16],
	    uint8_t ltk[16]);
int	le_f6(const uint8_t w[16], const uint8_t n1[16], const uint8_t n2[16],
	    const uint8_t r[16], const uint8_t iocap[3], const uint8_t a1[7],
	    const uint8_t a2[7], uint8_t out[16]);
int	le_g2(const uint8_t u[32], const uint8_t v[32], const uint8_t x[16],
	    const uint8_t y[16], uint32_t *out);
/* P-256: a fresh key pair; pk is X || Y, each in wire order. */
void	*le_ecdh_new(uint8_t pk[64]);
/* The shared secret with the peer's key; -1 if it is not on the curve. */
int	le_ecdh_shared(void *key, const uint8_t peer[64], uint8_t dhkey[32]);
void	le_ecdh_free(void *key);
void	le_random(void *buf, size_t len);

/* ---- hogp.c ------------------------------------------------------------------------ */

/* The link is ready (encrypted, or not needed): find HID over GATT. */
void	hogp_start(struct le_conn *c);
/* A notification or indication for `handle`. */
void	hogp_notify(struct le_conn *c, uint16_t handle, const uint8_t *v,
	    size_t len);
/* The link is going away: retire the HID device. */
void	hogp_stop(struct le_conn *c);

#endif /* _BLUETOOTHD_LE_H_ */
