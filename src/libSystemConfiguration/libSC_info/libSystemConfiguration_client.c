/*
 * Copyright (c) 2012, 2013 Apple Inc. All rights reserved.
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
 * Vendored from apple-oss-distributions/configd @ configd-596.15 (the 10.9.5
 * baseline) libSystemConfiguration/libSystemConfiguration_client.c,
 * built into libSystemConfiguration. NextBSD: logs through syslog(3), has
 * no libSystem fork handlers, and handles libxpc returning NULL.
 */

#include <Availability.h>
#include <TargetConditionals.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <mach/mach.h>
#include <dispatch/dispatch.h>
#include <xpc/xpc.h>

#include "libSystemConfiguration_client.h"


#pragma mark -
#pragma mark Support functions


static void
log_xpc_object(const char *msg, xpc_object_t obj)
{
	char	*desc;

	desc = xpc_copy_description(obj);
	syslog(LOG_ERR, "%s = %s", msg, desc);
	free(desc);
}


__private_extern__
libSC_info_client_t *
libSC_info_client_create(dispatch_queue_t	q,
			 const char		*service_name,
			 const char		*service_description)
{
	xpc_connection_t	c;
	libSC_info_client_t	*client;
#if	!TARGET_IPHONE_SIMULATOR
	const uint64_t		flags	=	XPC_CONNECTION_MACH_SERVICE_PRIVILEGED;
#else	// !TARGET_IPHONE_SIMULATOR
	const uint64_t		flags	=	0;
#endif	// !TARGET_IPHONE_SIMULATOR

	client = malloc(sizeof(libSC_info_client_t));
	client->active = TRUE;
	client->service_description = strdup(service_description);
	client->service_name = strdup(service_name);

	c = xpc_connection_create_mach_service(service_name, q, flags);
	if (c == NULL) {
		// NextBSD: libxpc returns NULL when the bootstrap look-up fails
		free(client->service_description);
		free(client->service_name);
		free(client);
		return NULL;
	}

	xpc_connection_set_event_handler(c, ^(xpc_object_t xobj) {
		xpc_type_t	type;

		type = xpc_get_type(xobj);
		if (type == XPC_TYPE_DICTIONARY) {
			syslog(LOG_ERR, "%s: unexpected message", client->service_name);
			log_xpc_object("  dict = ", xobj);
		} else if (type == XPC_TYPE_ERROR) {
			if (xobj == XPC_ERROR_CONNECTION_INVALID) {
				syslog(LOG_ERR, "%s: server not available", client->service_name);
				client->active = FALSE;
			} else if (xobj == XPC_ERROR_CONNECTION_INTERRUPTED) {
				syslog(LOG_DEBUG, "%s: server failed", client->service_name);
			} else {
				const char	*desc;

				desc = xpc_dictionary_get_string(xobj, XPC_ERROR_KEY_DESCRIPTION);
				syslog(LOG_DEBUG,
					"%s: connection error: %d : %s",
					client->service_name,
					xpc_connection_get_pid(c),
					desc);
			}

		} else {
			syslog(LOG_ERR,
				"%s: unknown event type : %p",
				client->service_name,
				type);
		}
	});

	client->connection = c;

	xpc_connection_resume(c);

	return client;
}


__private_extern__
void
libSC_info_client_release(libSC_info_client_t *client)
{
	xpc_release(client->connection);
	free(client->service_description);
	free(client->service_name);
	free(client);
}


__private_extern__
xpc_object_t
libSC_send_message_with_reply_sync(libSC_info_client_t	*client,
				   xpc_object_t		message)
{
	xpc_object_t	reply;

	while (TRUE) {
		// send request to the DNS configuration server
		reply = xpc_connection_send_message_with_reply_sync(client->connection, message);
		if (reply != NULL) {
			xpc_type_t      type;

			type = xpc_get_type(reply);
			if (type == XPC_TYPE_DICTIONARY) {
				// reply available
				break;
			}

			if ((type == XPC_TYPE_ERROR) && (reply == XPC_ERROR_CONNECTION_INTERRUPTED)) {
				syslog(LOG_DEBUG,
					"%s server failure, retrying",
					client->service_description);
				// retry request
				xpc_release(reply);
				continue;
			}

			if ((type == XPC_TYPE_ERROR) && (reply == XPC_ERROR_CONNECTION_INVALID)) {
				syslog(LOG_ERR,
					"%s server not available",
					client->service_description);
				client->active = FALSE;
			} else {
				syslog(LOG_ERR,
					"%s xpc_connection_send_message_with_reply_sync() with unexpected reply",
					client->service_description);
				log_xpc_object("  reply", reply);
			}

			xpc_release(reply);
			reply = NULL;
			break;
		}

		// NextBSD: libxpc returns NULL, not an XPC error object, when the
		// connection is bad or no reply came. Retrying would never end.
		syslog(LOG_ERR, "%s server did not reply", client->service_description);
		client->active = FALSE;
		break;
	}

	return reply;
}
