/*
 * hci_compat.h — the kernel's HCI and raw-socket headers, plus the opcodes,
 * events and parameter layouts bluetoothd needs that ng_hci.h lacks.
 *
 * The kernel headers are included directly, in the order libbluetooth's
 * <bluetooth.h> uses. Not <bluetooth.h> itself: the MIG user header generated
 * from bluetooth.defs is also called bluetooth.h and sits on the include path
 * (MIGOUT). Nothing from libbluetooth is linked.
 *
 * Everything below is from the Core specification (v5.4, Vol 4 Part E, §7),
 * named HCI_* rather than NG_HCI_* so it never collides with a later ng_hci.h
 * that grows the same definitions. This header stays local: nothing here is
 * sent upstream.
 */
#ifndef _BLUETOOTHD_HCI_COMPAT_H_
#define _BLUETOOTHD_HCI_COMPAT_H_

/* ng_btsocket.h refuses sockaddr_l2cap users that have not opted in. */
#ifndef L2CAP_SOCKET_CHECKED
#define	L2CAP_SOCKET_CHECKED
#endif
#include <sys/types.h>
#include <sys/endian.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <bitstring.h>

#include <netgraph/ng_message.h>
#include <netgraph/bluetooth/include/ng_bluetooth.h>
#include <netgraph/bluetooth/include/ng_hci.h>
#include <netgraph/bluetooth/include/ng_l2cap.h>
#include <netgraph/bluetooth/include/ng_btsocket.h>

#include <stdint.h>

/* ---- Link Control (OGF 0x01) ------------------------------------------- */

#define	HCI_OCF_USER_PASSKEY_REQ_REPLY		0x002e
#define	HCI_OCF_USER_PASSKEY_REQ_NEG_REPLY	0x002f

typedef struct {
	bdaddr_t	bdaddr;
	uint32_t	passkey;	/* 0 .. 999999, little endian */
} __attribute__ ((packed)) hci_user_passkey_reply_cp;

/* ---- Controller & Baseband (OGF 0x03) ---------------------------------- */

#define	HCI_OCF_WRITE_INQUIRY_MODE		0x0045
#define	HCI_OCF_WRITE_SIMPLE_PAIRING_MODE	0x0056

/* Write_Inquiry_Mode values. */
#define	HCI_INQUIRY_MODE_STANDARD		0x00
#define	HCI_INQUIRY_MODE_RSSI			0x01
#define	HCI_INQUIRY_MODE_EXTENDED		0x02	/* with EIR */

/* Write_Scan_Enable values. */
#define	HCI_SCAN_NONE				0x00
#define	HCI_SCAN_INQUIRY			0x01	/* discoverable */
#define	HCI_SCAN_PAGE				0x02	/* connectable */

/* ---- Events ------------------------------------------------------------ */

#define	HCI_EVENT_INQUIRY_RESULT_RSSI		0x22
#define	HCI_EVENT_EXT_INQUIRY_RESULT		0x2f
#define	HCI_EVENT_ENCRYPTION_KEY_REFRESH	0x30
#define	HCI_EVENT_IO_CAPABILITY_RESPONSE	0x32
#define	HCI_EVENT_USER_PASSKEY_REQUEST		0x34
#define	HCI_EVENT_USER_PASSKEY_NOTIFICATION	0x3b
#define	HCI_EVENT_KEYPRESS_NOTIFICATION		0x3c

/* One Inquiry_Result_with_RSSI response (there may be several). */
typedef struct {
	bdaddr_t	bdaddr;
	uint8_t		page_scan_rep_mode;
	uint8_t		reserved;
	uint8_t		uclass[NG_HCI_CLASS_SIZE];
	uint16_t	clock_offset;
	int8_t		rssi;
} __attribute__ ((packed)) hci_inquiry_response_rssi;

/* Extended_Inquiry_Result: always exactly one response, then 240 EIR octets. */
#define	HCI_EIR_SIZE				240
typedef struct {
	uint8_t		num_responses;	/* always 1 */
	bdaddr_t	bdaddr;
	uint8_t		page_scan_rep_mode;
	uint8_t		reserved;
	uint8_t		uclass[NG_HCI_CLASS_SIZE];
	uint16_t	clock_offset;
	int8_t		rssi;
	uint8_t		eir[HCI_EIR_SIZE];
} __attribute__ ((packed)) hci_ext_inquiry_result_ep;

typedef struct {
	bdaddr_t	bdaddr;
	uint8_t		io_capability;
	uint8_t		oob_data_present;
	uint8_t		authentication_requirements;
} __attribute__ ((packed)) hci_io_capability_response_ep;

typedef struct {
	bdaddr_t	bdaddr;
} __attribute__ ((packed)) hci_user_passkey_request_ep;

typedef struct {
	bdaddr_t	bdaddr;
	uint32_t	passkey;
} __attribute__ ((packed)) hci_user_passkey_notification_ep;

/* EIR / AD data types used here. */
#define	HCI_EIR_NAME_SHORT			0x08
#define	HCI_EIR_NAME_COMPLETE			0x09

/* ---- Event masks -------------------------------------------------------- */

/*
 * Set_Event_Mask. The spec default (bits 0-44) plus every Secure Simple
 * Pairing and Extended Inquiry event (bits 45-59), plus LE Meta (bit 61).
 * ng_hci.h's NG_HCI_EVMSK_LE_META names bit 60, which is reserved; the LE
 * Meta event is bit 61, as NG_HCI_EVENT_MASK_LE has it. Synchronous
 * Connection Complete and Changed (events 0x2c and 0x2d, bits 43 and 44) must
 * stay set: the kernel's eSCO setup for HFP waits for them.
 */
#define	HCI_EVENT_MASK_BLUETOOTHD		0x2fffffffffffffffULL

/*
 * LE_Set_Event_Mask: the default 0x1f (Connection Complete, Advertising
 * Report, Connection Update Complete, Read Remote Features Complete, LTK
 * Request), plus Remote Connection Parameter Request (subevent 0x06, bit 5)
 * and Enhanced Connection Complete (subevent 0x0a, bit 9), which the LE
 * hardening in NgHCI (nextbsd-kernel-extensions#30) handles.
 */
#define	HCI_LE_EVENT_MASK_BLUETOOTHD		0x000000000000023fULL

/* ---- LMP features (Read_Local_Supported_Features, page 0) -------------- */

#define	HCI_FEAT_RSSI_INQUIRY(f)	(((f)[3] & 0x40) != 0)
#define	HCI_FEAT_LE(f)			(((f)[4] & 0x40) != 0)
#define	HCI_FEAT_EIR(f)			(((f)[6] & 0x01) != 0)
#define	HCI_FEAT_SSP(f)			(((f)[6] & 0x08) != 0)

/* ---- IO capabilities and authentication requirements ------------------- */

#define	HCI_IO_DISPLAY_ONLY			0x00
#define	HCI_IO_DISPLAY_YES_NO			0x01
#define	HCI_IO_KEYBOARD_ONLY			0x02
#define	HCI_IO_NO_INPUT_NO_OUTPUT		0x03

/* MITM protection required, dedicated bonding. */
#define	HCI_AUTH_MITM_DEDICATED_BONDING		0x03

/* ---- Error codes -------------------------------------------------------- */

#define	HCI_ERR_PAIRING_NOT_ALLOWED		0x18
#define	HCI_ERR_REMOTE_USER_TERMINATED		0x13
#define	HCI_ERR_REMOTE_POWER_OFF		0x15
#define	HCI_ERR_AUTH_FAILURE			0x05

#endif /* _BLUETOOTHD_HCI_COMPAT_H_ */
