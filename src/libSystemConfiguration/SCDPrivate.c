/*
 * Copyright (c) 2000-2013 Apple Inc. All rights reserved.
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
 * SCDPrivate.c — NextBSD port of the "Miscellaneous" helpers from
 * configd-596.15 SystemConfiguration.fproj/SCDPrivate.c. Declared in
 * <SystemConfiguration/SCPrivate.h>; hostnamed and scutil share them
 * from here.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SCPrivate.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>


#pragma mark -
#pragma mark Miscellaneous


char *
_SC_cfstring_to_cstring(CFStringRef cfstr, char *buf, CFIndex bufLen, CFStringEncoding encoding)
{
	CFIndex	converted;
	CFIndex	last	= 0;
	CFIndex	len;

	if (cfstr == NULL) {
		cfstr = CFSTR("");
	}
	len = CFStringGetLength(cfstr);

	/* how much buffer space will we really need? */
	converted = CFStringGetBytes(cfstr,
				     CFRangeMake(0, len),
				     encoding,
				     0,
				     FALSE,
				     NULL,
				     0,
				     &last);
	if (converted < len) {
		/* if full string could not be converted */
		if (buf != NULL) {
			buf[0] = '\0';
		}
		return NULL;
	}

	if (buf != NULL) {
		if (bufLen < (last + 1)) {
			/* if the size of the provided buffer is too small */
			buf[0] = '\0';
			return NULL;
		}
	} else {
		/* allocate a buffer */
		bufLen = last + 1;
		buf = CFAllocatorAllocate(NULL, bufLen, 0);
		if (buf == NULL) {
			return NULL;
		}
	}

	(void)CFStringGetBytes(cfstr,
			       CFRangeMake(0, len),
			       encoding,
			       0,
			       FALSE,
			       (UInt8 *)buf,
			       bufLen,
			       &last);
	buf[last] = '\0';

	return buf;
}


void
_SC_sockaddr_to_string(const struct sockaddr *address, char *buf, size_t bufLen)
{
	union {
		const struct sockaddr		*sa;
		const struct sockaddr_in	*sin;
		const struct sockaddr_in6	*sin6;
		const struct sockaddr_dl	*sdl;
	} addr;

	addr.sa = address;

	bzero(buf, bufLen);
	switch (address->sa_family) {
		case AF_INET :
			(void)inet_ntop(addr.sin->sin_family,
					&addr.sin->sin_addr,
					buf,
					bufLen);
			break;
		case AF_INET6 : {
			(void)inet_ntop(addr.sin6->sin6_family,
					&addr.sin6->sin6_addr,
					buf,
					bufLen);
			if (addr.sin6->sin6_scope_id != 0) {
				int	n;

				n = strlen(buf);
				if ((n+IF_NAMESIZE+1) <= (int)bufLen) {
					buf[n++] = '%';
					if_indextoname(addr.sin6->sin6_scope_id, &buf[n]);
				}
			}
			break;
		}
		default :
			snprintf(buf, bufLen, "unexpected address family %d", address->sa_family);
			break;
	}

	return;
}


struct sockaddr *
_SC_string_to_sockaddr(const char *str, sa_family_t af, void *buf, size_t bufLen)
{
	union {
		void			*buf;
		struct sockaddr		*sa;
		struct sockaddr_in	*sin;
		struct sockaddr_in6	*sin6;
	} addr;

	if (buf == NULL) {
		bufLen = sizeof(struct sockaddr_storage);
		addr.buf = CFAllocatorAllocate(NULL, bufLen, 0);
	} else {
		addr.buf = buf;
	}

	bzero(addr.buf, bufLen);
	if (((af == AF_UNSPEC) || (af == AF_INET)) &&
	    (bufLen >= sizeof(struct sockaddr_in)) &&
	    inet_aton(str, &addr.sin->sin_addr) == 1) {
		// if IPv4 address
		addr.sin->sin_len    = sizeof(struct sockaddr_in);
		addr.sin->sin_family = AF_INET;
	} else if (((af == AF_UNSPEC) || (af == AF_INET6)) &&
		   (bufLen >= sizeof(struct sockaddr_in6)) &&
		   inet_pton(AF_INET6, str, &addr.sin6->sin6_addr) == 1) {
		// if IPv6 address
		char	*p;

		addr.sin6->sin6_len    = sizeof(struct sockaddr_in6);
		addr.sin6->sin6_family = AF_INET6;

		// configd-596.15 searched `buf` here (the sockaddr, NULL when
		// the caller asks us to allocate); configd-1405 searches `str`.
		p = strchr(str, '%');
		if (p != NULL) {
			addr.sin6->sin6_scope_id = if_nametoindex(p + 1);
		}

		if (IN6_IS_ADDR_LINKLOCAL(&addr.sin6->sin6_addr) ||
		    IN6_IS_ADDR_MC_LINKLOCAL(&addr.sin6->sin6_addr)) {
			uint16_t	if_index;

			if_index = ntohs(addr.sin6->sin6_addr.__u6_addr.__u6_addr16[1]);
			addr.sin6->sin6_addr.__u6_addr.__u6_addr16[1] = 0;
			if (addr.sin6->sin6_scope_id == 0) {
				// use the scope id that was embedded in the [link local] IPv6 address
				addr.sin6->sin6_scope_id = if_index;
			}
		}
	} else {
		if (addr.buf != buf) {
			CFAllocatorDeallocate(NULL, addr.buf);
		}
		addr.buf = NULL;
	}

	return addr.sa;
}
