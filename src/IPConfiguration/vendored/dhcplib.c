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
 * dhcplib.c
 * NextBSD: dhcp_packet_print() from bootp-268.1 bootplib/dhcplib.c, printing
 * to a FILE rather than through a CFString and SCPrint(), with the same
 * output. sname and file are not always NUL terminated, so their print is
 * bounded to the field.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <net/if_arp.h>
#include <string.h>
#include "dhcp_options.h"

void
dhcp_packet_fprint(FILE * f, struct dhcp * dp, int pkt_len)
{
    int 		i;
    int			j;
    int			len;

    if (pkt_len < sizeof(struct dhcp)) {
	fprintf(f, "Packet is too short %d < %d\n", pkt_len,
		(int)sizeof(struct dhcp));
	return;
    }
    fprintf(f, "op = ");
    if (dp->dp_op == BOOTREQUEST) {
	fprintf(f, "BOOTREQUEST\n");
    }
    else if (dp->dp_op == BOOTREPLY) {
	fprintf(f, "BOOTREPLY\n");
    }
    else {
	i = dp->dp_op;
	fprintf(f, "OP(%d)\n", i);
    }
    
    i = dp->dp_htype;
    fprintf(f, "htype = %d\n", i);
    
    fprintf(f, "flags = %x\n", ntohs(dp->dp_flags));
    len = dp->dp_hlen;
    fprintf(f, "hlen = %d\n", len);
    
    i = dp->dp_hops;
    fprintf(f, "hops = %d\n", i);
    
    fprintf(f, "xid = %lu\n", (u_long)ntohl(dp->dp_xid));
    
    fprintf(f, "secs = %hu\n", ntohs(dp->dp_secs));
    
    fprintf(f, "ciaddr = %s\n", inet_ntoa(dp->dp_ciaddr));
    fprintf(f, "yiaddr = %s\n", inet_ntoa(dp->dp_yiaddr));
    fprintf(f, "siaddr = %s\n", inet_ntoa(dp->dp_siaddr));
    fprintf(f, "giaddr = %s\n", inet_ntoa(dp->dp_giaddr));
    
    fprintf(f, "chaddr = ");
    for (j = 0; j < len; j++) {
	i = dp->dp_chaddr[j];
	fprintf(f, "%0x", i);
	if (j < (len - 1)) fprintf(f, ":");
    }
    fprintf(f, "\n");
    
    fprintf(f, "sname = %.*s\n", (int)sizeof(dp->dp_sname),
	    (const char *)dp->dp_sname);
    fprintf(f, "file = %.*s\n", (int)sizeof(dp->dp_file),
	    (const char *)dp->dp_file);
    
    {
	dhcpol_t t;
	
	dhcpol_init(&t);
	if (dhcpol_parse_packet(&t, dp, pkt_len, NULL)) {
	    fprintf(f, "options:\n");
	    dhcpol_fprint(f, &t);
	}
	dhcpol_free(&t);
    }
    return;
}

void
dhcp_packet_print(struct dhcp *dp, int pkt_len)
{
    dhcp_packet_fprint(stdout, dp, pkt_len);
    (void)fflush(stdout);
    return;
}
