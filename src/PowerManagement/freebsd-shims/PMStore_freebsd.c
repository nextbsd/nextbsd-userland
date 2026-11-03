/*
 * PMStore_freebsd.c — builds Apple's pmconfigd/PMStore.c, unmodified.
 *
 * PMStore.c is pmconfigd's cache in front of SCDynamicStore (PMStoreLoad,
 * PMStoreSetValue, PMStoreRemoveValue). It needs nothing NextBSD lacks
 * except two private SystemConfiguration headers, which freebsd-shims/
 * SystemConfiguration/ supplies, and _getPMMainQueue() from PrivateLib.h.
 * PrivateLib.h does not build yet, and PMStore.c includes it with quotes,
 * which always finds Apple's copy beside it. So this file declares what
 * PMStore.c uses from it (PrivateLib_freebsd.h, which also defines
 * PrivateLib.h's include guard, so Apple's copy is skipped) and includes the
 * Apple file.
 *
 * When PrivateLib.h builds, pmconfigd/PMStore.c goes into SRCS directly
 * and this file is deleted.
 */
#include <assert.h>

#include "PrivateLib_freebsd.h"	/* defines _privatelib_h_ */

#include "../pmconfigd/PMStore.c"
