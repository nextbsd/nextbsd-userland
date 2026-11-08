/*
 * freebsd-launchd-mach hostnamed freebsd-shim.
 *
 * SCPrivate.h SPI subset that Apple's set-hostname.c calls. Apple's
 * real header lives in SystemConfiguration.framework's private
 * include tree; we ship what set-hostname.c uses that the library does
 * not export (_SC_CFStringIsValidDNSName). The notify(3) names
 * (_SC_NOTIFY_NETWORK_CHANGE), _SC_string_to_sockaddr and
 * _SC_cfstring_to_cstring come from libSystemConfiguration's installed
 * <SystemConfiguration/SCPrivate.h>, which the
 * SystemConfiguration/SCPrivate.h indirect pulls in alongside this.
 */

#ifndef _FREEBSD_LAUNCHD_MACH_SC_PRIVATE_H_
#define _FREEBSD_LAUNCHD_MACH_SC_PRIVATE_H_

#include <CoreFoundation/CoreFoundation.h>

/*
 * _SC_CFStringIsValidDNSName — TRUE iff the CFString is a syntactically
 * valid RFC 1035 LDH name (letters/digits/hyphens, no leading or
 * trailing hyphen, total length 1..253, label length 1..63).
 */
Boolean	_SC_CFStringIsValidDNSName(CFStringRef cfstr);

#endif	/* _FREEBSD_LAUNCHD_MACH_SC_PRIVATE_H_ */
