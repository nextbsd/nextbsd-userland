/*
 * NextBSD: SCO and eSCO parameters on Bluetooth SCO sockets
 * (nextbsd/nextbsd-kernel#237). The interface bluetoothd's HFP Audio Gateway
 * includes (it keeps a copy, as libIOKit does of hidresource.h). Not upstream.
 *
 * Socket. socket(PF_BLUETOOTH, SOCK_SEQPACKET, BLUETOOTH_PROTO_SCO), as in
 * releng/15.1 (<netgraph/bluetooth/include/ng_btsocket.h>), bound to the
 * local adapter's BD_ADDR (or NG_HCI_BDADDR_ANY) and connected to the
 * headset's, over the ACL link that the RFCOMM service-level connection
 * already holds. Each read returns one SCO packet's payload; each write sends
 * one, of at most SO_SCO_MTU bytes. Write packets the size the reads return:
 * with a USB controller that size follows the isochronous alternate setting
 * (48 bytes for one CVSD link at alt 2, 24 for transparent at alt 1), which
 * NgUbt selects by itself from the link's air mode (ng_ubt_sco.h). Write at
 * the rate packets arrive; there is no flow control.
 *
 * SO_SCO_SYNC_PARAMS (set, before connect() or listen(); get any time): the
 * parameters of the synchronous link. Without it, connect() opens a legacy SCO
 * link with Add_SCO_Connection (CVSD, the controller's voice setting), and an
 * incoming request is accepted with CVSD from 16-bit linear PCM. With it,
 * connect() uses Setup_Synchronous_Connection and accept answers with
 * Accept_Synchronous_Connection_Request, both with these parameters; an
 * accepted socket inherits the listening socket's. A set on a connecting or
 * connected socket fails with EISCONN; a get before any set with ENOENT.
 *
 * SO_SCO_SYNC_INFO (get, connected socket): what the controller negotiated,
 * from Synchronous_Connection_Complete, updated by _Changed. ENOENT for a
 * legacy link (Add_SCO_Connection reports none of it).
 *
 * Choosing the codec. CVSD: voice_setting SCO_VOICE_CVSD and 8000 bytes/s
 * each way. mSBC (wide-band speech): voice_setting SCO_VOICE_TRANSPARENT,
 * 8000 bytes/s, and bluetoothd runs the mSBC codec itself (57-byte frames
 * with the H2 header, 60 bytes per 7.5 ms). HFP 1.8 (5.7.1, 5.7.2) gives the
 * settings to try in order, falling back when connect() fails:
 *
 *	set	pkt_type	max_latency	retrans_effort	voice_setting
 *	T2	0x0380 (2-EV3)	0x000d		0x02		TRANSPARENT
 *	T1	0x03c8 (EV3)	0x0008		0x02		TRANSPARENT
 *	S4	0x0380 (2-EV3)	0x000c		0x02		CVSD
 *	S3	0x0380 (2-EV3)	0x000a		0x01		CVSD
 *	S1	0x03c8 (EV3)	0x0007		0x01		CVSD
 *	D1	0x03c4 (HV3)	0xffff		0xff		CVSD
 *
 * A setup the controller refuses fails connect() (ECONNREFUSED) at once, not
 * at the connect timeout. The controller's event mask must keep events 0x2c
 * and 0x2d (Synchronous_Connection_Complete and _Changed) enabled.
 */
#ifndef _NG_BTSOCKET_SCO_SYNC_H_
#define _NG_BTSOCKET_SCO_SYNC_H_

#include <sys/types.h>

/* Level SOL_SCO. Far above releng/15.1's SO_SCO_MTU (1) and _CONNINFO (2). */
#define SO_SCO_SYNC_PARAMS	0x4e420001	/* get/set struct sco_sync_params */
#define SO_SCO_SYNC_INFO	0x4e420002	/* get struct sco_sync_info */

/* Voice_Setting values (Core 5.x Vol 4 Part E 6.12) */
#define SCO_VOICE_CVSD		0x0060	/* 16-bit linear PCM in, CVSD on air */
#define SCO_VOICE_TRANSPARENT	0x0063	/* 16-bit, transparent air (mSBC) */

/* Packet_Type bits; the EDR bits forbid the packet type when set */
#define SCO_PKT_HV1		0x0001
#define SCO_PKT_HV2		0x0002
#define SCO_PKT_HV3		0x0004
#define SCO_PKT_EV3		0x0008
#define SCO_PKT_EV4		0x0010
#define SCO_PKT_EV5		0x0020
#define SCO_PKT_NO_2EV3		0x0040
#define SCO_PKT_NO_3EV3		0x0080
#define SCO_PKT_NO_2EV5		0x0100
#define SCO_PKT_NO_3EV5		0x0200

/* sco_sync_info link_type and air_mode */
#define SCO_LINK_SCO		0x00
#define SCO_LINK_ESCO		0x02
#define SCO_AIR_ULAW		0x00
#define SCO_AIR_ALAW		0x01
#define SCO_AIR_CVSD		0x02
#define SCO_AIR_TRANSPARENT	0x03

struct sco_sync_params {
	uint32_t	tx_bandwidth;	/* bytes/s, 8000 for HFP */
	uint32_t	rx_bandwidth;	/* bytes/s, 8000 for HFP */
	uint16_t	max_latency;	/* ms, 0xffff don't care */
	uint16_t	voice_setting;	/* SCO_VOICE_* */
	uint16_t	pkt_type;	/* SCO_PKT_* */
	uint8_t		retrans_effort;	/* 0 none, 1 power, 2 quality, 0xff */
	uint8_t		reserved;	/* 0 */
};

struct sco_sync_info {
	uint16_t	con_handle;	/* as SO_SCO_CONNINFO */
	uint8_t		link_type;	/* SCO_LINK_* */
	uint8_t		air_mode;	/* SCO_AIR_* */
	uint8_t		tx_interval;	/* slots, 0 for SCO */
	uint8_t		retrans_window;	/* slots */
	uint16_t	rx_pkt_len;	/* air packet length, bytes */
	uint16_t	tx_pkt_len;	/* air packet length, bytes */
};

#ifdef _KERNEL
/*
 * Per-socket state. ng_btsocket_sco_attach() allocates each PCB as a
 * struct ng_btsocket_sco_pcb_ext, so struct ng_btsocket_sco_pcb keeps its
 * releng/15.1 layout and this rides behind it.
 */
struct ng_btsocket_sco_sync {
	u_int8_t		params_set;
	u_int8_t		info_valid;
	struct sco_sync_params	params;
	struct sco_sync_info	info;
};

struct ng_btsocket_sco_pcb_ext {
	struct ng_btsocket_sco_pcb	pcb;	/* first */
	struct ng_btsocket_sco_sync	sync;
};

#define NG_BTSOCKET_SCO_SYNC(p) \
	(&__containerof((p), struct ng_btsocket_sco_pcb_ext, pcb)->sync)

int		 ng_btsocket_sco_sync_ctloutput(struct ng_btsocket_sco_pcb *,
		    struct sockopt *);
struct ng_mesg	*ng_btsocket_sco_sync_con_req(struct ng_btsocket_sco_pcb *);
struct ng_mesg	*ng_btsocket_sco_sync_con_rsp(struct ng_btsocket_sco_pcb *,
		    bdaddr_p, int);
void		 ng_btsocket_sco_sync_accepted(struct ng_btsocket_sco_pcb *,
		    struct ng_btsocket_sco_pcb *);
void		 ng_btsocket_sco_sync_set_info(struct ng_btsocket_sco_pcb *,
		    const void *);
#endif /* _KERNEL */

#endif /* _NG_BTSOCKET_SCO_SYNC_H_ */
