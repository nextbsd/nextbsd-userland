# Host-side unit tests for the shared account helpers.
#
#   make check
#
# The same source is also built for the image (TIER 3 in build-userland.sh,
# ACCT-HELPERS marker), which is where hashing is really covered: a build
# host's crypt(3) may not implement the format we require, and this binary
# says so rather than passing on a weaker one.

CC?=	cc
CFLAGS?=	-O1 -g
CFLAGS+=	-Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes
CFLAGS+=	-I..

acct_test: acct_test.c ../acct.c ../acct.h
	@case "`uname -s`" in \
	Darwin)	lib="" ;; \
	*)	lib="-lcrypt" ;; \
	esac; \
	$(CC) $(CFLAGS) -o $@ acct_test.c ../acct.c $$lib

check: acct_test
	./acct_test

clean:
	rm -f acct_test
	rm -rf acct_test.dSYM

.PHONY: check clean
