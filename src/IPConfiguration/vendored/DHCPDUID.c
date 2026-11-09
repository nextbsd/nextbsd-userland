/*
 * Copyright (c) 2009-2013 Apple Inc. All rights reserved.
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
 * DHCPDUID.c
 *
 * NextBSD: Apple's bootp-268.1 bootplib/DHCPDUID.c. Printing goes to a
 * FILE rather than through a CFString, with the same output.
 */

/* 
 * Modification History
 *
 * September 30, 2009		Dieter Siegmund (dieter@apple.com)
 * - created
 */

#include "DHCPDUID.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <mach/mach.h>
#include "symbol_scope.h"

STATIC void
fprint_hex(FILE * file, const uint8_t * bytes, int length, char separator)
{
    int i;

    for (i = 0; i < length; i++) {
	if (i != 0) {
	    fputc(separator, file);
	}
	fprintf(file, "%02x", bytes[i]);
    }
    return;
}

void
DHCPDUIDFPrint(FILE * file, const DHCPDUIDRef duid, int duid_len)
{
    int			required_len;
    DHCPDUIDType	type;

    required_len = offsetof(DHCPDUID_LLT, hardware_type);
    if (duid_len < required_len) {
	goto too_short;
    }
    type = DHCPDUIDGetType(duid);
    switch (type) {
    case kDHCPDUIDTypeLLT:
	required_len = offsetof(DHCPDUID_LLT, linklayer_address);
	if (duid_len < required_len) {
	    goto too_short;
	}
	fprintf(file, "DUID LLT HW %d Time %u Addr ",
			     DHCPDUID_LLTGetHardwareType(&duid->llt),
			     DHCPDUID_LLTGetTime(&duid->llt));
	fprint_hex(file, duid->llt.linklayer_address,
				    duid_len - required_len, ':');
	break;
    case kDHCPDUIDTypeEN:
	required_len = offsetof(DHCPDUID_EN, identifier);
	if (duid_len < required_len) {
	    goto too_short;
	}
	fprintf(file, "DUID EN Num %d Id ",
			     DHCPDUID_ENGetEnterpriseNumber(&duid->en));
	fprint_hex(file, duid->en.identifier,
				    duid_len - required_len, ':');
	break;
    case kDHCPDUIDTypeLL:
	required_len = offsetof(DHCPDUID_LL, linklayer_address);
	if (duid_len < required_len) {
	    goto too_short;
	}
	fprintf(file, "DUID LL HW %d Addr ",
			     DHCPDUID_LLGetHardwareType(&duid->ll));
	fprint_hex(file, duid->ll.linklayer_address,
				    duid_len - required_len, ':');
	break;
    default:
	fprintf(file, "DUID (unrecognized type=%d): ",
			     type);
	fprint_hex(file, (const uint8_t *)duid, duid_len, ' ');
	break;
    }
    return;

 too_short:
    fprintf(file, "DUID too short (%d < %d), Data = { ",
			 duid_len, required_len);
    fprint_hex(file, (const uint8_t *)duid, duid_len, ' ');
    fprintf(file, " }");
    return;
}

bool
DHCPDUIDIsValid(const DHCPDUIDRef duid, int duid_len)
{
    int			required_len;
    DHCPDUIDType	type;

    required_len = offsetof(DHCPDUID_LLT, hardware_type);
    if (duid_len < required_len) {
	return (FALSE);
    }
    type = DHCPDUIDGetType(duid);
    switch (type) {
    case kDHCPDUIDTypeLLT:
	required_len = offsetof(DHCPDUID_LLT, linklayer_address);
	if (duid_len <= required_len) {
	    return (FALSE);
	}
	break;
    case kDHCPDUIDTypeEN:
	required_len = offsetof(DHCPDUID_EN, identifier);
	if (duid_len <= required_len) {
	    return (FALSE);
	}
	break;
    case kDHCPDUIDTypeLL:
	required_len = offsetof(DHCPDUID_LL, linklayer_address);
	if (duid_len <= required_len) {
	    return (FALSE);
	}
	break;
    default:
	break;
    }
    return (TRUE);
}
