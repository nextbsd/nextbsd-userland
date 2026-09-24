# Host-side unit tests for libds. They run on the build host, Darwin or
# FreeBSD, against that host's own CoreFoundation, in a temporary fixture
# directory. No root, and nothing outside the fixture is touched.
#
# The tests also parse libds's output with the NSS module's own
# independent, CoreFoundation-free reader, which is the acceptance
# criterion that the two halves agree on the format.
#
# No conditional directives: this file has to work under both GNU make
# (Darwin) and bmake (FreeBSD), so the platform test lives in the recipe.
#
#   make check

CC?=	cc
CFLAGS?=	-O1 -g
CFLAGS+=	-Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes
CFLAGS+=	-fblocks -DLIBDS_TEST -I.. -I../../nss_directory_services

NSSSRC=	../../nss_directory_services/plist.c

libds_test: libds_test.c ../libds.c ../libds.h $(NSSSRC)
	@case "`uname -s`" in \
	Darwin)	cflib="-framework CoreFoundation" ;; \
	*)	cflib="-lCoreFoundation" ;; \
	esac; \
	echo "$(CC) $(CFLAGS) -o $@ ... $$cflib"; \
	$(CC) $(CFLAGS) -o $@ libds_test.c ../libds.c $(NSSSRC) $$cflib

check: libds_test
	./libds_test

clean:
	rm -f libds_test
	rm -rf libds_test.dSYM

.PHONY: check clean
