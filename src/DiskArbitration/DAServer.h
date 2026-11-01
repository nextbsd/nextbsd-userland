/*
 * DAServer.h — diskarbitrationd's MIG server (nextbsd-userland#197).
 *
 * The C counterpart of Apple's diskarbitrationd/DAServer.c session half
 * (DiskArbitration-535.120.1.0.1): the _DAServer* routines of DAServer.defs
 * and the receive loop that serves them. Apple demuxes on dispatch_mach
 * channels; NextBSD's libdispatch has no dispatch_mach, so this is
 * configd's shape: one port set holding the service port and every session
 * port, a mach_msg receive loop on the main thread, and each message
 * handled on the daemon queue (dispatch_sync_f), where the disk model
 * lives.
 */
#ifndef DISKARBITRATION_DASERVER_H
#define DISKARBITRATION_DASERVER_H

#include <dispatch/dispatch.h>

#include <mach/mach.h>

#include <signal.h>

/* The port set the receive loop serves; sessions add their ports to it. */
extern mach_port_t gDAServerPortSet;

/*
 * Put the checked-in service port into a new port set and remember the
 * daemon queue. Returns 0, or -1 if the port set cannot be made.
 */
int	DAServerInit(mach_port_t service, dispatch_queue_t queue);

/* Serve until *stop is set (checked at least once a second). */
void	DAServerRun(volatile sig_atomic_t *stop);

#endif /* DISKARBITRATION_DASERVER_H */
