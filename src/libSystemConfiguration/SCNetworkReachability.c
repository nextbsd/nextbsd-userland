/*
 * Copyright (c) 2003-2013 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

/*
 * Modification History
 *
 * April 12, 2011		Allan Nathanson <ajn@apple.com>
 * - add SCNetworkReachability "server"
 *
 * March 31, 2004		Allan Nathanson <ajn@apple.com>
 * - use [SC] DNS configuration information
 *
 * January 19, 2003		Allan Nathanson <ajn@apple.com>
 * - add advanced reachability APIs
 */

/*
 * NextBSD: SCNetworkReachability from configd-596.15, without the
 * reachability server (HAVE_REACHABILITY_SERVER), on-demand, or VPN/PPP/IPSec,
 * which have nothing to report on here.
 *
 * Route lookups use PF_ROUTE RTM_GET on the socket fallback path (there is no
 * RTM_GET_SILENT and no RTF_IFSCOPE). Resolver state comes from dnsinfo
 * (dns_configuration_copy) and reachability flags from nwi (nwi_state_copy);
 * changes arrive on the notify keys Apple uses. Apple's getaddrinfo_async_start
 * is Libinfo-only, and mDNSPosix reads /etc/resolv.conf only at start, so an
 * asynchronous lookup is getaddrinfo() on a dispatch queue, as Darwin's fallback
 * path would give, rather than DNSServiceGetAddrInfo.
 *
 * Kept from the PTR-only port: kSCNetworkReachabilityOptionPTRAddress targets
 * are a reverse lookup over libdns_sd for hostnamed's set-hostname.c. Those
 * use the reachabilityTypePTR code near the end of this file.
 */

#include "SCInternal.h"		/* first: config_types.h before SCDynamicStore.h */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFRuntime.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCPrivate.h>
#include <dispatch/dispatch.h>
#include <pthread.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/route.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <resolv.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <notify.h>
#include <dnsinfo.h>
#include <network_information.h>
#include <dns_sd.h>

#ifndef kSCStatusReachabilityUnknown
#define kSCStatusReachabilityUnknown	4001
#endif

#define	_sc_debug	FALSE
#define	_sc_log		0

static void
_sc_reach_log(int level, CFStringRef format, ...)
{
	va_list		ap;
	char		buf[512];
	CFStringRef	str;

	va_start(ap, format);
	str = CFStringCreateWithFormatAndArguments(NULL, NULL, format, ap);
	va_end(ap);
	if (str == NULL) {
		return;
	}
	if (CFStringGetCString(str, buf, sizeof(buf), kCFStringEncodingUTF8)) {
		syslog(level, "SCNetworkReachability: %s", buf);
	}
	CFRelease(str);
	return;
}

#define	SCLog(cond, level, ...)	do {					\
	if (cond) _sc_reach_log((level), __VA_ARGS__);			\
} while (0)

/* NextBSD: the isA_CF* helpers of Apple's SCValidation.h. */
static __inline__ CFTypeRef
_isA_CFType(CFTypeRef obj, CFTypeID type)
{
	return (((obj != NULL) && (CFGetTypeID(obj) == type)) ? obj : NULL);
}
#define	isA_CFArray(obj)	((CFArrayRef)_isA_CFType((obj), CFArrayGetTypeID()))
#define	isA_CFBoolean(obj)	((CFBooleanRef)_isA_CFType((obj), CFBooleanGetTypeID()))
#define	isA_CFData(obj)		((CFDataRef)_isA_CFType((obj), CFDataGetTypeID()))
#define	isA_CFDictionary(obj)	((CFDictionaryRef)_isA_CFType((obj), CFDictionaryGetTypeID()))
#define	isA_CFString(obj)	((CFStringRef)_isA_CFType((obj), CFStringGetTypeID()))

static __inline__ Boolean
_SC_CFEqual(CFTypeRef cf1, CFTypeRef cf2)
{
	if (cf1 == cf2)			return TRUE;
	if ((cf1 == NULL) || (cf2 == NULL))	return FALSE;
	return CFEqual(cf1, cf2);
}

/*
 * NextBSD: Apple's run-loop list helpers (SCDPlugin): rlList holds
 * {obj, runLoop, runLoopMode} triples.
 */
static Boolean
_SC_isScheduled(CFTypeRef obj, CFRunLoopRef runLoop, CFStringRef runLoopMode, CFMutableArrayRef rlList)
{
	CFIndex	i;
	CFIndex	n	= CFArrayGetCount(rlList);

	for (i = 0; i < n; i += 3) {
		if ((obj != NULL)         && !CFEqual(obj,         CFArrayGetValueAtIndex(rlList, i))) {
			continue;
		}
		if ((runLoop != NULL)     && !CFEqual(runLoop,     CFArrayGetValueAtIndex(rlList, i+1))) {
			continue;
		}
		if ((runLoopMode != NULL) && !CFEqual(runLoopMode, CFArrayGetValueAtIndex(rlList, i+2))) {
			continue;
		}
		return TRUE;
	}

	return FALSE;
}

static void
_SC_schedule(CFTypeRef obj, CFRunLoopRef runLoop, CFStringRef runLoopMode, CFMutableArrayRef rlList)
{
	CFArrayAppendValue(rlList, obj);
	CFArrayAppendValue(rlList, runLoop);
	CFArrayAppendValue(rlList, runLoopMode);

	return;
}

static Boolean
_SC_unschedule(CFTypeRef obj, CFRunLoopRef runLoop, CFStringRef runLoopMode, CFMutableArrayRef rlList, Boolean all)
{
	CFIndex	i	= 0;
	Boolean	found	= FALSE;
	CFIndex	n	= CFArrayGetCount(rlList);

	while (i < n) {
		if ((obj != NULL)         && !CFEqual(obj,         CFArrayGetValueAtIndex(rlList, i))) {
			i += 3;
			continue;
		}
		if ((runLoop != NULL)     && !CFEqual(runLoop,     CFArrayGetValueAtIndex(rlList, i+1))) {
			i += 3;
			continue;
		}
		if ((runLoopMode != NULL) && !CFEqual(runLoopMode, CFArrayGetValueAtIndex(rlList, i+2))) {
			i += 3;
			continue;
		}

		found = TRUE;

		CFArrayRemoveValueAtIndex(rlList, i + 2);
		CFArrayRemoveValueAtIndex(rlList, i + 1);
		CFArrayRemoveValueAtIndex(rlList, i);

		if (!all) {
			return found;
		}

		n -= 3;
	}

	return found;
}

static void
_SC_signalRunLoop(CFTypeRef obj, CFRunLoopSourceRef rls, CFArrayRef rlList)
{
	CFIndex	i;
	CFIndex	n	= CFArrayGetCount(rlList);

	(void)rls;
	for (i = 0; i < n; i += 3) {
		if (CFEqual(obj, CFArrayGetValueAtIndex(rlList, i))) {
			CFRunLoopWakeUp((CFRunLoopRef)CFArrayGetValueAtIndex(rlList, i+1));
		}
	}

	return;
}


#pragma mark -
#pragma mark SCNetworkReachability


#define kSCNetworkReachabilityFlagsFirstResolvePending	(1<<31)

#define kSCNetworkReachabilityFlagsMask			0x00ffffff	// top 8-bits reserved for implementation


typedef enum {
	reachabilityTypeAddress,
	reachabilityTypeAddressPair,
	reachabilityTypeName,
	reachabilityTypePTR		// NextBSD: hostnamed's reverse lookup
} ReachabilityAddressType;


typedef struct {
	uint64_t			cycle;
	SCNetworkReachabilityFlags	flags;
	unsigned int			if_index;
	char				if_name[IFNAMSIZ];
	Boolean				sleeping;
} ReachabilityInfo;


typedef struct __SCNetworkReachability {

	/* base CFType information */
	CFRuntimeBase			cfBase;

	/* lock */
	pthread_mutex_t			lock;

	/* address type */
	ReachabilityAddressType		type;

	/* target host name */
	const char			*name;
	Boolean				needResolve;
	CFArrayRef			resolvedAddresses;	/* CFArray[CFData] */
	int				resolvedError;
	SCNetworkReachabilityFlags	resolverFlags;

	/* [scoped routing] interface constraints */
	unsigned int			if_index;
	char				if_name[IFNAMSIZ];

	/* local & remote addresses */
	struct sockaddr			*localAddress;
	struct sockaddr			*remoteAddress;

	/* current reachability flags */
	uint64_t			cycle;
	ReachabilityInfo		info;
	ReachabilityInfo		last_notify;

	/* run loop source, callout, context, rl scheduling info */
	Boolean				scheduled;
	CFRunLoopSourceRef		rls;
	SCNetworkReachabilityCallBack	rlsFunction;
	SCNetworkReachabilityContext	rlsContext;
	CFMutableArrayRef		rlList;

	dispatch_group_t		dispatchGroup;
	dispatch_queue_t		dispatchQueue;		// SCNetworkReachabilitySetDispatchQueue

	/* [async] DNS query info */
	Boolean				haveDNS;
	Boolean				dnsActive;		// if getaddrinfo() is running
	uint64_t			dnsGeneration;		// NextBSD: discards a cancelled result
	Boolean				dnsBlocked;		// if DNS query blocked

	Boolean				resolverBypass;		// set this flag to bypass resolving the name

	/* logging */
	char				log_prefix[32];

	/* NextBSD: reachabilityTypePTR over libdns_sd */
	CFDataRef			ptrAddress;
	dispatch_queue_t		ptrQueue;
	dispatch_source_t		ptrSource;
	DNSServiceRef			ptrRef;
	CFMutableArrayRef		ptrResolved;		/* CFArray[CFString] */
	SCNetworkReachabilityFlags	ptrFlags;
	int				ptrError;

} SCNetworkReachabilityPrivate, *SCNetworkReachabilityPrivateRef;


const CFStringRef kSCNetworkReachabilityOptionPTRAddress = CFSTR("PTRAddress");


#define	DEBUG_REACHABILITY_TYPE_NAME			"create w/name"
#define	DEBUG_REACHABILITY_TYPE_NAME_OPTIONS		"    + options"

#define	DEBUG_REACHABILITY_TYPE_ADDRESS		"create w/address"
#define	DEBUG_REACHABILITY_TYPE_ADDRESS_OPTIONS	"       + options"

#define	DEBUG_REACHABILITY_TYPE_ADDRESSPAIR		"create w/address pair"
#define	DEBUG_REACHABILITY_TYPE_ADDRESSPAIR_OPTIONS	"            + options"

static pthread_mutexattr_t	lock_attr;

#define MUTEX_INIT(m) {							\
	int _lock_ = (pthread_mutex_init(m, &lock_attr) == 0);		\
	assert(_lock_);							\
}

#define	MUTEX_LOCK(m) {							\
	int _lock_ = (pthread_mutex_lock(m) == 0);			\
	assert(_lock_);							\
}

#define	MUTEX_UNLOCK(m) {						\
	int _unlock_ = (pthread_mutex_unlock(m) == 0);			\
	assert(_unlock_);						\
}

#define	MUTEX_ASSERT_HELD(m) {						\
	int _locked_ = (pthread_mutex_lock(m) == EDEADLK);		\
	assert(_locked_);						\
}


#define SCNETWORKREACHABILITY_TRIGGER_KEY	CFSTR("org.nextbsd.SCNetworkReachability:FORCE-CHANGE")


#define	N_QUICK	64


static CFStringRef	__SCNetworkReachabilityCopyDescription	(CFTypeRef cf);
static void		__SCNetworkReachabilityDeallocate	(CFTypeRef cf);
static void		reachPerform				(void *info);
static void		ptrQueryStop				(SCNetworkReachabilityPrivateRef targetPrivate);
static Boolean		ptrQueryStart				(SCNetworkReachabilityPrivateRef targetPrivate,
								 dispatch_queue_t		queue);


static Boolean
__SCNetworkReachabilityScheduleWithRunLoop	(SCNetworkReachabilityRef	target,
						 CFRunLoopRef			runLoop,
						 CFStringRef			runLoopMode,
						 dispatch_queue_t		queue);

static Boolean
__SCNetworkReachabilityUnscheduleFromRunLoop	(SCNetworkReachabilityRef	target,
						 CFRunLoopRef			runLoop,
						 CFStringRef			runLoopMode);


static CFTypeID __kSCNetworkReachabilityTypeID	= _kCFRuntimeNotATypeID;


static const CFRuntimeClass __SCNetworkReachabilityClass = {
	0,					// version
	"SCNetworkReachability",		// className
	NULL,					// init
	NULL,					// copy
	__SCNetworkReachabilityDeallocate,	// dealloc
	NULL,					// equal
	NULL,					// hash
	NULL,					// copyFormattingDesc
	__SCNetworkReachabilityCopyDescription	// copyDebugDesc
};


static pthread_once_t		initialized	= PTHREAD_ONCE_INIT;
static const ReachabilityInfo	NOT_REACHABLE	= { 0, 0,		0, { 0 }, FALSE };
static const ReachabilityInfo	NOT_REPORTED	= { 0, 0xFFFFFFFF,	0, { 0 }, FALSE };
static int			rtm_seq		= 0;


static const struct addrinfo	HINTS_DEFAULT	= {
#ifdef	AI_PARALLEL
	.ai_flags	= AI_ADDRCONFIG | AI_PARALLEL,
#else	// AI_PARALLEL
	.ai_flags	= AI_ADDRCONFIG,
#endif	// AI_PARALLEL
};


/*
 * host "something has changed" notifications
 */

// Note: protected by _hn_target_queue()
static SCDynamicStoreRef	hn_store	= NULL;
static CFMutableSetRef		hn_targets	= NULL;


static dispatch_queue_t
_hn_changes_queue()
{
	static dispatch_once_t	once;
	static dispatch_queue_t	q = NULL;

	dispatch_once(&once, ^{
		q = dispatch_queue_create("SCNetworkReachabilty.handleChanges", NULL);
	});

	return q;
}


static dispatch_queue_t
_hn_target_queue()
{
	static dispatch_once_t	once;
	static dispatch_queue_t	q;

	dispatch_once(&once, ^{
		q = dispatch_queue_create("SCNetworkReachabilty.targetManagement", NULL);
	});

	return q;
}


/*
 * DNS configuration
 */

typedef struct {
	dns_config_t	*config;
	int		refs;
} dns_configuration_t;


// Note: protected by "dns_lock"
static pthread_mutex_t		dns_lock		= PTHREAD_MUTEX_INITIALIZER;
static dns_configuration_t	*dns_configuration	= NULL;
static int			dns_token;
static Boolean			dns_token_valid		= FALSE;


static __inline__ Boolean
__reach_changed(ReachabilityInfo *r1, ReachabilityInfo *r2)
{
	if (r1->flags != r2->flags) {
		// if the reachability flags changed
		return TRUE;
	}

	if (r1->if_index != r2->if_index) {
		// if the target interface changed
		return TRUE;
	}

	if ((r1->sleeping != r2->sleeping) && !r2->sleeping) {
		// if our sleep/wake status changed and if we
		// are no longer sleeping
		return TRUE;
	}

	return FALSE;
}


static __inline__ void
_reach_set(ReachabilityInfo		*dst,
	   const ReachabilityInfo	*src,
	   uint64_t			cycle,
	   unsigned int			requested_if_index,
	   const char			*requested_if_name)
{
	memcpy(dst, src, sizeof(ReachabilityInfo));
	dst->cycle = cycle;

	if (!(dst->flags & kSCNetworkReachabilityFlagsReachable) ||
		(dst->flags & kSCNetworkReachabilityFlagsConnectionRequired)) {
		// if not reachable or connection required, return the
		// requested if_index and if_name.
		dst->if_index = requested_if_index;
		if (requested_if_name != NULL) {
			strlcpy(dst->if_name, requested_if_name, sizeof(dst->if_name));
		} else {
			dst->if_name[0] = '\0';
		}
	}

	return;
}


#pragma mark -
#pragma mark Reachability engine


/* NextBSD: the routing socket rounds up to sizeof(long); Apple's to uint32_t. */
#define NEXT_SA(ap) (ap) = (struct sockaddr *) ((caddr_t)(ap) + SA_SIZE(ap))

static void
get_rtaddrs(int addrs, struct sockaddr *sa, struct sockaddr **rti_info)
{
	int             i;

	for (i = 0; i < RTAX_MAX; i++) {
		if (addrs & (1 << i)) {
			rti_info[i] = sa;
			NEXT_SA(sa);
		} else
			rti_info[i] = NULL;
	}
}


#define BUFLEN (sizeof(struct rt_msghdr) + 512)	/* 8 * sizeof(struct sockaddr_in6) = 192 */


typedef struct {
	union {
		char			bytes[BUFLEN];
		struct rt_msghdr	rtm;
	} buf;
	int			error;
	struct sockaddr         *rti_info[RTAX_MAX];
	struct rt_msghdr	*rtm;
	struct sockaddr_dl	*sdl;
} route_info, *route_info_p;

/*
 * route_get()
 *	returns	zero if route exists and data returned, EHOSTUNREACH
 *	if no route, or errno for any other error.
 */
static int
route_get(const struct sockaddr	*address,
	  unsigned int		if_index,
	  route_info		*info)
{
	int			n;
	pid_t			pid		= getpid();
	int			rsock;
	struct sockaddr         *sa;
	int32_t			seq		= __sync_add_and_fetch(&rtm_seq, 1);
#ifndef	RTM_GET_SILENT
	static pthread_mutex_t	lock		= PTHREAD_MUTEX_INITIALIZER;
	int			sosize		= 48 * 1024;
#endif

	bzero(info, sizeof(*info));

	info->rtm = &info->buf.rtm;
	info->rtm->rtm_msglen  = sizeof(struct rt_msghdr);
	info->rtm->rtm_version = RTM_VERSION;
#ifdef	RTM_GET_SILENT
	info->rtm->rtm_type    = RTM_GET_SILENT;
#else
	info->rtm->rtm_type    = RTM_GET;
#endif
	info->rtm->rtm_flags   = RTF_STATIC|RTF_UP|RTF_HOST|RTF_GATEWAY;
	info->rtm->rtm_addrs   = RTA_DST|RTA_IFP; /* Both destination and device */
	info->rtm->rtm_pid     = pid;
	info->rtm->rtm_seq     = seq;

	sa  = (struct sockaddr *) (info->rtm + 1);
	bcopy(address, sa, address->sa_len);
	n = SA_SIZE(sa);
	info->rtm->rtm_msglen += n;

	info->sdl = (struct sockaddr_dl *) ((void *)sa + n);
	info->sdl->sdl_family = AF_LINK;
	info->sdl->sdl_len = sizeof (struct sockaddr_dl);
	n = SA_SIZE(info->sdl);
	info->rtm->rtm_msglen += n;

#ifndef	RTM_GET_SILENT
	pthread_mutex_lock(&lock);
#endif
	rsock = socket(PF_ROUTE, SOCK_RAW, PF_ROUTE);
	if (rsock == -1) {
		int	error	= errno;

#ifndef	RTM_GET_SILENT
		pthread_mutex_unlock(&lock);
#endif
		SCLog(TRUE, LOG_ERR, CFSTR("socket(PF_ROUTE) failed: %s"), strerror(error));
		return error;
	}
	/*
	 * NextBSD: blocking, with a timeout. The routing socket reply is queued
	 * to the netisr thread here, so a non-blocking read() can run first.
	 */
	{
		struct timeval	tv	= { 1, 0 };

		if (setsockopt(rsock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
			int	error	= errno;

			(void)close(rsock);
#ifndef	RTM_GET_SILENT
			pthread_mutex_unlock(&lock);
#endif
			SCLog(TRUE, LOG_ERR, CFSTR("setsockopt(SO_RCVTIMEO) failed: %s"), strerror(error));
			return error;
		}
	}

#ifndef	RTM_GET_SILENT
	if (setsockopt(rsock, SOL_SOCKET, SO_RCVBUF, &sosize, sizeof(sosize)) == -1) {
		int	error	= errno;

		(void)close(rsock);
		pthread_mutex_unlock(&lock);
		SCLog(TRUE, LOG_ERR, CFSTR("setsockopt(SO_RCVBUF) failed: %s"), strerror(error));
		return error;
	}
#endif

	if (write(rsock, &info->buf, info->rtm->rtm_msglen) == -1) {
		int	error	= errno;

		(void)close(rsock);
#ifndef	RTM_GET_SILENT
		pthread_mutex_unlock(&lock);
#endif
		if (error != ESRCH) {
			SCLog(TRUE, LOG_ERR, CFSTR("write() failed: %s"), strerror(error));
			return error;
		}
		return EHOSTUNREACH;
	}

	/*
	 * Type, seq, pid identify our response.
	 * Routing sockets are broadcasters on input.
	 */
	while (TRUE) {
		int	n;

		n = read(rsock, &info->buf, sizeof(info->buf));
		if (n == -1) {
			int	error	= errno;

			if (error == EINTR) {
				continue;
			}
			(void)close(rsock);
#ifndef	RTM_GET_SILENT
			pthread_mutex_unlock(&lock);
#endif
			SCLog(TRUE, LOG_ERR,
			      CFSTR("SCNetworkReachability: routing socket"
				    " read() failed: %s"), strerror(error));
			return error;
		}
		if ((info->rtm->rtm_type == RTM_GET) 	&&
		    (info->rtm->rtm_seq == seq) 	&&
		    (info->rtm->rtm_pid == pid)) {
		    break;
		}
	}

	(void)close(rsock);
#ifndef	RTM_GET_SILENT
	pthread_mutex_unlock(&lock);
#endif

	get_rtaddrs(info->rtm->rtm_addrs, sa, info->rti_info);

//#define LOG_RTADDRS
#ifdef	LOG_RTADDRS
	{
		int	i;

		SCLog(_sc_debug, LOG_DEBUG, CFSTR("rtm_flags = 0x%8.8x"), info->rtm->rtm_flags);

		if ((info->rti_info[RTAX_NETMASK] != NULL) && (info->rti_info[RTAX_DST] != NULL)) {
			info->rti_info[RTAX_NETMASK]->sa_family = info->rti_info[RTAX_DST]->sa_family;
		}

		for (i = 0; i < RTAX_MAX; i++) {
			if (info->rti_info[i] != NULL) {
				char	addr[128];

				_SC_sockaddr_to_string(info->rti_info[i], addr, sizeof(addr));
				SCLog(_sc_debug, LOG_DEBUG, CFSTR("%d: %s"), i, addr);
			}
		}
	}
#endif	// LOG_RTADDRS

	if ((info->rti_info[RTAX_IFP] == NULL) ||
	    (info->rti_info[RTAX_IFP]->sa_family != AF_LINK)) {
		/* no interface info */
		SCLog(TRUE, LOG_DEBUG, CFSTR("route_get() no interface info"));
		return EINVAL;
	}

	/* ALIGN: accessors are retrieving byte values, cast ok. */
	info->sdl = (struct sockaddr_dl *)(void *) info->rti_info[RTAX_IFP];
	if ((info->sdl->sdl_nlen == 0) || (info->sdl->sdl_nlen > IFNAMSIZ)) {
		/* no interface name */
		return EHOSTUNREACH;
	}

	/*
	 * NextBSD: no scoped routes (RTF_IFSCOPE). A scoped request is reachable
	 * this way only if the route leaves through the requested interface;
	 * checkAddress_with_nwi() handles the other scoped case.
	 */
	if ((if_index != 0) && (info->sdl->sdl_index != if_index)) {
		return EHOSTUNREACH;
	}

	return 0;
}

static int
checkAddressRoute(const struct sockaddr	*address,
		  unsigned int		if_index,
		  char			*if_name,
		  struct ifreq		*ifr,
		  ReachabilityInfo	*reach_info,
		  route_info		*info,
		  int			*sc_status,
		  const char		*log_prefix)
{
	int			isock		= -1;
	int			ret		= 0;
	char			*statusMessage	= NULL;
	struct sockaddr_in	v4mapped;

	switch (address->sa_family) {
		case AF_INET :
		case AF_INET6 :
			if (_sc_debug) {
				char	addr[128];
				char	if_name[IFNAMSIZ + 1];

				_SC_sockaddr_to_string(address, addr, sizeof(addr));

				if ((if_index != 0) &&
				    (if_indextoname(if_index, &if_name[1]) != NULL)) {
					if_name[0] = '%';
				} else {
					if_name[0] = '\0';
				}

				SCLog(TRUE, LOG_INFO, CFSTR("%scheckAddress(%s%s)"),
				      log_prefix,
				      addr,
				      if_name);
			}
			break;
		default :
			/*
			 * if no code for this address family (yet)
			 */
			SCLog(TRUE, LOG_INFO,
			      CFSTR("checkAddress(): unexpected address family %d"),
			      address->sa_family);
			*sc_status = kSCStatusInvalidArgument;
			ret = EPERM;
			goto done;
	}

	if (address->sa_family == AF_INET6) {
		/* ALIGN: sin6_addr accessed aligned, cast ok. */
		struct sockaddr_in6	*sin6	= (struct sockaddr_in6 *)(void *)address;

		if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) {
			bzero(&v4mapped, sizeof(v4mapped));
			v4mapped.sin_len         = sizeof(v4mapped);
			v4mapped.sin_family      = AF_INET;
			v4mapped.sin_port        = sin6->sin6_port;
			v4mapped.sin_addr.s_addr = sin6->sin6_addr.__u6_addr.__u6_addr32[3];
			address = (struct sockaddr *)&v4mapped;
		}
	}

	ret = route_get(address, if_index, info);
	switch (ret) {
		case 0 :
			break;
		case EHOSTUNREACH :
			// if no route
			goto done;
		default :
			// if error
			*sc_status = ret;
			goto done;
	}

	/* get the interface flags */

	isock = socket(AF_INET, SOCK_DGRAM, 0);
	if (isock == -1) {
		ret = errno;
		SCLog(TRUE, LOG_ERR, CFSTR("socket() failed: %s"), strerror(errno));
		goto done;
	}

	bzero(ifr, sizeof(*ifr));
	bcopy(info->sdl->sdl_data, ifr->ifr_name, info->sdl->sdl_nlen);

	if (ioctl(isock, SIOCGIFFLAGS, (char *)ifr) == -1) {
		ret = errno;
		SCLog(TRUE, LOG_ERR, CFSTR("ioctl(SIOCGIFFLAGS) failed: %s"), strerror(errno));
		goto done;
	}

	if (!(ifr->ifr_flags & IFF_UP)) {
		ret = EHOSTUNREACH;
		goto done;
	}

	statusMessage = "isReachable";
	reach_info->flags |= kSCNetworkReachabilityFlagsReachable;

	if (info->rtm->rtm_flags & RTF_LOCAL) {
		statusMessage = "isReachable (is a local address)";
		reach_info->flags |= kSCNetworkReachabilityFlagsIsLocalAddress;
	} else if (ifr->ifr_flags & IFF_LOOPBACK) {
		statusMessage = "isReachable (is loopback network)";
		reach_info->flags |= kSCNetworkReachabilityFlagsIsLocalAddress;
	} else if ((info->rti_info[RTAX_IFA] != NULL) &&
		   (info->rti_info[RTAX_IFA]->sa_family != AF_LINK)) {
		void	*addr1	= (void *)address;
		void	*addr2	= (void *)info->rti_info[RTAX_IFA];
		size_t	len	= address->sa_len;

		if ((address->sa_family != info->rti_info[RTAX_IFA]->sa_family) &&
		    (address->sa_len    != info->rti_info[RTAX_IFA]->sa_len)) {
			SCLog(TRUE, LOG_NOTICE,
			      CFSTR("address family/length mismatch: %d/%d != %d/%d"),
			      address->sa_family,
			      address->sa_len,
			      info->rti_info[RTAX_IFA]->sa_family,
			      info->rti_info[RTAX_IFA]->sa_len);
			goto done;
		}

		switch (address->sa_family) {
			case AF_INET :
				/* ALIGN: cast ok, because only bcmp is used. */
				addr1 = &((struct sockaddr_in *)(void *)address)->sin_addr;
				addr2 = &((struct sockaddr_in *)(void *)info->rti_info[RTAX_IFA])->sin_addr;
				len = sizeof(struct in_addr);

				/*
				 * check if 0.0.0.0
				 */
				/* ALIGN: sin_addr should be aligned, cast ok. */
				if (((struct sockaddr_in *)(void *)address)->sin_addr.s_addr == 0) {
					statusMessage = "isReachable (this host)";
					reach_info->flags |= kSCNetworkReachabilityFlagsIsLocalAddress;
				}
				break;
			case AF_INET6 :
				/* ALIGN: cast ok, because only bcmp is used. */
				addr1 = &((struct sockaddr_in6 *)(void *)address)->sin6_addr;
				addr2 = &((struct sockaddr_in6 *)(void *)info->rti_info[RTAX_IFA])->sin6_addr;
				len = sizeof(struct in6_addr);
				break;
			default :
				break;
		}

		if (bcmp(addr1, addr2, len) == 0) {
			statusMessage = "isReachable (is interface address)";
			reach_info->flags |= kSCNetworkReachabilityFlagsIsLocalAddress;
		}
	}

	if (!(info->rtm->rtm_flags & RTF_GATEWAY) &&
	    (info->rti_info[RTAX_GATEWAY] != NULL) &&
	    (info->rti_info[RTAX_GATEWAY]->sa_family == AF_LINK) &&
	    !(ifr->ifr_flags & IFF_POINTOPOINT)) {
		reach_info->flags |= kSCNetworkReachabilityFlagsIsDirect;
	}

	bzero(if_name, IFNAMSIZ);
	bcopy(info->sdl->sdl_data,
	      if_name,
	      (info->sdl->sdl_nlen <= IFNAMSIZ) ? info->sdl->sdl_nlen : IFNAMSIZ);

	strlcpy(reach_info->if_name, if_name, sizeof(reach_info->if_name));
	reach_info->if_index = info->sdl->sdl_index;

	if (_sc_debug) {
		SCLog(TRUE, LOG_INFO, CFSTR("%s  status    = %s"), log_prefix, statusMessage);
		SCLog(TRUE, LOG_INFO, CFSTR("%s  device    = %s (%hu)"), log_prefix, if_name, info->sdl->sdl_index);
		SCLog(TRUE, LOG_INFO, CFSTR("%s  sdl_type  = 0x%x"), log_prefix, info->sdl->sdl_type);
		SCLog(TRUE, LOG_INFO, CFSTR("%s  ifr_flags = 0x%04hx"), log_prefix, ifr->ifr_flags);
		SCLog(TRUE, LOG_INFO, CFSTR("%s  rtm_flags = 0x%08x"), log_prefix, info->rtm->rtm_flags);
	}
done :
	if (isock != -1) (void)close(isock);
	return ret;
}

static Boolean
checkAddress_with_nwi(const struct sockaddr     *address,
		      unsigned int		if_index,
		      ReachabilityInfo		*reach_info,
		      const char		*log_prefix)
{
	route_info		info;
	struct ifreq		ifr;
	char			if_name[IFNAMSIZ];
	nwi_ifstate_t		ifstate;
	nwi_state_t		nwi_state;
	int			ret;
	int			sc_status	= kSCStatusReachabilityUnknown;

	_reach_set(reach_info, &NOT_REACHABLE, reach_info->cycle, if_index, NULL);

	nwi_state = nwi_state_copy();

	if (address != NULL) {
		ret = checkAddressRoute(address,
					if_index,
					if_name,
					&ifr,
					reach_info,
					&info,
					&sc_status,
					log_prefix);

	}
	else {
		/* special case: check only for available paths off the system */
		ret = EHOSTUNREACH;
	}

	if (ret == 0) {
		sc_status = kSCStatusOK;

		ifstate = nwi_state_get_ifstate(nwi_state, if_name);

		if (ifstate == NULL) {
			goto done;
		}

		reach_info->flags |= nwi_ifstate_get_reachability_flags(ifstate);


	} else if (ret == EHOSTUNREACH) {
		if (if_index == 0) {
			sc_status = kSCStatusOK;
			reach_info->flags |= nwi_state_get_reachability_flags(nwi_state,
									      (address != NULL) ? address->sa_family
									      : AF_UNSPEC);
		} else {
			char		scoped_name[IFNAMSIZ];

			// if scoped request
			sc_status = kSCStatusNoKey;

			/*
			 * NextBSD: with no scoped routes, the route lookup above
			 * only finds the primary interface. nwi lists every
			 * interface with connectivity, so treat one that is
			 * listed as having a route of its own, as a Darwin
			 * scoped route would.
			 */
			if ((address != NULL) &&
			    (if_indextoname(if_index, scoped_name) != NULL)) {
				ifstate = nwi_state_get_ifstate(nwi_state, scoped_name);
				if (ifstate != NULL) {
					sc_status = kSCStatusOK;
					reach_info->flags |= nwi_ifstate_get_reachability_flags(ifstate);
					reach_info->if_index = if_index;
					strlcpy(reach_info->if_name, scoped_name, sizeof(reach_info->if_name));
				}
			}
		}

	}

done:
	if (reach_info->flags == 0) {
		SCLog(_sc_debug, LOG_INFO, CFSTR("%s  cannot be reached"), log_prefix);
	}

	if (nwi_state != NULL) {
		nwi_state_release(nwi_state);
	}

	if ((sc_status != kSCStatusOK) && (sc_status != kSCStatusNoKey)) {
		_SCErrorSet(sc_status);
		return FALSE;
	}

	return TRUE;
}


#pragma mark -
#pragma mark SCNetworkReachability APIs


static __inline__ CFTypeRef
isA_SCNetworkReachability(CFTypeRef obj)
{
	return (_isA_CFType(obj, SCNetworkReachabilityGetTypeID()));
}


CFStringRef
_SCNetworkReachabilityCopyTargetDescription(SCNetworkReachabilityRef target)
{
	CFAllocatorRef			allocator	= CFGetAllocator(target);
	CFMutableStringRef		str;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	str = CFStringCreateMutable(allocator, 0);
	switch (targetPrivate->type) {
		case reachabilityTypeAddress :
		case reachabilityTypeAddressPair : {
			char		buf[64];

			if (targetPrivate->localAddress != NULL) {
				_SC_sockaddr_to_string(targetPrivate->localAddress, buf, sizeof(buf));
				CFStringAppendFormat(str, NULL, CFSTR("local address = %s"),
						     buf);
			}

			if (targetPrivate->remoteAddress != NULL) {
				_SC_sockaddr_to_string(targetPrivate->remoteAddress, buf, sizeof(buf));
				CFStringAppendFormat(str, NULL, CFSTR("%s%saddress = %s"),
						     targetPrivate->localAddress ? ", " : "",
						     (targetPrivate->type == reachabilityTypeAddressPair) ? "remote " : "",
						     buf);
			}
			break;
		}
		case reachabilityTypeName : {
			CFStringAppendFormat(str, NULL, CFSTR("name = %s"), targetPrivate->name);
			break;
		}
		case reachabilityTypePTR : {
			CFStringAppendFormat(str, NULL, CFSTR("PTR"));
			break;
		}
	}

	return str;
}

CFStringRef
_SCNetworkReachabilityCopyTargetFlags(SCNetworkReachabilityRef target)
{
	CFAllocatorRef			allocator	= CFGetAllocator(target);
	CFStringRef			str;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	str = CFStringCreateWithFormat(allocator,
				       NULL,
				       CFSTR("flags = 0x%08x, if_index = %u%s"),
				       targetPrivate->info.flags,
				       targetPrivate->info.if_index,
				       targetPrivate->info.sleeping ? ", z" : "");
	return str;
}

static CFStringRef
__SCNetworkReachabilityCopyDescription(CFTypeRef cf)
{
	CFAllocatorRef			allocator	= CFGetAllocator(cf);
	CFMutableStringRef		result;
	CFStringRef			str;
	SCNetworkReachabilityRef	target		= (SCNetworkReachabilityRef)cf;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	result = CFStringCreateMutable(allocator, 0);
	CFStringAppendFormat(result, NULL, CFSTR("<SCNetworkReachability %p [%p]> {"), cf, allocator);

	// add target description
	str = _SCNetworkReachabilityCopyTargetDescription(target);
	CFStringAppend(result, str);
	CFRelease(str);

	// add additional "name" info
	if (targetPrivate->type == reachabilityTypeName) {
		if (targetPrivate->dnsActive) {
			CFStringAppendFormat(result, NULL, CFSTR(" (DNS query active)"));
		} else if ((targetPrivate->resolvedAddresses != NULL) || (targetPrivate->resolvedError != NETDB_SUCCESS)) {
			if (targetPrivate->resolvedAddresses != NULL) {
				if (isA_CFArray(targetPrivate->resolvedAddresses)) {
					CFIndex	i;
					CFIndex	n	= CFArrayGetCount(targetPrivate->resolvedAddresses);

					CFStringAppendFormat(result, NULL, CFSTR(" ("));
					for (i = 0; i < n; i++) {
						CFDataRef	address;
						char		buf[64];
						struct sockaddr	*sa;

						address	= CFArrayGetValueAtIndex(targetPrivate->resolvedAddresses, i);
						sa      = (struct sockaddr *)CFDataGetBytePtr(address);
						_SC_sockaddr_to_string(sa, buf, sizeof(buf));
						CFStringAppendFormat(result, NULL, CFSTR("%s%s"),
								     i > 0 ? ", " : "",
								     buf);
					}
					CFStringAppendFormat(result, NULL, CFSTR(")"));
				} else if (CFEqual(targetPrivate->resolvedAddresses, kCFNull)) {
					CFStringAppendFormat(result, NULL, CFSTR(" (%s)"),
							     gai_strerror(targetPrivate->resolvedError));
				} else {
					CFStringAppendFormat(result, NULL, CFSTR(" (no addresses)"));
				}
			} else {
				CFStringAppendFormat(result, NULL, CFSTR(" (%s)"),
						     gai_strerror(targetPrivate->resolvedError));
			}
		}
	}

	if (targetPrivate->resolverBypass) {
		CFStringAppendFormat(result, NULL, CFSTR(", !resolve"));
	}


	// add flags
	if (targetPrivate->scheduled) {
		str = _SCNetworkReachabilityCopyTargetFlags(target);
		CFStringAppendFormat(result, NULL, CFSTR(", %@"), str);
		CFRelease(str);
	}

	CFStringAppendFormat(result, NULL, CFSTR("}"));

	return result;
}


static void
__SCNetworkReachabilityDeallocate(CFTypeRef cf)
{
	SCNetworkReachabilityRef	target		= (SCNetworkReachabilityRef)cf;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	SCLog((_sc_debug && (_sc_log > 0)), LOG_INFO, CFSTR("%srelease"),
	      targetPrivate->log_prefix);

	/* release resources */

	ptrQueryStop(targetPrivate);

	pthread_mutex_destroy(&targetPrivate->lock);

	if (targetPrivate->name != NULL)
		CFAllocatorDeallocate(NULL, (void *)targetPrivate->name);

	if (targetPrivate->resolvedAddresses != NULL)
		CFRelease(targetPrivate->resolvedAddresses);

	if (targetPrivate->localAddress != NULL)
		CFAllocatorDeallocate(NULL, (void *)targetPrivate->localAddress);

	if (targetPrivate->remoteAddress != NULL)
		CFAllocatorDeallocate(NULL, (void *)targetPrivate->remoteAddress);

	if (targetPrivate->rlsContext.release != NULL) {
		(*targetPrivate->rlsContext.release)(targetPrivate->rlsContext.info);
	}

	if (targetPrivate->ptrAddress != NULL) {
		CFRelease(targetPrivate->ptrAddress);
	}

	if (targetPrivate->ptrResolved != NULL) {
		CFRelease(targetPrivate->ptrResolved);
	}

	return;
}


static void
__SCNetworkReachabilityInitialize(void)
{
	__kSCNetworkReachabilityTypeID = _CFRuntimeRegisterClass(&__SCNetworkReachabilityClass);

	pthread_mutexattr_init(&lock_attr);
	pthread_mutexattr_settype(&lock_attr, PTHREAD_MUTEX_ERRORCHECK);

	return;
}


static dispatch_queue_t
__SCNetworkReachability_concurrent_queue()
{
	static dispatch_once_t	once;
	static dispatch_queue_t	q;

	dispatch_once(&once, ^{
		q = dispatch_queue_create("SCNetworkReachabilty.concurrent",
					  DISPATCH_QUEUE_CONCURRENT);
	});

	return q;
}


/*
 * __SCNetworkReachabilityPerformLocked
 *
 * Calls reachPerform()
 * - caller must be holding the target lock
 * - caller not running on the __SCNetworkReachability_concurrent_queue()
 */
static void
__SCNetworkReachabilityPerformLocked(SCNetworkReachabilityRef target)
{
	dispatch_queue_t		queue;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_ASSERT_HELD(&targetPrivate->lock);

	queue = targetPrivate->dispatchQueue;
	if (queue != NULL) {
		dispatch_retain(queue);
		CFRetain(target);
		dispatch_group_async(targetPrivate->dispatchGroup, __SCNetworkReachability_concurrent_queue(), ^{
			dispatch_sync(queue, ^{
				reachPerform((void *)target);
				CFRelease(target);
			});
			dispatch_release(queue);
		});
	} else if (targetPrivate->rls != NULL) {
		CFRunLoopSourceSignal(targetPrivate->rls);
		_SC_signalRunLoop(target, targetPrivate->rls, targetPrivate->rlList);
	}

	return;
}


static SCNetworkReachabilityPrivateRef
__SCNetworkReachabilityCreatePrivate(CFAllocatorRef	allocator)
{
	SCNetworkReachabilityPrivateRef		targetPrivate;
	uint32_t				size;

	/* initialize runtime */
	pthread_once(&initialized, __SCNetworkReachabilityInitialize);

	/* allocate target */
	size          = sizeof(SCNetworkReachabilityPrivate) - sizeof(CFRuntimeBase);
	targetPrivate = (SCNetworkReachabilityPrivateRef)_CFRuntimeCreateInstance(allocator,
										  __kSCNetworkReachabilityTypeID,
										  size,
										  NULL);
	if (targetPrivate == NULL) {
		return NULL;
	}

	bzero((void *)targetPrivate + sizeof(CFRuntimeBase), size);

	MUTEX_INIT(&targetPrivate->lock);

	targetPrivate->cycle				= 1;
	targetPrivate->last_notify			= NOT_REPORTED;



	targetPrivate->log_prefix[0] = '\0';
	if (_sc_log > 0) {
		snprintf(targetPrivate->log_prefix,
			 sizeof(targetPrivate->log_prefix),
			 "[%p] ",
			 targetPrivate);
	}

	return targetPrivate;
}


static const struct sockaddr *
is_valid_address(const struct sockaddr *address)
{
	const struct sockaddr	*valid	= NULL;
	static Boolean	warned	= FALSE;

	if ((address != NULL) &&
	    (address->sa_len <= sizeof(struct sockaddr_storage))) {
		switch (address->sa_family) {
			case AF_INET :
				if (address->sa_len >= sizeof(struct sockaddr_in)) {
					valid = address;
				} else {
					if (!warned) {
						SCLog(TRUE, LOG_ERR,
						      CFSTR("SCNetworkReachabilityCreateWithAddress[Pair] called with \"struct sockaddr *\" len %d < %d"),
						      address->sa_len,
						      sizeof(struct sockaddr_in));
						warned = TRUE;
					}
				}
				break;
			case AF_INET6 :
				if (address->sa_len >= sizeof(struct sockaddr_in6)) {
					valid = address;
				} else if (!warned) {
					SCLog(TRUE, LOG_ERR,
					      CFSTR("SCNetworkReachabilityCreateWithAddress[Pair] called with \"struct sockaddr *\" len %d < %d"),
					      address->sa_len,
					      sizeof(struct sockaddr_in6));
					warned = TRUE;
				}
				break;
			default :
				if (!warned) {
					SCLog(TRUE, LOG_ERR,
					      CFSTR("SCNetworkReachabilityCreateWithAddress[Pair] called with invalid address family %d"),
					      address->sa_family);
					warned = TRUE;
				}
		}
	}

	return valid;
}


SCNetworkReachabilityRef
SCNetworkReachabilityCreateWithAddress(CFAllocatorRef		allocator,
				       const struct sockaddr	*address)
{
	SCNetworkReachabilityPrivateRef	targetPrivate;

	address = is_valid_address(address);
	if (address == NULL) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	targetPrivate = __SCNetworkReachabilityCreatePrivate(allocator);
	if (targetPrivate == NULL) {
		return NULL;
	}

	targetPrivate->type = reachabilityTypeAddress;
	targetPrivate->remoteAddress = CFAllocatorAllocate(NULL, address->sa_len, 0);
	bcopy(address, targetPrivate->remoteAddress, address->sa_len);


	SCLog((_sc_debug && (_sc_log > 0)), LOG_INFO, CFSTR("%s%s %@"),
	      targetPrivate->log_prefix,
	      DEBUG_REACHABILITY_TYPE_ADDRESS,
	      targetPrivate);

	return (SCNetworkReachabilityRef)targetPrivate;
}


SCNetworkReachabilityRef
SCNetworkReachabilityCreateWithAddressPair(CFAllocatorRef		allocator,
					   const struct sockaddr	*localAddress,
					   const struct sockaddr	*remoteAddress)
{
	SCNetworkReachabilityPrivateRef	targetPrivate;

	if ((localAddress == NULL) && (remoteAddress == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	if (localAddress != NULL) {
		localAddress = is_valid_address(localAddress);
		if (localAddress == NULL) {
			_SCErrorSet(kSCStatusInvalidArgument);
			return NULL;
		}
	}

	if (remoteAddress != NULL) {
		remoteAddress = is_valid_address(remoteAddress);
		if (remoteAddress == NULL) {
			_SCErrorSet(kSCStatusInvalidArgument);
			return NULL;
		}
	}

	targetPrivate = __SCNetworkReachabilityCreatePrivate(allocator);
	if (targetPrivate == NULL) {
		return NULL;
	}

	targetPrivate->type = reachabilityTypeAddressPair;

	if (localAddress != NULL) {
		targetPrivate->localAddress = CFAllocatorAllocate(NULL, localAddress->sa_len, 0);
		bcopy(localAddress, targetPrivate->localAddress, localAddress->sa_len);
	}

	if (remoteAddress != NULL) {
		targetPrivate->remoteAddress = CFAllocatorAllocate(NULL, remoteAddress->sa_len, 0);
		bcopy(remoteAddress, targetPrivate->remoteAddress, remoteAddress->sa_len);
	}


	SCLog((_sc_debug && (_sc_log > 0)), LOG_INFO, CFSTR("%s%s %@"),
	      targetPrivate->log_prefix,
	      DEBUG_REACHABILITY_TYPE_ADDRESSPAIR,
	      targetPrivate);

	return (SCNetworkReachabilityRef)targetPrivate;
}


SCNetworkReachabilityRef
SCNetworkReachabilityCreateWithName(CFAllocatorRef	allocator,
				    const char		*nodename)
{
	union {
		struct sockaddr		sa;
		struct sockaddr_in	sin;
		struct sockaddr_in6	sin6;
	} addr;
	int				nodenameLen;
	SCNetworkReachabilityPrivateRef	targetPrivate;

	if (nodename == NULL) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	nodenameLen = strlen(nodename);
	if (nodenameLen == 0) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	if (_SC_string_to_sockaddr(nodename, AF_UNSPEC, (void *)&addr, sizeof(addr)) != NULL) {
		/* if this "nodename" is really an IP[v6] address in disguise */
		return SCNetworkReachabilityCreateWithAddress(allocator, &addr.sa);
	}

	targetPrivate = __SCNetworkReachabilityCreatePrivate(allocator);
	if (targetPrivate == NULL) {
		return NULL;
	}

	targetPrivate->type = reachabilityTypeName;

	targetPrivate->name = CFAllocatorAllocate(NULL, nodenameLen + 1, 0);
	strlcpy((char *)targetPrivate->name, nodename, nodenameLen + 1);

	targetPrivate->needResolve = TRUE;
	targetPrivate->info.flags |= kSCNetworkReachabilityFlagsFirstResolvePending;
	SCLog((_sc_debug && (_sc_log > 0)), LOG_INFO, CFSTR("%s%s %@"),
	      targetPrivate->log_prefix,
	      DEBUG_REACHABILITY_TYPE_NAME,
	      targetPrivate);

	return (SCNetworkReachabilityRef)targetPrivate;
}



SCNetworkReachabilityRef
SCNetworkReachabilityCreateWithOptions(CFAllocatorRef	allocator,
				       CFDictionaryRef	options)
{
	const struct sockaddr		*addr_l		= NULL;
	const struct sockaddr		*addr_r		= NULL;
	CFDataRef			data;
	CFStringRef			interface	= NULL;
	CFStringRef			nodename;
	CFDataRef			ptrAddress;
	CFBooleanRef			resolverBypass;
	SCNetworkReachabilityRef	target;
	SCNetworkReachabilityPrivateRef	targetPrivate;

	if (!isA_CFDictionary(options)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	/* NextBSD: a PTR target is a reverse lookup only, for hostnamed. */
	ptrAddress = CFDictionaryGetValue(options, kSCNetworkReachabilityOptionPTRAddress);
	if (ptrAddress != NULL) {
		if (!isA_CFData(ptrAddress)) {
			_SCErrorSet(kSCStatusInvalidArgument);
			return NULL;
		}
		targetPrivate = __SCNetworkReachabilityCreatePrivate(allocator);
		if (targetPrivate == NULL) {
			return NULL;
		}
		targetPrivate->type = reachabilityTypePTR;
		targetPrivate->ptrAddress = (CFDataRef)CFRetain(ptrAddress);
		return (SCNetworkReachabilityRef)targetPrivate;
	}

	nodename = CFDictionaryGetValue(options, kSCNetworkReachabilityOptionNodeName);
	if ((nodename != NULL) &&
	    (!isA_CFString(nodename) || (CFStringGetLength(nodename) == 0))) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	data = CFDictionaryGetValue(options, kSCNetworkReachabilityOptionLocalAddress);
	if (data != NULL) {
		if (!isA_CFData(data) || (CFDataGetLength(data) < sizeof(struct sockaddr_in))) {
			_SCErrorSet(kSCStatusInvalidArgument);
			return NULL;
		}
		addr_l = (const struct sockaddr *)CFDataGetBytePtr(data);
	}
	data = CFDictionaryGetValue(options, kSCNetworkReachabilityOptionRemoteAddress);
	if (data != NULL) {
		if (!isA_CFData(data) || (CFDataGetLength(data) < sizeof(struct sockaddr_in))) {
			_SCErrorSet(kSCStatusInvalidArgument);
			return NULL;
		}
		addr_r = (const struct sockaddr *)CFDataGetBytePtr(data);
	}
	interface = CFDictionaryGetValue(options, kSCNetworkReachabilityOptionInterface);
	if ((interface != NULL) &&
	    (!isA_CFString(interface) || (CFStringGetLength(interface) == 0))) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	resolverBypass = CFDictionaryGetValue(options, kSCNetworkReachabilityOptionResolverBypass);
	if ((resolverBypass != NULL) && !isA_CFBoolean(resolverBypass)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}


	if (nodename != NULL) {
		const char	*name;

		if ((addr_l != NULL) || (addr_r != NULL)) {
			// can't have both a nodename and an address
			_SCErrorSet(kSCStatusInvalidArgument);
			return NULL;
		}

		name = _SC_cfstring_to_cstring(nodename, NULL, 0, kCFStringEncodingUTF8);
		target = SCNetworkReachabilityCreateWithName(allocator, name);
		CFAllocatorDeallocate(NULL, (void *)name);
	} else {
		if ((addr_l != NULL) && (addr_r != NULL)) {
			target = SCNetworkReachabilityCreateWithAddressPair(NULL, addr_l, addr_r);
		} else if (addr_r != NULL) {
			target = SCNetworkReachabilityCreateWithAddress(NULL, addr_r);
		} else if (addr_l != NULL) {
			target = SCNetworkReachabilityCreateWithAddressPair(NULL, addr_l, NULL);
		} else {
			_SCErrorSet(kSCStatusInvalidArgument);
			return NULL;
		}
	}
	if (target == NULL) {
		return NULL;
	}

	targetPrivate = (SCNetworkReachabilityPrivateRef)target;

	if (interface != NULL) {
		if ((_SC_cfstring_to_cstring(interface,
					     targetPrivate->if_name,
					     sizeof(targetPrivate->if_name),
					     kCFStringEncodingASCII) == NULL) ||
		    ((targetPrivate->if_index = if_nametoindex(targetPrivate->if_name)) == 0)) {
			CFRelease(targetPrivate);
			_SCErrorSet(kSCStatusInvalidArgument);
			return NULL;
		}
	}


	if (resolverBypass != NULL) {
		targetPrivate->resolverBypass = CFBooleanGetValue(resolverBypass);
	}

	return (SCNetworkReachabilityRef)targetPrivate;
}


CFTypeID
SCNetworkReachabilityGetTypeID(void)
{
	pthread_once(&initialized, __SCNetworkReachabilityInitialize);	/* initialize runtime */
	return __kSCNetworkReachabilityTypeID;
}


CFArrayRef	/* CFArray[CFData], where each CFData is a (struct sockaddr *) */
SCNetworkReachabilityCopyResolvedAddress(SCNetworkReachabilityRef	target,
					 int				*error_num)
{
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	if (!isA_SCNetworkReachability(target)) {
		_SCErrorSet(kSCStatusInvalidArgument);
	       return NULL;
	}

	if (targetPrivate->type == reachabilityTypePTR) {
		/* NextBSD: a PTR target's results are CFStrings */
		if (error_num != NULL) {
			*error_num = targetPrivate->ptrError;
		}
		if (targetPrivate->ptrResolved == NULL) {
			return NULL;
		}
		return (CFArrayRef)CFArrayCreateCopy(NULL, targetPrivate->ptrResolved);
	}

	if (targetPrivate->type != reachabilityTypeName) {
		_SCErrorSet(kSCStatusInvalidArgument);
	       return NULL;
	}

	if (error_num) {
		*error_num = targetPrivate->resolvedError;
	}

	if (targetPrivate->resolvedAddresses != NULL) {
		if (isA_CFArray(targetPrivate->resolvedAddresses)) {
			return CFRetain(targetPrivate->resolvedAddresses);
		} else {
			/* if status is known but no resolved addresses to return */
			_SCErrorSet(kSCStatusOK);
			return NULL;
		}
	}

	_SCErrorSet(kSCStatusReachabilityUnknown);
	return NULL;
}


static void
__SCNetworkReachabilitySetResolvedAddresses(int32_t			status,
					    struct addrinfo		*res,
					    SCNetworkReachabilityRef	target)
{
	struct addrinfo				*resP;
	SCNetworkReachabilityPrivateRef		targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_ASSERT_HELD(&targetPrivate->lock);

	if (targetPrivate->resolvedAddresses != NULL) {
		CFRelease(targetPrivate->resolvedAddresses);
		targetPrivate->resolvedAddresses = NULL;
	}

	if ((status == 0) && (res != NULL)) {
		CFMutableArrayRef	addresses;

		addresses = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

		for (resP = res; resP; resP = resP->ai_next) {
			CFIndex		n;
			CFDataRef	newAddress;

			newAddress = CFDataCreate(NULL, (void *)resP->ai_addr, resP->ai_addr->sa_len);
			n = CFArrayGetCount(addresses);
			if ((n == 0) ||
			    !CFArrayContainsValue(addresses, CFRangeMake(0, n), newAddress)) {
				CFArrayAppendValue(addresses, newAddress);
			}
			CFRelease(newAddress);
		}

		/* save the resolved address[es] */
		targetPrivate->resolvedAddresses = addresses;
		targetPrivate->resolvedError     = NETDB_SUCCESS;
	} else {
		SCLog(_sc_debug, LOG_INFO, CFSTR("%sgetaddrinfo() failed: %s"),
		      targetPrivate->log_prefix,
		      gai_strerror(status));

		/* save the error associated with the attempt to resolve the name */
		targetPrivate->resolvedAddresses = CFRetain(kCFNull);
		targetPrivate->resolvedError     = status;
	}
	targetPrivate->needResolve = FALSE;

	if (res != NULL)	freeaddrinfo(res);

	if (targetPrivate->scheduled) {
		__SCNetworkReachabilityPerformLocked(target);
	}

	return;
}


static void
__SCNetworkReachabilityCallbackSetResolvedAddresses(int32_t status, struct addrinfo *res, void *context)
{
	SCNetworkReachabilityRef	target		= (SCNetworkReachabilityRef)context;

	__SCNetworkReachabilitySetResolvedAddresses(status, res, target);
	return;
}


/*
 * rankReachability()
 *   Not reachable       == 0
 *   Connection Required == 1
 *   Reachable           == 2
 */
static int
rankReachability(SCNetworkReachabilityFlags flags)
{
	int	rank = 0;

	if (flags & kSCNetworkReachabilityFlagsReachable)		rank = 2;
	if (flags & kSCNetworkReachabilityFlagsConnectionRequired)	rank = 1;
	return rank;
}


#pragma mark -
#pragma mark DNS name resolution


/*
 * NextBSD: Apple's getaddrinfo_async_start() is Libinfo-only; the libc
 * resolver is synchronous, so run it on the concurrent queue. A scoped
 * (if_index) lookup is not possible with getaddrinfo(); the scoped reachability
 * check is done on the resulting addresses instead.
 */
static Boolean
enqueueDNSQuery(SCNetworkReachabilityRef target)
{
	uint64_t			generation;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_ASSERT_HELD(&targetPrivate->lock);

	targetPrivate->dnsActive = TRUE;
	generation = ++targetPrivate->dnsGeneration;

	CFRetain(target);
	dispatch_async(__SCNetworkReachability_concurrent_queue(), ^{
		int		error;
		struct addrinfo	*res	= NULL;

		error = getaddrinfo(targetPrivate->name, NULL, &HINTS_DEFAULT, &res);

		MUTEX_LOCK(&targetPrivate->lock);
		if (targetPrivate->dnsActive &&
		    (targetPrivate->dnsGeneration == generation)) {
			targetPrivate->dnsActive = FALSE;
			__SCNetworkReachabilityCallbackSetResolvedAddresses(error, res, (void *)target);
		} else if (res != NULL) {
			/* cancelled, or replaced by a newer query */
			freeaddrinfo(res);
		}
		MUTEX_UNLOCK(&targetPrivate->lock);

		CFRelease(target);
	});

	return TRUE;
}


static void
dequeueDNSQuery(SCNetworkReachabilityRef target)
{
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_ASSERT_HELD(&targetPrivate->lock);

	/* any answer still on its way is now stale */
	targetPrivate->dnsActive = FALSE;
	targetPrivate->dnsGeneration++;

	return;
}


static Boolean
check_resolver_reachability(dns_resolver_t		*resolver,
			    SCNetworkReachabilityFlags	*flags,
			    Boolean			*haveDNS,
			    uint32_t			*resolver_if_index,
			    const char			*log_prefix)
{
	Boolean		ok	= TRUE;

	if (resolver_if_index) *resolver_if_index = 0;

	if (resolver->n_nameserver > 0) {
		*flags   = (SCNetworkReachabilityFlags)resolver->reach_flags;
		if (resolver_if_index != NULL) {
			*resolver_if_index = resolver->if_index;
		}
		*haveDNS = TRUE;
	} else {
		*flags   = kSCNetworkReachabilityFlagsReachable;
		*haveDNS = FALSE;
	}

	return ok;
}


static Boolean
check_matching_resolvers(dns_config_t			*dns_config,
			 const char			*fqdn,
			 unsigned int			if_index,
			 SCNetworkReachabilityFlags	*flags,
			 Boolean			*haveDNS,
			 uint32_t			*resolver_if_index,
			 int				*dns_config_index,
			 const char			*log_prefix)
{
	int		i;
	Boolean		matched		= FALSE;
	const char	*name		= fqdn;
	int32_t		n_resolvers;
	dns_resolver_t	**resolvers;

	if (if_index == 0) {
		n_resolvers = dns_config->n_resolver;
		resolvers   = dns_config->resolver;
	} else {
		n_resolvers = dns_config->n_scoped_resolver;
		resolvers   = dns_config->scoped_resolver;
	}

	/* In case we couldn't find a match, setting an index of -1
	   and resolver_if_index 0 */
	if (dns_config_index != NULL) *dns_config_index = -1;
	if (resolver_if_index != NULL) *resolver_if_index = 0;

	while (!matched && (name != NULL)) {
		int	len;

		/*
		 * check if the provided name (or sub-component)
		 * matches one of our resolver configurations.
		 */
		len = strlen(name);
		for (i = 0; i < n_resolvers; i++) {
			char		*domain;
			dns_resolver_t	*resolver;

			resolver = resolvers[i];
			if ((if_index != 0) && (if_index != resolver->if_index)) {
				continue;
			}

			domain   = resolver->domain;
			if (domain != NULL && (len == strlen(domain))) {
				if (strcasecmp(name, domain) == 0) {
					Boolean	ok;

					/*
					 * if name matches domain
					 */
					matched = TRUE;
					ok = check_resolver_reachability(resolver, flags, haveDNS,
									 resolver_if_index, log_prefix);
					if (!ok) {
						/* not today */
						return FALSE;
					}
					if (dns_config_index != NULL) *dns_config_index = i;
				}
			}
		}

		if (!matched) {
			/*
			 * we have not found a matching resolver, try
			 * a less qualified domain
			 */
			name = strchr(name, '.');
			if ((name != NULL) && (*name != '\0')) {
				name++;
			} else {
				name = NULL;
			}
		}
	}

	return matched;
}


static dns_resolver_t *
get_default_resolver(dns_config_t *dns_config, unsigned int if_index)
{
	int		i;
	int32_t		n_resolvers;
	dns_resolver_t	*resolver	= NULL;
	dns_resolver_t	**resolvers;

	if (if_index == 0) {
		n_resolvers = dns_config->n_resolver;
		resolvers   = dns_config->resolver;
	} else {
		n_resolvers = dns_config->n_scoped_resolver;
		resolvers   = dns_config->scoped_resolver;
	}

	for (i = 0; i < n_resolvers; i++) {
		if ((if_index != 0) && (if_index != resolvers[i]->if_index)) {
			continue;
		}

		if (((if_index == 0) && (i == 0)) ||
		    ((if_index != 0) && (resolver == NULL))) {
			// if this is the first (aka default) resolver
			resolver = resolvers[i];
		} else if ((resolvers[i]->domain == NULL) &&
			   (resolvers[i]->search_order < resolver->search_order)) {
			// if this is a default resolver with a lower search order
			resolver = resolvers[i];
		}
	}

	return resolver;
}


static dns_configuration_t *
dns_configuration_retain()
{
	pthread_mutex_lock(&dns_lock);

	if ((dns_configuration != NULL) && dns_token_valid) {
		int		check	= 0;
		uint32_t	status;

		/*
		 * check if the global [DNS] configuration snapshot needs
		 * to be updated
		 */
		status = notify_check(dns_token, &check);
		if (status != NOTIFY_STATUS_OK) {
			SCLog(TRUE, LOG_INFO, CFSTR("notify_check() failed, status=%lu"), status);
		}

		if ((status != NOTIFY_STATUS_OK) || (check != 0)) {
			/*
			 * if the snapshot needs to be refreshed
			 */
			if (dns_configuration->refs == 0) {
				dns_configuration_free(dns_configuration->config);
				CFAllocatorDeallocate(NULL, dns_configuration);
			}
			dns_configuration = NULL;
		}
	}

	if (dns_configuration == NULL) {
		dns_config_t	*new_config;

		new_config = dns_configuration_copy();
		if (new_config != NULL) {
			dns_configuration = CFAllocatorAllocate(NULL, sizeof(dns_configuration_t), 0);
			dns_configuration->config = new_config;
			dns_configuration->refs   = 0;
		}
	}

	if (dns_configuration != NULL) {
		dns_configuration->refs++;
	}

	pthread_mutex_unlock(&dns_lock);
	return dns_configuration;
}


static void
dns_configuration_release(dns_configuration_t *config)
{
	pthread_mutex_lock(&dns_lock);

	config->refs--;
	if (config->refs == 0) {
		if ((dns_configuration != config)) {
			dns_configuration_free(config->config);
			CFAllocatorDeallocate(NULL, config);
		}
	}

	pthread_mutex_unlock(&dns_lock);
	return;
}


static Boolean
dns_configuration_watch()
{
	int		dns_check	= 0;
	const char	*dns_key;
	Boolean		ok		= FALSE;
	uint32_t	status;

	pthread_mutex_lock(&dns_lock);

	dns_key = dns_configuration_notify_key();
	if (dns_key == NULL) {
		SCLog(TRUE, LOG_INFO, CFSTR("dns_configuration_notify_key() failed"));
		goto done;
	}

	status = notify_register_check(dns_key, &dns_token);
	if (status == NOTIFY_STATUS_OK) {
		dns_token_valid = TRUE;
	} else {
		SCLog(TRUE, LOG_INFO, CFSTR("notify_register_check() failed, status=%lu"), status);
		goto done;
	}

	status = notify_check(dns_token, &dns_check);
	if (status != NOTIFY_STATUS_OK) {
		SCLog(TRUE, LOG_INFO, CFSTR("notify_check() failed, status=%lu"), status);
		(void)notify_cancel(dns_token);
		dns_token_valid = FALSE;
		goto done;
	}

	ok = TRUE;

    done :

	pthread_mutex_unlock(&dns_lock);
	return ok;
}


static void
dns_configuration_unwatch()
{
	pthread_mutex_lock(&dns_lock);

	(void)notify_cancel(dns_token);
	dns_token_valid = FALSE;

	if ((dns_configuration != NULL) && (dns_configuration->refs == 0)) {
		dns_configuration_free(dns_configuration->config);
		CFAllocatorDeallocate(NULL, dns_configuration);
		dns_configuration = NULL;
	}

	pthread_mutex_unlock(&dns_lock);
	return;
}


static Boolean
_SC_R_checkResolverReachability(SCNetworkReachabilityFlags	*flags,
				Boolean				*haveDNS,
				const char			*nodename,
				unsigned int			if_index,
				uint32_t			*resolver_if_index,
				int				*dns_config_index,
				const char			*log_prefix
				)
{
	dns_resolver_t		*default_resolver;
	dns_configuration_t	*dns;
	Boolean			found			= FALSE;
	char			*fqdn			= (char *)nodename;
	int			i;
	Boolean			isFQDN			= FALSE;
	uint32_t		len;
	int			ndots			= 1;
	Boolean			ok			= TRUE;
	Boolean			useDefault		= FALSE;

	if (resolver_if_index) *resolver_if_index = 0;
	if (dns_config_index) *dns_config_index = -1;

	/*
	 * We first assume that all of the configured DNS servers
	 * are available.  Since we don't know which name server will
	 * be consulted to resolve the specified nodename we need to
	 * check the availability of ALL name servers.  We can only
	 * proceed if we know that our query can be answered.
	 */

	*flags   = kSCNetworkReachabilityFlagsReachable;
	*haveDNS = FALSE;

	len = (nodename != NULL) ? strlen(nodename) : 0;
	if (len == 0) {
		// if no nodename, return not reachable
		*flags = 0;
		return ok;
	}

	dns = dns_configuration_retain();
	if (dns == NULL) {
		// if error
		SCLog(_sc_debug, LOG_INFO, CFSTR("%sDNS: no configuration"), log_prefix);
		goto done;
	}

	default_resolver = get_default_resolver(dns->config, if_index);
	if (default_resolver == NULL) {
		// if no resolver configuration
		SCLog(_sc_debug, LOG_INFO, CFSTR("%sDNS: no resolvers"), log_prefix);
		goto done;
	}

	if (fqdn[len - 1] == '.') {
		isFQDN = TRUE;

		// trim trailing '.''s
		while ((len > 0) && (fqdn[len-1] == '.')) {
			if (fqdn == nodename) {
				fqdn = strdup(nodename);
			}
			fqdn[--len] = '\0';
		}
	}

	/*
	 * check if the provided name matches a supplemental domain
	 */
	found = check_matching_resolvers(dns->config, fqdn, if_index,
					 flags, haveDNS, resolver_if_index,
					 dns_config_index, log_prefix);

	if (!found && !isFQDN) {
		/*
		 * if we did not match a supplemental domain name and if the
		 * provided name has enough "."s then the first query will be
		 * directed to the default resolver.
		 */
		char	*cp;
		int	dots;

#define	NDOTS_OPT	"ndots="
#define	NDOTS_OPT_LEN	(sizeof("ndots=") - 1)

		if (default_resolver->options != NULL) {
			cp = strstr(default_resolver->options, NDOTS_OPT);
			if ((cp != NULL) &&
			    ((cp == default_resolver->options) || isspace(cp[-1])) &&
			    ((cp[NDOTS_OPT_LEN] != '\0') && isdigit(cp[NDOTS_OPT_LEN]))) {
				char    *end;
				long    val;

				cp +=  NDOTS_OPT_LEN;
				errno = 0;
				val = strtol(cp, &end, 10);
				if ((*cp != '\0') && (cp != end) && (errno == 0) &&
				((*end == '\0') || isspace(*end))) {
					ndots = val;
				}
			}
		}

		dots = 0;
		for (cp = fqdn; *cp != '\0'; cp++) {
			if (*cp == '.') dots++;
		}

		/* Per KB: HT4845 */
		if (dots >= ndots) {
			useDefault = TRUE;
		}
	}

	if (!found && !isFQDN && !useDefault && (dns->config->n_resolver > 1)) {
		/*
		 * FQDN not specified, try matching w/search domains
		 */
		if (default_resolver->n_search > 0) {
			for (i = 0; !found && (i < default_resolver->n_search); i++) {
				int	ret;
				char	*search_fqdn	= NULL;

				ret = asprintf(&search_fqdn, "%s.%s", fqdn, default_resolver->search[i]);
				if (ret == -1) {
					continue;
				}

				// try the provided name with the search domain appended
				found = check_matching_resolvers(dns->config,
								 search_fqdn,
								 if_index,
								 flags,
								 haveDNS,
								 resolver_if_index,
								 dns_config_index,
								 log_prefix);
				free(search_fqdn);
			}
		} else if (default_resolver->domain != NULL) {
			char	*dp;
			int	domain_parts	= 0;

			// count domain parts
			for (dp = default_resolver->domain; *dp != '\0'; dp++) {
				if (*dp == '.') {
					domain_parts++;
				}
			}

			// remove trailing dots
			for (dp--; (dp >= default_resolver->domain) && (*dp == '.'); dp--) {
				*dp = '\0';
				domain_parts--;
			}

			if (dp >= default_resolver->domain) {
				// dots are separators, bump # of components
				domain_parts++;
			}

			dp = default_resolver->domain;
			for (i = LOCALDOMAINPARTS; !found && (i <= (domain_parts - ndots)); i++) {
				int	ret;
				char	*search_fqdn	= NULL;

				ret = asprintf(&search_fqdn, "%s.%s", fqdn, dp);
				if (ret == -1) {
					continue;
				}

				// try the provided name with the [default] domain appended
				found = check_matching_resolvers(dns->config,
								 search_fqdn,
								 if_index,
								 flags,
								 haveDNS,
								 resolver_if_index,
								 dns_config_index,
								 log_prefix);
				free(search_fqdn);

				// move to the next component of the [default] domain
				dp = strchr(dp, '.') + 1;
			}
		}
	}

	if (!found) {
		/*
		 * check the reachability of the default resolver
		 */
		ok = check_resolver_reachability(default_resolver, flags, haveDNS,
						 resolver_if_index, log_prefix);
		if (ok && dns_config_index != NULL) *dns_config_index = 0;
	}

	if (fqdn != nodename)	free(fqdn);

    done :

	if (dns != NULL) {
		dns_configuration_release(dns);
	}

	return ok;
}


/*
 * NextBSD SPI: the check behind scutil --dns <hostname>. Apple passes a
 * dynamic store session for the VPN and PPP checks, which do not exist here.
 */
Boolean
__SC_checkResolverReachabilityInternal(SCDynamicStoreRef		*storeP,
				       SCNetworkReachabilityFlags	*flags,
				       Boolean				*haveDNS,
				       const char			*nodename,
				       uint32_t				*resolver_if_index,
				       int				*dns_config_index)
{
	(void)storeP;
	return _SC_R_checkResolverReachability(flags, haveDNS, nodename,
					       0, resolver_if_index, dns_config_index, "");
}


const char *
__SCNetworkReachabilityGetTargetName(SCNetworkReachabilityRef	target,
				     unsigned int		*if_index)
{
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	if (!isA_SCNetworkReachability(target) ||
	    (targetPrivate->type != reachabilityTypeName)) {
		return NULL;
	}

	if (if_index != NULL) {
		*if_index = targetPrivate->if_index;
	}

	return targetPrivate->name;
}


#pragma mark -
#pragma mark DNS refresh support


// Note: protected by _hn_changes_queue()
static SCDynamicStoreCallBack	dns_callback		= NULL;
static int			dns_refresh_token;
static Boolean			dns_refresh_token_valid	= FALSE;
static SCDynamicStoreRef	dns_store		= NULL;


/*
 * dns_refresh_handler
 *
 * Called to notify/update all SCNetworkReachability by-name targets of
 * a network/DNS change.  The change should [re-]start a DNS query to
 * resolve the name.
 * - caller must be running on the _hn_changes_queue()
 */
static void
dns_refresh_handler()
{
	CFArrayRef	changes;
	CFStringRef	key;

	if (!dns_refresh_token_valid || (dns_callback == NULL)) {
		return;
	}

	key = SCDynamicStoreKeyCreateNetworkGlobalEntity(NULL,
							 kSCDynamicStoreDomainState,
							 kSCEntNetDNS);
	changes = CFArrayCreate(NULL, (const void **)&key, 1, &kCFTypeArrayCallBacks);
	(*dns_callback)(dns_store, changes, NULL);
	CFRelease(changes);
	CFRelease(key);

	return;
}


/*
 * dns_refresh_enable
 *
 * Called to monitor for network/DNS changes that should restart a DNS query.
 * - caller must be running on the _hn_changes_queue()
 */
static Boolean
dns_refresh_enable(dispatch_queue_t q, SCDynamicStoreRef store, SCDynamicStoreCallBack callback)
{
	uint32_t	status;

	dns_callback = callback;
	dns_store = CFRetain(store);

	status = notify_register_dispatch(_SC_NOTIFY_NETWORK_CHANGE,	// resolver or address change
					  &dns_refresh_token,
					  q,
					  ^(int token){
						  dns_refresh_handler();
					  });
	if (status != NOTIFY_STATUS_OK) {
		SCLog(TRUE, LOG_INFO, CFSTR("notify_register_dispatch() failed, status=%lu"), status);
		return FALSE;
	}

	dns_refresh_token_valid = TRUE;

	return TRUE;
}


/*
 * dns_refresh_disable
 *
 * Called to stop monitoring for network/DNS changes
 * - caller must be running on the _hn_changes_queue()
 */
static void
dns_refresh_disable()
{
	(void)notify_cancel(dns_refresh_token);
	dns_refresh_token_valid = FALSE;
	CFRelease(dns_store);
	dns_store = NULL;
	dns_callback = NULL;
	return;
}


#pragma mark -
#pragma mark Network Information support


// Note: protected by _hn_changes_queue()
static SCDynamicStoreCallBack	network_changed_callback	= NULL;
static int			network_changed_token;
static Boolean			network_changed_token_valid	= FALSE;
static SCDynamicStoreRef	network_change_store		= NULL;


/*
 * nwi_refresh_handler
 *
 * Called to notify/update network changed events
 * - caller must be running on the _hn_changes_queue()
 */
static void
nwi_refresh_handler()
{
	CFArrayRef	changes;
	CFStringRef	key;

	if (!network_changed_token_valid || (network_changed_callback == NULL)) {
		return;
	}

	// Fake a network change.
	key = SCDynamicStoreKeyCreateNetworkGlobalEntity(NULL,
							 kSCDynamicStoreDomainState,
							 kSCEntNetIPv4);

	changes = CFArrayCreate(NULL, (const void **)&key, 1, &kCFTypeArrayCallBacks);
	(*network_changed_callback)(network_change_store, changes, NULL);
	CFRelease(changes);
	CFRelease(key);

	return;
}


/*
 * nwi_refresh_enable
 *
 * Called to monitor for network changes.
 * - caller must be running on the _hn_changes_queue()
 */
static Boolean
nwi_refresh_enable(dispatch_queue_t q, SCDynamicStoreRef store, SCDynamicStoreCallBack callback)
{
	uint32_t	status;

	network_changed_callback = callback;
	network_change_store = CFRetain(store);

	status = notify_register_dispatch(_SC_NOTIFY_NETWORK_CHANGE_NWI,	// trailing nwi_state_get_notify_key()
					  &network_changed_token,
					  q,
					  ^(int token){
						  nwi_refresh_handler();
					  });
	if (status != NOTIFY_STATUS_OK) {
		SCLog(TRUE, LOG_INFO, CFSTR("notify_register_dispatch() failed for network changes, status=%lu"), status);
		return FALSE;
	}

	network_changed_token_valid = TRUE;

	return TRUE;
}


/*
 * nwi_refresh_disable
 *
 * Called to stop monitoring for network changes
 * - caller must be running on the _hn_changes_queue()
 */
static void
nwi_refresh_disable()
{
	if (network_changed_token_valid) {
		(void)notify_cancel(network_changed_token);
		network_changed_token_valid = FALSE;
	}
	if (network_change_store != NULL) {
		CFRelease(network_change_store);
		network_change_store = NULL;
		network_changed_callback = NULL;
	}
	return;
}


#pragma mark -
#pragma mark Reachability Flags


static Boolean
__SCNetworkReachabilityGetFlags(SCNetworkReachabilityRef	target,
				ReachabilityInfo		*reach_info,
				Boolean				async)
{
	CFMutableArrayRef		addresses	= NULL;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;
	ReachabilityInfo		my_info		= NOT_REACHABLE;
	Boolean				ok		= TRUE;

	MUTEX_ASSERT_HELD(&targetPrivate->lock);

	_reach_set(reach_info, &NOT_REACHABLE, reach_info->cycle, targetPrivate->if_index, targetPrivate->if_name);

	if (!isA_SCNetworkReachability(target)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	switch (targetPrivate->type) {
		case reachabilityTypeAddress :
		case reachabilityTypeAddressPair : {
			/*
			 * Check "local" address
			 */
			if (targetPrivate->localAddress != NULL) {
				/*
				 * Check "local" address
				 */
				ok = checkAddress_with_nwi(targetPrivate->localAddress,
						  targetPrivate->if_index,
						  &my_info,
						  targetPrivate->log_prefix);
				if (!ok) {
					goto error;	/* not today */
				}

				if (!(my_info.flags & kSCNetworkReachabilityFlagsIsLocalAddress)) {
					goto error;	/* not reachable, non-"local" address */
				}
			}

			/*
			 * Check "remote" address
			 */
			if (targetPrivate->remoteAddress != NULL) {
				/*
				 * in cases where we have "local" and "remote" addresses
				 * we need to re-initialize the to-be-returned flags.
				 */
				my_info = NOT_REACHABLE;

				/*
				 * Check "remote" address
				 */
				ok = checkAddress_with_nwi(targetPrivate->remoteAddress,
						  targetPrivate->if_index,
						  &my_info,
						  targetPrivate->log_prefix);
				if (!ok) {
					goto error;	/* not today */
				}
			}

			break;

		}

		case reachabilityTypePTR :
			// NextBSD: reported through the callback only
			break;

		case reachabilityTypeName : {
			int				error;
			int				ns_dns_config	= -1;
			SCNetworkReachabilityFlags	ns_flags	= 0;
			uint32_t			ns_if_index	= 0;
			struct addrinfo			*res;

			addresses = (CFMutableArrayRef)SCNetworkReachabilityCopyResolvedAddress(target, &error);
			if ((addresses != NULL) || (error != NETDB_SUCCESS)) {
				/* if resolved or an error had been detected */
				if (!async) {
					/* if not an async request */
					goto checkResolvedAddresses;
				} else if (targetPrivate->dnsActive) {
					/* if [m]DNS query active */
					goto checkResolvedAddresses;
				} else if (!targetPrivate->needResolve) {
					/*
					 * if this is an async request (i.e. someone is watching the reachability
					 * of this target), if no query active, and if no query is needed
					 */
					goto checkResolvedAddresses;
				}
			}

			targetPrivate->dnsBlocked = FALSE;

			/* check the reachability of the DNS servers */
			ok = _SC_R_checkResolverReachability(&ns_flags,
							     &targetPrivate->haveDNS,
							     targetPrivate->name,
							     targetPrivate->if_index,
							     &ns_if_index,
							     &ns_dns_config,
							     targetPrivate->log_prefix);
			if (!ok) {
				/* if we could not get DNS server info */
				SCLog(_sc_debug, LOG_INFO, CFSTR("%sDNS server reachability unknown"),
				      targetPrivate->log_prefix);
				targetPrivate->resolverFlags = kSCNetworkReachabilityFlagsReachable;
				goto error;
			} else {

				// save resolver reachability flags
				targetPrivate->resolverFlags = ns_flags;

				if (rankReachability(ns_flags) < 2) {
					/*
					 * if DNS servers are not (or are no longer) reachable, set
					 * flags based on the availability of configured (but not
					 * active) services.
					 */

					SCLog(_sc_debug, LOG_INFO, CFSTR("%sDNS server(s) not available"),
					      targetPrivate->log_prefix);

					if (!targetPrivate->dnsBlocked) {
						ok = checkAddress_with_nwi(NULL,
								  targetPrivate->if_index,
								  &my_info,
								  targetPrivate->log_prefix);
						if (!ok) {
							SCLog(_sc_debug, LOG_INFO, CFSTR("%sNo available networks"),
							      targetPrivate->log_prefix);
							goto error;
						}
					} else {
						// if not checking "available" networks
						my_info.flags = ns_flags;
						my_info.if_index = ns_if_index;
					}

					if (async && targetPrivate->scheduled) {
						/*
						 * return "host not found", set flags appropriately,
						 * and schedule notification.
						 */
						__SCNetworkReachabilityCallbackSetResolvedAddresses(EAI_NONAME,
												    NULL,
												    (void *)target);
						my_info.flags |= (targetPrivate->info.flags & kSCNetworkReachabilityFlagsFirstResolvePending);

						SCLog(_sc_debug, LOG_INFO, CFSTR("%sno DNS servers are reachable"),
						      targetPrivate->log_prefix);
						__SCNetworkReachabilityPerformLocked(target);
					}
					break;
				}
			}

			if (targetPrivate->resolverBypass) {
				/* if we are not resolving the name,
				 * set the flags of the resolvers */
				my_info.flags = ns_flags;
				my_info.if_index = ns_if_index;
				break;
			}

			if (async) {
				/* for async requests we return the last known status */
				my_info = targetPrivate->info;

				if (targetPrivate->dnsActive) {
					/* if [m]DNS query active */
					SCLog(_sc_debug, LOG_INFO,
					      CFSTR("%swaiting for DNS reply"),
					      targetPrivate->log_prefix);
					if ((addresses != NULL) || (error != NETDB_SUCCESS)) {
						/* updated reachability based on the previous reply */
						goto checkResolvedAddresses;
					}
					break;
				}

				SCLog(_sc_debug, LOG_INFO,
				      CFSTR("%sstart DNS query for name = %s"),
				      targetPrivate->log_prefix,
				      targetPrivate->name);

				if (enqueueDNSQuery(target)) {
					/* request initiated */
					break;
				}

				/* if we could not initiate the request, process error */
				goto checkResolvedAddresses;
			}

			SCLog(_sc_debug, LOG_INFO,
			      CFSTR("%scheck DNS for name = %s"),
			      targetPrivate->log_prefix,
			      targetPrivate->name);

			/*
			 * OK, all of the DNS name servers are available.  Let's
			 * resolve the nodename into an address.
			 */
				error = getaddrinfo(targetPrivate->name,
						    NULL,
						    &HINTS_DEFAULT,
						    &res);
			__SCNetworkReachabilitySetResolvedAddresses(error, res, target);

			addresses = (CFMutableArrayRef)SCNetworkReachabilityCopyResolvedAddress(target, &error);

		    checkResolvedAddresses :

			/*
			 * We first assume that the requested host is NOT available.
			 * Then, check each address for accessibility and return the
			 * best status available.
			 */
			my_info = NOT_REACHABLE;

			if (isA_CFArray(addresses)) {
				CFIndex		i;
				CFIndex		n	= CFArrayGetCount(addresses);

				for (i = 0; i < n; i++) {
					ReachabilityInfo	ns_info	= NOT_REACHABLE;
					struct sockaddr		*sa;

					sa = (struct sockaddr *)CFDataGetBytePtr(CFArrayGetValueAtIndex(addresses, i));

					ok = checkAddress_with_nwi(sa,
							  targetPrivate->if_index,
							  &ns_info,
							  targetPrivate->log_prefix);
					if (!ok) {
						goto error;	/* not today */
					}

					if (rankReachability(ns_info.flags) > rankReachability(my_info.flags)) {
						/* return the best case result */
						my_info = ns_info;
						if (rankReachability(my_info.flags) == 2) {
							/* we're in luck */
							break;
						}
					}
				}
			} else {
				if ((error == EAI_NONAME)
#if defined(EAI_NODATA) && (EAI_NODATA != EAI_NONAME)
				     || (error == EAI_NODATA)
#endif
				     ) {
					/*
					 * the target host name could not be resolved
					 */

					if (!targetPrivate->haveDNS) {
						/*
						 * No DNS servers are defined. Set flags based on
						 * the availability of configured (but not active)
						 * services.
						 */
						ok = checkAddress_with_nwi(NULL,
								  targetPrivate->if_index,
								  &my_info,
								  targetPrivate->log_prefix);
						if (!ok) {
							goto error;	/* not today */
						}

						if ((my_info.flags & kSCNetworkReachabilityFlagsReachable) &&
							(my_info.flags & kSCNetworkReachabilityFlagsConnectionRequired)) {
							/*
							 * Since we might pick up a set of DNS servers when this connection
							 * is established, don't reply with a "HOST NOT FOUND" error just yet.
							 */
							break;
						}

						/* Host not found, not reachable! */
						my_info = NOT_REACHABLE;
					}
				}
			}

			break;
		}
	}

    done:


	_reach_set(reach_info, &my_info, targetPrivate->cycle, targetPrivate->if_index, targetPrivate->if_name);

    error :

	if (addresses != NULL)	CFRelease(addresses);
	return ok;
}

int
SCNetworkReachabilityGetInterfaceIndex(SCNetworkReachabilityRef target)
{
	int				if_index	= -1;
	Boolean				ok		= TRUE;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	if (!isA_SCNetworkReachability(target)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return if_index;
	}

	MUTEX_LOCK(&targetPrivate->lock);

	if (targetPrivate->scheduled) {
		// if being watched, return the last known (and what should be current) status
		goto done;
	}


	ok = __SCNetworkReachabilityGetFlags(target, &targetPrivate->info, FALSE);

    done :

	/* Only return the if_index if the connection is reachable not for reachable connection
	 * required etc ... */
	if (ok && rankReachability(targetPrivate->info.flags) == 2) {
		if_index = targetPrivate->info.if_index;
	}

	MUTEX_UNLOCK(&targetPrivate->lock);
	return if_index;
}


Boolean
SCNetworkReachabilityGetFlags(SCNetworkReachabilityRef		target,
			      SCNetworkReachabilityFlags	*flags)
{
	Boolean				ok		= TRUE;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	if (!isA_SCNetworkReachability(target)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	MUTEX_LOCK(&targetPrivate->lock);

	if (targetPrivate->scheduled) {
		// if being watched, return the last known (and what should be current) status
		*flags = targetPrivate->info.flags & kSCNetworkReachabilityFlagsMask;
		goto done;
	}


	ok = __SCNetworkReachabilityGetFlags(target, &targetPrivate->info, FALSE);
	if (_sc_debug) {
		SCLog(TRUE, LOG_INFO, CFSTR("%s  flags     = 0x%08x"), targetPrivate->log_prefix, targetPrivate->info.flags);
	}

	*flags = targetPrivate->info.flags & kSCNetworkReachabilityFlagsMask;

    done :

	MUTEX_UNLOCK(&targetPrivate->lock);
	return ok;
}


#pragma mark -
#pragma mark Notifications


static void
__SCNetworkReachabilitySetNotifications(SCDynamicStoreRef	store)
{
	CFMutableArrayRef		keys;

	keys = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

	// SCDynamicStore key to force posting a reachability change
	CFArrayAppendValue(keys, SCNETWORKREACHABILITY_TRIGGER_KEY);

	(void)SCDynamicStoreSetNotificationKeys(store, keys, NULL);
	CFRelease(keys);

	return;
}


static void
__SCNetworkReachabilityHandleChanges(SCDynamicStoreRef	store,
				     CFArrayRef		changedKeys,
				     void		*info)
{
	Boolean			dnsConfigChanged	= FALSE;
	CFIndex			i;
	Boolean			forcedChange		= FALSE;
	CFStringRef		key;
	CFIndex			nChanges;
	CFIndex			nGlobals		= 0;
	CFIndex			nTargets;
	Boolean			networkConfigChanged	= FALSE;
	struct timeval		now;
	const void *		targets_q[N_QUICK];
	const void **		targets			= targets_q;
	__block CFSetRef	watchers		= NULL;

	nChanges = CFArrayGetCount(changedKeys);
	if (nChanges == 0) {
		/* if no changes */
		return;
	}

	dispatch_sync(_hn_target_queue(), ^{
		/* grab the currently watched targets */
		if (hn_targets != NULL) {
			watchers = CFSetCreateCopy(NULL, hn_targets);
		}
	});

	nTargets = (watchers != NULL) ? CFSetGetCount(watchers) : 0;
	if (nTargets == 0) {
		/* if no addresses being monitored */
		goto done;
	}

	key = SCDynamicStoreKeyCreateNetworkGlobalEntity(NULL,
							 kSCDynamicStoreDomainState,
							 kSCEntNetDNS);
	if (CFArrayContainsValue(changedKeys, CFRangeMake(0, nChanges), key)) {
		nGlobals++;
		dnsConfigChanged = TRUE;	/* the DNS server(s) have changed */
	}
	CFRelease(key);


	if (CFArrayContainsValue(changedKeys, CFRangeMake(0, nChanges), SCNETWORKREACHABILITY_TRIGGER_KEY)) {
		nGlobals++;
		forcedChange = TRUE;		/* an SCDynamicStore driven "network" change */
	}

	if (nChanges > nGlobals) {
		networkConfigChanged = TRUE;
	}

	if (nTargets > (CFIndex)(sizeof(targets_q) / sizeof(CFTypeRef)))
		targets = CFAllocatorAllocate(NULL, nTargets * sizeof(CFTypeRef), 0);
	CFSetGetValues(watchers, targets);
	for (i = 0; i < nTargets; i++) {
		Boolean				dnsNeedsUpdate	= FALSE;
		SCNetworkReachabilityRef	target		= targets[i];
		SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

		MUTEX_LOCK(&targetPrivate->lock);


		if (targetPrivate->type == reachabilityTypeName) {
			Boolean		dnsChanged	= (dnsConfigChanged	|
							   dnsNeedsUpdate);

			if (!dnsChanged) {
				/*
				 * if the DNS configuration didn't change we still need to
				 * check that the DNS servers are accessible.
				 */
				Boolean				ns_blocked	= FALSE;
				int				ns_dns_config	= -1;
				SCNetworkReachabilityFlags	ns_flags	= 0;
				uint32_t			ns_if_index	= 0;
				Boolean				ok;

				/* check the reachability of the DNS servers */
				ok = _SC_R_checkResolverReachability(&ns_flags,
								     &targetPrivate->haveDNS,
								     targetPrivate->name,
								     targetPrivate->if_index,
								     &ns_if_index,
								     &ns_dns_config,
								     targetPrivate->log_prefix);

				if (!ok) {
					/* if we could not get DNS server info */
					SCLog(_sc_debug, LOG_INFO, CFSTR("%sDNS server reachability unknown"),
					      targetPrivate->log_prefix);
					ns_flags = kSCNetworkReachabilityFlagsReachable;
					dnsChanged = TRUE;
				} else {

					if (rankReachability(ns_flags) < 2) {
						/*
						 * if DNS servers are not (or are no longer) reachable, set
						 * flags based on the availability of configured (but not
						 * active) services.
						 */
						SCLog(_sc_debug, LOG_INFO, CFSTR("%sDNS server(s) not available"),
						      targetPrivate->log_prefix);
						dnsChanged = TRUE;
					}
				}

				if ((targetPrivate->dnsBlocked != ns_blocked) ||
				    (targetPrivate->resolverFlags != ns_flags)) {
					// if the DNS blocked or resolver reachability changed
					targetPrivate->dnsBlocked = ns_blocked;
					targetPrivate->resolverFlags = ns_flags;
					dnsChanged = TRUE;
				}
			}

			if (dnsChanged) {
				if (targetPrivate->dnsActive) {
					// if we have an outstanding [m]DNS query
					SCLog(_sc_debug, LOG_INFO,
					      CFSTR("%scancel [m]DNS query for name = %s"),
					      targetPrivate->log_prefix,
					      targetPrivate->name);
					dequeueDNSQuery(target);
				}

				/* schedule request to resolve the name again */
				targetPrivate->needResolve = TRUE;
			}
		}

		if (forcedChange) {
			targetPrivate->cycle++;
		}

		if (targetPrivate->scheduled) {
			__SCNetworkReachabilityPerformLocked(target);
		}

		MUTEX_UNLOCK(&targetPrivate->lock);
	}
	if (targets != targets_q)	CFAllocatorDeallocate(NULL, targets);

    done :

	if (watchers != NULL) CFRelease(watchers);
	return;
}


static void
reachPerform(void *info)
{
	void				*context_info;
	void				(*context_release)(const void *);
	uint64_t			cycle;
	Boolean				defer		= FALSE;
	Boolean				forced;
	Boolean				ok;
	ReachabilityInfo		reach_info	= NOT_REACHABLE;
	SCNetworkReachabilityCallBack	rlsFunction;
	SCNetworkReachabilityRef	target		= (SCNetworkReachabilityRef)info;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	SCLog(_sc_debug, LOG_INFO, CFSTR("%schecking target reachability"),
	      targetPrivate->log_prefix);


	MUTEX_LOCK(&targetPrivate->lock);

	if (!targetPrivate->scheduled) {
		// if not currently scheduled
		MUTEX_UNLOCK(&targetPrivate->lock);
		return;
	}

	/* update reachability, notify if status changed */
	ok = __SCNetworkReachabilityGetFlags(target, &reach_info, TRUE);
	if (!ok) {
		/* if reachability status not available */
		SCLog(_sc_debug, LOG_INFO, CFSTR("%flags not available"),
		      targetPrivate->log_prefix);
		reach_info = NOT_REACHABLE;
	}

	cycle = targetPrivate->cycle;
	forced = ((cycle != 0) && (targetPrivate->info.cycle != cycle));

	if (!forced && !__reach_changed(&targetPrivate->info, &reach_info)) {
		if (_sc_debug) {
			if (targetPrivate->info.sleeping == reach_info.sleeping) {
				SCLog(TRUE, LOG_INFO,
				      CFSTR("%sflags/interface match (now 0x%08x/%hu%s)"),
				      targetPrivate->log_prefix,
				      reach_info.flags,
				      reach_info.if_index,
				      reach_info.sleeping ? ", z" : "");
			} else {
				SCLog(TRUE, LOG_INFO,
				      CFSTR("%sflags/interface equiv (was 0x%08x/%hu%s, now 0x%08x/%hu%s)"),
				      targetPrivate->log_prefix,
				      targetPrivate->info.flags,
				      targetPrivate->info.if_index,
				      targetPrivate->info.sleeping ? ", z" : "",
				      reach_info.flags,
				      reach_info.if_index,
				      reach_info.sleeping ? ", z" : "");
			}

		}
		MUTEX_UNLOCK(&targetPrivate->lock);
		return;
	}

	SCLog(_sc_debug, LOG_INFO,
	      CFSTR("%sflags/interface have changed (was 0x%08x/%hu%s, now 0x%08x/%hu%s)%s%s"),
	      targetPrivate->log_prefix,
	      targetPrivate->info.flags,
	      targetPrivate->info.if_index,
	      targetPrivate->info.sleeping ? ", z" : "",
	      reach_info.flags,
	      reach_info.if_index,
	      reach_info.sleeping ? ", z" : "",
	      defer ? ", deferred" : "",
	      forced ? ", forced" : "");

	/* update flags / interface */
	_reach_set(&targetPrivate->info, &reach_info, cycle, targetPrivate->if_index, targetPrivate->if_name);

	/* save last notification info */
	_reach_set(&targetPrivate->last_notify, &reach_info, cycle, targetPrivate->if_index, targetPrivate->if_name);

	/* as needed, defer the notification */
	if (defer) {
		MUTEX_UNLOCK(&targetPrivate->lock);
		return;
	}

	/* callout */
	rlsFunction = targetPrivate->rlsFunction;
	if (targetPrivate->rlsContext.retain != NULL) {
		context_info	= (void *)(*targetPrivate->rlsContext.retain)(targetPrivate->rlsContext.info);
		context_release	= targetPrivate->rlsContext.release;
	} else {
		context_info	= targetPrivate->rlsContext.info;
		context_release	= NULL;
	}

	MUTEX_UNLOCK(&targetPrivate->lock);

	if (rlsFunction != NULL) {
		(*rlsFunction)(target,
			       reach_info.flags & kSCNetworkReachabilityFlagsMask,
			       context_info);
	}

	if (context_release != NULL) {
		(*context_release)(context_info);
	}

	return;
}


Boolean
SCNetworkReachabilitySetCallback(SCNetworkReachabilityRef	target,
				 SCNetworkReachabilityCallBack	callout,
				 SCNetworkReachabilityContext	*context)
{
	SCNetworkReachabilityPrivateRef	targetPrivate = (SCNetworkReachabilityPrivateRef)target;

	MUTEX_LOCK(&targetPrivate->lock);

	if (targetPrivate->rlsContext.release != NULL) {
		/* let go of the current context */
		(*targetPrivate->rlsContext.release)(targetPrivate->rlsContext.info);
	}

	targetPrivate->rlsFunction   			= callout;
	targetPrivate->rlsContext.info			= NULL;
	targetPrivate->rlsContext.retain		= NULL;
	targetPrivate->rlsContext.release		= NULL;
	targetPrivate->rlsContext.copyDescription	= NULL;
	if (context) {
		bcopy(context, &targetPrivate->rlsContext, sizeof(SCNetworkReachabilityContext));
		if (context->retain != NULL) {
			targetPrivate->rlsContext.info = (void *)(*context->retain)(context->info);
		}
	}

	MUTEX_UNLOCK(&targetPrivate->lock);

	return TRUE;
}


static CFStringRef
reachRLSCopyDescription(const void *info)
{
	SCNetworkReachabilityRef		target	= (SCNetworkReachabilityRef)info;

	return CFStringCreateWithFormat(NULL,
					NULL,
					CFSTR("<SCNetworkReachability RLS> {target = %p}"),
					target);
}


static Boolean
__SCNetworkReachabilityScheduleWithRunLoop(SCNetworkReachabilityRef	target,
					   CFRunLoopRef			runLoop,
					   CFStringRef			runLoopMode,
					   dispatch_queue_t		queue)
{
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;
	Boolean				init		= FALSE;
	__block Boolean			ok		= FALSE;

	MUTEX_LOCK(&targetPrivate->lock);

	if ((targetPrivate->dispatchQueue != NULL) ||		// if we are already scheduled with a dispatch queue
	    ((queue != NULL) && targetPrivate->scheduled)) {	// if we are already scheduled on a CFRunLoop
		_SCErrorSet(kSCStatusInvalidArgument);
		goto done;
	}

	/* schedule the SCNetworkReachability did-something-change handler */

	dispatch_sync(_hn_target_queue(), ^{
		ok = FALSE;

		if (hn_store == NULL) {
			hn_store = SCDynamicStoreCreate(NULL,
							CFSTR("SCNetworkReachability"),
							__SCNetworkReachabilityHandleChanges,
							NULL);
			if (hn_store == NULL) {
				SCLog(TRUE, LOG_ERR, CFSTR("SCDynamicStoreCreate() failed"));
				return;
			}

			__SCNetworkReachabilitySetNotifications(hn_store);

			ok = SCDynamicStoreSetDispatchQueue(hn_store, _hn_changes_queue());
			if (!ok) {
				SCLog(TRUE, LOG_ERR, CFSTR("SCDynamicStoreSetDispatchQueue() failed"));
				CFRelease(hn_store);
				hn_store = NULL;
				return;
			}

			if (!dns_configuration_watch()) {
				// if error
				SCDynamicStoreSetDispatchQueue(hn_store, NULL);
				CFRelease(hn_store);
				hn_store = NULL;
				_SCErrorSet(kSCStatusFailed);
				return;
			}

			if (!dns_refresh_enable(_hn_changes_queue(),
						hn_store,
						__SCNetworkReachabilityHandleChanges)) {
				// if error
				dns_configuration_unwatch();
				SCDynamicStoreSetDispatchQueue(hn_store, NULL);
				CFRelease(hn_store);
				hn_store = NULL;
				_SCErrorSet(kSCStatusFailed);
				return;
			}

			if (!nwi_refresh_enable(_hn_changes_queue(),
						hn_store,
						__SCNetworkReachabilityHandleChanges)) {
				// if error
				dns_configuration_unwatch();
				dns_refresh_disable();
				SCDynamicStoreSetDispatchQueue(hn_store, NULL);
				CFRelease(hn_store);
				hn_store = NULL;
				_SCErrorSet(kSCStatusFailed);
				return;
			}


			hn_targets = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
		}

		CFSetAddValue(hn_targets, target);

		ok = TRUE;
	});

	if (!ok) {
		goto done;
	}

	if (!targetPrivate->scheduled) {
		CFRunLoopSourceContext	context = { 0				// version
						  , (void *)target		// info
						  , CFRetain			// retain
						  , CFRelease			// release
						  , reachRLSCopyDescription	// copyDescription
						  , CFEqual			// equal
						  , CFHash			// hash
						  , NULL			// schedule
						  , NULL			// cancel
						  , reachPerform		// perform
						  };

		if (runLoop != NULL) {
			targetPrivate->rls    = CFRunLoopSourceCreate(NULL, 0, &context);
			targetPrivate->rlList = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
		}

		if (targetPrivate->type == reachabilityTypeName) {
			/*
			 * we're now scheduled so let's ensure that we
			 * are starting with a clean slate before we
			 * resolve the name
			 */
			if (targetPrivate->resolvedAddresses != NULL) {
				CFRelease(targetPrivate->resolvedAddresses);
				targetPrivate->resolvedAddresses = NULL;
			}
			targetPrivate->resolvedError = NETDB_SUCCESS;
			targetPrivate->needResolve = TRUE;
			_reach_set(&targetPrivate->info,
				   &NOT_REACHABLE,
				   targetPrivate->info.cycle,
				   targetPrivate->if_index,
				   targetPrivate->if_name);
			targetPrivate->info.flags |= kSCNetworkReachabilityFlagsFirstResolvePending;
		}


		targetPrivate->scheduled = TRUE;

		init = TRUE;
	}

	if (queue != NULL) {
		// retain dispatch queue
		dispatch_retain(queue);
		targetPrivate->dispatchQueue = queue;

		//
		// We've taken a reference to the client's dispatch_queue and we
		// want to hold on to that reference until we've processed any/all
		// notifications.  To facilitate this we create a group, dispatch
		// any notification blocks to via that group, and when the caller
		// has told us to stop the notifications (unschedule) we wait for
		// the group to empty and use the group's finalizer to release
		// our reference to the client's queue.
		//

		// make sure that we have group to track any async requests
		targetPrivate->dispatchGroup = dispatch_group_create();

		// retain the target ... and release it when the group is released
		CFRetain(target);
		dispatch_set_context(targetPrivate->dispatchGroup, (void *)target);
		dispatch_set_finalizer_f(targetPrivate->dispatchGroup, (dispatch_function_t)CFRelease);
	} else {
		if (!_SC_isScheduled(NULL, runLoop, runLoopMode, targetPrivate->rlList)) {
			/*
			 * if we do not already have host notifications scheduled with
			 * this runLoop / runLoopMode
			 */
			CFRunLoopAddSource(runLoop, targetPrivate->rls, runLoopMode);

		}

		_SC_schedule(target, runLoop, runLoopMode, targetPrivate->rlList);
	}

	if (init) {
		ReachabilityInfo	reach_info	= NOT_REACHABLE;

		/*
		 * if we have yet to schedule SC notifications for this address
		 * - initialize current reachability status
		 */
		if (__SCNetworkReachabilityGetFlags(target, &reach_info, TRUE)) {
			/*
			 * if reachability status available
			 * - set flags
			 * - schedule notification to report status via callback
			 */
			_reach_set(&targetPrivate->info,
				   &reach_info,
				   targetPrivate->cycle,
				   targetPrivate->if_index,
				   targetPrivate->if_name);
			__SCNetworkReachabilityPerformLocked(target);
		} else {
			/* if reachability status not available, async lookup started */
			_reach_set(&targetPrivate->info,
				   &NOT_REACHABLE,
				   targetPrivate->cycle,
				   targetPrivate->if_index,
				   targetPrivate->if_name);
		}
	}

	SCLog((_sc_debug && (_sc_log > 0)), LOG_INFO, CFSTR("%sscheduled"),
	      targetPrivate->log_prefix);

	ok = TRUE;

    done :

	MUTEX_UNLOCK(&targetPrivate->lock);
	return ok;
}


static Boolean
__SCNetworkReachabilityUnscheduleFromRunLoop(SCNetworkReachabilityRef	target,
					     CFRunLoopRef		runLoop,
					     CFStringRef		runLoopMode)
{
	dispatch_group_t		drainGroup	= NULL;
	dispatch_queue_t		drainQueue	= NULL;
	CFIndex				n		= 0;
	Boolean				ok		= FALSE;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	// hold a reference while we unschedule
	CFRetain(target);

	MUTEX_LOCK(&targetPrivate->lock);

	if (((runLoop == NULL) && (targetPrivate->dispatchQueue == NULL)) ||	// if we should be scheduled on a dispatch queue (but are not)
	    ((runLoop != NULL) && (targetPrivate->dispatchQueue != NULL))) {	// if we should be scheduled on a CFRunLoop (but are not)
		_SCErrorSet(kSCStatusInvalidArgument);
		goto done;
	}

	if (!targetPrivate->scheduled) {
		// if not currently scheduled
		_SCErrorSet(kSCStatusInvalidArgument);
		goto done;
	}

	// unschedule the target specific sources
	if (targetPrivate->dispatchQueue != NULL) {
		// save dispatchQueue, release reference when we've queue'd blocks complete, allow re-scheduling
		drainGroup = targetPrivate->dispatchGroup;
		targetPrivate->dispatchGroup = NULL;
		drainQueue = targetPrivate->dispatchQueue;
		targetPrivate->dispatchQueue = NULL;
	} else {
		if (!_SC_unschedule(target, runLoop, runLoopMode, targetPrivate->rlList, FALSE)) {
			// if not currently scheduled
			_SCErrorSet(kSCStatusInvalidArgument);
			goto done;
		}

		n = CFArrayGetCount(targetPrivate->rlList);
		if ((n == 0) || !_SC_isScheduled(NULL, runLoop, runLoopMode, targetPrivate->rlList)) {
			// if target is no longer scheduled for this runLoop / runLoopMode
			CFRunLoopRemoveSource(runLoop, targetPrivate->rls, runLoopMode);

			if (n == 0) {
				// if *all* notifications have been unscheduled
				CFRelease(targetPrivate->rlList);
				targetPrivate->rlList = NULL;
				CFRunLoopSourceInvalidate(targetPrivate->rls);
				CFRelease(targetPrivate->rls);
				targetPrivate->rls = NULL;
			}
		}
	}

	if (n == 0) {
		// if *all* notifications have been unscheduled
		targetPrivate->scheduled = FALSE;
	}

	if (n == 0) {
		if (targetPrivate->dnsActive) {
			// if we have an active [m]DNS query
			dequeueDNSQuery(target);
		}

		dispatch_sync(_hn_target_queue(), ^{
			CFSetRemoveValue(hn_targets, target);

			if (CFSetGetCount(hn_targets) > 0) {
				return;
			}

			// if we are no longer monitoring any targets
			SCDynamicStoreSetDispatchQueue(hn_store, NULL);
			CFRelease(hn_store);
			hn_store = NULL;
			CFRelease(hn_targets);
			hn_targets = NULL;

			/*
			 * until we start monitoring again, ensure that
			 * any resources associated with tracking the
			 * network changes (nwi) have been released.
			 */
			nwi_refresh_disable();

			/*
			 * until we start monitoring again, ensure that
			 * any resources associated with restarting
			 * DNS queries have been released.
			 */
			dns_refresh_disable();

			/*
			 * until we start monitoring again, ensure that
			 * any resources associated with tracking the
			 * DNS configuration have been released.
			 */
			dns_configuration_unwatch();
		});
	}

	SCLog((_sc_debug && (_sc_log > 0)), LOG_INFO, CFSTR("%sunscheduled"),
	      targetPrivate->log_prefix);

	ok = TRUE;

    done :

	MUTEX_UNLOCK(&targetPrivate->lock);

	if (drainGroup != NULL) {
		dispatch_group_notify(drainGroup, __SCNetworkReachability_concurrent_queue(), ^{
			// release group/queue references
			dispatch_release(drainQueue);
			dispatch_release(drainGroup);	// releases our target reference
		});
	}

	// release our reference
	CFRelease(target);

	return ok;
}


Boolean
SCNetworkReachabilityScheduleWithRunLoop(SCNetworkReachabilityRef	target,
					 CFRunLoopRef			runLoop,
					 CFStringRef			runLoopMode)
{
	if (!isA_SCNetworkReachability(target) || (runLoop == NULL) || (runLoopMode == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	return __SCNetworkReachabilityScheduleWithRunLoop(target, runLoop, runLoopMode, NULL);
}

Boolean
SCNetworkReachabilityUnscheduleFromRunLoop(SCNetworkReachabilityRef	target,
					   CFRunLoopRef			runLoop,
					   CFStringRef			runLoopMode)
{
	if (!isA_SCNetworkReachability(target) || (runLoop == NULL) || (runLoopMode == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	return __SCNetworkReachabilityUnscheduleFromRunLoop(target, runLoop, runLoopMode);
}

Boolean
SCNetworkReachabilitySetDispatchQueue(SCNetworkReachabilityRef	target,
				      dispatch_queue_t		queue)
{
	Boolean				ok		= FALSE;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	if (!isA_SCNetworkReachability(target)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	if (targetPrivate->type == reachabilityTypePTR) {
		/* NextBSD: hostnamed's reverse lookup */
		if (queue == NULL) {
			ptrQueryStop(targetPrivate);
			return TRUE;
		}
		if (targetPrivate->ptrRef != NULL) {
			/* already scheduled */
			return FALSE;
		}
		return ptrQueryStart(targetPrivate, queue);
	}

	if (queue != NULL) {
		ok = __SCNetworkReachabilityScheduleWithRunLoop(target, NULL, NULL, queue);
	} else {
		ok = __SCNetworkReachabilityUnscheduleFromRunLoop(target, NULL, NULL);
	}

	return ok;
}


#pragma mark -
#pragma mark PTR targets (hostnamed)


/* Build "<d>.<c>.<b>.<a>.in-addr.arpa" from an IPv4 sockaddr. */
static int
build_reverse_inaddr(const struct sockaddr *sa, char *out, size_t outsz)
{
	const struct sockaddr_in *sin;
	const uint8_t *a;
	int n;

	if (sa == NULL || sa->sa_family != AF_INET)
		return (0);	/* AF_INET6 ip6.arpa not needed by hostnamed */
	sin = (const struct sockaddr_in *)(const void *)sa;
	a = (const uint8_t *)&sin->sin_addr;
	n = snprintf(out, outsz, "%u.%u.%u.%u.in-addr.arpa",
	    a[3], a[2], a[1], a[0]);
	return (n > 0 && (size_t)n < outsz);
}

/* Decode the first label of a length-prefixed PTR rdata wire encoding
 * into a CFString. PTR rdata is the wire-format domain name:
 * length-prefixed labels terminated by a zero byte. We stitch the
 * labels back into "<label1>.<label2>..." form. Returns NULL on a
 * malformed encoding or a name pointing past the buffer. */
static CFStringRef
decode_ptr_rdata(const uint8_t *rdata, uint16_t rdlen)
{
	CFMutableStringRef name;
	size_t off = 0;
	int parts = 0;

	if (rdata == NULL || rdlen < 1)
		return (NULL);
	name = CFStringCreateMutable(NULL, 0);
	if (name == NULL)
		return (NULL);
	while (off < rdlen) {
		uint8_t label_len = rdata[off];

		if (label_len == 0)
			break;
		/* Defend against DNS compression pointers (0xC0+). uds_daemon
		 * decompresses, but defend anyway. */
		if (label_len >= 64 ||
		    (uint16_t)(off + 1 + label_len) > rdlen) {
			CFRelease(name);
			return (NULL);
		}
		if (parts > 0)
			CFStringAppendCString(name, ".",
			    kCFStringEncodingASCII);
		{
			char buf[64];
			(void)memcpy(buf, rdata + off + 1, label_len);
			buf[label_len] = '\0';
			CFStringAppendCString(name, buf,
			    kCFStringEncodingUTF8);
		}
		parts++;
		off += 1 + label_len;
	}
	if (parts == 0) {
		CFRelease(name);
		return (NULL);
	}
	return (name);
}

/* libdns_sd PTR callback. Decodes rdata, stashes the hostname, fires
 * the user callback. Runs on the dispatch queue (driven by the
 * DNSServiceProcessResult call inside our dispatch source). */
static void DNSSD_API
ptr_cb(DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t ifIdx,
    DNSServiceErrorType err, const char *fullname, uint16_t rrtype,
    uint16_t rrclass, uint16_t rdlen, const void *rdata, uint32_t ttl,
    void *context)
{
	SCNetworkReachabilityPrivateRef p = context;
	CFStringRef name;

	(void)sdRef;
	(void)flags;
	(void)ifIdx;
	(void)fullname;
	(void)rrtype;
	(void)rrclass;
	(void)ttl;

	if (p == NULL)
		return;
	p->ptrError = (int)err;
	if (err != kDNSServiceErr_NoError) {
		p->ptrFlags = 0;
	} else {
		name = decode_ptr_rdata(rdata, rdlen);
		if (name != NULL) {
			if (p->ptrResolved == NULL)
				p->ptrResolved = CFArrayCreateMutable(NULL,
				    1, &kCFTypeArrayCallBacks);
			if (p->ptrResolved != NULL)
				CFArrayAppendValue(p->ptrResolved, name);
			CFRelease(name);
			p->ptrFlags = kSCNetworkReachabilityFlagsReachable;
		} else {
			p->ptrFlags = 0;
		}
	}
	if (p->rlsFunction != NULL) {
		p->rlsFunction((SCNetworkReachabilityRef)p, p->ptrFlags,
		    p->rlsContext.info);
	}
}

/* Tear down: cancel dispatch source, deallocate libdns_sd, drop queue. */
static void
ptrQueryStop(SCNetworkReachabilityPrivateRef p)
{
	if (p->ptrSource != NULL) {
		dispatch_source_cancel(p->ptrSource);
		dispatch_release(p->ptrSource);
		p->ptrSource = NULL;
	}
	if (p->ptrRef != NULL) {
		DNSServiceRefDeallocate(p->ptrRef);
		p->ptrRef = NULL;
	}
	if (p->ptrQueue != NULL) {
		dispatch_release(p->ptrQueue);
		p->ptrQueue = NULL;
	}
}

/* Start: build the reverse name, kick the libdns_sd PTR query, wire
 * the libdns_sd socket FD into a dispatch source on the queue. */
static Boolean
ptrQueryStart(SCNetworkReachabilityPrivateRef p, dispatch_queue_t queue)
{
	char reverse[128];
	DNSServiceErrorType err;
	int fd;

	if (p->ptrAddress == NULL ||
	    CFDataGetLength(p->ptrAddress) < (CFIndex)sizeof(struct sockaddr))
		return (FALSE);
	if (!build_reverse_inaddr(
	    (const struct sockaddr *)CFDataGetBytePtr(p->ptrAddress),
	    reverse, sizeof(reverse)))
		return (FALSE);

	err = DNSServiceQueryRecord(&p->ptrRef,
	    kDNSServiceFlagsForceMulticast | kDNSServiceFlagsTimeout,
	    0 /* any interface */, reverse,
	    kDNSServiceType_PTR, kDNSServiceClass_IN,
	    ptr_cb, p);
	if (err != kDNSServiceErr_NoError) {
		p->ptrError = (int)err;
		return (FALSE);
	}
	fd = DNSServiceRefSockFD(p->ptrRef);
	if (fd < 0) {
		DNSServiceRefDeallocate(p->ptrRef);
		p->ptrRef = NULL;
		return (FALSE);
	}

	dispatch_retain(queue);
	p->ptrQueue = queue;
	p->ptrSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
	    (uintptr_t)fd, 0, queue);
	if (p->ptrSource == NULL) {
		ptrQueryStop(p);
		return (FALSE);
	}
	dispatch_source_set_event_handler(p->ptrSource, ^{
		if (p->ptrRef != NULL)
			(void)DNSServiceProcessResult(p->ptrRef);
	});
	dispatch_activate(p->ptrSource);
	return (TRUE);
}
