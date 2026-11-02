/*
 * IOKit/hid/IOHIDUserDevice.h — virtual HID devices created from userland.
 *
 * Apple's IOHIDUserDevice API (the public <IOKit/hid/IOHIDUserDevice.h> of
 * the macOS SDK, plus the callback and HandleReport calls of IOKitUser's
 * private header). A daemon describes a device with a property dictionary
 * and a report descriptor, feeds it input reports, and answers the get- and
 * set-report requests the kernel's HID drivers make. To the rest of the
 * system the device is an ordinary HID device: on NextBSD it is a hidbus
 * device that hkbd, hms, hmt and the other HID drivers attach to, and the
 * IORegistry shows it as IOHIDUserDevice. bluetoothd uses it for Bluetooth
 * keyboards, mice and media keys.
 *
 * Life cycle, as on macOS:
 *
 *	device = IOHIDUserDeviceCreateWithProperties(NULL, props, 0);
 *	IOHIDUserDeviceSetDispatchQueue(device, queue);
 *	IOHIDUserDeviceRegisterGetReportBlock(device, ...);
 *	IOHIDUserDeviceRegisterSetReportBlock(device, ...);
 *	IOHIDUserDeviceSetCancelHandler(device, ^{ CFRelease(device); });
 *	IOHIDUserDeviceActivate(device);
 *	... IOHIDUserDeviceHandleReportWithTimeStamp(device, 0, r, len) ...
 *	IOHIDUserDeviceCancel(device);
 *
 * The device is published to the kernel's HID drivers at Activate, so the
 * get- and set-report handlers must be registered before then: drivers ask
 * for reports while they attach. Handlers run on the device's queue.
 *
 * NextBSD specifics:
 *  - The transport is /dev/hidresource, provided by IOHIDUserDevice.kext,
 *    which the first create loads through kextd. Creating a device needs
 *    root.
 *  - kIOHIDTransportKey "Bluetooth" and "BluetoothLowEnergy" make the device
 *    a Bluetooth HID device; any other value makes it a virtual one.
 *  - Of the property keys, the kernel uses ReportDescriptor (required, at
 *    most 4096 bytes), VendorID, ProductID, VersionNumber, CountryCode,
 *    Product, SerialNumber, Transport and RequestTimeout. Other keys are
 *    kept by the object and returned by IOHIDUserDeviceCopyProperty.
 *  - IOHIDUserDeviceSetProperty only works before Activate.
 *  - Timestamps are passed to the kernel as given; 0 means now.
 *  - There is no run-loop scheduling; use a dispatch queue.
 */
#ifndef _IOKIT_HID_IOHIDUSERDEVICE_H_
#define _IOKIT_HID_IOHIDUSERDEVICE_H_

#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDKeys.h>

#include <dispatch/dispatch.h>
#include <stdint.h>

__BEGIN_DECLS

/*
 * IOReturn codes this API returns that IOKitLib.h does not define yet, with
 * the same bare common-error values IOKitLib.h uses.
 */
#ifndef kIOReturnNoMemory
#define kIOReturnNoMemory	0x2bd
#endif
#ifndef kIOReturnNoResources
#define kIOReturnNoResources	0x2be
#endif
#ifndef kIOReturnNotPrivileged
#define kIOReturnNotPrivileged	0x2c1
#endif
#ifndef kIOReturnIOError
#define kIOReturnIOError	0x2ca
#endif
#ifndef kIOReturnTimeout
#define kIOReturnTimeout	0x2d6
#endif
#ifndef kIOReturnOffline
#define kIOReturnOffline	0x2d7
#endif
#ifndef kIOReturnNotReady
#define kIOReturnNotReady	0x2d8
#endif
#ifndef kIOReturnAborted
#define kIOReturnAborted	0x2eb
#endif

typedef struct __IOHIDUserDevice *IOHIDUserDeviceRef;

/*
 * Handlers for the kernel's get- and set-report requests. The report buffer
 * starts with the report ID when the descriptor numbers its reports. A get
 * handler fills report and sets *reportLength to the bytes written (it is
 * the buffer size on entry). Output reports from the drivers (keyboard LEDs,
 * for example) arrive at the set handler as kIOHIDReportTypeOutput.
 */
typedef IOReturn (^IOHIDUserDeviceGetReportBlock)(IOHIDReportType type,
    uint32_t reportID, uint8_t *report, CFIndex *reportLength);
typedef IOReturn (^IOHIDUserDeviceSetReportBlock)(IOHIDReportType type,
    uint32_t reportID, const uint8_t *report, CFIndex reportLength);

/* The callback forms, from IOKitUser's private header. */
typedef IOReturn (*IOHIDUserDeviceReportCallback)(void *refcon,
    IOHIDReportType type, uint32_t reportID, uint8_t *report,
    CFIndex reportLength);
typedef IOReturn (*IOHIDUserDeviceReportWithReturnLengthCallback)(
    void *refcon, IOHIDReportType type, uint32_t reportID, uint8_t *report,
    CFIndex *reportLength);

CFTypeID	IOHIDUserDeviceGetTypeID(void);

/*
 * Returns NULL if the properties have no ReportDescriptor, or if the
 * transport cannot be opened (no kext, or not root). options is reserved;
 * pass 0.
 */
IOHIDUserDeviceRef
		IOHIDUserDeviceCreateWithProperties(CFAllocatorRef allocator,
		    CFDictionaryRef properties, IOOptionBits options);

CFTypeRef	IOHIDUserDeviceCopyProperty(IOHIDUserDeviceRef device,
		    CFStringRef key);
Boolean		IOHIDUserDeviceSetProperty(IOHIDUserDeviceRef device,
		    CFStringRef key, CFTypeRef property);

/* Set once, before Activate. Handlers and the cancel handler run on it. */
void		IOHIDUserDeviceSetDispatchQueue(IOHIDUserDeviceRef device,
		    dispatch_queue_t queue);
void		IOHIDUserDeviceSetCancelHandler(IOHIDUserDeviceRef device,
		    dispatch_block_t handler);

void		IOHIDUserDeviceRegisterGetReportBlock(IOHIDUserDeviceRef device,
		    IOHIDUserDeviceGetReportBlock block);
void		IOHIDUserDeviceRegisterSetReportBlock(IOHIDUserDeviceRef device,
		    IOHIDUserDeviceSetReportBlock block);
void		IOHIDUserDeviceRegisterGetReportCallback(
		    IOHIDUserDeviceRef device,
		    IOHIDUserDeviceReportCallback callback, void *refcon);
void		IOHIDUserDeviceRegisterGetReportWithReturnLengthCallback(
		    IOHIDUserDeviceRef device,
		    IOHIDUserDeviceReportWithReturnLengthCallback callback,
		    void *refcon);
void		IOHIDUserDeviceRegisterSetReportCallback(
		    IOHIDUserDeviceRef device,
		    IOHIDUserDeviceReportCallback callback, void *refcon);

/*
 * Publishes the device and starts serving requests. Needs a dispatch queue.
 * Cancel tears the device down; the cancel handler runs on the queue once
 * no other handler can run, and is the place to release the device.
 */
void		IOHIDUserDeviceActivate(IOHIDUserDeviceRef device);
void		IOHIDUserDeviceCancel(IOHIDUserDeviceRef device);

/* Private API: SetDispatchQueue followed by Activate. */
void		IOHIDUserDeviceScheduleWithDispatchQueue(
		    IOHIDUserDeviceRef device, dispatch_queue_t queue);

/*
 * Delivers one input report. Callable from any thread once the device is
 * active; returns kIOReturnNotReady before Activate and kIOReturnOffline
 * after Cancel.
 */
IOReturn	IOHIDUserDeviceHandleReportWithTimeStamp(
		    IOHIDUserDeviceRef device, uint64_t timestamp,
		    const uint8_t *report, CFIndex reportLength);
IOReturn	IOHIDUserDeviceHandleReport(IOHIDUserDeviceRef device,
		    const uint8_t *report, CFIndex reportLength);

__END_DECLS

#endif /* _IOKIT_HID_IOHIDUSERDEVICE_H_ */
