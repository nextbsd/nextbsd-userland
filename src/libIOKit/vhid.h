/*
 * MIRROR — keep in sync with the kernel canonical.
 *
 * Canonical: the vhid header of IOHIDUserDevice.kext, the virtual-HID
 * transport behind /dev/hidresource (nextbsd/nextbsd-kernel-extensions#25;
 * design: nextbsd-userland wiki E16-Audit-3-Virtual-HID-Prior-Art §4.3).
 * Userland in this repo cannot include the kext source, so libIOKit carries
 * its own copy of the ABI here, as it does for ioregistry.h. Only
 * IOHIDUserDevice.c includes this file. If the kext's ioctl numbers, structs
 * or event codes change, change them here and nowhere else.
 *
 * Protocol, one call per IOHIDResourceDeviceUserClient method on Darwin:
 *
 *   open(/dev/hidresource)	one open per device (IOServiceOpen)
 *   VHIDIOCCREATE		Create: a packed nvlist keyed with Apple's
 *				property names (the VHID_KEY_* below)
 *   write(2) / VHIDIOCINPUT	HandleReport: one input report, report ID
 *				first when the descriptor numbers its reports
 *   read(2)			the data queue: one struct vhid_event, then
 *				its payload, per read
 *   VHIDIOCREPLY		PostReportResult: completes a GET_REPORT or
 *				SET_REPORT event by token
 *   VHIDIOCDESTROY		Terminate
 *   close(2)			clientClose / clientDied: same as DESTROY
 */
#ifndef _LIBIOKIT_VHID_H_
#define _LIBIOKIT_VHID_H_

#include <sys/types.h>
#include <sys/ioccom.h>

#define	VHID_CTL_PATH		"/dev/hidresource"

/*
 * The kext that provides VHID_CTL_PATH, loaded through kextd on the first
 * IOHIDUserDeviceCreateWithProperties when the node is absent.
 */
#define	VHID_KEXT_IDENTIFIER	"org.nextbsd.kext.IOHIDUserDevice"

#define	VHID_MAX_DESCRIPTOR	4096	/* ReportDescriptor bytes */
#define	VHID_MAX_REPORT		4096	/* one report, ID byte included */

/*
 * VHIDIOCCREATE nvlist keys: Apple's IOHIDKeys.h names, the only keys the
 * kernel accepts (any other key fails the create). ReportDescriptor is
 * binary, Product, SerialNumber and Transport are strings, the rest are
 * numbers. RequestTimeout is in milliseconds (the kernel's default is
 * 1000). Transport "Bluetooth" and "BluetoothLowEnergy" give the hidbus
 * device BUS_BLUETOOTH for a privileged creator; anything else, or an
 * unprivileged creator, gets BUS_VIRTUAL.
 */
#define	VHID_KEY_REPORT_DESCRIPTOR	"ReportDescriptor"
#define	VHID_KEY_VENDOR_ID		"VendorID"
#define	VHID_KEY_PRODUCT_ID		"ProductID"
#define	VHID_KEY_VERSION_NUMBER		"VersionNumber"
#define	VHID_KEY_COUNTRY_CODE		"CountryCode"
#define	VHID_KEY_PRODUCT		"Product"
#define	VHID_KEY_SERIAL_NUMBER		"SerialNumber"
#define	VHID_KEY_TRANSPORT		"Transport"
#define	VHID_KEY_REQUEST_TIMEOUT	"RequestTimeout"

#define	VHID_TRANSPORT_BLUETOOTH	"Bluetooth"
#define	VHID_TRANSPORT_BLUETOOTH_LE	"BluetoothLowEnergy"
#define	VHID_TRANSPORT_VIRTUAL		"Virtual"

struct vhid_create {
	const void	*vc_nvlist;	/* packed nvlist */
	size_t		 vc_nvlist_len;
};

/* An input report with a caller-supplied timestamp (0 = now). */
struct vhid_input {
	uint64_t	 vi_timestamp;
	const void	*vi_data;
	uint32_t	 vi_len;
};

/* Report types, the hid_if.m values (HID_INPUT_REPORT and friends). */
#define	VHID_REPORT_INPUT	1
#define	VHID_REPORT_OUTPUT	2
#define	VHID_REPORT_FEATURE	3

/* Event types read(2) returns. */
#define	VHID_EV_START		1	/* hidbus intr_start */
#define	VHID_EV_STOP		2	/* hidbus intr_stop */
#define	VHID_EV_OPEN		3
#define	VHID_EV_CLOSE		4
#define	VHID_EV_OUTPUT		5	/* hid_write; data, no reply */
#define	VHID_EV_GET_REPORT	6	/* reply with data */
#define	VHID_EV_SET_REPORT	7	/* data; reply */
#define	VHID_EV_SET_PROTOCOL	8	/* ve_value = protocol; reply */
#define	VHID_EV_SET_IDLE	9	/* ve_value = duration; reply */

/*
 * One event. ve_len bytes of data follow the header in the same read: the
 * report for OUTPUT and SET_REPORT, nothing for the others. For GET_REPORT
 * ve_len is the largest report the caller can take.
 */
struct vhid_event {
	uint32_t	ve_type;	/* VHID_EV_* */
	uint8_t		ve_report_type;	/* VHID_REPORT_* */
	uint8_t		ve_report_id;
	uint16_t	ve_value;
	uint32_t	ve_token;	/* for VHIDIOCREPLY */
	uint32_t	ve_len;
};

/* PostReportResult. vr_error is an errno; vr_data is read for GET_REPORT. */
struct vhid_reply {
	uint32_t	 vr_token;
	int32_t		 vr_error;
	const void	*vr_data;
	uint32_t	 vr_len;
};

#define	VHIDIOCCREATE	_IOW('V', 1, struct vhid_create)
#define	VHIDIOCINPUT	_IOW('V', 2, struct vhid_input)
#define	VHIDIOCREPLY	_IOW('V', 3, struct vhid_reply)
#define	VHIDIOCDESTROY	_IO('V', 4)

#endif /* _LIBIOKIT_VHID_H_ */
