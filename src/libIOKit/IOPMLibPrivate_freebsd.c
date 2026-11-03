/*
 * IOPMLibPrivate_freebsd.c — the connection to pmconfigd that IOKitUser's
 * power management clients share: _pm_connect() and _pm_disconnect(), from
 * IOKitUser-100231.120.3 pwr_mgt.subproj/IOPMLibPrivate.c. The ps.subproj
 * sources (IOPowerSources.c, IOPowerSourcesPrivate.c) call them.
 *
 * The code is Apple's. The __FreeBSD__ changes: os_unfair_lock is a pthread
 * mutex, and a failed lookup is not logged (libIOKit links no ASL).
 *
 * Delete this file when libIOKit builds Apple's IOPMLibPrivate.c
 * (nextbsd-userland#233), which defines both functions.
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <servers/bootstrap.h>
#include <bootstrap_priv.h>
#include <pthread.h>

#include "ps_freebsd.h"

static mach_port_t powerd_connection = MACH_PORT_NULL;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;


static void _reset_connection( )
{
  powerd_connection = MACH_PORT_NULL;
  lock = PTHREAD_MUTEX_INITIALIZER;
}

IOReturn _pm_connect(mach_port_t *newConnection)
{
    kern_return_t       kern_result = KERN_SUCCESS;
    IOReturn            ret = kIOReturnSuccess;

    if(!newConnection) return kIOReturnBadArgument;
    if (powerd_connection != MACH_PORT_NULL) {
        *newConnection = powerd_connection;

        return kIOReturnSuccess;
    }

    pthread_mutex_lock(&lock);

    // Check again and see if the port is created by another thread
    if (powerd_connection != MACH_PORT_NULL) {
        *newConnection = powerd_connection;
        goto exit;
    }
    // open reference to powerd
    kern_result = bootstrap_look_up2(bootstrap_port,
                                     kIOPMServerBootstrapName,
                                     &powerd_connection,
                                     0,
                                     BOOTSTRAP_PRIVILEGED_SERVER);
    if(KERN_SUCCESS != kern_result) {
        *newConnection = powerd_connection =  MACH_PORT_NULL;
        ret = kIOReturnError;
        goto exit;
    }
    *newConnection = powerd_connection;
    if (pthread_atfork(NULL, NULL, _reset_connection) != 0)
       powerd_connection = MACH_PORT_NULL;

exit:
    pthread_mutex_unlock(&lock);

    return ret;
}

IOReturn _pm_disconnect(mach_port_t connection __unused)
{
    // Do nothing. We re-use the mach port
    return kIOReturnSuccess;
}
