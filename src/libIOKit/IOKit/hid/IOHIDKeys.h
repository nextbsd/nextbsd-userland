/*
 * IOKit/hid/IOHIDKeys.h — the IOHIDFamily keys and types libIOKit uses.
 *
 * The subset of Apple's <IOKit/hid/IOHIDKeys.h> that IOHIDUserDevice needs:
 * the device property keys, the Transport values and IOHIDReportType. The
 * names and values are Apple's, so code written against the macOS SDK builds
 * unchanged. Keys are C strings, used as CFSTR(kIOHIDVendorIDKey).
 */
#ifndef _IOKIT_HID_IOHIDKEYS_H_
#define _IOKIT_HID_IOHIDKEYS_H_

#include <sys/cdefs.h>

__BEGIN_DECLS

/* Device property keys. */
#define kIOHIDTransportKey		"Transport"
#define kIOHIDVendorIDKey		"VendorID"
#define kIOHIDVendorIDSourceKey		"VendorIDSource"
#define kIOHIDProductIDKey		"ProductID"
#define kIOHIDVersionNumberKey		"VersionNumber"
#define kIOHIDManufacturerKey		"Manufacturer"
#define kIOHIDProductKey		"Product"
#define kIOHIDSerialNumberKey		"SerialNumber"
#define kIOHIDCountryCodeKey		"CountryCode"
#define kIOHIDLocationIDKey		"LocationID"
#define kIOHIDReportDescriptorKey	"ReportDescriptor"
#define kIOHIDPrimaryUsageKey		"PrimaryUsage"
#define kIOHIDPrimaryUsagePageKey	"PrimaryUsagePage"
#define kIOHIDRequestTimeoutKey		"RequestTimeout"

/* kIOHIDTransportKey values. */
#define kIOHIDTransportUSBValue			"USB"
#define kIOHIDTransportBluetoothValue		"Bluetooth"
#define kIOHIDTransportBluetoothLowEnergyValue	"BluetoothLowEnergy"
#define kIOHIDTransportI2CValue			"I2C"
#define kIOHIDTransportSPIValue			"SPI"

typedef enum IOHIDReportType {
	kIOHIDReportTypeInput = 0,
	kIOHIDReportTypeOutput,
	kIOHIDReportTypeFeature,
	kIOHIDReportTypeCount
} IOHIDReportType;

__END_DECLS

#endif /* _IOKIT_HID_IOHIDKEYS_H_ */
