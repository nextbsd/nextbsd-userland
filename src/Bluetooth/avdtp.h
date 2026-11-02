/*
 * avdtp.h — AVDTP signalling (AVDTP 1.3), as the A2DP sink (#219) needs it.
 *
 * Signalling runs over an L2CAP channel on PSM 0x19; each stream's media then
 * gets a second channel to the same PSM. A message starts with one header
 * octet (transaction label, packet type, message type) and, for a single or
 * start packet, the signal identifier. The few messages an acceptor sends
 * are small enough for a single packet; fragmented commands from the peer
 * (start / continue / end) are reassembled by avdtp_recv().
 *
 * Only the acceptor side lives in bluetoothd: as a source, virtual_oss's
 * voss_bt.so is the initiator (audio.h).
 */
#ifndef _BLUETOOTHD_AVDTP_H_
#define _BLUETOOTHD_AVDTP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define	AVDTP_PSM			0x0019

/* Signal identifiers. */
#define	AVDTP_DISCOVER			0x01
#define	AVDTP_GET_CAPABILITIES		0x02
#define	AVDTP_SET_CONFIGURATION		0x03
#define	AVDTP_GET_CONFIGURATION		0x04
#define	AVDTP_RECONFIGURE		0x05
#define	AVDTP_OPEN			0x06
#define	AVDTP_START			0x07
#define	AVDTP_CLOSE			0x08
#define	AVDTP_SUSPEND			0x09
#define	AVDTP_ABORT			0x0a
#define	AVDTP_SECURITY_CONTROL		0x0b
#define	AVDTP_GET_ALL_CAPABILITIES	0x0c
#define	AVDTP_DELAY_REPORT		0x0d

/* Message types (low two bits of the header). */
#define	AVDTP_MSG_COMMAND		0x00
#define	AVDTP_MSG_GENERAL_REJECT	0x01
#define	AVDTP_MSG_ACCEPT		0x02
#define	AVDTP_MSG_REJECT		0x03

/* Packet types (bits 2-3). */
#define	AVDTP_PKT_SINGLE		0x00
#define	AVDTP_PKT_START			0x01
#define	AVDTP_PKT_CONTINUE		0x02
#define	AVDTP_PKT_END			0x03

/* Service categories. */
#define	AVDTP_CAT_MEDIA_TRANSPORT	0x01
#define	AVDTP_CAT_REPORTING		0x02
#define	AVDTP_CAT_RECOVERY		0x03
#define	AVDTP_CAT_CONTENT_PROTECTION	0x04
#define	AVDTP_CAT_HEADER_COMPRESSION	0x05
#define	AVDTP_CAT_MULTIPLEXING		0x06
#define	AVDTP_CAT_MEDIA_CODEC		0x07
#define	AVDTP_CAT_DELAY_REPORTING	0x08

/* Error codes. */
#define	AVDTP_ERR_BAD_HEADER_FORMAT	0x01
#define	AVDTP_ERR_BAD_LENGTH		0x11
#define	AVDTP_ERR_BAD_ACP_SEID		0x12
#define	AVDTP_ERR_SEP_IN_USE		0x13
#define	AVDTP_ERR_SEP_NOT_IN_USE	0x14
#define	AVDTP_ERR_BAD_SERV_CATEGORY	0x17
#define	AVDTP_ERR_BAD_PAYLOAD_FORMAT	0x18
#define	AVDTP_ERR_NOT_SUPPORTED_COMMAND	0x19
#define	AVDTP_ERR_INVALID_CAPABILITIES	0x1a
#define	AVDTP_ERR_UNSUPPORTED_CONFIG	0x29
#define	AVDTP_ERR_BAD_STATE		0x31

/* Media types and codecs (A2DP 1.3, §4.2). */
#define	AVDTP_MEDIA_AUDIO		0x00
#define	A2DP_CODEC_SBC			0x00

/* SBC codec information element bits (A2DP 1.3, §4.3.2). */
#define	SBC_FREQ_16000			0x80
#define	SBC_FREQ_32000			0x40
#define	SBC_FREQ_44100			0x20
#define	SBC_FREQ_48000			0x10
#define	SBC_CH_MONO			0x08
#define	SBC_CH_DUAL			0x04
#define	SBC_CH_STEREO			0x02
#define	SBC_CH_JOINT			0x01
#define	SBC_BLOCKS_4			0x80
#define	SBC_BLOCKS_8			0x40
#define	SBC_BLOCKS_12			0x20
#define	SBC_BLOCKS_16			0x10
#define	SBC_SUBBANDS_4			0x08
#define	SBC_SUBBANDS_8			0x04
#define	SBC_ALLOC_SNR			0x02
#define	SBC_ALLOC_LOUDNESS		0x01
#define	SBC_BITPOOL_MIN			2
#define	SBC_BITPOOL_MAX			250

#define	AVDTP_MSG_MAX			1024

/* One received signalling message. */
struct avdtp_msg {
	uint8_t		label;		/* transaction label */
	uint8_t		type;		/* AVDTP_MSG_* */
	uint8_t		signal;		/* AVDTP_* signal id */
	size_t		len;		/* parameter bytes */
	uint8_t		params[AVDTP_MSG_MAX];
	int		frags_left;	/* reassembling: packets to come */
};

/*
 * Read one message from the (non-blocking) signalling socket `fd`. Returns
 * 1 with `m` filled, 0 when no complete message is there yet (a fragment was
 * kept in `m`), or -1 when the channel is closed or broken.
 */
int	avdtp_recv(int fd, struct avdtp_msg *m);

/* Send one single-packet message. 0 on success. */
int	avdtp_send(int fd, uint8_t label, uint8_t type, uint8_t signal,
	    const void *params, size_t len);

/* Reject `m` with `len` bytes of error parameters. */
int	avdtp_reject(int fd, const struct avdtp_msg *m, const uint8_t *err,
	    size_t len);

/* An RTP media packet: the payload past the RTP header, or NULL. */
const uint8_t	*avdtp_rtp_payload(const uint8_t *pkt, size_t len,
		    size_t *plen);

#endif /* _BLUETOOTHD_AVDTP_H_ */
