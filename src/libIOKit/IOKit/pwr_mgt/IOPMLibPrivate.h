/*
 * IOPMLibPrivate.h — Private Power Management assertion definitions for NextBSD.
 *
 * Matching nextbsd-userland#233 / #236.
 */

#ifndef _IOKIT_PWR_MGT_IOPMLIBPRIVATE_H_
#define _IOKIT_PWR_MGT_IOPMLIBPRIVATE_H_

#include <IOKit/pwr_mgt/IOPMLib.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Assertion property keys
 */
#define kIOPMAssertionOnBehalfOfPID \
    CFSTR("OnBehalfOfPID")

#ifdef __cplusplus
}
#endif

#endif /* _IOKIT_PWR_MGT_IOPMLIBPRIVATE_H_ */
