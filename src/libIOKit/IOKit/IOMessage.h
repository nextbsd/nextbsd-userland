/*
 * IOKit/IOMessage.h — Darwin's IOKit message codes, as delivered to an
 * IOServiceInterestCallback (IOServiceAddInterestNotification, IOKitLib.h).
 *
 * The values are Apple's (IOKit/IOMessage.h): a message is
 * sys_iokit | sub_iokit_common | code, i.e. 0xe0000000 | code for the common
 * family. The full common set is declared so Darwin sources compile; the
 * NextBSD kernel currently sends only kIOMessageServiceIsTerminated (the
 * service departed; last message of the registration) and
 * kIOMessageServicePropertyChange (for example a DRM connector hotplug on a
 * drmn entry).
 */
#ifndef _IOKIT_IOMESSAGE_H_
#define _IOKIT_IOMESSAGE_H_

#include <stdint.h>

typedef uint32_t IOMessage;

#define iokit_common_msg(message)	((uint32_t)(0xe0000000u | (message)))
#define iokit_family_msg(sub, message)	((uint32_t)(0xe0000000u | (sub) | (message)))
/* sub_iokit_vendor_specific == err_sub(-2) == 0xffe << 14 */
#define iokit_vendor_specific_msg(message) \
					((uint32_t)(0xe0000000u | (0xffeu << 14) | (message)))

#define kIOMessageServiceIsTerminated		iokit_common_msg(0x010)
#define kIOMessageServiceIsSuspended		iokit_common_msg(0x020)
#define kIOMessageServiceIsResumed		iokit_common_msg(0x030)
#define kIOMessageServiceIsRequestingClose	iokit_common_msg(0x100)
#define kIOMessageServiceIsAttemptingOpen	iokit_common_msg(0x101)
#define kIOMessageServiceWasClosed		iokit_common_msg(0x110)
#define kIOMessageServiceBusyStateChange	iokit_common_msg(0x120)
#define kIOMessageConsoleSecurityChange		iokit_common_msg(0x128)
#define kIOMessageServicePropertyChange		iokit_common_msg(0x130)
#define kIOMessageCopyClientID			iokit_common_msg(0x330)
#define kIOMessageSystemCapabilityChange	iokit_common_msg(0x340)
#define kIOMessageDeviceSignaledWakeup		iokit_common_msg(0x350)

#define kIOMessageDeviceWillPowerOff		iokit_common_msg(0x210)
#define kIOMessageDeviceHasPoweredOn		iokit_common_msg(0x230)
#define kIOMessageSystemWillPowerOff		iokit_common_msg(0x250)
#define kIOMessageSystemWillRestart		iokit_common_msg(0x310)
#define kIOMessageSystemPagingOff		iokit_common_msg(0x255)
#define kIOMessageCanSystemSleep		iokit_common_msg(0x270)
#define kIOMessageSystemWillNotSleep		iokit_common_msg(0x290)
#define kIOMessageSystemWillSleep		iokit_common_msg(0x280)
#define kIOMessageSystemWillPowerOn		iokit_common_msg(0x320)
#define kIOMessageSystemHasPoweredOn		iokit_common_msg(0x300)
#define kIOMessageCanDevicePowerOff		iokit_common_msg(0x200)
#define kIOMessageDeviceWillNotPowerOff		iokit_common_msg(0x220)
#define kIOMessageSystemWillNotPowerOff		iokit_common_msg(0x260)
#define kIOMessageCanSystemPowerOff		iokit_common_msg(0x240)
#define kIOMessageDeviceWillPowerOn		iokit_common_msg(0x215)
#define kIOMessageDeviceHasPoweredOff		iokit_common_msg(0x225)

#endif /* _IOKIT_IOMESSAGE_H_ */
