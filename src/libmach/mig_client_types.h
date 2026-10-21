/*
 * mig_client_types.h -- type vocabulary for the generated task, host_priv
 * and mach_host MIG clients (nextbsd-userland#84 / #93).
 *
 * PRIVATE to libmach; not installed.  The Makefile force-includes it
 * (-include) into every libmach TU when MIGOUT is set, so the generated
 * taskUser.c / host_privUser.c / mach_hostUser.c and their *_mig.h headers
 * see these names without the generated code needing to know.
 *
 * Why not <mach/mach_types.h>: these clients declare every routine in
 * their subsystem, so they name types (zone info, vouchers, memory
 * objects, vm_prot_t, ...) that no libmach consumer uses.  Apple has them
 * in the public headers, but adding them there would put typedefs in front
 * of every launchd / libdispatch / kext_tools TU that includes
 * <mach/mach_types.h> -- and some of those have their own spelling of the
 * same names (kext_tools/compat/mach/vm_prot.h; FreeBSD's <vm/vm.h> has
 * vm_prot_t as u_char).  Kept here, the blast radius is libmach itself.
 *
 * Every definition is Apple's userland one; the wire layout of each
 * comes from the .defs (and is what the kernel server uses), so these
 * only have to name the C types and, for inline structs, get their size
 * right.
 */
#ifndef _LIBMACH_MIG_CLIENT_TYPES_H_
#define _LIBMACH_MIG_CLIENT_TYPES_H_

#include <stdint.h>
#include <mach/mach_types.h>
#include <mach/clock_types.h>
#include <mach/exception.h>
#include <mach/host_info.h>
#include <mach/task_policy.h>

/* <mach/vm_prot.h> */
typedef int		vm_prot_t;

/* <mach/exception_types.h> */
typedef exception_mask_t	*exception_mask_array_t;
typedef exception_behavior_t	*exception_behavior_array_t;
typedef thread_state_flavor_t	*exception_flavor_array_t;

/* <mach/task_info.h> */
typedef natural_t	task_flavor_t;
typedef integer_t	*task_info_t;

/* <mach/vm_purgable.h>: task_purgable_info is inline (struct[68]) on the wire. */
struct vm_purgeable_stat {
	uint64_t	count;
	uint64_t	size;
};
typedef struct vm_purgeable_stat	vm_purgeable_stat_t;
struct vm_purgeable_info {
	vm_purgeable_stat_t	fifo_data[8];
	vm_purgeable_stat_t	obsolete_data;
	vm_purgeable_stat_t	lifo_data[8];
};
typedef struct vm_purgeable_info	task_purgable_info_t;

/* <mach/policy.h> */
typedef int		policy_t;
typedef integer_t	*policy_info_t;
typedef integer_t	*policy_base_t;
typedef integer_t	*policy_limit_t;

/* <mach/processor_info.h>, <mach/mach_types.h> */
typedef int		processor_flavor_t;
typedef integer_t	*processor_info_array_t;
typedef processor_set_name_t	*processor_set_name_array_t;
typedef ledger_t	*ledger_array_t;

/* <mach/memory_object_types.h> */
typedef mach_port_t	memory_object_t;
typedef mach_port_t	memory_object_default_t;
typedef unsigned long long	memory_object_size_t;
typedef natural_t	memory_object_cluster_size_t;

/* <mach/vm_types.h> (NextBSD's mach_types.defs names it; a port on the wire) */
typedef mach_port_t	mach_vm_map_t;

/* <mach/mach_voucher_types.h> */
typedef uint32_t	mach_voucher_selector_t;
typedef uint32_t	mach_voucher_attr_key_t;
typedef uint64_t	mach_voucher_attr_value_handle_t;
typedef uint8_t		*mach_voucher_attr_raw_recipe_array_t;
typedef mach_port_t	mach_voucher_attr_manager_t;
typedef mach_port_t	ipc_voucher_attr_control_t;

/* <mach/mach_types.h> UNDServer */
typedef mach_port_t	UNDServerRef;

/* <mach_debug/zone_info.h>, <mach_debug/hash_info.h> */
typedef struct mach_zone_name {
	char		mzn_name[80];
} mach_zone_name_t;
typedef mach_zone_name_t	*mach_zone_name_array_t;

typedef struct mach_zone_info_data {
	uint64_t	mzi_count;
	uint64_t	mzi_cur_size;
	uint64_t	mzi_max_size;
	uint64_t	mzi_elem_size;
	uint64_t	mzi_alloc_size;
	uint64_t	mzi_sum_size;
	uint64_t	mzi_exhaustible;
	uint64_t	mzi_collectable;
} mach_zone_info_t;
typedef mach_zone_info_t	*mach_zone_info_array_t;

typedef struct task_zone_info_data {
	uint64_t	tzi_count;
	uint64_t	tzi_cur_size;
	uint64_t	tzi_max_size;
	uint64_t	tzi_elem_size;
	uint64_t	tzi_alloc_size;
	uint64_t	tzi_sum_size;
	uint64_t	tzi_exhaustible;
	uint64_t	tzi_collectable;
	uint64_t	tzi_caller_acct;
	uint64_t	tzi_task_alloc;
	uint64_t	tzi_task_free;
} task_zone_info_t;
typedef task_zone_info_t	*task_zone_info_array_t;

typedef struct hash_info_bucket {
	natural_t	hib_count;
} hash_info_bucket_t;
typedef hash_info_bucket_t	*hash_info_bucket_array_t;

#endif /* !_LIBMACH_MIG_CLIENT_TYPES_H_ */
