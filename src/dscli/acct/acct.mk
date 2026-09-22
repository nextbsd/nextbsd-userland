# The account tools' shared code (prompting, the joined-client refusal),
# compiled into passwd, chpass and pw alongside libds. Include from a
# bsd.prog.mk Makefile after libds.mk:
#
#   .include "${.CURDIR}/../libds/libds.mk"
#   .include "${.CURDIR}/../acct/acct.mk"

ACCT_DIR:=	${.PARSEDIR}
.PATH:		${ACCT_DIR}
CFLAGS+=	-I${ACCT_DIR}
SRCS+=		acct.c
