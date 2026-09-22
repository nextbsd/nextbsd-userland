# Host-side unit tests for libds: the plist round trip, record edits,
# hashing and verification, locking, atomic writes, homes, and the files
# promote / join / leave own. Needs only libc (plus libcrypt on FreeBSD:
# `make check LDLIBS=-lcrypt`). `make check` builds and runs them.

CC?=		cc
CFLAGS?=	-O1 -g -Wall -Wextra -Werror
CFLAGS+=	-I../libds
LDLIBS?=

LIBSRCS=	../libds/dsrec.c ../libds/dscrypt.c ../libds/dsio.c \
		../libds/dsnet.c ../libds/dsroute.c ../libds/plist.c
SRCS=		ds_test.c ${LIBSRCS}

ds_test: ${SRCS} ../libds/ds.h ../libds/plist.h
	${CC} ${CFLAGS} -o $@ ${SRCS} ${LDLIBS}

# The account tools' routing (E18 U9): where an account is and where a
# new one goes, over fixture copies of the plists and the system files.
route_test: route_test.c ${LIBSRCS} ../libds/ds.h
	${CC} ${CFLAGS} -o $@ route_test.c ${LIBSRCS} ${LDLIBS}

check: ds_test route_test
	./ds_test
	./route_test

clean:
	rm -rf ds_test ds_test.dSYM route_test route_test.dSYM

.PHONY: check clean
