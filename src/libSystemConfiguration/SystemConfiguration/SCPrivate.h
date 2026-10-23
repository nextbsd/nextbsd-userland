/*
 * Copyright (c) 2000-2023 Apple Inc. All rights reserved.
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
 * @APPLE_LICENSE_HEADER_END@
 */

/*
 * SCPrivate.h — SystemConfiguration SPI. NextBSD port of the private
 * header from apple-oss-distributions/configd/SystemConfiguration.fproj/
 * SCPrivate.h, populated as in-tree consumers need it.
 *
 * Only the notify(3) names are exported so far: configd's
 * KernelEventMonitor posts _SC_NOTIFY_NETWORK_CHANGE on network state
 * changes and set-hostname.c (hostnamed) registers for it. The values
 * are Apple's stable notification names; do not edit them.
 */

#ifndef _SCPRIVATE_H
#define _SCPRIVATE_H

/* notify(3) keys */

#define _SC_NOTIFY_PREFIX			"com.apple.system.config"

#define _SC_NOTIFY_NETWORK_CHANGE		_SC_NOTIFY_PREFIX ".network_change"
#define _SC_NOTIFY_NETWORK_CHANGE_DNS		_SC_NOTIFY_NETWORK_CHANGE ".dns"
#define _SC_NOTIFY_NETWORK_CHANGE_NWI		_SC_NOTIFY_NETWORK_CHANGE ".nwi"
#define _SC_NOTIFY_NETWORK_CHANGE_PROXY		_SC_NOTIFY_NETWORK_CHANGE ".proxy"

#endif	/* _SCPRIVATE_H */
