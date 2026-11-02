/*
 * NextBSD: the /dev/hidresource interface of IOHIDUserDevice.kext
 * (nextbsd-kernel-extensions#25). A root daemon creates HID devices from
 * userland through it, and hidbus's drivers (hkbd, hms, hmt, hcons, ...)
 * attach to them as they do to USB and I2C devices. Modelled on Darwin's
 * IOHIDResourceDeviceUserClient, whose six methods map onto it:
 *
 *   Create            HIDRESOURCE_IOC_CREATE
 *   Terminate         HIDRESOURCE_IOC_DESTROY, or close(2)
 *   HandleReport      write(2) of one input report, or HIDRESOURCE_IOC_INPUT
 *   PostReportResult  HIDRESOURCE_IOC_REPLY
 *   the data queue    read(2), with poll(2) and kqueue(2) EVFILT_READ
 *
 * Each open of /dev/hidresource is one device; closing the descriptor, or the
 * process exiting, destroys it. The node is root:wheel 0600.
 *
 * This header is used by the kext and by userland (libIOKit's IOHIDUserDevice,
 * nextbsd-userland#521, keeps a copy). It is additive: no existing ABI
 * changes. Not upstream.
 */
#ifndef _HIDRESOURCE_H_
#define _HIDRESOURCE_H_

#include <sys/types.h>
#include <sys/ioccom.h>
#ifndef _KERNEL
#include <stdint.h>
#endif

#define	HIDRESOURCE_PATH	"/dev/hidresource"

#define	HIDRESOURCE_DESC_MAX	4096	/* report descriptor, bytes */
#define	HIDRESOURCE_REPORT_MAX	4096	/* one report, bytes */
#define	HIDRESOURCE_NAME_MAX	80	/* name and uniq, with the NUL */

/*
 * Bus types, the evdev values (<dev/evdev/input.h>). The bus type becomes the
 * evdev device's, and the IORegistry's IOHIDDevice entry derives Transport
 * from it: BUS_BLUETOOTH is "Bluetooth", BUS_VIRTUAL "Virtual". Only a
 * privileged creator (PRIV_DRIVER) gets Bluetooth; anyone else gets Virtual,
 * as Darwin forces Transport = "Virtual" without its entitlement.
 */
#define	HIDRESOURCE_BUS_BLUETOOTH	0x05
#define	HIDRESOURCE_BUS_VIRTUAL		0x06

/* Report types, as HID and <dev/hid/hid.h> number them. */
#define	HIDRESOURCE_REPORT_INPUT	1
#define	HIDRESOURCE_REPORT_OUTPUT	2
#define	HIDRESOURCE_REPORT_FEATURE	3

/*
 * HIDRESOURCE_IOC_CREATE: Darwin's Create. hc_name is the product name
 * (Product), hc_uniq the serial number (SerialNumber; for Bluetooth the
 * remote BD_ADDR, "00:11:22:33:44:55"). hc_timeout is how long a request
 * waits for HIDRESOURCE_IOC_REPLY, in milliseconds (RequestTimeout): 0 means
 * 1000, and other values are clamped to 50..1200000. On return hc_bus holds
 * the bus type granted. The ioctl returns at once; the device attaches in the
 * background, and its drivers' first requests may arrive on read(2) before the
 * ioctl has returned. EBUSY if this descriptor already has a device, ENOSPC
 * past the hw.hidresource.max_devices limit.
 */
struct hidresource_create {
	uint16_t	hc_bus;		/* HIDRESOURCE_BUS_*; in and out */
	uint16_t	hc_vendor;	/* VendorID */
	uint16_t	hc_product;	/* ProductID */
	uint16_t	hc_version;	/* VersionNumber */
	uint32_t	hc_timeout;	/* RequestTimeout, ms; 0 = 1000 */
	uint32_t	hc_desc_len;	/* bytes used in hc_desc, 1..DESC_MAX */
	char		hc_name[HIDRESOURCE_NAME_MAX];
	char		hc_uniq[HIDRESOURCE_NAME_MAX];
	uint8_t		hc_desc[HIDRESOURCE_DESC_MAX];	/* ReportDescriptor */
};

/*
 * HIDRESOURCE_IOC_INPUT: Darwin's HandleReport, the same as a write(2) of
 * hi_data. One input report, its report ID first when the descriptor numbers
 * its reports. A report that arrives while no driver has the device open is
 * dropped (hw.hidresource.dropped_input counts them).
 */
struct hidresource_input {
	uint32_t	hi_len;		/* 1..REPORT_MAX */
	uint32_t	hi_reserved;	/* 0 */
	uint8_t		hi_data[HIDRESOURCE_REPORT_MAX];
};

/*
 * HIDRESOURCE_IOC_REPLY: Darwin's PostReportResult. Completes the request
 * read(2) delivered with he_token. hr_error is 0 or a positive errno, which
 * the driver's call returns. For GET_REPORT, hr_data holds the report (its
 * report ID first when numbered) and hr_len its length; anything past the
 * request's he_maxlen is cut off. ESRCH if no request with that token is
 * pending (it timed out, or the device is gone).
 */
struct hidresource_reply {
	uint32_t	hr_token;
	int32_t		hr_error;
	uint32_t	hr_len;		/* 0..REPORT_MAX */
	uint32_t	hr_reserved;	/* 0 */
	uint8_t		hr_data[HIDRESOURCE_REPORT_MAX];
};

#define	HIDRESOURCE_IOC_CREATE	_IOWR('h', 1, struct hidresource_create)
#define	HIDRESOURCE_IOC_DESTROY	_IO('h', 2)
#define	HIDRESOURCE_IOC_INPUT	_IOW('h', 3, struct hidresource_input)
#define	HIDRESOURCE_IOC_REPLY	_IOW('h', 4, struct hidresource_reply)

/*
 * read(2) returns one message: a struct hidresource_event, then he_len bytes
 * of report data. A buffer smaller than the whole message gets EMSGSIZE and
 * the message stays queued; HIDRESOURCE_EVENT_MAX always fits. read(2) blocks
 * unless O_NONBLOCK is set (EAGAIN), and returns ENXIO when the queue is empty
 * and the descriptor has no device.
 *
 * Messages with a nonzero he_token are requests from a driver, which waits
 * for HIDRESOURCE_IOC_REPLY with that token until the device's timeout; the
 * driver then gets ETIMEDOUT. Several can be pending at once. The others need
 * no reply.
 *
 *   START         a driver opened the device: input is now delivered
 *   STOP          the last driver closed it
 *   OUTPUT        an output report (keyboard LEDs), he_len bytes, its
 *                 report ID first when numbered: HIDP DATA on the interrupt
 *                 channel
 *   GET_REPORT    request: he_report_type, he_report_id, and he_maxlen, the
 *                 most bytes wanted; reply with the report
 *   SET_REPORT    request: he_report_type, he_report_id, and the report,
 *                 he_len bytes, its report ID first when numbered
 *   SET_IDLE      request: he_report_id, and he_value the duration in ms
 *   SET_PROTOCOL  request: he_value 0 for boot, 1 for report protocol
 */
struct hidresource_event {
	uint32_t	he_type;	/* HIDRESOURCE_EVENT_* */
	uint32_t	he_token;	/* nonzero: reply with IOC_REPLY */
	uint8_t		he_report_type;	/* HIDRESOURCE_REPORT_* */
	uint8_t		he_report_id;	/* 0 if the reports are not numbered */
	uint16_t	he_value;	/* SET_IDLE, SET_PROTOCOL */
	uint32_t	he_maxlen;	/* GET_REPORT */
	uint32_t	he_len;		/* data bytes after this header */
	uint32_t	he_reserved;
};

#define	HIDRESOURCE_EVENT_START		1
#define	HIDRESOURCE_EVENT_STOP		2
#define	HIDRESOURCE_EVENT_OUTPUT	3
#define	HIDRESOURCE_EVENT_GET_REPORT	4
#define	HIDRESOURCE_EVENT_SET_REPORT	5
#define	HIDRESOURCE_EVENT_SET_IDLE	6
#define	HIDRESOURCE_EVENT_SET_PROTOCOL	7

#define	HIDRESOURCE_EVENT_MAX	\
	(sizeof(struct hidresource_event) + HIDRESOURCE_REPORT_MAX)

#endif /* _HIDRESOURCE_H_ */
