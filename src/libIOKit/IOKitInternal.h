/*
 * IOKitInternal.h — private to libIOKit.
 *
 * Defines the client-side handle struct that backs every io_object_t
 * the facade hands out, plus the shared lazy-init helper for the
 * kernel /dev/ioregistry fd. iter 1 — read-only walk; later iters
 * extend the struct as new handle flavors appear (matching
 * iterators, notification iterators).
 */
#ifndef _IOKIT_INTERNAL_H_
#define _IOKIT_INTERNAL_H_

#include <IOKit/IOKitLib.h>

#include <stdatomic.h>
#include <stdint.h>
#include <mach/mach.h>

/*
 * Handle flavors. iter 1 only has two — a registry entry (a single
 * registry node id) and an iterator (a captured uint64_t id array).
 * Each iter that adds a new flavor adds a constant here and a
 * branch in IOObjectRelease.
 */
#define IOOBJ_KIND_ENTRY	1
#define IOOBJ_KIND_ITERATOR	2
#define IOOBJ_KIND_INTEREST	3	/* IOServiceAddInterestNotification */
#define IOOBJ_KIND_CONNECT	4	/* io_connect_t: IOPMLib.c */

struct __io_interest;

struct __IOObject {
	uint32_t	kind;		/* IOOBJ_KIND_* */
	atomic_int	refcnt;		/* IOObjectRetain/Release */
	/* entry: */
	uint64_t	node_id;
	/* iterator: */
	uint64_t	*ids;		/* malloced; freed in release */
	uint32_t	count;
	uint32_t	cursor;
	/* interest notification (IOKitNotify.c owns it): */
	struct __io_interest *interest;
	/* connection (IOPMLib.c): the /dev/iopmroot descriptor standing in for
	 * the root domain's user client, -1 once IOServiceClose closed it.
	 * node_id is the IOPMrootDomain entry. */
	atomic_int	fd;
};

/*
 * __io_interest_release — called by IOObjectRelease when the last reference
 * to an IOOBJ_KIND_INTEREST object goes: unregisters the interest from its
 * IONotificationPort and frees it (IOKitNotify.c).
 */
void	__io_interest_release(struct __io_interest *interest);

/*
 * __io_connect_release — called by IOObjectRelease when the last reference
 * to an IOOBJ_KIND_CONNECT object goes: closes its descriptor (IOPMLib.c).
 */
void	__io_connect_release(io_connect_t connect);

/*
 * __io_pmroot_fd — cached process-wide descriptor on /dev/iopmroot (the
 * in-kernel IOPMrootDomain, nextbsd-kernel#243), opened O_RDWR | O_CLOEXEC on
 * first call (pthread_once-guarded) and kept for the life of the process.
 * IORegistryEntrySetCFProperty on the root domain and the acknowledgements
 * libIOKit sends on behalf of a released registration go through it. -1 on
 * a kernel without the root domain. IOPMLib.c.
 */
int	__io_pmroot_fd(void);

/*
 * __io_pmroot_set_maintenance_wake — kPMSetMaintenanceWakeCalendar: hand the
 * root domain a maintenance or SleepService wake time (IOPMCalendarStruct,
 * UTC) through IOPMROOT_SETMAINTWAKE on __io_pmroot_fd(). Like xnu's
 * RootDomainUserClient method it returns KERN_SUCCESS once the call is made
 * and the root domain's own result in *result. IOPMAutoWake.c's
 * doAMaintenanceWake. IOPMLib.c.
 */
struct IOPMCalendarStruct;
kern_return_t	__io_pmroot_set_maintenance_wake(
	    const struct IOPMCalendarStruct *date, IOReturn *result);

/*
 * __io_ioregistry_fd — cached read-only fd to the kernel /dev/ioregistry
 * device (the K1 in-kernel registry, nextbsd#214), opened with
 * O_CLOEXEC on first call (pthread_once-guarded). Returns -1 if the
 * device is absent (an old-kernel image predating K1) — every facade
 * entry point then returns kIOReturnNoDevice / IO_OBJECT_NULL.
 */
int	__io_ioregistry_fd(void);

/*
 * Shared per-op node accessor over /dev/ioregistry (IOREGIOCNODE). Both
 * IOKitLib.c (name/path) and IOKitMatching.c (class) call it so the
 * access logic lives in one place. Any out-pointer may be NULL to skip
 * that field. `state` is the IOREG_STATE_* liveness value. Returns a
 * kern_return_t (KERN_SUCCESS on success).
 */
kern_return_t	__io_node(uint64_t node_id, uint64_t *parent_id, int *state,
		    char name[32], char classname[32], char driver[32],
		    char path[256]);

/*
 * Internal allocators. __io_alloc_iterator takes ownership of the
 * id array (it is freed by IOObjectRelease) — on calloc failure the
 * array is freed and IO_OBJECT_NULL returned.
 */
io_object_t	__io_alloc_entry(uint64_t node_id);
io_object_t	__io_alloc_iterator(uint64_t *ids, uint32_t count);

/*
 * The criteria fields the kernel match accepts — `name`, `class`,
 * `driver` (empty string = wildcard). All three are char[32], matching
 * the kernel `struct ioreg_criteria` string fields.
 */
#define IO_CRITERIA_FIELD_MAX	32

/*
 * Numeric property criteria (nextbsd-kernel#391): the USB matching keys
 * (idVendor, idProduct, bcdDevice, the bDevice and bInterface class triples,
 * locationID) that Darwin's IOUSBHostDevice / IOUSBHostInterface match on.
 * The kernel criteria struct has no room for them, so the kernel matches the
 * class and name and libIOKit compares these against the entry's property
 * bag. `key` indexes a fixed key table in IOKitMatching.c.
 */
#define IO_CRITERIA_PROPS_MAX	12

struct io_prop_criterion {
	unsigned	key;
	int64_t		value;
};

struct io_criteria {
	char	name[IO_CRITERIA_FIELD_MAX];
	char	klass[IO_CRITERIA_FIELD_MAX];	/* "class" — C++ keyword */
	char	driver[IO_CRITERIA_FIELD_MAX];
	unsigned nprops;
	struct io_prop_criterion props[IO_CRITERIA_PROPS_MAX];
};

/*
 * Extract the understood criterion strings from an Apple-shape matching
 * CFDictionary. Unknown keys are silently ignored.
 *
 *  IOProviderClass / IOClass	-> out->klass
 *  IONameMatch			-> out->name
 *  idVendor, idProduct, ...	-> out->props (CFNumber values only)
 *
 * `matching` may carry other keys (IOPropertyMatch, IOBSDName,
 * regex), all currently ignored.
 */
void	__io_extract_criteria(CFDictionaryRef matching,
	    struct io_criteria *out);

/*
 * True if registry entry `node_id` satisfies the numeric property criteria
 * in `c` (always true when it has none): each property must be present in
 * the entry's bag as a number equal to the criterion. A departed entry is
 * still readable while it lingers, so this also works for a terminated
 * notification.
 */
bool	__io_props_match(uint64_t node_id, const struct io_criteria *c);

/*
 * Fill a flat kernel `struct ioreg_criteria` from an io_criteria for the
 * /dev/ioregistry IOREGIOC{WATCH,LOOKUP} ioctls (#218). These ioctls carry
 * the criteria as a fixed by-value struct (no packed nvlist): there is no
 * serialization, so the former libxpc-vs-libnv wire-format mismatch that broke
 * the #218 round-trip simply cannot happen. Non-empty string fields and
 * non-zero numeric fields constrain the match (zero-as-wildcard). Declared with
 * a forward struct ref so this header pulls in no kernel ABI.
 */
struct ioreg_criteria;
void	__io_fill_criteria(const struct io_criteria *c,
		    struct ioreg_criteria *out);

#endif /* _IOKIT_INTERNAL_H_ */
