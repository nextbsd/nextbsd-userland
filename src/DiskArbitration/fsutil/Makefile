# fsutil — the <name>.util helper of the NextBSD filesystem bundles
# (E15 C4, nextbsd-userland#194). One binary in /usr/libexec, symlinked as
# /System/Library/Filesystems/<name>.fs/Contents/Resources/<name>.util; the
# bundles' Info.plists come from overlay/System/Library/Filesystems.
#
# Build:   make -C src/DiskArbitration/fsutil SYSROOT=/path/to/rootfs
# Install: make DESTDIR=/path/to/rootfs install  (Resources dirs must exist)

PROG=		fsutil
MAN=
BINDIR=		/usr/libexec

# Same warning set as diskarbitrationd, without the bsd.sys.mk -Werror.
WARNS=		0
CFLAGS+=	-O2 -pipe
CFLAGS+=	-Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS+=	-Wmissing-prototypes -Wpointer-arith

# md: SHA-1 for the name-based -k UUID. ufs: libufs superblock read for -q.
LIBADD+=	md ufs

FSDIR=		/System/Library/Filesystems
.for _fs in msdos exfat ntfs ufs cd9660 udf
SYMLINKS+=	${BINDIR}/${PROG} ${FSDIR}/${_fs}.fs/Contents/Resources/${_fs}.util
.endfor

.include <bsd.prog.mk>
