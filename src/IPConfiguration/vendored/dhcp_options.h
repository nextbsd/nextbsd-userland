/*
 * Copyright (c) 1999-2013 Apple Inc. All rights reserved.
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
 * dhcp_options.h
 * - routines to parse and access dhcp options
 *
 * NextBSD: trimmed from bootp-268.1 bootplib/dhcp_options.h, see
 * dhcp_options.c.
 */


#ifndef _S_DHCP_OPTIONS_H
#define _S_DHCP_OPTIONS_H

/*
 * Modification History
 *
 * November 23, 1999	Dieter Siegmund (dieter@apple)
 * - created
 */

#include <stdio.h>
#include <mach/mach.h>
#include "ptrlist.h"
#include "dhcp_packet.h"
#include "gen_dhcp_tags.h"
#include "gen_dhcp_types.h"

#define DHCP_OPTION_TAG_MAX	255

/*
 * DHCP_OPTION_SIZE_MAX
 * - the largest size that an option can be (limited to an 8-bit quantity)
 */
#define DHCP_OPTION_SIZE_MAX	255

#define TAG_OFFSET	0
#define LEN_OFFSET	1
#define OPTION_OFFSET	2

typedef struct {
    char	str[256];
} dhcpo_err_str_t;

/*
 * Module: dhcpol (dhcp options list)
 *
 * Purpose:
 *   Routines to parse and retrieve dhcp options.
 */

typedef ptrlist_t dhcpol_t;

void			dhcpol_init(dhcpol_t * list);
void			dhcpol_free(dhcpol_t * list);
int			dhcpol_count(dhcpol_t * list);
boolean_t		dhcpol_add(dhcpol_t * list, void * element);
void *			dhcpol_element(dhcpol_t * list, int i);
boolean_t		dhcpol_concat(dhcpol_t * list, dhcpol_t * extra);
boolean_t		dhcpol_parse_buffer(dhcpol_t * list, void * buffer,
					    int length, dhcpo_err_str_t * err);
void *			dhcpol_find(dhcpol_t * list, int tag, int * len_p,
				    int * start);
boolean_t		dhcpol_parse_packet(dhcpol_t * options,
					    struct dhcp * pkt, int len,
					    dhcpo_err_str_t * err);
void			dhcpol_fprint(FILE * f, dhcpol_t * list);

/*
 * Functions: dhcptype_*, dhcptag_*
 *
 * Purpose:
 *   Get tag and type information as well as do conversions.
 */

const dhcptype_info_t *	dhcptype_info(dhcptype_t type);
boolean_t		dhcptype_to_str(char * str, size_t str_len, const void * opt,
					int len, int type,
					dhcpo_err_str_t * err);
void			dhcptype_fprint_simple(FILE * f, dhcptype_t type,
					       const void * opt, int option_len);
void			dhcptype_fprint(FILE * f, dhcptype_t type,
					const void * option, int option_len);

const dhcptag_info_t *	dhcptag_info(dhcptag_t tag);
dhcptag_t		dhcptag_with_name(const char * name);
const char *		dhcptag_name(int tag);
boolean_t		dhcptag_to_str(char * tmp, size_t tmplen, int tag,
				       const void * opt, int len,
				       dhcpo_err_str_t * err);

/* dhcplib.c */
void			dhcp_packet_fprint(FILE * f, struct dhcp * dp,
					   int pkt_len);
void			dhcp_packet_print(struct dhcp * dp, int pkt_len);

#endif /* _S_DHCP_OPTIONS_H */
