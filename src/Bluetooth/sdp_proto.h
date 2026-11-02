/*
 * sdp_proto.h — the Service Discovery Protocol wire format, shared by
 * bluetoothd's SDP server (sdp_server.c) and SDP client (sdp_client.c).
 *
 * Written from the Bluetooth Core Specification, Vol 3 Part B. libsdp is not
 * used: its client blocks in connect() with no timeout and its server half is
 * the /var/run/sdp protocol NextBSD does not serve; and its <bluetooth.h>
 * would collide with the MIG user header of the same name that bluetooth.defs
 * generates. The kernel's L2CAP socket headers are all either side needs.
 *
 * THE FORMAT IN ONE PARAGRAPH
 *
 * A PDU is a 5-byte header (PDU ID, transaction ID, parameter length, all
 * big-endian) and parameters. Every value inside is a "data element": one
 * descriptor byte (type << 3 | size index), an optional 1/2/4-byte length, and
 * the value. A service record is a list of (attribute ID, value) pairs; an
 * attribute ID is a uint16 element. A record matches a search when every UUID
 * of the search pattern appears anywhere in it. UUIDs compare as 128-bit
 * values: a 16- or 32-bit UUID is short for 0000xxxx-0000-1000-8000-
 * 00805F9B34FB. Responses that do not fit one L2CAP MTU are continued: the
 * response ends with an opaque continuation state, which the client sends back
 * verbatim in an otherwise identical request.
 */
#ifndef _BLUETOOTHD_SDP_PROTO_H_
#define _BLUETOOTHD_SDP_PROTO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* L2CAP PSM of SDP, and the MTU every L2CAP channel starts with. */
#define	SDP_PSM				0x0001
#define	SDP_DEFAULT_MTU			672
#define	SDP_MIN_MTU			48

/* PDU IDs. */
#define	SDP_PDU_ERROR_RESPONSE		0x01
#define	SDP_PDU_SS_REQUEST		0x02	/* ServiceSearch */
#define	SDP_PDU_SS_RESPONSE		0x03
#define	SDP_PDU_SA_REQUEST		0x04	/* ServiceAttribute */
#define	SDP_PDU_SA_RESPONSE		0x05
#define	SDP_PDU_SSA_REQUEST		0x06	/* ServiceSearchAttribute */
#define	SDP_PDU_SSA_RESPONSE		0x07

#define	SDP_PDU_HDR_LEN			5

/* ErrorResponse codes. */
#define	SDP_ERR_INVALID_VERSION		0x0001
#define	SDP_ERR_INVALID_HANDLE		0x0002
#define	SDP_ERR_INVALID_SYNTAX		0x0003
#define	SDP_ERR_INVALID_PDU_SIZE	0x0004
#define	SDP_ERR_INVALID_CONTINUATION	0x0005
#define	SDP_ERR_INSUFFICIENT_RESOURCES	0x0006

/* Continuation state: an InfoLength byte, then at most 16 bytes. */
#define	SDP_CONT_MAX			16

/* At most this many UUIDs in a ServiceSearchPattern. */
#define	SDP_MAX_PATTERN			12

/* Data element types. */
#define	SDP_DE_NIL			0
#define	SDP_DE_UINT			1
#define	SDP_DE_INT			2
#define	SDP_DE_UUID			3
#define	SDP_DE_TEXT			4
#define	SDP_DE_BOOL			5
#define	SDP_DE_SEQ			6	/* DES */
#define	SDP_DE_ALT			7	/* DEA */
#define	SDP_DE_URL			8

/* Universal attribute IDs. */
#define	SDP_ATTR_RECORD_HANDLE		0x0000
#define	SDP_ATTR_SERVICE_CLASS_ID_LIST	0x0001
#define	SDP_ATTR_RECORD_STATE		0x0002
#define	SDP_ATTR_SERVICE_ID		0x0003
#define	SDP_ATTR_PROTOCOL_DESC_LIST	0x0004
#define	SDP_ATTR_BROWSE_GROUP_LIST	0x0005
#define	SDP_ATTR_LANG_BASE_LIST		0x0006
#define	SDP_ATTR_TIME_TO_LIVE		0x0007
#define	SDP_ATTR_AVAILABILITY		0x0008
#define	SDP_ATTR_PROFILE_DESC_LIST	0x0009
#define	SDP_ATTR_DOC_URL		0x000a
#define	SDP_ATTR_CLIENT_EXEC_URL	0x000b
#define	SDP_ATTR_ICON_URL		0x000c
#define	SDP_ATTR_ADD_PROTOCOL_DESC_LIST	0x000d
/* Offsets from the primary language base (0x0100). */
#define	SDP_ATTR_PRIMARY_LANG_BASE	0x0100
#define	SDP_ATTR_SERVICE_NAME		0x0000
#define	SDP_ATTR_SERVICE_DESCRIPTION	0x0001
#define	SDP_ATTR_PROVIDER_NAME		0x0002

/* The SDP server record (handle 0). */
#define	SDP_ATTR_VERSION_NUMBER_LIST	0x0200
#define	SDP_ATTR_DATABASE_STATE		0x0201

/* PnP Information (Device ID profile). */
#define	SDP_ATTR_PNP_SPECIFICATION_ID	0x0200
#define	SDP_ATTR_PNP_VENDOR_ID		0x0201
#define	SDP_ATTR_PNP_PRODUCT_ID		0x0202
#define	SDP_ATTR_PNP_VERSION		0x0203
#define	SDP_ATTR_PNP_PRIMARY_RECORD	0x0204
#define	SDP_ATTR_PNP_VENDOR_ID_SOURCE	0x0205

/* HID. */
#define	SDP_ATTR_HID_DEVICE_RELEASE	0x0200
#define	SDP_ATTR_HID_PARSER_VERSION	0x0201
#define	SDP_ATTR_HID_DEVICE_SUBCLASS	0x0202
#define	SDP_ATTR_HID_COUNTRY_CODE	0x0203
#define	SDP_ATTR_HID_VIRTUAL_CABLE	0x0204
#define	SDP_ATTR_HID_RECONNECT_INITIATE	0x0205
#define	SDP_ATTR_HID_DESCRIPTOR_LIST	0x0206
#define	SDP_ATTR_HID_LANGID_BASE_LIST	0x0207
#define	SDP_ATTR_HID_SDP_DISABLE	0x0208
#define	SDP_ATTR_HID_BATTERY_POWER	0x0209
#define	SDP_ATTR_HID_REMOTE_WAKE	0x020a
#define	SDP_ATTR_HID_PROFILE_VERSION	0x020b
#define	SDP_ATTR_HID_SUPERVISION_TIMEOUT 0x020c
#define	SDP_ATTR_HID_NORMALLY_CONNECTABLE 0x020d
#define	SDP_ATTR_HID_BOOT_DEVICE	0x020e
#define	SDP_ATTR_HID_SSR_HOST_MAX_LATENCY 0x020f
#define	SDP_ATTR_HID_SSR_HOST_MIN_TIMEOUT 0x0210

/* Profile attributes the local records use. */
#define	SDP_ATTR_NETWORK		0x0301	/* HFP AG */
#define	SDP_ATTR_SUPPORTED_FEATURES	0x0311

/* HID descriptor type of a report descriptor, in HIDDescriptorList. */
#define	SDP_HID_DESC_TYPE_REPORT	0x22

/* 16-bit UUIDs: protocols. */
#define	SDP_UUID_SDP			0x0001
#define	SDP_UUID_RFCOMM			0x0003
#define	SDP_UUID_ATT			0x0007
#define	SDP_UUID_OBEX			0x0008
#define	SDP_UUID_BNEP			0x000f
#define	SDP_UUID_HIDP			0x0011
#define	SDP_UUID_AVCTP			0x0017
#define	SDP_UUID_AVDTP			0x0019
#define	SDP_UUID_L2CAP			0x0100

/* 16-bit UUIDs: service classes and profiles. */
#define	SDP_UUID_SDP_SERVER		0x1000
#define	SDP_UUID_PUBLIC_BROWSE_ROOT	0x1002
#define	SDP_UUID_SERIAL_PORT		0x1101
#define	SDP_UUID_HEADSET		0x1108
#define	SDP_UUID_AUDIO_SOURCE		0x110a
#define	SDP_UUID_AUDIO_SINK		0x110b
#define	SDP_UUID_AV_REMOTE_TARGET	0x110c
#define	SDP_UUID_ADVANCED_AUDIO		0x110d
#define	SDP_UUID_AV_REMOTE		0x110e
#define	SDP_UUID_AV_REMOTE_CONTROLLER	0x110f
#define	SDP_UUID_HEADSET_AG		0x1112
#define	SDP_UUID_PANU			0x1115
#define	SDP_UUID_NAP			0x1116
#define	SDP_UUID_HANDSFREE		0x111e
#define	SDP_UUID_HANDSFREE_AG		0x111f
#define	SDP_UUID_HID			0x1124
#define	SDP_UUID_PNP_INFORMATION	0x1200
#define	SDP_UUID_GENERIC_AUDIO		0x1203

/* ------------------------------------------------------------------ */
/* Reading data elements                                               */
/* ------------------------------------------------------------------ */

/* One parsed element: its type and the bytes of its value. */
struct sdp_de {
	uint8_t		type;		/* SDP_DE_* */
	const uint8_t	*val;
	uint32_t	len;		/* bytes at val */
};

/*
 * Parse the element at p (no further than end). Returns the bytes it takes,
 * header included, or 0 if it is malformed or runs past end.
 */
size_t	sdp_de_get(const uint8_t *p, const uint8_t *end, struct sdp_de *out);

/* An unsigned integer (or a bool) of 1, 2, 4 or 8 bytes. */
bool	sdp_de_uint(const struct sdp_de *de, uint64_t *out);

/* A UUID of any size, as 128 bits in big-endian order. */
bool	sdp_de_uuid128(const struct sdp_de *de, uint8_t out[16]);

/*
 * A UUID that is a short form of the base UUID, as its 32-bit value (a 16-bit
 * UUID's value fits in the low half). False for a UUID outside the base.
 */
bool	sdp_de_uuid32(const struct sdp_de *de, uint32_t *out);

/* The 128-bit form of a short UUID. */
void	sdp_uuid128_from32(uint32_t u, uint8_t out[16]);

/* Walk the members of a sequence or alternative. */
struct sdp_iter {
	const uint8_t	*p;
	const uint8_t	*end;
	bool		bad;		/* a member was malformed */
};

/* False if `de` is not a DES or DEA. */
bool	sdp_iter_init(struct sdp_iter *it, const struct sdp_de *de);
bool	sdp_iter_next(struct sdp_iter *it, struct sdp_de *out);

/*
 * Walk (attribute ID, value) pairs: a record as the server stores it, or the
 * members of one record's DES in a response. False at the end or on a
 * malformed pair (then it->bad is set).
 */
bool	sdp_iter_attr(struct sdp_iter *it, uint16_t *id, struct sdp_de *val);

/* ------------------------------------------------------------------ */
/* Writing data elements                                               */
/* ------------------------------------------------------------------ */

/*
 * A growable output buffer. Any allocation failure sets `err` and later
 * writes are dropped, so a builder checks once at the end.
 */
struct sdp_wbuf {
	uint8_t		*b;
	size_t		len;
	size_t		cap;
	bool		err;
};

void	sdp_wbuf_free(struct sdp_wbuf *w);
void	sdp_put_raw(struct sdp_wbuf *w, const void *p, size_t n);
void	sdp_put_be16(struct sdp_wbuf *w, uint16_t v);
void	sdp_put_be32(struct sdp_wbuf *w, uint32_t v);

void	sdp_put_uint8(struct sdp_wbuf *w, uint8_t v);
void	sdp_put_uint16(struct sdp_wbuf *w, uint16_t v);
void	sdp_put_uint32(struct sdp_wbuf *w, uint32_t v);
void	sdp_put_uuid16(struct sdp_wbuf *w, uint16_t v);
void	sdp_put_bool(struct sdp_wbuf *w, bool v);
void	sdp_put_text(struct sdp_wbuf *w, const void *s, size_t n);

/*
 * Open a DES with a 2-byte length; returns the mark sdp_seq_end needs. Nested
 * sequences are fine.
 */
size_t	sdp_seq_begin(struct sdp_wbuf *w);
void	sdp_seq_end(struct sdp_wbuf *w, size_t mark);

/* Copy one already-encoded element (a parsed sdp_de) verbatim. */
void	sdp_put_de(struct sdp_wbuf *w, const struct sdp_de *de);

/* ------------------------------------------------------------------ */
/* Names, for the bluetooth(8) browse                                  */
/* ------------------------------------------------------------------ */

/* "AudioSink", "L2CAP", ... or NULL for a UUID this table does not know. */
const char	*sdp_uuid_name(uint32_t uuid);

/*
 * The name of attribute `id` in a record whose first service class is
 * `svc_class` (attribute IDs from 0x0200 up are per profile), or NULL.
 */
const char	*sdp_attr_name(uint16_t id, uint32_t svc_class);

#endif /* _BLUETOOTHD_SDP_PROTO_H_ */
