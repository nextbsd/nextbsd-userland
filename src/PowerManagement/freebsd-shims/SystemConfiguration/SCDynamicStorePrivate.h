/*
 * SystemConfiguration/SCDynamicStorePrivate.h — NextBSD shim for pmconfigd.
 *
 * Apple's PMStore.c includes this private header for one call,
 * SCDynamicStoreSetDisconnectCallBack(): when configd restarts, Apple's
 * SystemConfiguration reconnects the session and calls the callback, and
 * PMStore pushes its cached keys back with SCDynamicStoreSetMultiple().
 *
 * NextBSD's libSystemConfiguration has no reconnect (scutil's session.c
 * notes the same), so there is nothing to register with. The call is a
 * no-op that reports success; PMStore's cache is still kept, so a later
 * reconnect callback can be wired here without touching PMStore.c.
 */
#ifndef _PM_SHIM_SCDYNAMICSTOREPRIVATE_H_
#define _PM_SHIM_SCDYNAMICSTOREPRIVATE_H_

#include <SystemConfiguration/SCDynamicStore.h>

typedef void (*SCDynamicStoreDisconnectCallBack)(SCDynamicStoreRef store,
    void *info);

static inline Boolean
SCDynamicStoreSetDisconnectCallBack(SCDynamicStoreRef store,
    SCDynamicStoreDisconnectCallBack callout)
{
	(void)store;
	(void)callout;
	return (TRUE);
}

#endif /* _PM_SHIM_SCDYNAMICSTOREPRIVATE_H_ */
