/*
 * Copyright (c) 2005, 2006, 2011 Apple Inc. All rights reserved.
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
 * DNSNameList.c
 * - convert the compact DNS form described in RFC 1035 to a list of
 *   DNS domain names
 *
 * NextBSD: DNSNameListCreate() and the helpers it uses, from bootp-268.1
 * bootplib/DNSNameList.c. DNSNameListBufferCreate() (building the wire
 * form) has no caller here and is not carried.
 */

/*
 * Modification History
 *
 * January 4, 2006	Dieter Siegmund (dieter@apple)
 * - created
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <mach/mach.h>
#include "DNSNameList.h"
#include "nbo.h"

#define DNS_PTR_PATTERN_BYTE_MASK	(uint8_t)0xc0
#define DNS_PTR_PATTERN_MASK		(uint16_t)0xc000
#define DNS_PTR_VALUE_MASK		(uint16_t)(~DNS_PTR_PATTERN_MASK)

#define DNS_LABEL_LENGTH_MAX		63

#define DNS_BUF_INITIAL_SIZE		128
typedef struct {
    uint8_t	db_buf_s[DNS_BUF_INITIAL_SIZE];
    bool	db_buf_user_supplied;
    uint8_t *	db_buf;
    int		db_buf_used;
    int		db_buf_size;
} DNSBuf, * DNSBufRef;

#define DNS_NAME_OFFSETS_N_START	8
typedef struct {
    uint32_t		dno_list_s[DNS_NAME_OFFSETS_N_START];
    uint32_t *		dno_list;
    int			dno_count;
    int			dno_size;
} DNSNameOffsets, * DNSNameOffsetsRef;

/**
 ** DNSBuf
 **/

static void
DNSBufInit(DNSBufRef db, uint8_t * buf, int buf_size)
{
    bzero(db, sizeof(*db));
    if (buf != NULL) {
	db->db_buf_user_supplied = TRUE;
	db->db_buf = buf;
	db->db_buf_size = buf_size;
    }
    else {
	db->db_buf = db->db_buf_s;
	db->db_buf_size = sizeof(db->db_buf_s);
    }
    return;
}

static void
DNSBufFreeElements(DNSBufRef db)
{
    if (db->db_buf_user_supplied == FALSE) {
	if (db->db_buf != NULL && db->db_buf != db->db_buf_s) {
	    free(db->db_buf);
	}
    }
    bzero(db, sizeof(*db));
    return;
}

static __inline__ int
DNSBufUsed(DNSBufRef db)
{
    return (db->db_buf_used);
}

static __inline__ uint8_t *
DNSBufBuffer(DNSBufRef db)
{
    return (db->db_buf);
}

static bool
DNSBufAddData(DNSBufRef db, const void * data, int data_len)
{
#ifdef DEBUG
    printf("Adding: ");
    print_data((void *)data, data_len);
#endif /* DEBUG */

    if (data_len > (db->db_buf_size - db->db_buf_used)) {
	if (db->db_buf_user_supplied) {
	    fprintf(stderr, 
		    "user-supplied buffer failed to add data with"
		    " length %d (> %d)\n",
		    data_len, (db->db_buf_size - db->db_buf_used));
	    return (FALSE);
	}
#ifdef DEBUG
	printf("Buffer growing from %d to %d\n", db->db_buf_size,
	       db->db_buf_size + ((data_len > DNS_BUF_INITIAL_SIZE) 
	       ? data_len : DNS_BUF_INITIAL_SIZE));
#endif /* DEBUG */
	db->db_buf_size += (data_len > DNS_BUF_INITIAL_SIZE) 
	    ? data_len : DNS_BUF_INITIAL_SIZE;
	if (db->db_buf == db->db_buf_s) {
	    db->db_buf = malloc(db->db_buf_size);
	    memcpy(db->db_buf, db->db_buf_s, db->db_buf_used);
	}
	else {
	    db->db_buf = reallocf(db->db_buf, db->db_buf_size);
	}
    }
    memcpy(db->db_buf + db->db_buf_used, data, data_len);
    db->db_buf_used += data_len;
    return (TRUE);
}

/**
 ** DNSNameOffsets
 **/
/**
 ** DNSNameOffsets
 **/
static void
DNSNameOffsetsInit(DNSNameOffsetsRef list)
{
    bzero(list, sizeof(*list));
    list->dno_size = DNS_NAME_OFFSETS_N_START;
    list->dno_list = list->dno_list_s;
    return;
}

static bool
DNSNameOffsetsContainsOffset(DNSNameOffsetsRef list, uint32_t off)
{
    int		i;

    for (i = 0; i < list->dno_count; i++) {
	if (list->dno_list[i] == off) {
	    return (TRUE);
	}
    }
    return (FALSE);
}

static void
DNSNameOffsetsFreeElements(DNSNameOffsetsRef list)
{
    if (list->dno_list != NULL && list->dno_list != list->dno_list_s) {
	free(list->dno_list);
    }
    DNSNameOffsetsInit(list);
    return;
}

static void
DNSNameOffsetsAdd(DNSNameOffsetsRef list, uint32_t this_offset)
{
    if (list->dno_size == list->dno_count) {
#ifdef DEBUG
	printf("Growing from %d to %d\n", list->dno_size, list->dno_size * 2);
#endif /* DEBUG */
	list->dno_size *= 2;
	if (list->dno_list == list->dno_list_s) {
	    list->dno_list = malloc(list->dno_size * sizeof(*list->dno_list));
	    bcopy(list->dno_list_s, list->dno_list, 
		  list->dno_count * sizeof(*list->dno_list));
	}
	else {
	    list->dno_list = reallocf(list->dno_list, 
				      list->dno_size * sizeof(*list->dno_list));
	}
    }
    list->dno_list[list->dno_count++] = this_offset;
    return;
}

/* 
 * Function: DNSNameListCreate
 *
 * Purpose:
 *   Convert the compact domain name list form described in RFC 1035 to a list
 *   of domain names.
 * 
 * Returns:
 *   A non-NULL malloc'd list of DNS domain names, and the non-zero count of
 *   elements in the list if the conversion was successful.
 *   NULL if an error occurred i.e. the supplied buffer contained an invalid
 *   encoding.
 *
 * Notes:
 *   This routine processes the encoded buffer, keeping track of the
 *   "read_head", and the "scan" point, the farthest we've reached in the
 *   buffer.  We look for either pointers, labels, or end labels.  
 *
 *   A pointer is checked to ensure that:
 *   - it is not truncated
 *   - it does not point past the last completed name
 *   - it points to the start of a label
 *   - it does not create an infinite loop
 *
 *   The label is checked to ensure that it is not truncated, and that it
 *   is not too long (<= 63 bytes).  When an end label is encountered, the
 *   domain name is complete, and the "read_head" advances to the "scan" point.
 *
 *   When processing is complete, the pointer array and name buffer are
 *   allocated in one contiguous chunk of memory.  The name buffer is copied
 *   to the newly allocated area, and the pointer array is populated by
 *   walking the name buffer looking for complete strings.
 */
const char * *
DNSNameListCreate(const uint8_t * buffer, int buffer_size,
		  int * names_count)
{
    DNSNameOffsets	buffer_offsets;
    bool		first;
    uint32_t		last_completed_name = 0;
    uint32_t		left;
    char * *		list = NULL;
    int			list_count = 0;
    DNSBuf		name_buf;
    uint32_t		read_head;
    uint32_t		scan;

    if (buffer_size == 0) {
	return (NULL);
    }

    DNSBufInit(&name_buf, NULL, 0);
    DNSNameOffsetsInit(&buffer_offsets);
    left = buffer_size;
    first = TRUE;
    for (read_head = scan = 0; read_head < buffer_size; ) {
	uint16_t	ptr;

	if ((buffer[read_head] & DNS_PTR_PATTERN_BYTE_MASK) 
	    == DNS_PTR_PATTERN_BYTE_MASK) {
	    /* got a pointer, validate it */
	    if (read_head >= scan) {
		if (left < 2) {
		    fprintf(stderr, "truncated pointer value\n");
		    goto failed;
		}
		scan += 2;
		left -= 2;
	    }

	    ptr = (net_uint16_get(buffer + read_head))
			     & DNS_PTR_VALUE_MASK;
	    if (ptr >= read_head) {
		fprintf(stderr,
			"pointer points at or ahead of current position\n");
		goto failed;
	    }
	    if (last_completed_name == 0
		|| ptr >= last_completed_name) {
		fprintf(stderr, "attempt to create infinite loop\n");
		goto failed;
	    }
	    if (DNSNameOffsetsContainsOffset(&buffer_offsets, ptr) == FALSE) {
		fprintf(stderr, "attempt to point off into the weeds\n");
		goto failed;
	    }
	    /* it's a good pointer, so follow it */
	    read_head = ptr;
	}
	else {
	    uint8_t	len = buffer[read_head];
	    
	    if (read_head >= scan) {
		if (len > DNS_LABEL_LENGTH_MAX) {
		    fprintf(stderr, "label length %d > %d\n", len,
			    DNS_LABEL_LENGTH_MAX);
		    goto failed;
		}
		if (left < (len + 1)) {
		    fprintf(stderr, "label truncated %d < %d\n",
			    left, len + 1);
		    goto failed;
		}
		scan += len + 1;
		left -= len + 1;
	    }
	    if (len == 0) {
		/* end label */
		char	ch = '\0';

		(void)DNSBufAddData(&name_buf, &ch, 1);
		last_completed_name = scan;
		read_head = scan;
		list_count++;
		/* start on the next domain name */
		first = TRUE;
	    }
	    else {
		/* got a label */
		char	dot = '.';

		if (DNSNameOffsetsContainsOffset(&buffer_offsets, read_head) 
		    == FALSE) {
		    DNSNameOffsetsAdd(&buffer_offsets, read_head);
		}
		if (first) {
		    /* got the start of a new domain name */
		    first = FALSE;
		}
		else {
		    (void)DNSBufAddData(&name_buf, &dot, 1);
		}
		/* add this label to the buffer */
		DNSBufAddData(&name_buf, buffer + read_head + 1, len);
		read_head += len + 1;
	    }
	}
    }

    if (list_count == 0) {
	if (DNSBufUsed(&name_buf) == 0) {
	    fprintf(stderr, "empty list\n");
	}
	else {
	    fprintf(stderr, "name missing end label\n");
	}
	goto failed;
    }

    /* create single returned allocation: name pointers + name strings */
    list = (char * *)
	malloc(DNSBufUsed(&name_buf) + sizeof(*list) * list_count);
    {
	int 		i;
	char *		name_start;

	name_start = (char *)(list) + sizeof(*list) * list_count;
	memcpy(name_start, DNSBufBuffer(&name_buf),
	       DNSBufUsed(&name_buf));
	for (i = 0; i < list_count; i++) {
	    list[i] = name_start;
	    name_start += strlen(name_start) + 1;
	}
    }
 failed:
    if (list != NULL) {
	*names_count = list_count;
    }
    else {
	*names_count = 0;
    }
    DNSBufFreeElements(&name_buf);
    DNSNameOffsetsFreeElements(&buffer_offsets);
    return ((const char * *)list);
}
