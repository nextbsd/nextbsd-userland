/*
 * IOHIDUserDevice.c — libIOKit: virtual HID devices created from userland
 * (nextbsd-userland#521).
 *
 * Apple's IOHIDUserDevice API over the /dev/hidresource transport of
 * IOHIDUserDevice.kext (nextbsd-kernel-extensions#25). On Darwin the object
 * wraps an IOHIDResourceDeviceUserClient connection; here it wraps one open
 * of /dev/hidresource, whose ioctls are that user client's methods one for
 * one (vhid.h). The kernel ABI lives only in vhid.h.
 *
 * Shape:
 *   - Create validates the properties, opens the node (loading the kext
 *     through kextd the first time the node is missing) and keeps a copy of
 *     the properties. Nothing is published yet.
 *   - Activate sends the properties to the kernel (VHIDIOCCREATE) and starts
 *     a DISPATCH_SOURCE_TYPE_READ source on the fd, targeted at the device's
 *     queue. The kernel attaches hidbus from a taskqueue, so the drivers'
 *     attach-time get/set-report requests arrive on that source and the
 *     handlers answer them on the device's queue. This is Darwin's
 *     StartWhenScheduled order: the service is registered only once the
 *     device can answer.
 *   - Input reports go out with write(2) (timestamp 0) or VHIDIOCINPUT, from
 *     any thread. The fd lock keeps them off a closing fd.
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
 *     API; they are answered with success.
 *   - Misuse that Apple traps with os_assert (registering after Activate,
 *     Activate without a queue) is logged and ignored.
 */
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDUserDevice.h>

#include "vhid.h"		/* mirror of the kext's ABI */
/* KextManager.h uses the AVAILABLE_MAC_OS_X_* macros without including them. */
#include <AvailabilityMacros.h>
#include "KextManager.h"	/* kext_tools/kext.subproj, linked in whole */

#include <CoreFoundation/CFRuntime.h>
#include <dispatch/dispatch.h>
#include <Block.h>

#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/nv.h>

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

#define	HID_EVENT_BUFSIZE	(sizeof(struct vhid_event) + VHID_MAX_REPORT)

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
	uint8_t				*event;		/* HID_EVENT_BUFSIZE */
	uint8_t				*report;	/* VHID_MAX_REPORT */
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
	free(device->report);
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

	fd = open(VHID_CTL_PATH, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0 || errno != ENOENT)
		return (fd);
	r = KextManagerLoadKextWithIdentifier(CFSTR(VHID_KEXT_IDENTIFIER),
	    NULL);
	if (r != kOSReturnSuccess) {
		syslog(LOG_ERR, "IOHIDUserDevice: cannot load %s: 0x%x",
		    VHID_KEXT_IDENTIFIER, (unsigned)r);
		errno = ENXIO;
		return (-1);
	}
	return (open(VHID_CTL_PATH, O_RDWR | O_NONBLOCK | O_CLOEXEC));
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
	case VHID_REPORT_INPUT:
		return (kIOHIDReportTypeInput);
	case VHID_REPORT_OUTPUT:
		return (kIOHIDReportTypeOutput);
	case VHID_REPORT_FEATURE:
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
	    CFDataGetLength(v) <= 0 || CFDataGetLength(v) > VHID_MAX_DESCRIPTOR)
		return (NULL);
	return (v);
}

/*
 * Packs the properties into the VHIDIOCCREATE nvlist: only the keys the
 * kernel accepts, with Transport reduced to the values it knows.
 */
static void *
hid_pack_properties(CFDictionaryRef props, size_t *lenp)
{
	static const char *const numbers[] = {
		VHID_KEY_VENDOR_ID, VHID_KEY_PRODUCT_ID, VHID_KEY_VERSION_NUMBER,
		VHID_KEY_COUNTRY_CODE, VHID_KEY_REQUEST_TIMEOUT,
	};
	static const char *const strings[] = {
		VHID_KEY_PRODUCT, VHID_KEY_SERIAL_NUMBER,
	};
	CFDataRef desc;
	nvlist_t *nvl;
	uint64_t n;
	size_t i;
	char *s;
	void *buf;

	if ((desc = dict_descriptor(props)) == NULL)
		return (NULL);
	if ((nvl = nvlist_create(0)) == NULL)
		return (NULL);
	nvlist_add_binary(nvl, VHID_KEY_REPORT_DESCRIPTOR,
	    CFDataGetBytePtr(desc), (size_t)CFDataGetLength(desc));
	for (i = 0; i < nitems(numbers); i++)
		if (dict_number(props, numbers[i], &n))
			nvlist_add_number(nvl, numbers[i], n);
	for (i = 0; i < nitems(strings); i++)
		if ((s = dict_string(props, strings[i])) != NULL) {
			nvlist_add_string(nvl, strings[i], s);
			free(s);
		}
	s = dict_string(props, VHID_KEY_TRANSPORT);
	if (s != NULL && (strcmp(s, VHID_TRANSPORT_BLUETOOTH) == 0 ||
	    strcmp(s, VHID_TRANSPORT_BLUETOOTH_LE) == 0))
		nvlist_add_string(nvl, VHID_KEY_TRANSPORT, s);
	else
		nvlist_add_string(nvl, VHID_KEY_TRANSPORT,
		    VHID_TRANSPORT_VIRTUAL);
	free(s);
	buf = nvlist_error(nvl) == 0 ? nvlist_pack(nvl, lenp) : NULL;
	nvlist_destroy(nvl);
	return (buf);
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
	device->event = malloc(HID_EVENT_BUFSIZE);
	device->report = malloc(VHID_MAX_REPORT);
	if (device->properties == NULL || device->event == NULL ||
	    device->report == NULL) {
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

static void
hid_reply(IOHIDUserDeviceRef device, uint32_t token, IOReturn ret,
    const void *data, CFIndex len)
{
	struct vhid_reply r;

	memset(&r, 0, sizeof(r));
	r.vr_token = token;
	r.vr_error = hid_ioreturn_to_errno(ret);
	if (ret == kIOReturnSuccess && data != NULL && len > 0) {
		r.vr_data = data;
		r.vr_len = (uint32_t)len;
	}
	/* ESRCH: the request already timed out in the kernel. */
	(void)ioctl(device->fd, VHIDIOCREPLY, &r);
}

static void
hid_get_report(IOHIDUserDeviceRef device, const struct vhid_event *ev)
{
	IOHIDReportType type;
	CFIndex max, len;
	IOReturn ret;

	type = hid_report_type(ev->ve_report_type, kIOHIDReportTypeFeature);
	max = ev->ve_len < VHID_MAX_REPORT ? ev->ve_len : VHID_MAX_REPORT;
	len = max;
	memset(device->report, 0, (size_t)max);
	if (device->getBlock != NULL)
		ret = device->getBlock(type, ev->ve_report_id, device->report,
		    &len);
	else if (device->getLengthCallback != NULL)
		ret = device->getLengthCallback(device->getLengthRefcon, type,
		    ev->ve_report_id, device->report, &len);
	else if (device->getCallback != NULL)
		ret = device->getCallback(device->getRefcon, type,
		    ev->ve_report_id, device->report, len);
	else
		ret = kIOReturnUnsupported;
	if (len < 0 || len > max)
		len = max;
	hid_reply(device, ev->ve_token, ret, device->report, len);
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
	struct vhid_event ev;
	const uint8_t *data;
	CFIndex len;
	ssize_t n;
	IOReturn ret;

	for (;;) {
		n = read(device->fd, device->event, HID_EVENT_BUFSIZE);
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
		if (ev.ve_len < len)
			len = ev.ve_len;

		switch (ev.ve_type) {
		case VHID_EV_GET_REPORT:
			hid_get_report(device, &ev);
			break;
		case VHID_EV_SET_REPORT:
			ret = hid_set_report(device, hid_report_type(
			    ev.ve_report_type, kIOHIDReportTypeFeature),
			    ev.ve_report_id, data, len);
			hid_reply(device, ev.ve_token, ret, NULL, 0);
			break;
		case VHID_EV_OUTPUT:
			/* Fire and forget, like an HIDP DATA output frame. */
			(void)hid_set_report(device, hid_report_type(
			    ev.ve_report_type, kIOHIDReportTypeOutput),
			    ev.ve_report_id, data, len);
			break;
		case VHID_EV_SET_PROTOCOL:
		case VHID_EV_SET_IDLE:
			hid_reply(device, ev.ve_token, kIOReturnSuccess, NULL,
			    0);
			break;
		default:
			/* START, STOP, OPEN, CLOSE: nothing to do. */
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
	struct vhid_create vc;
	dispatch_source_t source;
	size_t len;
	void *buf;
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
	buf = hid_pack_properties(device->properties, &len);
	if (buf == NULL) {
		pthread_mutex_unlock(&device->lock);
		syslog(LOG_ERR, "IOHIDUserDevice: cannot pack properties");
		return;
	}
	vc.vc_nvlist = buf;
	vc.vc_nvlist_len = len;
	error = ioctl(device->fd, VHIDIOCCREATE, &vc) == -1 ? errno : 0;
	free(buf);
	if (error != 0) {
		pthread_mutex_unlock(&device->lock);
		syslog(LOG_ERR, "IOHIDUserDevice: create failed: %s",
		    strerror(error));
		return;
	}

	source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
	    (uintptr_t)device->fd, 0, device->queue);
	if (source == NULL) {
		(void)ioctl(device->fd, VHIDIOCDESTROY);
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
    uint64_t timestamp, const uint8_t *report, CFIndex reportLength)
{
	struct vhid_input vi;
	IOReturn ret;
	int error;

	if (device == NULL || report == NULL || reportLength <= 0 ||
	    reportLength > VHID_MAX_REPORT)
		return (kIOReturnBadArgument);

	pthread_mutex_lock(&device->lock);
	if (device->state != HID_STATE_ACTIVE) {
		ret = device->state == HID_STATE_INACTIVE ? kIOReturnNotReady :
		    kIOReturnOffline;
		pthread_mutex_unlock(&device->lock);
		return (ret);
	}
	error = 0;
	if (timestamp != 0) {
		vi.vi_timestamp = timestamp;
		vi.vi_data = report;
		vi.vi_len = (uint32_t)reportLength;
		if (ioctl(device->fd, VHIDIOCINPUT, &vi) == -1)
			error = errno;
	}
	if (timestamp == 0 || error == ENOTTY) {
		error = 0;
		if (write(device->fd, report, (size_t)reportLength) == -1)
			error = errno;
	}
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
