# Host-side unit tests for the plist reader and the database layer. They
# need only libc, so they run on the build host (FreeBSD or Darwin) with
# no nsswitch involved. `make check` builds and runs them.

CC?=		cc
CFLAGS?=	-O1 -g -Wall -Wextra -Werror
CFLAGS+=	-DDSDB_TEST -I..

dsdb_test: dsdb_test.c ../dsdb.c ../plist.c ../dsdb.h ../plist.h
	${CC} ${CFLAGS} -o $@ dsdb_test.c ../dsdb.c ../plist.c

check: dsdb_test
	./dsdb_test

clean:
	rm -f dsdb_test
	rm -rf dsdb_test.d

.PHONY: check clean
