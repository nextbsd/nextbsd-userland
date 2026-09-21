/*
 * IOPMAssertions.c — Power Management assertion stub implementation.
 *
 * Implements IOPMAssertionCreateWithDescription, IOPMAssertionRelease,
 * IOPMAssertionRetain, and IOPMAssertionSetProperty.
 *
 * Stubs power assertions for clients like caffeinate(8) until full daemon
 * integration (#233) lands.
 */

#include <IOKit/IOKitLib.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/pwr_mgt/IOPMLibPrivate.h>
#include <stdatomic.h>

static atomic_uint_fast32_t gNextAssertionID = 1;

IOReturn
IOPMAssertionCreateWithDescription(
    CFStringRef         AssertionType __unused,
    CFStringRef         Name __unused,
    CFStringRef         Details __unused,
    CFStringRef         HumanReadableReason __unused,
    CFStringRef         LocalizationBundlePath __unused,
    CFTimeInterval      Timeout __unused,
    CFStringRef         TimeoutAction __unused,
    IOPMAssertionID     *AssertionID)
{
    if (AssertionID == NULL) {
        return kIOReturnBadArgument;
    }

    *AssertionID = (IOPMAssertionID)atomic_fetch_add(&gNextAssertionID, 1);
    return kIOReturnSuccess;
}

IOReturn
IOPMAssertionRelease(
    IOPMAssertionID     AssertionID __unused)
{
    return kIOReturnSuccess;
}

IOReturn
IOPMAssertionRetain(
    IOPMAssertionID     AssertionID __unused)
{
    return kIOReturnSuccess;
}

IOReturn
IOPMAssertionSetProperty(
    IOPMAssertionID     AssertionID __unused,
    CFStringRef         PropertyKey __unused,
    CFTypeRef           PropertyValue __unused)
{
    return kIOReturnSuccess;
}
