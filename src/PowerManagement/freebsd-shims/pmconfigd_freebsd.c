/*
 * pmconfigd_freebsd.c — pmconfigd's entry point on NextBSD, until
 * pmconfigd/pmconfigd.m builds.
 *
 * Apple's pmconfigd.m holds main(), powerd_init() and the two listeners, but
 * it also references every pmconfigd subsystem (assertions, power sources,
 * sleep/wake, display, battery health, ...), so it cannot link until those
 * are ported. This file carries the part of it pmconfigd needs to run as a
 * launchd MachService today, kept in Apple's shape and with Apple's names so
 * that each later ticket only adds the next *_prime() call:
 *
 *   main()                  pmconfigd.m: dispatch_sync_f(powerd_init),
 *                           then dispatch_main()
 *   powerd_init()           pmconfigd.m: check in com.apple.PowerManagement
 *                           .control, register com.apple.iokit.powerdxpc,
 *                           connect the MIG listener
 *   pmListenerHandler()     pmconfigd.m, verbatim: dispatch_mach_mig_demux
 *                           over the powermanagement subsystem
 *   xpc_register()          pmconfigd.m, plus a NULL check: NextBSD's libxpc
 *                           returns NULL when the check-in fails
 *   incoming_XPC_connection() pmconfigd.m's peer handler with no message
 *                           handlers yet (each XPC message belongs to a
 *                           subsystem that is not built; see below)
 *   _getPMMainQueue()       PrivateLib.m, verbatim
 *
 * When pmconfigd.m and PrivateLib.m are added to SRCS, this file is deleted.
 *
 * Both MachServices keep Apple's names. Apple's IOKit client code looks them
 * up by name (kIOPMServerBootstrapName, "com.apple.iokit.powerdxpc"), so the
 * job is org.nextbsd.powerd but the services it vends are Apple's.
 *
 * Quiet by design: nothing reaches the console. os_log goes to stderr, which
 * launchd sends to /var/log/pmconfigd.stderr, and on a normal start the
 * daemon writes a single line there.
 */
#include <dispatch/dispatch.h>
#include <dispatch/private.h>		/* dispatch_mach_* (DISPATCH_MACH_SPI) */
#include <mach/mach.h>
#include <os/log.h>
#include <servers/bootstrap.h>
#include <xpc/xpc.h>

#include <IOKit/pwr_mgt/powermanagement_mig.h>
#include "powermanagementServer.h"	/* MIG: _powermanagement_subsystem */

#include <stdbool.h>
#include <stddef.h>

/* IOKitUser pwr_mgt.subproj/IOPMLibPrivate.h */
#ifndef kIOPMServerBootstrapName
#define kIOPMServerBootstrapName	"com.apple.PowerManagement.control"
#endif

/* pmconfigd.m: the XPC MachService. */
#define kIOPMServerXPCName		"com.apple.iokit.powerdxpc"

__private_extern__ dispatch_queue_t _getPMMainQueue(void);

static mach_port_t      serverPort = MACH_PORT_NULL;
static dispatch_mach_t  gListener;

/*
 * PrivateLib.m. Every pmconfigd subsystem runs on this one serial queue, so
 * the daemon's state needs no locks.
 */
__private_extern__ dispatch_queue_t
_getPMMainQueue(void)
{
    static dispatch_queue_t     pmRLS = NULL;

    if (!pmRLS) {
        pmRLS = dispatch_queue_create_with_target("Power Management main queue", NULL, NULL);
    }

    return pmRLS;
}

static void
pmListenerHandler(void *context, dispatch_mach_reason_t reason,
                  dispatch_mach_msg_t msg, mach_error_t error)
{
    (void)context;
    (void)error;

    if (reason == DISPATCH_MACH_MESSAGE_RECEIVED) {
        static const struct mig_subsystem *const subsystems[] = {
            (mig_subsystem_t)&_powermanagement_subsystem,
        };
        if (!dispatch_mach_mig_demux(NULL, subsystems, 1, msg)) {
            mach_msg_destroy(dispatch_mach_msg_get_msg(msg, NULL));
        }
    }
}

/*
 * pmconfigd.m dispatches each XPC dictionary on its first key:
 * kUserActivityRegister / kUserActivityTimeoutUpdate (PMAssertions.c),
 * kClaimSystemWakeEvent (PMConnection.m), kAssertionCreateMsg and the
 * other async-assertion keys (PMAssertions.c), and the power-source keys
 * (BatteryTimeRemaining.m). None of those files is built yet, so the peer
 * is accepted and its messages are dropped; Apple's own handler drops a
 * message whose key it does not know in the same way. Each ticket that
 * builds one of those files adds its branch here, or moves this handler
 * back into pmconfigd.m.
 */
static void
incoming_XPC_connection(xpc_connection_t peer)
{
    xpc_connection_set_target_queue(peer, _getPMMainQueue());

    xpc_connection_set_event_handler(peer,
             ^(xpc_object_t event) {
                 if (xpc_get_type(event) == XPC_TYPE_DICTIONARY) {
                     os_log_debug(OS_LOG_DEFAULT, "XPC message from PID %u dropped\n",
                                  xpc_connection_get_pid(peer));
                 }
             });

    xpc_connection_resume(peer);
}

static void
xpc_register(void)
{
    xpc_connection_t        connection;

    connection = xpc_connection_create_mach_service(
                                                    kIOPMServerXPCName,
                                                    _getPMMainQueue(),
                                                    XPC_CONNECTION_MACH_SERVICE_LISTENER);
    if (connection == NULL) {
        os_log_error(OS_LOG_DEFAULT, "xpc listener \"%s\" could not check in\n",
                     kIOPMServerXPCName);
        return;
    }

    xpc_connection_set_target_queue(connection, _getPMMainQueue());

    xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
            xpc_type_t type = xpc_get_type(event);
            if (type == XPC_TYPE_CONNECTION) {
                incoming_XPC_connection((xpc_connection_t)event);
            }
        });

    xpc_connection_resume(connection);
}

static void
powerd_init(void *__unused context)
{
    kern_return_t           kern_result = 0;

    kern_result = bootstrap_check_in(
                            bootstrap_port,
                            kIOPMServerBootstrapName,
                            &serverPort);

    xpc_register();

    if (BOOTSTRAP_SUCCESS != kern_result) {
        os_log_error(OS_LOG_DEFAULT, "PM configd: bootstrap_register \"%s\" error = %d\n",
                                kIOPMServerBootstrapName, kern_result);
    }

    if (MACH_PORT_NULL != serverPort)
    {
        gListener = dispatch_mach_create_f("PowerManagement", _getPMMainQueue(), NULL, pmListenerHandler);
        dispatch_mach_connect(gListener, serverPort, MACH_PORT_NULL, NULL);
    }

    /*
     * Apple's powerd_init() continues with PMStoreLoad(), the preference
     * and notification set-up, and the *_prime() call of every subsystem
     * (PMConnection, PMSettings, AutoWake, PMAssertions, PMSystemEvents,
     * SystemLoad, BatteryTimeRemaining, TTYKeepAwake, ...). Each is added
     * here, in Apple's order, by the ticket that ports it.
     */

    os_log(OS_LOG_DEFAULT, "pmconfigd started: %s%s, %s\n",
           kIOPMServerBootstrapName,
           MACH_PORT_NULL != serverPort ? "" : " (not checked in)",
           kIOPMServerXPCName);
}

int main(int argc __unused, char *argv[] __unused)
{
    /*
     * This dispatch_sync() ensures that no event can be received concurrently
     * to initializing services, and allow for launch at priority
     * to function properly by checking in on the main thread.
     */
    dispatch_sync_f(_getPMMainQueue(), NULL, powerd_init);
    dispatch_main();
    return 0;
}
