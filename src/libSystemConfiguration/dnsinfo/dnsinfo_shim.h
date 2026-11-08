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
 * dnsinfo_shim.h -- the SystemConfiguration SPI dnsinfo expects from
 * <SystemConfiguration/SCPrivate.h>, which NextBSD's libSystemConfiguration
 * does not carry: the _SC_* helpers from configd-596.15
 * SystemConfiguration.fproj/SCDPrivate.c and SCPrivate.h, as static
 * inlines shared by ipconfigd and scutil.
 */
#ifndef _DNSINFO_SHIM_H_
#define	_DNSINFO_SHIM_H_

#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <CoreFoundation/CoreFoundation.h>

#ifndef __private_extern__
#define	__private_extern__	__attribute__((__visibility__("hidden")))
#endif

static inline __unused Boolean
_SC_CFEqual(CFTypeRef val1, CFTypeRef val2)
{
	if (val1 == val2) {
		return TRUE;
	}
	if (val1 != NULL && val2 != NULL) {
		return CFEqual(val1, val2);
	}
	return FALSE;
}

static inline __unused char *
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

static inline __unused void
_SC_sockaddr_to_string(const struct sockaddr *address, char *buf, size_t bufLen)
{
	union {
		const struct sockaddr		*sa;
		const struct sockaddr_in	*sin;
		const struct sockaddr_in6	*sin6;
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

/*
 * NextBSD: ipconfigd hands over Setup's link-local servers as "fe80::1%em0"
 * (accumulate_servers() in dns_config.c), and inet_pton() does not take a
 * scope, so it is split off first and turned into sin6_scope_id.
 */
static inline __unused struct sockaddr *
_SC_string_to_sockaddr(const char *str, sa_family_t af, void *buf, size_t bufLen)
{
	union {
		void			*buf;
		struct sockaddr		*sa;
		struct sockaddr_in	*sin;
		struct sockaddr_in6	*sin6;
	} addr;
	char		host[INET6_ADDRSTRLEN];
	const char	*p;

	addr.buf = buf;
	bzero(addr.buf, bufLen);

	p = strchr(str, '%');
	if ((p != NULL) && ((size_t)(p - str) < sizeof(host))) {
		bcopy(str, host, p - str);
		host[p - str] = '\0';
	} else {
		(void)strlcpy(host, str, sizeof(host));
		p = NULL;
	}

	if (((af == AF_UNSPEC) || (af == AF_INET)) &&
	    (bufLen >= sizeof(struct sockaddr_in)) &&
	    inet_aton(str, &addr.sin->sin_addr) == 1) {
		// if IPv4 address
		addr.sin->sin_len    = sizeof(struct sockaddr_in);
		addr.sin->sin_family = AF_INET;
	} else if (((af == AF_UNSPEC) || (af == AF_INET6)) &&
		   (bufLen >= sizeof(struct sockaddr_in6)) &&
		   inet_pton(AF_INET6, host, &addr.sin6->sin6_addr) == 1) {
		// if IPv6 address
		addr.sin6->sin6_len    = sizeof(struct sockaddr_in6);
		addr.sin6->sin6_family = AF_INET6;

		if (p != NULL) {
			addr.sin6->sin6_scope_id = if_nametoindex(p + 1);
		}
	} else {
		addr.buf = NULL;
	}

	return addr.sa;
}

static inline __unused CFStringRef
_SC_trimDomain(CFStringRef domain)
{
	CFIndex	length;

	if ((domain == NULL) || (CFGetTypeID(domain) != CFStringGetTypeID())) {
		return NULL;
	}

	// remove any leading/trailing dots
	length = CFStringGetLength(domain);
	if ((length > 0) &&
	    (CFStringFindWithOptions(domain,
				     CFSTR("."),
				     CFRangeMake(0, 1),
				     kCFCompareAnchored,
				     NULL) ||
	     CFStringFindWithOptions(domain,
				     CFSTR("."),
				     CFRangeMake(0, length),
				     kCFCompareAnchored|kCFCompareBackwards,
				     NULL))) {
		CFMutableStringRef	trimmed;

		trimmed = CFStringCreateMutableCopy(NULL, 0, domain);
		CFStringTrim(trimmed, CFSTR("."));
		domain = (CFStringRef)trimmed;
		length = CFStringGetLength(domain);
	} else {
		CFRetain(domain);
	}

	if (length == 0) {
		CFRelease(domain);
		return NULL;
	}

	return domain;
}

#endif /* _DNSINFO_SHIM_H_ */
