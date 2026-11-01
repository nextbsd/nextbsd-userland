/*
 * Copyright (c) 2005 Finlay Dobbie
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of Finlay Dobbie nor the names of his contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * sw_vers(1): print the NextBSD product name, version and build.
 *
 * Ported from Apple's DarwinTools-1 (sw_vers.c), the sw_vers that Darwin
 * shipped through OS X 10.9. Same options and the same output, read from
 * the same file. NextBSD changes:
 *
 *   - Apple's reads the plist through _CFCopySystemVersionDictionary()
 *     (CFPriv.h). NextBSD's CoreFoundation is the swift-corelibs one, where
 *     that SPI returns NULL on every platform but Darwin and Windows, so
 *     this reads /System/Library/CoreServices/SystemVersion.plist itself
 *     with the public CFPropertyList API. There is no ServerVersion.plist
 *     on NextBSD, so Apple's _CFCopyServerVersionDictionary() step goes.
 *   - The summary uses a tab after each label, as OS X 10.9's sw_vers
 *     prints it (DarwinTools-1 padded with spaces), so scripts written
 *     against OS X parse it the same way.
 *   - A missing or unreadable plist, or a missing key, is reported on
 *     stderr with exit status 1 instead of a silent exit (or, for a missing
 *     key, a CFRetain(NULL) crash). nextbsd-pkg writes the plist into
 *     NextBSD-everything (nextbsd/nextbsd#536); a system installed before
 *     that has none.
 *   - -productVersion no longer consults MacOSXProductVersion, a key only
 *     Darwin's own plist carried.
 */

#include <CoreFoundation/CoreFoundation.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SYSTEM_VERSION_PLIST	"/System/Library/CoreServices/SystemVersion.plist"
#define PLIST_MAX		(64 * 1024)

static const char *progname;

static void
usage(void)
{
	fprintf(stderr, "Usage: %s [-productName|-productVersion|-buildVersion]\n",
	    progname);
	exit(1);
}

static CFDictionaryRef
copy_system_version_dictionary(void)
{
	static UInt8 buf[PLIST_MAX];
	CFPropertyListRef plist;
	CFDataRef data;
	FILE *f;
	size_t n;

	if ((f = fopen(SYSTEM_VERSION_PLIST, "r")) == NULL) {
		if (errno == ENOENT)
			fprintf(stderr, "%s: %s not found; this system has no "
			    "version information (it predates "
			    "SystemVersion.plist; upgrade NextBSD-everything)\n",
			    progname, SYSTEM_VERSION_PLIST);
		else
			fprintf(stderr, "%s: %s: %s\n", progname,
			    SYSTEM_VERSION_PLIST, strerror(errno));
		return (NULL);
	}
	n = fread(buf, 1, sizeof(buf), f);
	if (ferror(f) || !feof(f)) {
		fprintf(stderr, "%s: %s: %s\n", progname, SYSTEM_VERSION_PLIST,
		    ferror(f) ? strerror(errno) : "file too large");
		fclose(f);
		return (NULL);
	}
	fclose(f);

	data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, buf, (CFIndex)n,
	    kCFAllocatorNull);
	if (data == NULL)
		return (NULL);
	plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data,
	    kCFPropertyListImmutable, NULL, NULL);
	CFRelease(data);
	if (plist == NULL || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		fprintf(stderr, "%s: %s is not a property list dictionary\n",
		    progname, SYSTEM_VERSION_PLIST);
		if (plist != NULL)
			CFRelease(plist);
		return (NULL);
	}
	return ((CFDictionaryRef)plist);
}

static CFStringRef
value(CFDictionaryRef dict, CFStringRef key)
{
	CFTypeRef v = CFDictionaryGetValue(dict, key);
	char k[64];

	if (v == NULL || CFGetTypeID(v) != CFStringGetTypeID()) {
		if (!CFStringGetCString(key, k, sizeof(k), kCFStringEncodingUTF8))
			k[0] = '\0';
		fprintf(stderr, "%s: %s has no %s string\n", progname,
		    SYSTEM_VERSION_PLIST, k);
		exit(1);
	}
	return ((CFStringRef)v);
}

int
main(int argc, char *argv[])
{
	CFDictionaryRef dict;
	CFStringRef str = NULL;
	char cstr[256];

	progname = argv[0];
	if (argc > 2)
		usage();
	if (argc == 2 && strcmp(argv[1], "-productName") != 0 &&
	    strcmp(argv[1], "-productVersion") != 0 &&
	    strcmp(argv[1], "-buildVersion") != 0)
		usage();

	if ((dict = copy_system_version_dictionary()) == NULL)
		exit(1);

	if (argc == 2) {
		if (!strcmp(argv[1], "-productName"))
			str = value(dict, CFSTR("ProductName"));
		else if (!strcmp(argv[1], "-productVersion"))
			str = value(dict, CFSTR("ProductVersion"));
		else
			str = value(dict, CFSTR("ProductBuildVersion"));
		CFRetain(str);
	} else {
		str = CFStringCreateWithFormat(NULL, NULL,
		    CFSTR("ProductName:\t%@\n"
			  "ProductVersion:\t%@\n"
			  "BuildVersion:\t%@"),
		    value(dict, CFSTR("ProductName")),
		    value(dict, CFSTR("ProductVersion")),
		    value(dict, CFSTR("ProductBuildVersion")));
	}

	if (!CFStringGetCString(str, cstr, sizeof(cstr), kCFStringEncodingUTF8))
		exit(1);

	printf("%s\n", cstr);

	CFRelease(str);
	CFRelease(dict);
	return (0);
}
