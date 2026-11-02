/*
 * classic.h — the Classic (BR/EDR) engine's private state, shared by its
 * three parts: discovery in classic_engine.c, the security manager in
 * security.c and the ACL links in links.c. Nothing outside the engine includes this; the policy layer sees
 * only engine.h, where struct bt_engine is opaque.
 *
 * The LE engine (#205) keeps its own state the same way, in its own private
 * header: each engine's ops only ever receive the struct bt_engine pointers
 * its own attach returned.
 */
#ifndef _BLUETOOTHD_CLASSIC_H_
#define _BLUETOOTHD_CLASSIC_H_

#include <stdbool.h>
#include <stdint.h>

#include "engine.h"

/* Devices one inquiry remembers for its name pass. */
#define	CLASSIC_INQ_MAX		64

struct classic_inq {
	struct bt_addr	addr;
	uint8_t		psrm;		/* page scan repetition mode */
	uint16_t	clock_offset;	/* as the inquiry reported it */
	bool		named;		/* name from EIR, cache or request */
};

/*
 * The one pairing the security manager runs at a time, per controller.
 * Pairing is rare and needs the user, so one at a time is no limit; a second
 * bt_pair gets bt_status_busy_e.
 */
typedef enum {
	SEC_IDLE = 0,
	SEC_CONNECTING,		/* Create_Connection sent */
	SEC_AUTHENTICATING	/* Authentication_Requested sent */
} sec_stage_t;

struct classic_sec {
	sec_stage_t		stage;
	struct bt_addr		peer;
	int			handle;		/* ACL handle, -1 until known */
	char			pin[BT_PIN_MAX + 1];	/* from bt_pair, or "" */
	bool			peer_io_known;
	uint8_t			peer_io;	/* HCI IO capability */
	uint8_t			peer_auth;	/* authentication reqs */
	bool			got_key;	/* Link_Key_Notification seen */
	bool			ssp;		/* Secure Simple Pairing used */

	/* The prompt parked with the policy layer, if any. */
	bt_pair_method_t	waiting;	/* bt_pair_none_e: none */
	uint32_t		prompt_id;
	uint64_t		user_deadline;	/* fail with timeout after */

	uint64_t		deadline;	/* the whole pairing */
};

/*
 * One ACL link (links.c). A link is recorded from the Connection_Request (the
 * peer paged us) or from bt_connect's Create_Connection, and lives until its
 * Disconnection_Complete. Links the kernel opened for an L2CAP connect (the
 * HID host, the SDP client) appear at their Connection_Complete.
 */
#define	CLASSIC_LINK_MAX	16

struct classic_link {
	bool			used;
	struct bt_addr		peer;
	int			handle;		/* -1 while connecting */
	bool			incoming;	/* Connection_Request seen */
	bool			requested;	/* our Create_Connection */
	bool			securing;	/* Authentication_Requested sent */
	bool			closing;	/* Disconnect sent */
	bool			encrypted;
	bool			central;
	int8_t			rssi;		/* 127 until read */
	uint32_t		class_of_device;
};

struct bt_engine {
	struct bt_engine_host	host;
	char			node[32];
	char			hci_node[32];
	int			fd;		/* the shared raw HCI socket */
	uint8_t			features[8];
	bool			powered;

	/* Discovery (classic_engine.c). */
	bool			inquiring;
	uint64_t		inquiry_deadline;	/* guard, past its length */
	struct classic_inq	inq[CLASSIC_INQ_MAX];
	int			ninq;
	bool			naming;		/* the name pass is running */
	int			name_idx;	/* entry being named, or -1 */
	uint64_t		name_deadline;	/* this one request */
	uint64_t		names_deadline;	/* the whole pass */

	/* Security (security.c). */
	struct classic_sec	sec;

	/* ACL links (links.c). */
	struct classic_link	links[CLASSIC_LINK_MAX];
	uint64_t		rssi_next;	/* next Read_RSSI round */
	bool			rssi_unsupported;
};

/* Is the pairing in progress (security.c) with `peer`? */
bool	classic_pairing_with(const struct bt_engine *e,
	    const struct bt_addr *peer);

/* Raise an event to the policy layer. */
void	classic_raise(struct bt_engine *e, const struct bt_event *ev);

/* Logging, "bluetoothd[classic] <node>: ...". */
void	classic_log(struct bt_engine *e, const char *fmt, ...)
	    __attribute__((__format__(__printf__, 2, 3)));

#endif /* _BLUETOOTHD_CLASSIC_H_ */
