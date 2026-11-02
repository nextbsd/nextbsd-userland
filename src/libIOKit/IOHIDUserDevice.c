/*
 * IOHIDUserDevice.c — libIOKit: virtual HID devices created from userland
 * (nextbsd-userland#521).
 *
 * Apple's IOHIDUserDevice API over the /dev/hidresource transport of
 * IOHIDUserDevice.kext (nextbsd-kernel-extensions#25). On Darwin the object
 * wraps an IOHIDResourceDeviceUserClient connection; here it wraps one open
 * of /dev/hidresource, whose ioctls are that user client's methods one for
 * one. The kernel ABI lives only in hidresource.h, a verbatim copy of the
 * kext's IOHIDUserDevice/src/hidresource.h; when the kext changes it, copy it
 * again.
 *
 * Shape:
 *   - Create validates the properties, opens the node (loading the kext
 *     through kextd the first time the node is missing) and keeps a copy of
 *     the properties. Nothing is published yet.
 *   - Activate sends the properties to the kernel (HIDRESOURCE_IOC_CREATE)
 *     and starts
 *     a DISPATCH_SOURCE_TYPE_READ source on the fd, targeted at the device's
 *     queue. The kernel attaches hidbus from a taskqueue, so the drivers'
 *     attach-time get/set-report requests arrive on that source and the
 *     handlers answer them on the device's queue. This is Darwin's
 *     StartWhenScheduled order: the service is registered only once the
 *     device can answer.
 *   - Input reports go out with write(2), from any thread. The fd lock keeps them off a closing fd.
 *   - Cancel cancels the source; its cancel handler, on the device's queue,
 *     closes the fd (which destroys the kernel device and fails its pending
 *     requests), runs the caller's cancel handler and drops the reference
 *     the source held. A read of EOF or an error (the kext went away) cancels
 *     the same way.
 *
 * What differs from Apple, and why:
 *   - No run-loop scheduling (NextBSD's CoreFoundation has no CFMachPort,
 *     and every consumer here uses dispatch). ScheduleWithDispatchQueue is
 *     kept.
 *   - SetProperty only edits the copy that Activate sends; there is no
 *     kernel setProperties path after that.
 *   - Kernel SET_PROTOCOL and SET_IDLE requests have no handler in Apple's
 *     API; they are answered with success at once, as hkbd's attach waits
 *     for them.
 *   - The transport has no timestamp, so HandleReportWithTimeStamp ignores
 *     it; evdev stamps the event when the kernel delivers it.
 *   - Of the properties, the kernel takes ReportDescriptor, VendorID,
 *     ProductID, VersionNumber, Product, SerialNumber, RequestTimeout and
 *     Transport (as a bus type). Numbers above 0xffff in the 16-bit fields
 *     are truncated, as USB and Bluetooth IDs are 16-bit anyway.
 *   - Misuse that Apple traps with os_assert (registering after Activate,
 *     Activate without a queue) is logged and ignored.
 */
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDUserDevice.h>

#include "hidresource.h"	/* verbatim copy of the kext's ABI */
/* KextManager.h uses the AVAILABLE_MAC_OS_X_* macros without including them. */
#include <AvailabilityMacros.h>
#include "KextManager.h"	/* kext_tools/kext.subproj, linked in whole */

#include <CoreFoundation/CFRuntime.h>
#include <dispatch/dispatch.h>
#include <Block.h>

#include <sys/param.h>
#include <sys/ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#define	HID_STATE_INACTIVE	0
#define	HID_STATE_ACTIVE	1
#define	HID_STATE_CANCELLED	2

/*
 * The kext that provides HIDRESOURCE_PATH, loaded through kextd on the first
 * IOHIDUserDeviceCreateWithProperties when the node is absent.
 */
#define	HID_KEXT_IDENTIFIER	"org.nextbsd.kext.IOHIDUserDevice"

struct __IOHIDUserDevice {
	CFRuntimeBase			 base;
	CFMutableDictionaryRef		 properties;
	pthread_mutex_t			 lock;		/* fd, state */
	int				 fd;
	int				 state;		/* HID_STATE_* */
	dispatch_queue_t		 queue;
	dispatch_source_t		 source;	/* READ on fd */
	dispatch_block_t		 cancelHandler;
	IOHIDUserDeviceGetReportBlock	 getBlock;
	IOHIDUserDeviceSetReportBlock	 setBlock;
	IOHIDUserDeviceReportCallback	 getCallback;
	void				*getRefcon;
	IOHIDUserDeviceReportWithReturnLengthCallback getLengthCallback;
	void				*getLengthRefcon;
	IOHIDUserDeviceReportCallback	 setCallback;
	void				*setRefcon;
	uint8_t				*event;		/* HIDRESOURCE_EVENT_MAX */
	struct hidresource_reply	*reply;
};

static CFTypeID __kIOHIDUserDeviceTypeID = _kCFRuntimeNotATypeID;
static pthread_once_t __IOHIDUserDeviceOnce = PTHREAD_ONCE_INIT;

static void
__IOHIDUserDeviceFinalize(CFTypeRef object)
{
	IOHIDUserDeviceRef device = (IOHIDUserDeviceRef)object;

	if (device->fd >= 0)
		(void)close(device->fd);
	if (device->source != NULL)
		dispatch_release(device->source);
	if (device->queue != NULL)
		dispatch_release(device->queue);
	if (device->cancelHandler != NULL)
		Block_release(device->cancelHandler);
	if (device->getBlock != NULL)
		Block_release(device->getBlock);
	if (device->setBlock != NULL)
		Block_release(device->setBlock);
	if (device->properties != NULL)
		CFRelease(device->properties);
	free(device->event);
	free(device->reply);
	pthread_mutex_destroy(&device->lock);
}

static CFStringRef
__IOHIDUserDeviceCopyDescription(CFTypeRef object)
{
	IOHIDUserDeviceRef device = (IOHIDUserDeviceRef)object;

	return (CFStringCreateWithFormat(CFGetAllocator(object), NULL,
	    CFSTR("<IOHIDUserDevice %p [%p]>{fd = %d, state = %d}"), object,
	    CFGetAllocator(object), device->fd, device->state));
}

static const CFRuntimeClass __IOHIDUserDeviceClass = {
	.version		= 0,
	.className		= "IOHIDUserDevice",
	.finalize		= __IOHIDUserDeviceFinalize,
	.copyDebugDesc		= __IOHIDUserDeviceCopyDescription,
};

static void
__IOHIDUserDeviceRegister(void)
{
	__kIOHIDUserDeviceTypeID =
	    _CFRuntimeRegisterClass(&__IOHIDUserDeviceClass);
}

CFTypeID
IOHIDUserDeviceGetTypeID(void)
{
	pthread_once(&__IOHIDUserDeviceOnce, __IOHIDUserDeviceRegister);
	return (__kIOHIDUserDeviceTypeID);
}

/*
 * Opens /dev/hidresource. If the node is missing, asks kextd to load
 * IOHIDUserDevice.kext (a no-op when it is already loaded) and tries again.
 */
static int
hid_open_resource(void)
{
	OSReturn r;
	int fd;

	fd = open(HIDRESOURCE_PATH, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0 || errno != ENOENT)
		return (fd);
	r = KextManagerLoadKextWithIdentifier(CFSTR(HID_KEXT_IDENTIFIER),
	    NULL);
	if (r != kOSReturnSuccess) {
		syslog(LOG_ERR, "IOHIDUserDevice: cannot load %s: 0x%x",
		    HID_KEXT_IDENTIFIER, (unsigned)r);
		errno = ENXIO;
		return (-1);
	}
	return (open(HIDRESOURCE_PATH, O_RDWR | O_NONBLOCK | O_CLOEXEC));
}

static IOReturn
hid_errno_to_ioreturn(int error)
{
	switch (error) {
	case 0:
		return (kIOReturnSuccess);
	case ENOMEM:
		return (kIOReturnNoMemory);
	case ENOBUFS:
	case EAGAIN:
		return (kIOReturnNoResources);
	case EPERM:
	case EACCES:
		return (kIOReturnNotPrivileged);
	case EINVAL:
	case EMSGSIZE:
		return (kIOReturnBadArgument);
	case ENXIO:
	case ENODEV:
	case EBADF:
		return (kIOReturnOffline);
	default:
		return (kIOReturnIOError);
	}
}

static int
hid_ioreturn_to_errno(IOReturn ret)
{
	switch (ret) {
	case kIOReturnSuccess:
		return (0);
	case kIOReturnUnsupported:
		return (EOPNOTSUPP);
	case kIOReturnNoMemory:
		return (ENOMEM);
	case kIOReturnBadArgument:
		return (EINVAL);
	case kIOReturnTimeout:
		return (ETIMEDOUT);
	case kIOReturnNotReady:
	case kIOReturnOffline:
	case kIOReturnNoDevice:
		return (ENXIO);
	default:
		return (EIO);
	}
}

static IOHIDReportType
hid_report_type(uint8_t type, IOHIDReportType dflt)
{
	switch (type) {
	case HIDRESOURCE_REPORT_INPUT:
		return (kIOHIDReportTypeInput);
	case HIDRESOURCE_REPORT_OUTPUT:
		return (kIOHIDReportTypeOutput);
	case HIDRESOURCE_REPORT_FEATURE:
		return (kIOHIDReportTypeFeature);
	default:
		return (dflt);
	}
}

static bool
dict_number(CFDictionaryRef d, const char *key, uint64_t *out)
{
	CFStringRef k;
	CFTypeRef v;
	long long n;

	k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	if (k == NULL)
		return (false);
	v = CFDictionaryGetValue(d, k);
	CFRelease(k);
	if (v == NULL || CFGetTypeID(v) != CFNumberGetTypeID() ||
	    !CFNumberGetValue(v, kCFNumberLongLongType, &n) || n < 0)
		return (false);
	*out = (uint64_t)n;
	return (true);
}

/* Returns a malloced UTF-8 copy of a string property, or NULL. */
static char *
dict_string(CFDictionaryRef d, const char *key)
{
	CFStringRef k;
	CFTypeRef v;
	CFIndex size;
	char *s;

	k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	if (k == NULL)
		return (NULL);
	v = CFDictionaryGetValue(d, k);
	CFRelease(k);
	if (v == NULL || CFGetTypeID(v) != CFStringGetTypeID())
		return (NULL);
	size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(v),
	    kCFStringEncodingUTF8) + 1;
	if ((s = malloc(size)) == NULL)
		return (NULL);
	if (!CFStringGetCString(v, s, size, kCFStringEncodingUTF8)) {
		free(s);
		return (NULL);
	}
	return (s);
}

static CFDataRef
dict_descriptor(CFDictionaryRef d)
{
	CFTypeRef v;

	v = CFDictionaryGetValue(d, CFSTR(kIOHIDReportDescriptorKey));
	if (v == NULL || CFGetTypeID(v) != CFDataGetTypeID() ||
	    CFDataGetLength(v) <= 0 || CFDataGetLength(v) > HIDRESOURCE_DESC_MAX)
		return (NULL);
	return (v);
}

/*
 * Fills HIDRESOURCE_IOC_CREATE from the properties. Transport "Bluetooth" and
 * "BluetoothLowEnergy" ask for BUS_BLUETOOTH, anything else BUS_VIRTUAL; the
 * kernel grants Bluetooth only to a privileged creator.
 */
static bool
hid_fill_create(CFDictionaryRef props, struct hidresource_create *hc)
{
	CFDataRef desc;
	uint64_t n;
	char *s;

	if ((desc = dict_descriptor(props)) == NULL)
		return (false);
	memset(hc, 0, sizeof(*hc));
	hc->hc_bus = HIDRESOURCE_BUS_VIRTUAL;
	s = dict_string(props, kIOHIDTransportKey);
	if (s != NULL && (strcmp(s, kIOHIDTransportBluetoothValue) == 0 ||
	    strcmp(s, kIOHIDTransportBluetoothLowEnergyValue) == 0))
		hc->hc_bus = HIDRESOURCE_BUS_BLUETOOTH;
	free(s);
	if (dict_number(props, kIOHIDVendorIDKey, &n))
		hc->hc_vendor = (uint16_t)n;
	if (dict_number(props, kIOHIDProductIDKey, &n))
		hc->hc_product = (uint16_t)n;
	if (dict_number(props, kIOHIDVersionNumberKey, &n))
		hc->hc_version = (uint16_t)n;
	if (dict_number(props, kIOHIDRequestTimeoutKey, &n))
		hc->hc_timeout = n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
	if ((s = dict_string(props, kIOHIDProductKey)) != NULL) {
		strlcpy(hc->hc_name, s, sizeof(hc->hc_name));
		free(s);
	}
	if ((s = dict_string(props, kIOHIDSerialNumberKey)) != NULL) {
		strlcpy(hc->hc_uniq, s, sizeof(hc->hc_uniq));
		free(s);
	}
	hc->hc_desc_len = (uint32_t)CFDataGetLength(desc);
	memcpy(hc->hc_desc, CFDataGetBytePtr(desc), hc->hc_desc_len);
	return (true);
}

IOHIDUserDeviceRef
IOHIDUserDeviceCreateWithProperties(CFAllocatorRef allocator,
    CFDictionaryRef properties, IOOptionBits options __unused)
{
	IOHIDUserDeviceRef device;
	int fd;

	if (properties == NULL || dict_descriptor(properties) == NULL)
		return (NULL);
	if ((fd = hid_open_resource()) < 0)
		return (NULL);

	device = (IOHIDUserDeviceRef)_CFRuntimeCreateInstance(allocator,
	    IOHIDUserDeviceGetTypeID(), sizeof(struct __IOHIDUserDevice) -
	    sizeof(CFRuntimeBase), NULL);
	if (device == NULL) {
		(void)close(fd);
		return (NULL);
	}
	memset((char *)device + sizeof(CFRuntimeBase), 0,
	    sizeof(struct __IOHIDUserDevice) - sizeof(CFRuntimeBase));
	pthread_mutex_init(&device->lock, NULL);
	device->fd = fd;
	device->state = HID_STATE_INACTIVE;
	device->properties = CFDictionaryCreateMutableCopy(allocator, 0,
	    properties);
	device->event = malloc(HIDRESOURCE_EVENT_MAX);
	device->reply = malloc(sizeof(*device->reply));
	if (device->properties == NULL || device->event == NULL ||
	    device->reply == NULL) {
		CFRelease(device);
		return (NULL);
	}
	return (device);
}

CFTypeRef
IOHIDUserDeviceCopyProperty(IOHIDUserDeviceRef device, CFStringRef key)
{
	CFTypeRef v;

	pthread_mutex_lock(&device->lock);
	v = CFDictionaryGetValue(device->properties, key);
	if (v != NULL)
		CFRetain(v);
	pthread_mutex_unlock(&device->lock);
	return (v);
}

Boolean
IOHIDUserDeviceSetProperty(IOHIDUserDeviceRef device, CFStringRef key,
    CFTypeRef property)
{
	Boolean ok = false;

	pthread_mutex_lock(&device->lock);
	if (device->state == HID_STATE_INACTIVE && key != NULL &&
	    property != NULL) {
		CFDictionarySetValue(device->properties, key, property);
		ok = true;
	}
	pthread_mutex_unlock(&device->lock);
	return (ok);
}

/* Setters below are for before Activate, as on Darwin. */
static bool
hid_configurable(IOHIDUserDeviceRef device, const char *what)
{
	bool ok;

	pthread_mutex_lock(&device->lock);
	ok = device->state == HID_STATE_INACTIVE;
	pthread_mutex_unlock(&device->lock);
	if (!ok)
		syslog(LOG_ERR, "IOHIDUserDevice: %s after activation ignored",
		    what);
	return (ok);
}

void
IOHIDUserDeviceSetDispatchQueue(IOHIDUserDeviceRef device,
    dispatch_queue_t queue)
{
	if (queue == NULL || !hid_configurable(device, "SetDispatchQueue"))
		return;
	dispatch_retain(queue);
	if (device->queue != NULL)
		dispatch_release(device->queue);
	device->queue = queue;
}

void
IOHIDUserDeviceSetCancelHandler(IOHIDUserDeviceRef device,
    dispatch_block_t handler)
{
	if (!hid_configurable(device, "SetCancelHandler"))
		return;
	if (device->cancelHandler != NULL)
		Block_release(device->cancelHandler);
	device->cancelHandler = handler != NULL ? Block_copy(handler) : NULL;
}

void
IOHIDUserDeviceRegisterGetReportBlock(IOHIDUserDeviceRef device,
    IOHIDUserDeviceGetReportBlock block)
{
	if (!hid_configurable(device, "RegisterGetReportBlock"))
		return;
	if (device->getBlock != NULL)
		Block_release(device->getBlock);
	device->getBlock = block != NULL ? Block_copy(block) : NULL;
}

void
IOHIDUserDeviceRegisterSetReportBlock(IOHIDUserDeviceRef device,
    IOHIDUserDeviceSetReportBlock block)
{
	if (!hid_configurable(device, "RegisterSetReportBlock"))
		return;
	if (device->setBlock != NULL)
		Block_release(device->setBlock);
	device->setBlock = block != NULL ? Block_copy(block) : NULL;
}

void
IOHIDUserDeviceRegisterGetReportCallback(IOHIDUserDeviceRef device,
    IOHIDUserDeviceReportCallback callback, void *refcon)
{
	if (!hid_configurable(device, "RegisterGetReportCallback"))
		return;
	device->getCallback = callback;
	device->getRefcon = refcon;
}

void
IOHIDUserDeviceRegisterGetReportWithReturnLengthCallback(
    IOHIDUserDeviceRef device,
    IOHIDUserDeviceReportWithReturnLengthCallback callback, void *refcon)
{
	if (!hid_configurable(device,
	    "RegisterGetReportWithReturnLengthCallback"))
		return;
	device->getLengthCallback = callback;
	device->getLengthRefcon = refcon;
}

void
IOHIDUserDeviceRegisterSetReportCallback(IOHIDUserDeviceRef device,
    IOHIDUserDeviceReportCallback callback, void *refcon)
{
	if (!hid_configurable(device, "RegisterSetReportCallback"))
		return;
	device->setCallback = callback;
	device->setRefcon = refcon;
}

/*
 * Answers a request. For GET_REPORT the report is already in
 * device->reply->hr_data.
 */
static void
hid_reply(IOHIDUserDeviceRef device, uint32_t token, IOReturn ret,
    CFIndex len)
{
	struct hidresource_reply *r = device->reply;

	r->hr_token = token;
	r->hr_error = hid_ioreturn_to_errno(ret);
	r->hr_len = ret == kIOReturnSuccess && len > 0 ? (uint32_t)len : 0;
	r->hr_reserved = 0;
	/* ESRCH: the request already timed out in the kernel. */
	(void)ioctl(device->fd, HIDRESOURCE_IOC_REPLY, r);
}

static void
hid_get_report(IOHIDUserDeviceRef device, const struct hidresource_event *ev)
{
	uint8_t *report = device->reply->hr_data;
	IOHIDReportType type;
	CFIndex max, len;
	IOReturn ret;

	type = hid_report_type(ev->he_report_type, kIOHIDReportTypeFeature);
	max = ev->he_maxlen < HIDRESOURCE_REPORT_MAX ? ev->he_maxlen :
	    HIDRESOURCE_REPORT_MAX;
	len = max;
	memset(report, 0, (size_t)max);
	if (device->getBlock != NULL)
		ret = device->getBlock(type, ev->he_report_id, report, &len);
	else if (device->getLengthCallback != NULL)
		ret = device->getLengthCallback(device->getLengthRefcon, type,
		    ev->he_report_id, report, &len);
	else if (device->getCallback != NULL)
		ret = device->getCallback(device->getRefcon, type,
		    ev->he_report_id, report, len);
	else
		ret = kIOReturnUnsupported;
	if (len < 0 || len > max)
		len = max;
	hid_reply(device, ev->he_token, ret, len);
}

static IOReturn
hid_set_report(IOHIDUserDeviceRef device, IOHIDReportType type,
    uint8_t id, const uint8_t *data, CFIndex len)
{
	if (device->setBlock != NULL)
		return (device->setBlock(type, id, data, len));
	if (device->setCallback != NULL)
		return (device->setCallback(device->setRefcon, type, id,
		    (uint8_t *)(uintptr_t)data, len));
	return (kIOReturnUnsupported);
}

/*
 * The read source's handler, on the device's queue: drains the kernel's
 * event queue. A read of EOF or a hard error means the kernel device is
 * gone, which cancels the device.
 */
static void
hid_read_events(IOHIDUserDeviceRef device)
{
	struct hidresource_event ev;
	const uint8_t *data;
	CFIndex len;
	ssize_t n;
	IOReturn ret;

	for (;;) {
		n = read(device->fd, device->event, HIDRESOURCE_EVENT_MAX);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			return;
		if (n <= 0) {
			dispatch_source_cancel(device->source);
			return;
		}
		if ((size_t)n < sizeof(ev))
			continue;
		memcpy(&ev, device->event, sizeof(ev));
		data = device->event + sizeof(ev);
		len = (CFIndex)((size_t)n - sizeof(ev));
		if (ev.he_len < len)
			len = ev.he_len;

		switch (ev.he_type) {
		case HIDRESOURCE_EVENT_GET_REPORT:
			hid_get_report(device, &ev);
			break;
		case HIDRESOURCE_EVENT_SET_REPORT:
			ret = hid_set_report(device, hid_report_type(
			    ev.he_report_type, kIOHIDReportTypeFeature),
			    ev.he_report_id, data, len);
			hid_reply(device, ev.he_token, ret, 0);
			break;
		case HIDRESOURCE_EVENT_OUTPUT:
			/* Fire and forget, like an HIDP DATA output frame. */
			(void)hid_set_report(device, hid_report_type(
			    ev.he_report_type, kIOHIDReportTypeOutput),
			    ev.he_report_id, data, len);
			break;
		case HIDRESOURCE_EVENT_SET_IDLE:
		case HIDRESOURCE_EVENT_SET_PROTOCOL:
			hid_reply(device, ev.he_token, kIOReturnSuccess, 0);
			break;
		default:
			/* START, STOP, and anything newer. */
			if (ev.he_token != 0)
				hid_reply(device, ev.he_token,
				    kIOReturnUnsupported, 0);
			break;
		}
	}
}

/* The read source's cancel handler, on the device's queue. */
static void
hid_source_cancelled(IOHIDUserDeviceRef device)
{
	pthread_mutex_lock(&device->lock);
	device->state = HID_STATE_CANCELLED;
	if (device->fd >= 0) {
		(void)close(device->fd);
		device->fd = -1;
	}
	pthread_mutex_unlock(&device->lock);
	if (device->cancelHandler != NULL)
		device->cancelHandler();
	CFRelease(device);	/* the source's reference */
}

void
IOHIDUserDeviceActivate(IOHIDUserDeviceRef device)
{
	struct hidresource_create *hc;
	dispatch_source_t source;
	int error;

	pthread_mutex_lock(&device->lock);
	if (device->state != HID_STATE_INACTIVE) {
		pthread_mutex_unlock(&device->lock);
		return;
	}
	if (device->queue == NULL) {
		pthread_mutex_unlock(&device->lock);
		syslog(LOG_ERR, "IOHIDUserDevice: Activate without a queue");
		return;
	}
	hc = malloc(sizeof(*hc));
	if (hc == NULL || !hid_fill_create(device->properties, hc)) {
		pthread_mutex_unlock(&device->lock);
		free(hc);
		syslog(LOG_ERR, "IOHIDUserDevice: bad properties");
		return;
	}
	error = ioctl(device->fd, HIDRESOURCE_IOC_CREATE, hc) == -1 ? errno : 0;
	free(hc);
	if (error != 0) {
		pthread_mutex_unlock(&device->lock);
		syslog(LOG_ERR, "IOHIDUserDevice: create failed: %s",
		    strerror(error));
		return;
	}

	source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
	    (uintptr_t)device->fd, 0, device->queue);
	if (source == NULL) {
		(void)ioctl(device->fd, HIDRESOURCE_IOC_DESTROY);
		pthread_mutex_unlock(&device->lock);
		syslog(LOG_ERR, "IOHIDUserDevice: no read source");
		return;
	}
	CFRetain(device);	/* released by the cancel handler */
	dispatch_source_set_event_handler(source, ^{
		hid_read_events(device);
	});
	dispatch_source_set_cancel_handler(source, ^{
		hid_source_cancelled(device);
	});
	device->source = source;
	device->state = HID_STATE_ACTIVE;
	pthread_mutex_unlock(&device->lock);
	dispatch_resume(source);
}

void
IOHIDUserDeviceScheduleWithDispatchQueue(IOHIDUserDeviceRef device,
    dispatch_queue_t queue)
{
	IOHIDUserDeviceSetDispatchQueue(device, queue);
	IOHIDUserDeviceActivate(device);
}

void
IOHIDUserDeviceCancel(IOHIDUserDeviceRef device)
{
	dispatch_block_t handler = NULL;
	int state;

	pthread_mutex_lock(&device->lock);
	state = device->state;
	if (state == HID_STATE_INACTIVE) {
		/* Never published: close now, and still run the handler. */
		device->state = HID_STATE_CANCELLED;
		if (device->fd >= 0) {
			(void)close(device->fd);
			device->fd = -1;
		}
		handler = device->cancelHandler;
	}
	pthread_mutex_unlock(&device->lock);

	if (state == HID_STATE_ACTIVE)
		dispatch_source_cancel(device->source);
	else if (handler != NULL && device->queue != NULL)
		dispatch_async(device->queue, handler);
	else if (handler != NULL)
		handler();
}

IOReturn
IOHIDUserDeviceHandleReportWithTimeStamp(IOHIDUserDeviceRef device,
    uint64_t timestamp __unused, const uint8_t *report, CFIndex reportLength)
{
	IOReturn ret;
	int error;

	if (device == NULL || report == NULL || reportLength <= 0 ||
	    reportLength > HIDRESOURCE_REPORT_MAX)
		return (kIOReturnBadArgument);

	pthread_mutex_lock(&device->lock);
	if (device->state != HID_STATE_ACTIVE) {
		ret = device->state == HID_STATE_INACTIVE ? kIOReturnNotReady :
		    kIOReturnOffline;
		pthread_mutex_unlock(&device->lock);
		return (ret);
	}
	error = write(device->fd, report, (size_t)reportLength) == -1 ?
	    errno : 0;
	pthread_mutex_unlock(&device->lock);
	return (hid_errno_to_ioreturn(error));
}

IOReturn
IOHIDUserDeviceHandleReport(IOHIDUserDeviceRef device, const uint8_t *report,
    CFIndex reportLength)
{
	return (IOHIDUserDeviceHandleReportWithTimeStamp(device, 0, report,
	    reportLength));
}
