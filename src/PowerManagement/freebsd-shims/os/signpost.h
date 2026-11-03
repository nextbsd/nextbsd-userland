/*
 * os/signpost.h — NextBSD shim for Apple's <os/signpost.h>, for pmconfigd.
 *
 * Signposts are performance-trace points read by Instruments; NextBSD has
 * no consumer for them. pmconfigd emits them around wake (PMAssertions.c),
 * so the API compiles to nothing. Arguments are not evaluated.
 */
#ifndef _PM_SHIM_OS_SIGNPOST_H_
#define _PM_SHIM_OS_SIGNPOST_H_

#include <os/log.h>
#include <stdint.h>

typedef uint64_t os_signpost_id_t;

#define OS_SIGNPOST_ID_NULL		((os_signpost_id_t)0)
#define OS_SIGNPOST_ID_INVALID		((os_signpost_id_t)~0ULL)
#define OS_SIGNPOST_ID_EXCLUSIVE	((os_signpost_id_t)0xEEEEB0B5B2B2EEEEULL)

#define os_signpost_enabled(log)			0
#define os_signpost_id_generate(log)			OS_SIGNPOST_ID_EXCLUSIVE
#define os_signpost_id_make_with_pointer(log, ptr)	OS_SIGNPOST_ID_EXCLUSIVE
#define os_signpost_event_emit(log, id, name, ...)	((void)0)
#define os_signpost_interval_begin(log, id, name, ...)	((void)0)
#define os_signpost_interval_end(log, id, name, ...)	((void)0)

#endif /* _PM_SHIM_OS_SIGNPOST_H_ */
