# Host-side unit tests for libds: the plist round trip, record edits,
# hashing and verification, locking, atomic writes, homes, and the files
# promote / join / leave own. Needs only libc (plus libcrypt on FreeBSD:
# `make check LDLIBS=-lcrypt`). `make check` builds and runs them.

CC?=		cc
CFLAGS?=	-O1 -g -Wall -Wextra -Werror
CFLAGS+=	-I../libds
LDLIBS?=

SRCS=		ds_test.c ../libds/dsrec.c ../libds/dscrypt.c ../libds/dsio.c \
		../libds/dsnet.c ../libds/plist.c

ds_test: ${SRCS} ../libds/ds.h ../libds/plist.h
	${CC} ${CFLAGS} -o $@ ${SRCS} ${LDLIBS}

check: ds_test
	./ds_test

clean:
	rm -rf ds_test ds_test.dSYM

.PHONY: check clean
