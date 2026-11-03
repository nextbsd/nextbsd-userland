/*
 * PMDisplay_freebsd.c — the clamshell part of Apple's pmconfigd/PMDisplay.m,
 * as C (nextbsd-userland#230, E17 D6).
 *
 * PMDisplay.m is Objective-C built around SkyLight, the WindowServer's
 * display-power client. Its clamshell code is small and does not need
 * SkyLight to be useful: pmconfigd keeps the lid state the root domain
 * reports and tells the rest of the system about it.
 *
 *   rootDomainClamshellState()        AppleClamshellState of IOPMrootDomain
 *   initializeClamshellState()        read once, when the root domain's
 *                                     general interest is set up
 *   updateClamshellState()            on kIOPMMessageClamshellStateChange
 *   postClamshellStateNotification()  com.apple.system.powermanagement.
 *                                     clamshellstate (notify(3); state 1
 *                                     while the lid is closed, else 0)
 *
 * Apple runs this path on Apple silicon Macs, where pmconfigd, not a
 * display wrangler in the kernel, owns the display; on Intel Macs
 * IODisplayWrangler and the WindowServer see the lid themselves. NextBSD is
 * in the Apple silicon position (pmconfigd runs the display idle timer,
 * nextbsd-userland#239), so this file follows that branch. What it leaves
 * out is the SkyLight request (requestClamshellState()) that tells the
 * WindowServer to switch the built-in display off or on; the window server
 * gets the notify(3) key instead.
 *
 * As on macOS, the lid state itself stays where the kernel keeps it: the
 * root domain's AppleClamshellState and AppleClamshellCausesSleep registry
 * properties (nextbsd-kernel#243; ioreg -r -n IOPMrootDomain), present only
 * on a machine with a lid. macOS has no dynamic store key for the lid, and
 * neither has NextBSD.
 *
 * Delete this file when PMDisplay.m builds.
 */
#include "PrivateLib_freebsd.h"

#include <notify.h>

enum {
    kPMClamshellOpen          = 1,
    kPMClamshellClosed        = 2,
    kPMClamshellUnknown       = 3,
    kPMClamshellDoesNotExist  = 4
};

uint32_t gClamshellState = kPMClamshellUnknown;
IOPMAssertionID gLidOpenAssertionID = kIOPMNullAssertionID;

int clamshellNotifyToken = 0;

void postClamshellStateNotification(void) {
    /* Apple: on (TARGET_OS_OSX && TARGET_CPU_ARM64) only; see above. */
    if (0 == clamshellNotifyToken) {
        notify_register_check("com.apple.system.powermanagement.clamshellstate",
                              &clamshellNotifyToken);
    }
    // Post 1 whenever clamshell is closed, 0 otherwise.
    notify_set_state(clamshellNotifyToken, (uint32_t) gClamshellState == kPMClamshellClosed);
    notify_post("com.apple.system.powermanagement.clamshellstate");
}

/*
 * Apple casts the message argument itself to the clamshell bits; NextBSD's
 * caller (RootDomainInterest()) passes argument[0] the same way.
 */
__private_extern__ void updateClamshellState(void *message)
{
    uint32_t bits = (uint32_t)(uintptr_t)message;
    uint32_t closed = bits & kClamshellStateBit;
    uint32_t new_state = closed ? kPMClamshellClosed : kPMClamshellOpen;
    if (gClamshellState != new_state) {
        gClamshellState = new_state;
        /* Apple: requestClamshellState() to the WindowServer (SkyLight). */
        if (new_state == kPMClamshellOpen) {
            // clamshell open. send activity tickle to rootDomain
            CFStringRef description = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("com.apple.powermanagement.lidopen"));
            if (description) {
                InternalDeclareUserActive(description, &gLidOpenAssertionID);
                CFRelease(description);
            }
        }
        postClamshellStateNotification();
    }
    INFO_LOG("ClamshellState. Closed : %u. ClamshellSleepState: isSleepDisabled : %d\n", closed, getClamshellSleepState());
}

uint32_t rootDomainClamshellState(void)
{
    CFBooleanRef clamshell_state = NULL;
    uint32_t result = kPMClamshellUnknown;
    clamshell_state = (CFBooleanRef)_copyRootDomainProperty(CFSTR(kAppleClamshellStateKey));
    if (clamshell_state != NULL) {
        if (kCFBooleanTrue == clamshell_state) {
            result = kPMClamshellClosed;
        } else {
            result = kPMClamshellOpen;
        }
        CFRelease(clamshell_state);
    } else {
        result = kPMClamshellDoesNotExist;
    }
    INFO_LOG("rootDomain clamshell state %u\n", result);
    return result;
}

void initializeClamshellState(void)
{
    if (gClamshellState == kPMClamshellUnknown) {
        gClamshellState = rootDomainClamshellState();
        postClamshellStateNotification();
    }
}
