/*
 * mach/host_info.h — host_info() / host_statistics() flavor codes and
 * the result structures.
 *
 * host_info() and host_statistics() are MIG RPCs to the kernel
 * (mach_host 200 / 216, nextbsd-userland#93), so every layout and count
 * here must match the kernel's sys/mach/host_info.h and
 * sys/mach/vm_statistics.h, which assert them (mach_host.c):
 * host_basic_info is Apple's 48-byte layout (HOST_BASIC_INFO_COUNT 12),
 * HOST_SCHED_INFO_COUNT 2, HOST_PRIORITY_INFO_COUNT 8, and struct
 * vm_statistics is Apple's 15-field one (HOST_VM_INFO_COUNT 15).
 * launchd-842's memory-pressure sampler (runtime.c) reads HOST_VM_INFO.
 */
#ifndef _MACH_HOST_INFO_H_
#define _MACH_HOST_INFO_H_

#include <mach/mach_types.h>	/* host_t, natural_t, integer_t */
#include <mach/message.h>	/* mach_msg_type_number_t */
#include <mach/machine.h>	/* cpu_type_t, cpu_subtype_t, cpu_threadtype_t */
#include <stdint.h>		/* uint64_t */

typedef integer_t	*host_info_t;
typedef integer_t	*host_info64_t;
typedef natural_t	host_flavor_t;

#define KERNEL_VERSION_MAX	(512)
typedef char	kernel_version_t[KERNEL_VERSION_MAX];

#define KERNEL_BOOT_INFO_MAX	(4096)
typedef char	kernel_boot_info_t[KERNEL_BOOT_INFO_MAX];

/*
 * host_info() flavors (Apple-canonical values).  The kernel implements
 * HOST_BASIC_INFO, HOST_SCHED_INFO and HOST_PRIORITY_INFO; any other
 * flavor is KERN_INVALID_ARGUMENT.
 */
#define HOST_BASIC_INFO		1	/* basic info */
#define HOST_SCHED_INFO		3	/* scheduling info */
#define HOST_RESOURCE_SIZES	4	/* kernel struct sizes */
#define HOST_PRIORITY_INFO	5	/* priority information */

/*
 * host_statistics() flavors (Apple-canonical values; a separate number
 * space from host_info()'s).  The kernel implements HOST_VM_INFO.
 */
#define HOST_LOAD_INFO		1	/* system loading stats */
#define HOST_VM_INFO		2	/* virtual memory stats */
#define HOST_CPU_LOAD_INFO	3	/* CPU load stats */

struct host_basic_info {
	integer_t	max_cpus;		/* max number of CPUs possible */
	integer_t	avail_cpus;		/* number of CPUs now available */
	natural_t	memory_size;		/* bytes, clamped to 2 GB */
	cpu_type_t	cpu_type;		/* cpu type */
	cpu_subtype_t	cpu_subtype;		/* cpu subtype */
	cpu_threadtype_t cpu_threadtype;	/* cpu threadtype */
	integer_t	physical_cpu;		/* physical CPUs now available */
	integer_t	physical_cpu_max;	/* max physical CPUs possible */
	integer_t	logical_cpu;		/* logical CPUs now available */
	integer_t	logical_cpu_max;	/* max logical CPUs possible */
	uint64_t	max_mem;		/* actual size of physical memory */
};
typedef struct host_basic_info	host_basic_info_data_t;
typedef struct host_basic_info	*host_basic_info_t;
#define HOST_BASIC_INFO_COUNT ((mach_msg_type_number_t) \
	(sizeof(host_basic_info_data_t) / sizeof(integer_t)))
#define HOST_BASIC_INFO_OLD_COUNT ((mach_msg_type_number_t)5)

struct host_sched_info {
	integer_t	min_timeout;	/* minimum timeout in milliseconds */
	integer_t	min_quantum;	/* minimum quantum in milliseconds */
};
typedef struct host_sched_info	host_sched_info_data_t;
typedef struct host_sched_info	*host_sched_info_t;
#define HOST_SCHED_INFO_COUNT ((mach_msg_type_number_t) \
	(sizeof(host_sched_info_data_t) / sizeof(integer_t)))

struct host_priority_info {
	integer_t	kernel_priority;
	integer_t	system_priority;
	integer_t	server_priority;
	integer_t	user_priority;
	integer_t	depress_priority;
	integer_t	idle_priority;
	integer_t	minimum_priority;
	integer_t	maximum_priority;
};
typedef struct host_priority_info	host_priority_info_data_t;
typedef struct host_priority_info	*host_priority_info_t;
#define HOST_PRIORITY_INFO_COUNT ((mach_msg_type_number_t) \
	(sizeof(host_priority_info_data_t) / sizeof(integer_t)))

struct vm_statistics {
	natural_t	free_count;
	natural_t	active_count;
	natural_t	inactive_count;
	natural_t	wire_count;
	natural_t	zero_fill_count;
	natural_t	reactivations;
	natural_t	pageins;
	natural_t	pageouts;
	natural_t	faults;
	natural_t	cow_faults;
	natural_t	lookups;
	natural_t	hits;
	natural_t	purgeable_count;
	natural_t	purges;
	natural_t	speculative_count;
};
typedef struct vm_statistics	*vm_statistics_t;
typedef struct vm_statistics	vm_statistics_data_t;

#define HOST_VM_INFO_COUNT \
	((mach_msg_type_number_t)(sizeof(vm_statistics_data_t) / sizeof(integer_t)))
/* Apple's revisions of the HOST_VM_INFO count; the kernel honours all three. */
#define HOST_VM_INFO_REV0_COUNT	((mach_msg_type_number_t)12)	/* to hits */
#define HOST_VM_INFO_REV1_COUNT	((mach_msg_type_number_t)14)	/* + purge */
#define HOST_VM_INFO_REV2_COUNT	HOST_VM_INFO_COUNT		/* + spec. */

#endif /* !_MACH_HOST_INFO_H_ */
