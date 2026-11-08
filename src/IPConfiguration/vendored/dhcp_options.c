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
 * dhcp_options.c
 * - routines to parse and access dhcp options
 * - handles overloaded areas as well as vendor-specific options
 *   that are encoded using the RFC 2132 encoding
 *
 * NextBSD: Apple's bootp-268.1 bootplib/dhcp_options.c, the option list
 * (dhcpol_*) and tag/type (dhcptag_*, dhcptype_*) halves. The option-area
 * builder (dhcpoa_*) and the string-to-option conversions have no caller
 * here. Printing goes to a FILE rather than through a CFString and
 * SCPrint(), with the same output.
 */

/*
 * Modification History
 *
 * November 23, 1999	Dieter Siegmund (dieter@apple)
 * - created from objective C version (dhcpOptions.m)
 * - generalized code so that the file, sname and vendor specific
 *   areas could be parsed using the same routines
 * - cleanly separated functions that deal with existing option areas
 *   (dhcpol_*) from those that create new option areas (dhpoa_*).
 */
#include "dhcp_packet.h"

#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <strings.h>

#include "rfc_options.h"
#include "gen_dhcp_types.h"

#include "dhcp_options.h"
#include "gen_dhcp_parse_table.h"
#include "DNSNameList.h"
#include "nbo.h"

#define MAX_TAG (sizeof(dhcptag_info_table) / sizeof(dhcptag_info_table[0]))

/* util.h */
#define IP_FORMAT	"%d.%d.%d.%d"
#define IP_CH(ip, i)	(((const uint8_t *)(ip))[i])
#define IP_LIST(ip)	IP_CH(ip,0),IP_CH(ip,1),IP_CH(ip,2),IP_CH(ip,3)

static const uint8_t	rfc_magic[] = RFC_OPTIONS_MAGIC;

/*
 * Function: dhcptype_info
 *
 * Purpose:
 *   Return the type information for the given type.
 */
const dhcptype_info_t *
dhcptype_info(dhcptype_t type) 
{
    if (type > dhcptype_last_e)
	return (NULL);
    return dhcptype_info_table + type;
}

/*
 * Function: print_data_fprint
 * Purpose:
 *   Displays the buffer as a series of 8-bit hex numbers with an ASCII
 *   representation off to the side.
 */
static void
print_data_fprint(FILE * f, const uint8_t * data_p, int n_bytes)
{
#define CHARS_PER_LINE 	16
    char		line_buf[CHARS_PER_LINE + 1];
    int			line_pos;
    int			offset;

    for (line_pos = 0, offset = 0; offset < n_bytes; offset++, data_p++) {
	if (line_pos == 0) {
	    fprintf(f, "%04x ", offset);
	}

	line_buf[line_pos] = isprint(*data_p) ? *data_p : '.';
	fprintf(f, " %02x", *data_p);
	line_pos++;
	if (line_pos == CHARS_PER_LINE) {
	    line_buf[CHARS_PER_LINE] = '\0';
	    fprintf(f, "  %s\n", line_buf);
	    line_pos = 0;
	}
	else if (line_pos == (CHARS_PER_LINE / 2))
	    fprintf(f, " ");
    }
    if (line_pos) { /* need to finish up the line */
	char * extra_space = "";
	if (line_pos < (CHARS_PER_LINE / 2)) {
	    extra_space = " ";
	}
	for (; line_pos < CHARS_PER_LINE; line_pos++) {
	    fprintf(f, "   ");
	    line_buf[line_pos] = ' ';
	}
	line_buf[CHARS_PER_LINE] = '\0';
	fprintf(f, "  %s%s\n", extra_space, line_buf);
    }
    return;
}

/*
 * Function: dhcptype_to_str
 *
 * Purpose:
 *   Give a string representation to the given type.
 */
boolean_t
dhcptype_to_str(char * tmp, size_t maxtmplen,
		const void * opt, int len, int type,
		dhcpo_err_str_t * err)
{
    const dhcptype_info_t *	type_info = dhcptype_info(type);

    /* NextBSD: an option can be shorter than its type (malformed, or empty) */
    if (type_info != NULL && len < type_info->size) {
	if (err) {
	    snprintf(err->str, sizeof(err->str),
		     "type %s: option too short (%d < %d)", type_info->name,
		     len, type_info->size);
	}
	return (FALSE);
    }
    switch (type) {
    case dhcptype_bool_e:
	/* NextBSD: the option is one byte, not a boolean_t */
	snprintf(tmp, maxtmplen, "%d", *((const uint8_t *)opt));
	break;
    case dhcptype_uint8_e:
	snprintf(tmp, maxtmplen, "%d", *((uint8_t *)opt));
	break;
    case dhcptype_uint16_e:
	snprintf(tmp, maxtmplen, "%d", net_uint16_get(opt));
	break;
    case dhcptype_int32_e:
	snprintf(tmp, maxtmplen, "%d", net_uint32_get(opt));
	break;
    case dhcptype_uint32_e:
	snprintf(tmp, maxtmplen, "%u", net_uint32_get(opt));
	break;
    case dhcptype_ip_e:
	snprintf(tmp, maxtmplen, IP_FORMAT, IP_LIST(opt));
	break;
    case dhcptype_string_e:
	if (len < maxtmplen) {
	    strncpy(tmp, opt, len);
	    tmp[len] = '\0';
	}
	else {
	    snprintf(err->str, sizeof(err->str),
		     "type dhcptype_string_e: string too long");
	    return (FALSE);
	}
	break;
    default:
	if (err) {
	    snprintf(err->str, sizeof(err->str),
		     "type %d: not supported", type);
	    return (FALSE);
	    break;
	}
    }
    return (TRUE);
}

void
dhcptype_fprint_simple(FILE * f, dhcptype_t type,
		       const void * opt, int option_len)
{
    const uint8_t *	option = opt;
    const dhcptype_info_t * type_info = dhcptype_info(type);

    /* NextBSD: an option can be shorter than its type (malformed, or empty) */
    if (type_info != NULL && option_len < type_info->size) {
	return;
    }
    switch (type) {
      case dhcptype_bool_e:
	  fprintf(f, "%s", *option ? "TRUE" : "FALSE");
	  break;
	
      case dhcptype_ip_e:
	  fprintf(f, IP_FORMAT, IP_LIST(opt));
	  break;
	
      case dhcptype_string_e: {
	  fprintf(f, "%.*s", option_len, (char *)option);
	  break;
      }
	
      case dhcptype_opaque_e:
	fprintf(f, "\n");
	print_data_fprint(f, option, option_len);
	break;

      case dhcptype_uint8_e:
	fprintf(f, "0x%x", *option);
	break;

      case dhcptype_uint16_e:
	fprintf(f, "0x%x", net_uint16_get(option));
	break;

      case dhcptype_int32_e:
      case dhcptype_uint32_e:
	fprintf(f, "0x%x", net_uint32_get(option));
	break;

      case dhcptype_dns_namelist_e: {
	  int			i;
	  const char * *	namelist;
	  int			namelist_length = 0;

	  namelist = DNSNameListCreate(option, option_len, &namelist_length);
	  fprintf(f, "{");
	  if (namelist != NULL) {
	      for (i = 0; i < namelist_length; i++) {
		  fprintf(f, "%s%s", (i == 0) ? "" : ", ",
			  namelist[i]);
	      }
	      free(namelist);
	  }
	  fprintf(f, "}");
	  break;
      }
	
      case dhcptype_none_e:
      default:
	break;
    }
    return;
}

void
dhcptype_fprint(FILE * f, dhcptype_t type, const void * option, int option_len)
{
    const dhcptype_info_t * 	type_info = dhcptype_info(type);

    if (type_info && type_info->multiple_of != dhcptype_none_e) {
	int 			i;
	int 			number;
	const uint8_t *		offset;
	int 			size;
	const dhcptype_info_t * subtype_info;

	subtype_info = dhcptype_info(type_info->multiple_of);
	if (subtype_info == NULL)
	    return;
	size = subtype_info->size;
	number = option_len / size;
	fprintf(f, "{");
	for (i = 0, offset = option; i < number; i++) {
	    if (i != 0)
		fprintf(f, ", ");
	    dhcptype_fprint_simple(f, type_info->multiple_of,
					offset, size);
	    offset += size;
	}
	fprintf(f, "}");
    }
    else {
	dhcptype_fprint_simple(f, type, option, option_len);
    }
    return;
}

static boolean_t
dhcptag_fprint(FILE * f, const void * vopt)
{
    const dhcptag_info_t * entry;
    const uint8_t *	opt = vopt;
    uint8_t 		tag = opt[TAG_OFFSET];
    uint8_t 		option_len = opt[LEN_OFFSET];
    const uint8_t * 	option = opt + OPTION_OFFSET;

    entry = dhcptag_info(tag);
    if (entry == NULL)
	return (FALSE);
    {	
	const dhcptype_info_t * type = dhcptype_info(entry->type);
	
	if (type == NULL) {
	    fprintf(f, "unknown type %d\n", entry->type);
	    return (FALSE);
	}
	fprintf(f, "%s (%s): ", entry->name, type->name);
	if (tag == dhcptag_dhcp_message_type_e)
	    fprintf(f, "%s ", dhcp_msgtype_names(*option));
	dhcptype_fprint(f, entry->type, option, option_len);
	fprintf(f, "\n");
    }
    return (TRUE);
}

/*
 * Function: dhcptag_info
 *
 * Purpose:
 *   Return the tag information for the give tag.
 */
const dhcptag_info_t *
dhcptag_info(dhcptag_t tag)
{
    if (tag >= MAX_TAG)
	return (NULL);
    return dhcptag_info_table + tag;
}

dhcptag_t
dhcptag_with_name(const char * name)
{
    dhcptag_t 		i;
    unsigned int 	v;

    for (i = 0; i < MAX_TAG; i++) {
	if (dhcptag_info_table[i].name) {
	    if (strcmp(dhcptag_info_table[i].name, name) == 0)
		return (i);
	}
    }
    if (strncmp(name, "option_", 7) == 0) {
	v = strtoul(name + 7, NULL, 10);
    }
    else {
	v = strtoul(name, NULL, 10);
    }
    if (v <= MAX_TAG) {
	return (v);
    }
    return (-1);
}

/*
 * Function: dhcptag_name
 * Purpose:
 *   Return the name of the tag.
 */
const char *
dhcptag_name(int tag)
{
    const dhcptag_info_t * info;

    info = dhcptag_info(tag);
    if (info == NULL)
	return (NULL);
    return (info->name);
}

/*
 * Function: dhcptag_to_str
 *
 * Purpose:
 *   Give a string representation to the given tag.
 * Note:
 *   For tags whose type is a multiple of a type, this routine
 *   only returns the string representation for the first
 *   occurrence.
 */
boolean_t
dhcptag_to_str(char * tmp, size_t tmplen, int tag, const void * opt,
	       int len, dhcpo_err_str_t * err)
{
    const dhcptag_info_t * 	tag_info;
    const dhcptype_info_t * 	type_info;
    dhcptype_t			type;

    tag_info = dhcptag_info(tag);
    if (tag_info == NULL)
	return (FALSE);
    type_info = dhcptype_info(tag_info->type);
    if (type_info->multiple_of == dhcptype_none_e)
	type = tag_info->type;
    else
	type = type_info->multiple_of;
    return (dhcptype_to_str(tmp, tmplen, opt, len, type, err));
}

/*
 * Functions: dhcpol_*
 *
 * Purpose:
 *   Routines to parse/access existing options buffers.
 */

/*
 * Functions: dhcpol_* 
 *
 * Purpose:
 *   Routines to parse/access existing options buffers.
 */
boolean_t
dhcpol_add(dhcpol_t * list, void * element)
{
    return (ptrlist_add((ptrlist_t *)list, element));
}

int
dhcpol_count(dhcpol_t * list)
{
    return (ptrlist_count((ptrlist_t *)list));
}

void *
dhcpol_element(dhcpol_t * list, int i)
{
    return (ptrlist_element((ptrlist_t *)list, i));
}

void
dhcpol_init(dhcpol_t * list)
{
    ptrlist_init((ptrlist_t *)list);
}

void
dhcpol_free(dhcpol_t * list)
{
    ptrlist_free((ptrlist_t *)list);
}

boolean_t
dhcpol_concat(dhcpol_t * list, dhcpol_t * extra)
{
    return (ptrlist_concat((ptrlist_t *)list, (ptrlist_t *)extra));
}

/*
 * Function: dhcpol_parse_buffer
 *
 * Purpose:
 *   Parse the given buffer into DHCP options, returning the
 *   list of option pointers in the given dhcpol_t.
 *   Parsing continues until we hit the end of the buffer or
 *   the end tag.
 */
boolean_t
dhcpol_parse_buffer(dhcpol_t * list, void * buffer, int length,
		    dhcpo_err_str_t * err)
{
    int		len;
    uint8_t *	scan;
    uint8_t	tag;

    if (err)
	err->str[0] = '\0';

    dhcpol_init(list);

    len = length;
    tag = dhcptag_pad_e;
    for (scan = (uint8_t *)buffer; tag != dhcptag_end_e && len > 0; ) {

	tag = scan[TAG_OFFSET];

	switch (tag) {
	  case dhcptag_end_e:
	      dhcpol_add(list, scan); /* remember that it was terminated */
	      scan++;
	      len--;
	      break;
	  case dhcptag_pad_e: /* ignore pad */
	      scan++;
	      len--;
	      break;
	  default: {
	      uint8_t	option_len;

	      /* NextBSD: don't read a length byte past the end of the area */
	      if (len < 2) {
		  len = -1;
		  break;
	      }
	      option_len = scan[LEN_OFFSET];
	      dhcpol_add(list, scan);
	      len -= (option_len + 2);
	      scan += (option_len + 2);
	      break;
	  }
	}
    }
    if (len < 0) {
	/* ran off the end */
	if (err)
	    snprintf(err->str, sizeof(err->str), "parse failed near tag %d", tag);
	dhcpol_free(list);
	return (FALSE);
    }
    return (TRUE);
}

/*
 * Function: dhcpol_find
 *
 * Purpose:
 *   Finds the first occurence of the given option, and returns its
 *   length and the option data pointer.
 *
 *   The optional start parameter allows this function to 
 *   return the next start point so that successive
 *   calls will retrieve the next occurence of the option.
 *   Before the first call, *start should be set to 0.
 */
void *
dhcpol_find(dhcpol_t * list, int tag, int * len_p, int * start)
{
    int 	i = 0;

    if (tag == dhcptag_end_e || tag == dhcptag_pad_e)
	return (NULL);

    if (start)
	i = *start;

    for (; i < dhcpol_count(list); i++) {
	uint8_t * option = dhcpol_element(list, i);
	
	if (option[TAG_OFFSET] == tag) {
	    if (len_p)
		*len_p = option[LEN_OFFSET];
	    if (start)
		*start = i + 1;
	    return (option + OPTION_OFFSET);
	}
    }
    return (NULL);
}

/*
 * Function: dhcpol_parse_packet
 *
 * Purpose:
 *    Parse the option areas in the DHCP packet.
 *    Verifies that the packet has the right magic number,
 *    then parses and accumulates the option areas.
 *    First the pkt->dp_options is parsed.  If that contains
 *    the overload option, it parses pkt->dp_file if specified,
 *    then parses pkt->dp_sname if specified.
 */
boolean_t
dhcpol_parse_packet(dhcpol_t * options, struct dhcp * pkt, int len,
		    dhcpo_err_str_t * err)
{
    dhcpol_init(options);	/* make sure it's empty */

    if (err)
	err->str[0] = '\0';

    if (len < (sizeof(*pkt) + RFC_MAGIC_SIZE)) {
	if (err) {
	    snprintf(err->str, sizeof(err->str), "packet is too short: %d < %d",
		    len, (int)sizeof(*pkt) + RFC_MAGIC_SIZE);
	}
	return (FALSE);
    }
    if (bcmp(pkt->dp_options, rfc_magic, RFC_MAGIC_SIZE)) {
	if (err)
	    snprintf(err->str, sizeof(err->str), "missing magic number");
	return (FALSE);
    }
    if (dhcpol_parse_buffer(options, pkt->dp_options + RFC_MAGIC_SIZE,
			    len - sizeof(*pkt) - RFC_MAGIC_SIZE, err) == FALSE)
	return (FALSE);
    { /* get overloaded options */
	uint8_t *	overload;
	int		overload_len;

	overload = (uint8_t *)dhcpol_find(options, dhcptag_option_overload_e, 
					  &overload_len, NULL);
	if (overload && overload_len == 1) { /* has overloaded options */
	    dhcpol_t	extra;

	    dhcpol_init(&extra);
	    if (*overload == DHCP_OVERLOAD_FILE
		|| *overload == DHCP_OVERLOAD_BOTH) {
		if (dhcpol_parse_buffer(&extra, pkt->dp_file, 
					 sizeof(pkt->dp_file), NULL)) {
		    dhcpol_concat(options, &extra);
		    dhcpol_free(&extra);
		}
	    }
	    if (*overload == DHCP_OVERLOAD_SNAME
		|| *overload == DHCP_OVERLOAD_BOTH) {
		if (dhcpol_parse_buffer(&extra, pkt->dp_sname, 
					 sizeof(pkt->dp_sname), NULL)) {
		    dhcpol_concat(options, &extra);
		    dhcpol_free(&extra);
		}
	    }
	}
    }
    return (TRUE);
}

void
dhcpol_fprint(FILE * f, dhcpol_t * list)
{
    int 		i;

    fprintf(f, "Options count is %d\n", dhcpol_count(list));
    for (i = 0; i < dhcpol_count(list); i++) {
	uint8_t * option = dhcpol_element(list, i);

	if (dhcptag_fprint(f, option) == FALSE) {
	    fprintf(f, "undefined tag %d len %d\n", option[TAG_OFFSET], 
			  option[LEN_OFFSET]);
	}
    }
    return;
}
