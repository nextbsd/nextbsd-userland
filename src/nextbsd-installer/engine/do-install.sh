#!/bin/sh
# do-install.sh — the NextBSD whole-disk install engine.
#
# Driven by the front-end (which parses the PROGRESS/STATUS lines below), but
# also runnable standalone. Honors NEXTBSD_DRYRUN=1 (print every destructive
# command instead of running it) so it's safe to exercise off-target.
#
# Design notes baked in:
#   * The installed system is ZFS (E31, nextbsd/nextbsd#600), whatever the
#     install medium is (the ISO and the Pi image stay UFS) and whatever
#     system it carries: pool NextBSD, boot environment NextBSD/ROOT/default,
#     and the shared datasets Local, root, private/tmp, private/var/log and
#     private/var/tmp (see create_pool). No swap. ZFS is compiled into the
#     kernel, so nothing is loaded; launchctl's boot pass mounts the datasets
#     (nextbsd-userland#200).
#   * Whole-disk GPT: freebsd-boot (BIOS, gptzfsboot) + EFI (ESP) +
#     freebsd-zfs. The loader boots the pool's bootfs, so loader.conf names
#     no root.
#   * ...EXCEPT on a Raspberry Pi 5-family board, which cannot boot that at
#     all. There is no UEFI and no loader(8): the EEPROM bootloader reads
#     config.txt off a FAT partition and enters the kernel directly. That
#     needs MBR + FAT32 + the pool in the second slice, selected
#     automatically -- see LAYOUT/PAYLOAD. The kernel stays on FAT and
#     cmdline.txt names the root dataset; the kernel imports the pool itself.
#   * Copy mode (-c) and install-over (-U) still make UFS (until
#     nextbsd-userland#659): a UFS root labelled NEXTBSD (not the medium's
#     ROOTFS; see LABEL below), found by vfs.root.mountfrom -- loader.conf on
#     EFI, cmdline.txt on a Pi -- so the install is disk-path agnostic. No
#     /etc/fstab is needed for root (nextbsd-overlays#5).
#   * Clone with cpdup from the live / (union ISO or plain image alike), then
#     SCRUB the volatile dirs — NextBSD has no rc.d cleanvar/cleartmp, so the
#     installer must hand off a boot-clean /var and /tmp itself.
#   * -c (COPY MODE, Installer.app on an installed system): the source is the
#     running system itself, read from a UFS snapshot of / so the copy is
#     consistent while the system keeps running. Temporary and per-boot files
#     and swap are left out, accounts, apps and settings are kept, and the
#     copy gets its own identity -- see steps 4 and 5.
#
# Usage: do-install.sh -d <disk> [-U] [-c]
#
# Besides PROGRESS and STATUS (free text, for the text installer) the engine
# prints PHASE<TAB>prepare|copy|setup|finish as it enters each stage, so a
# front end can say what's happening in its own words without parsing STATUS.
# PROGRESS climbs steadily through the copy, measured by what has reached the
# target so far.
#
# The installer does NOT create a user or set a hostname: the installed system
# boots with a passwordless root (from the base image) and hostnamed's
# synthesized default name. The operator sets a root password / adds users /
# changes the hostname after first boot.
set -eu

# Never leave the probe mount behind, whatever happens next -- including a
# dry run, a failure, or an interrupt. The installer inspects media it does
# not own; it should leave the machine exactly as it found it.
SRCBOOT=/tmp/nbi-srcboot
# Copy mode's snapshot of /, the md(4) unit holding it and where it's mounted
# (step 4), and the copy's progress reporter: all undone here too.
SNAPFILE="" SNAPMD="" SNAPMNT=/tmp/nbi-snapshot MONITOR=""
POOLMADE=""
cleanup() {
	umount "$SRCBOOT" 2>/dev/null || true
	rmdir "$SRCBOOT" 2>/dev/null || true
	[ -n "$MONITOR" ] && kill "$MONITOR" 2>/dev/null || true
	if [ -n "$SNAPMD" ]; then
		umount "$SNAPMNT" 2>/dev/null || true
		mdconfig -d -u "$SNAPMD" 2>/dev/null || true
	fi
	rmdir "$SNAPMNT" 2>/dev/null || true
	# A UFS snapshot left behind keeps its blocks forever.
	[ -n "$SNAPFILE" ] && rm -f "$SNAPFILE" 2>/dev/null || true
	# A pool made by a run that stopped part way: exported, so it is not
	# left imported under its temporary name.
	[ -n "$POOLMADE" ] && zpool export -f "$TPOOL" 2>/dev/null || true
	# A run that stopped part way leaves the target mounted; a finished
	# one has unmounted it already.
	for m in "${EFIMNT:-}" "${BOOTMNT:-}" "${MNT:-}"; do
		[ -n "$m" ] && umount "$m" 2>/dev/null || true
	done
}
# Stopped (INT, TERM): clean up and stop, rather than carry on where the
# signal found the script. Installer.app's helper signals the engine's
# whole process group, cpdup included.
trap cleanup EXIT
trap 'exit 1' INT TERM

DISK="" UPGRADE=0 COPY=0
MNT=/tmp/nbi-mnt
# What gets installed: the running system's root. NEXTBSD_SOURCE names another
# (a release image's root, mounted read-only) to try an install from the image
# on a machine that isn't running it; copy mode ignores it.
SRC=${NEXTBSD_SOURCE:-/}
# Root label for the INSTALLED system — deliberately NOT "ROOTFS" (the live
# install medium's label). A shared label makes a leftover/failed install a
# boot-hijacker: two ufs/ROOTFS providers and the kernel mounts the wrong one.
# vfs.root.mountfrom in loader.conf (EFI, step 5b) or cmdline.txt (Pi, step 6)
# points the target here.
LABEL=NEXTBSD

# The pool, for a ZFS install: NextBSD on disk. While this runs it is known by
# a temporary in-core name (zpool create -t), so it can never clash with a
# pool the running system has imported, a ZFS-installed NextBSD's included.
# The kernel imports a root pool by its on-disk name (cmdline.txt on a Pi,
# the loader's bootfs elsewhere).
POOL=NextBSD
TPOOL="nbi$$"

# --- Which boot layout does this machine need? ------------------------------
# Detected rather than flagged, so the same live image does the right thing
# wherever it boots. The device tree's root "compatible" is the honest source:
# a Pi 500+ reads "raspberrypi,500" + "brcm,bcm2712". Anything matching
# brcm,bcm2712 is a Pi 5-family board (5 B, 500, 500+, CM5) and needs the
# firmware boot layout rather than GPT+ESP.
#
# ofwdump(8) reads it straight out of the tree the kernel booted with, and
# ships in base. It exits non-zero on a machine with no OFW/FDT (amd64), which
# is exactly the "generic" answer.
# Which boot method does this machine need?
#
# Three cases, and the tests have to be done in this order because the later
# signals are present in more than one of them.
#
#   generic   GPT + ESP (+ freebsd-boot on x86 BIOS). A loader runs.
#   fdtboot   MBR + FAT32 holding config.txt. NO loader: the Raspberry Pi
#             firmware enters the kernel directly at EL2.
#
# 1. /dev/efi exists only when the kernel found real EFI runtime services --
#    efidev(4) refuses to attach otherwise ("If we have no efi environment,
#    then don't create the device"). kenv efi-version is set only by
#    loader.efi. Either one means a UEFI loader ran, on any architecture.
#
#    Do NOT use machdep.efi_map for this: it is declared once, in
#    sys/amd64/amd64/machdep.c, and does not exist on arm64 at all.
#
# 2. Only then ask whether there is a device tree -- and note that the
#    presence of an FDT proves nothing on its own, because loader.efi
#    installs the DTB it got from the EFI configuration table
#    ("Using DTB provided by EFI at %p"). A UEFI arm64 VM has a perfectly
#    good /dev/openfirm. That is why this test comes second, and why it
#    matches the board rather than merely asking whether OFW exists.
#
# 3. x86 keeps its own answer: machdep.bootmethod is BIOS, UEFI or PVH, and
#    is what bsdinstall itself uses (partedit_x86.c).
# Two independent questions, deliberately kept apart.
#
# LAYOUT -- who reads the partition table? That is fixed in silicon or in an
# EEPROM and does not change because something UEFI-shaped runs afterwards. On
# a Raspberry Pi the GPU firmware always reads it, so a Pi always wants
# MBR + FAT, even under EDK2 or U-Boot's EFI payload. Getting this from the
# board is what makes the answer right for a Pi that boots through a UEFI
# layer -- stock FreeBSD's Pi image does exactly that.
#
# PAYLOAD -- what goes on the boot partition? That follows how this system was
# actually started: an EFI tree if a UEFI loader ran, config.txt and a kernel
# if the firmware entered the kernel itself.
#
# NOT machdep.efi_map for the UEFI test: it is declared once, in
# sys/amd64/amd64/machdep.c, and does not exist on arm64 at all. And not "is
# there an FDT", because loader.efi installs the DTB it got from the EFI
# configuration table -- a UEFI arm64 VM has a perfectly good /dev/openfirm.
detect_layout() {
	# hw.fdt.compatible carries the whole property with the NULs already
	# joined by the kernel; ofwdump needs root and truncates at the first.
	case "$(sysctl -n hw.fdt.compatible 2>/dev/null)" in
	*raspberrypi,*|*brcm,bcm2*) echo mbr-fat; return ;;
	esac
	echo gpt
}

detect_payload() {
	if [ -e /dev/efi ] || [ -n "$(kenv -q efi-version 2>/dev/null)" ]; then
		echo efi
		return
	fi
	echo firmware
}

# The live medium's boot partition, located WITHOUT mounting anything.
#
# gpart reports partition types straight from the disk metadata, so the shape
# is visible without touching a filesystem: an MBR disk carrying a fat32lba
# slice IS the Raspberry Pi boot arrangement, whoever built it and whatever the
# volume happens to be labelled. Structural, so it recognises a stock
# Raspberry Pi OS card or a hand-made stick, not only our own images.
#
# The label lookup is a fallback for a boot partition on a scheme we did not
# recognise. Sets BOOTSRC to a device path, or leaves it empty.
find_boot_source() {
	for d in $(sysctl -n kern.disks 2>/dev/null); do
		[ "$d" = "$DISK" ] && continue
		gpart show -p "$d" 2>/dev/null | grep -q "MBR" || continue
		part=$(gpart show -p "$d" 2>/dev/null | awk '$4 ~ /^fat32/ {print $3; exit}')
		if [ -n "$part" ] && [ -e "/dev/$part" ]; then
			BOOTSRC=/dev/$part
			return 0
		fi
	done
	for l in /dev/msdosfs/*; do
		[ -e "$l" ] || continue
		prov=$(glabel status -s 2>/dev/null | awk -v n="${l#/dev/}" '$1 == n {print $3}')
		[ -n "$prov" ] || continue
		case "$prov" in ${DISK}*) continue ;; esac
		BOOTSRC=$l
		return 0
	done
	return 1
}
BOOTSRC=""

while getopts "d:Uc" o; do
	case "$o" in
		d) DISK=$OPTARG ;;
		U) UPGRADE=1 ;;
		c) COPY=1 ;;
		*) echo "usage: do-install.sh -d disk [-U] [-c]" >&2; exit 2 ;;
	esac
done
[ -n "$DISK" ] || { echo "do-install.sh: missing -d <disk>" >&2; exit 2; }
[ "$COPY" = 1 ] && [ "$UPGRADE" = 1 ] && { echo "do-install.sh: -c and -U don't go together" >&2; exit 2; }

# The installed system's file system: ZFS. Copy mode and install-over keep
# UFS until nextbsd-userland#659. NEXTBSD_FS=ufs is the escape hatch, like
# NEXTBSD_LAYOUT below: an install should never be impossible because one
# path misbehaves on some machine.
FS=zfs
if [ "$COPY" = 1 ] || [ "$UPGRADE" = 1 ] || [ "${NEXTBSD_FS:-}" = ufs ]; then
	FS=ufs
fi

# A copy is a second NextBSD that may well stay attached to this machine, so
# it can't share the running system's root label (two ufs/NEXTBSD providers,
# and the kernel mounts whichever it finds first). It gets a label of its own,
# which its loader.conf names (step 5b).
if [ "$COPY" = 1 ]; then
	LABEL="NEXTBSD-$(od -An -N2 -tx2 /dev/random | tr -d ' ')"
	SRC=/
fi

# Detect AFTER $DISK is known: find_boot_source has to skip the install target,
# and with $DISK empty it would happily inspect the very disk we are about to
# erase -- or conclude from a stale config.txt still sitting on it.
LAYOUT=${NEXTBSD_LAYOUT:-$(detect_layout)}
PAYLOAD=${NEXTBSD_PAYLOAD:-$(detect_payload)}
# Overridable for testing and for the case where detection is wrong; the
# installer should never be un-runnable because a probe misfired.
#
# NEXTBSD_BOARD is the older single-valued knob, kept working.
case "${NEXTBSD_BOARD:-}" in
fdtboot) LAYOUT=mbr-fat; PAYLOAD=firmware ;;
generic) LAYOUT=gpt;     PAYLOAD=efi      ;;
esac

progress() { printf 'PROGRESS\t%s\n' "$1"; }
status()   { printf 'STATUS\t%s\n'   "$1"; }
phase()    { printf 'PHASE\t%s\n'    "$1"; }
run() {
	if [ "${NEXTBSD_DRYRUN:-0}" = 1 ]; then echo "DRYRUN: $*"; else "$@"; fi
}

# --- 0. Unmount the target disk ----------------------------------------------
# The live system may already have the disk mounted: diskarbitrationd mounts
# every volume it finds when the medium boots, so a disk holding an old
# install arrives mounted under /Volumes, and gpart cannot replace a table
# whose partitions are open. Unmount every partition of it first, as the
# Gershwin installer this engine replaced does (umount_disk_partitions).
umount_disk_partitions() {
	mount | while read -r line; do
		dev=$(echo "$line" | cut -d' ' -f1)
		case "$dev" in
		"/dev/$DISK"|"/dev/${DISK}p"*|"/dev/${DISK}s"*)
			mp=$(echo "$line" | sed 's/^[^ ]* on \(.*\) (.*)/\1/')
			if [ -n "$mp" ] && [ "$mp" != "/" ]; then
				status "Unmounting $mp"
				run umount -f "$mp" 2>/dev/null || true
			fi
			;;
		esac
	done
}

# A ZFS target: whatever pool the disk holds now goes. One this system has
# imported is exported first (gpart can't replace a table whose partitions
# are open), and every old ZFS label is cleared: a pool's last two labels sit
# at the end of its partition, and when the new layout ends at the same
# place the kernel would still find the old pool there -- under the same name
# as the new one, on a reinstall.
release_target_pools() {
	command -v zpool >/dev/null 2>&1 || return 0
	rootpool=""
	if [ "$(mount -p 2>/dev/null | awk '$2=="/"{print $3; exit}')" = zfs ]; then
		rootpool="$(mount -p 2>/dev/null | awk '$2=="/"{print $1; exit}')"
		rootpool="${rootpool%%/*}"
	fi
	pools="$(zpool list -vHP 2>/dev/null | awk -F'\t' '
		$1 != "" { pool = $1; next }
		{ for (i = 2; i <= NF; i++) if ($i ~ /^ *\/dev\//) {
			v = $i; sub(/^ *\/dev\//, "", v); print pool "\t" v } }' |
	while IFS="$(printf '\t')" read -r pool prov; do
		# A label (gpt/..., diskid/...) names a partition through GEOM.
		dev="$(glabel status -s 2>/dev/null | awk -v n="$prov" '$1 == n {print $3; exit}')"
		dev="${dev:-$prov}"
		case "$dev" in
		"$DISK"|"${DISK}p"[0-9]*|"${DISK}s"[0-9]*) echo "$pool" ;;
		esac
	done | sort -u)"
	for pool in $pools; do
		if [ "$pool" = "$rootpool" ]; then
			echo "do-install.sh: $DISK holds $pool, the pool this system runs from" >&2
			exit 1
		fi
		status "Exporting pool $pool from $DISK"
		run zpool export -f "$pool"
	done
	for p in $(gpart show -p "$DISK" 2>/dev/null | awk -v d="$DISK" 'NR > 1 && index($3, d) == 1 {print $3}') "$DISK"; do
		[ -e "/dev/$p" ] || continue
		run zpool labelclear -f "/dev/$p" 2>/dev/null || true
	done
}

# Is the target on USB? The kernel may then look for its root before the disk
# has attached: measured on a Pi 500+ with a USB SSD, the first try ran too
# early and only vfs.mountroot.timeout made it boot (nextbsd-userland#658).
is_usb_disk() {
	camcontrol devlist -v 2>/dev/null | awk -v d="$DISK" '
		/^scbus/ { usb = ($0 ~ /umass/); next }
		usb && $0 ~ ("[(,]" d "[,)]") { found = 1 }
		END { exit !found }'
}

# The pool and its datasets, mounted under $MNT (the altroot). Everything not
# listed stays in the boot environment, so a rollback (bectl) takes it back
# too; what's here survives one:
#   ROOT             none              the boot environments
#   ROOT/default     /                 canmount=noauto, as bectl makes BEs
#   Local            /Local            accounts, network, display
#                                      arrangement, homes, apps
#   root             /root
#   private          none              canmount=off
#   private/tmp      /private/tmp      setuid=off
#   private/var      none              canmount=off
#   private/var/log  /private/var/log  exec=off, setuid=off
#   private/var/tmp  /private/var/tmp  setuid=off
# The boot environment is mounted first so the others land inside it.
create_pool() {
	status "Creating ZFS pool $POOL on $ZPART"
	run zpool create -f -t "$TPOOL" -R "$MNT" -m none \
	    -o ashift=12 -o autotrim=on \
	    -O compression=lz4 -O atime=off \
	    "$POOL" "/dev/$ZPART"
	POOLMADE=1
	run zfs create -o mountpoint=none "$TPOOL/ROOT"
	run zfs create -o mountpoint=/ -o canmount=noauto "$TPOOL/ROOT/default"
	run zfs mount "$TPOOL/ROOT/default"
	run zfs create -o mountpoint=/Local "$TPOOL/Local"
	run zfs create -o mountpoint=/root "$TPOOL/root"
	run zfs create -o mountpoint=none -o canmount=off "$TPOOL/private"
	run zfs create -o mountpoint=/private/tmp -o setuid=off "$TPOOL/private/tmp"
	run zfs create -o mountpoint=none -o canmount=off "$TPOOL/private/var"
	run zfs create -o mountpoint=/private/var/log -o exec=off -o setuid=off "$TPOOL/private/var/log"
	run zfs create -o mountpoint=/private/var/tmp -o setuid=off "$TPOOL/private/var/tmp"
	run zpool set bootfs="$TPOOL/ROOT/default" "$TPOOL"
}

# Space used on the target so far, in KB, for the progress reporter: the
# whole pool's logical size on ZFS (compression would otherwise make the copy
# look smaller than its source), the root file system's on UFS.
target_used_k() {
	if [ "$FS" = zfs ]; then
		zfs get -Hp -o value logicalused "$TPOOL" 2>/dev/null |
		    awk '{printf "%d\n", $1 / 1024}'
	else
		df -k "$MNT" 2>/dev/null | awk 'NR==2{print $3}'
	fi
}

phase prepare
umount_disk_partitions
if [ "$FS" = zfs ]; then
	release_target_pools
fi

# --- 1. Partition (skipped on upgrade — keep the existing layout) ------------
if [ "$UPGRADE" = 0 ]; then
	if [ "$LAYOUT" = mbr-fat ]; then
		# MBR, not GPT, and a plain FAT32 data partition rather than an
		# ESP. The BCM2712 bootloader lives in EEPROM: it looks for a FAT
		# partition holding config.txt, reads the DTB and kernel named
		# there, and enters the kernel directly at EL2. No UEFI, no
		# loader(8), so nothing here is negotiable.
		#
		# fat32lba (type 0x0c) is what Raspberry Pi OS itself uses; a
		# plain fat32 (0x0b) is CHS-addressed and wrong.
		status "Partitioning $DISK (MBR: FAT32 boot + freebsd)"
		progress 4
		run gpart destroy -F "$DISK" 2>/dev/null || true
		run gpart create -s mbr "$DISK"
		run gpart add -t fat32lba -s 100m "$DISK"
		run gpart add -t freebsd "$DISK"
		# A UFS root lives in a BSD label inside the freebsd slice; a
		# pool takes the whole slice, with no label.
		#
		# Destroy anything GEOM may have tasted there first. Creating a
		# freebsd slice over a disk that previously held one makes GEOM
		# read the old BSD label out of the sectors still sitting at that
		# offset and instantiate a scheme from it -- after which
		# `gpart create -s bsd` fails with "File exists" and the install
		# stops half-partitioned. Wiping the label sector as well, since
		# destroy alone leaves the bytes that caused the taste.
		run gpart destroy -F "/dev/${DISK}s2" 2>/dev/null || true
		run dd if=/dev/zero of="/dev/${DISK}s2" bs=512 count=34 2>/dev/null || true
		if [ "$FS" = ufs ]; then
			run gpart create -s bsd "/dev/${DISK}s2"
			run gpart add -t freebsd-ufs "/dev/${DISK}s2"
		fi
		progress 8

		if [ "$FS" = zfs ]; then
			ZPART="${DISK}s2"
			create_pool
		else
			status "Creating UFS root (label $LABEL)"
			run newfs -U -L "$LABEL" "/dev/${DISK}s2a"
		fi
		# -c 1: one sector per cluster. Without it newfs_msdos picks a
		# larger cluster and a 100 MB partition yields ~12,700 clusters,
		# below the 65,525 that *defines* FAT32 -- so it refuses. This is
		# the same choice build.sh makes (sectors_per_cluster=1) and that
		# Raspberry Pi OS makes (mkdosfs -F 32 -s 1).
		run newfs_msdos -F 32 -c 1 -L NEXTBSD "/dev/${DISK}s1"
		progress 14
	else
		status "Partitioning $DISK (GPT: freebsd-boot + EFI + freebsd-$FS)"
		progress 4
		run gpart destroy -F "$DISK" 2>/dev/null || true
		run gpart create -s gpt "$DISK"
		run gpart add -a 4k -s 512k -t freebsd-boot -l bootcode "$DISK"
		run gpart add -a 1m   -s 260m -t efi          -l EFI      "$DISK"
		if [ "$FS" = zfs ]; then
			run gpart add -a 1m        -t freebsd-zfs  -l zfs0     "$DISK"
		else
			run gpart add -a 1m        -t freebsd-ufs  -l ROOTFS   "$DISK"
		fi
		progress 8

		# --- 2. Filesystems -------------------------------------------
		if [ "$FS" = zfs ]; then
			ZPART="${DISK}p3"
			create_pool
		else
			status "Creating UFS root (label $LABEL)"
			run newfs -U -L "$LABEL" "/dev/${DISK}p3"
		fi
		run newfs_msdos -L EFI "/dev/${DISK}p2"
		progress 14
	fi
fi

# Where the root filesystem ended up, so the mount and boot-config steps below
# do not each have to re-derive it.
if [ "$LAYOUT" = mbr-fat ]; then
	ROOTPART="${DISK}s2a"
	BOOTPART="${DISK}s1"
else
	ROOTPART="${DISK}p3"
	BOOTPART="${DISK}p2"
fi

# --- 3. Mount target --------------------------------------------------------
# Mount by DEVICE PATH (${DISK}p3), never by ufs label — unambiguous no matter
# what else is attached. (This also dodged the old "Device busy" when the live
# ROOTFS medium shadowed the target's label.)
# A pool is mounted already, under its altroot (create_pool).
if [ "$FS" = ufs ]; then
	status "Mounting target"
	run mkdir -p "$MNT"
	run mount "/dev/$ROOTPART" "$MNT"
fi
progress 16

# --- 4. Clone the running system (cpdup) ------------------------------------
# Works the same on the union ISO and a plain image: cpdup reads / through the
# VFS and (don't-cross-mounts) skips /dev and $MNT automatically. cpdup vendored
# alongside this engine.
phase copy
EXCLUDE=""
if [ "$COPY" = 1 ]; then
	# Copy mode reads a snapshot of /, not / itself: a live system keeps
	# writing, and a copy taken file by file while it does is a copy of no
	# moment in particular. mksnap_ffs freezes the filesystem for an
	# instant; the snapshot is mounted read-only through md(4), as
	# mount(8) describes, and removed again by cleanup(). Should a
	# snapshot not be possible (not UFS, no room), / is read live.
	status "Taking a snapshot of the running system"
	run mkdir -p /.snap
	SNAPFILE="/.snap/nextbsd-installer.$$"
	if run mksnap_ffs "$SNAPFILE"; then
		if [ "${NEXTBSD_DRYRUN:-0}" = 1 ]; then
			echo "DRYRUN: mdconfig -a -t vnode -o readonly -f $SNAPFILE; mount -o ro /dev/mdN $SNAPMNT"
			SRC=$SNAPMNT
		else
			SNAPMD="$(mdconfig -a -t vnode -o readonly -f "$SNAPFILE")"
			mkdir -p "$SNAPMNT"
			mount -o ro "/dev/$SNAPMD" "$SNAPMNT"
			SRC=$SNAPMNT
		fi
	else
		status "No snapshot possible; copying the running system as it is"
		SNAPFILE=""
	fi

	# Left out, by path in the source: what only means something while
	# this system runs (temporary files, per-boot state, swap and the
	# sleep image), the snapshot itself, and the mount points' contents
	# (/Volumes; a snapshot holds none, the live fallback might). The
	# directories themselves are copied, empty, with their modes. cpdup
	# matches each path as it spells it, which from / starts "//".
	EXCLUDE="$(mktemp /tmp/nbi-exclude.XXXXXX)"
	S=${SRC%/}
	for p in private/tmp private/var/tmp private/var/run private/var/vm \
	    .snap Volumes; do
		printf '%s/%s/*\n' "$S" "$p"
		if [ -z "$S" ]; then printf '//%s/*\n' "$p"; fi
	done > "$EXCLUDE"
fi

# Room for it all? Better to say so now than to stop at 90%.
if [ "${NEXTBSD_DRYRUN:-0}" != 1 ]; then
	src_k="$(df -k "$SRC" 2>/dev/null | awk 'NR==2{print $3}')"
	if [ "$COPY" = 1 ]; then
		# Less what's left out (see EXCLUDE).
		S=${SRC%/}
		skip_k="$(du -skx "$S/private/tmp" "$S/private/var/tmp" \
		    "$S/private/var/vm" 2>/dev/null | awk '{s += $1} END {print s + 0}')"
		src_k=$((src_k - skip_k))
	fi
	tgt_k="$(df -k "$MNT" 2>/dev/null | awk 'NR==2{print $4}')"
	if [ "$COPY" = 1 ] && [ "$src_k" -gt "$tgt_k" ]; then
		echo "do-install.sh: $DISK is too small: $src_k KB to copy, $tgt_k KB free" >&2
		exit 1
	fi

	# Progress through the copy, by what has reached the target: from 16
	# to 85 as the target's used space approaches the source's. In the
	# background, killed once cpdup is done (or by cleanup()).
	base_k="$(target_used_k)"
	(
		last=16
		while sleep 2; do
			now_k="$(target_used_k)"
			p="$(awk -v s="$src_k" -v b="$base_k" -v n="$now_k" 'BEGIN{
				p = s > 0 ? 16 + int(70 * (n - b) / s) : 16
				if (p > 85) p = 85
				print p }')"
			if [ "$p" -gt "$last" ] 2>/dev/null; then progress "$p"; last=$p; fi
		done
	) &
	MONITOR=$!
fi

status "Cloning base with cpdup"
if [ "$COPY" = 1 ]; then
	run cpdup -i0 -X "$EXCLUDE" "$SRC" "$MNT"
elif [ "$UPGRADE" = 1 ]; then
	# Upgrade: replace base, KEEP data. -o = don't delete extras in the
	# preserved trees; we clone over the top but spare /etc /var /home /Users.
	for keep in etc var home Users usr/local; do
		[ -e "$MNT/$keep" ] && run cpdup -o -i0 "$SRC/$keep" "$MNT/$keep"
	done
	run cpdup -i0 -X /dev/null "$SRC" "$MNT"
else
	run cpdup -i0 "$SRC" "$MNT"
fi
if [ -n "$MONITOR" ]; then kill "$MONITOR" 2>/dev/null || true; MONITOR=""; fi
if [ -n "$EXCLUDE" ]; then rm -f "$EXCLUDE"; fi
progress 86
phase setup

# --- 5. Boot-clean the volatile dirs (no rc.d cleanvar/cleartmp on NextBSD) --
status "Scrubbing volatile state"
for d in tmp var/run var/tmp var/log; do
	[ -d "$MNT/$d" ] && run find "$MNT/$d" -mindepth 1 -delete 2>/dev/null || true
done
# The temporary datasets' own top directories, as /tmp must be.
if [ "$FS" = zfs ]; then
	run chmod 1777 "$MNT/private/tmp" "$MNT/private/var/tmp"
fi

# --- 5, copy mode: a machine of its own ---------------------------------------
# Copy mode keeps the accounts, apps and settings -- that's the point -- but
# not what identifies this particular machine, which two systems must not
# share: the SSH host keys (sshd-keygen-wrapper makes new ones at the copy's
# first boot), a recorded host ID or machine ID, the random seed, and the DHCP
# client's identifier and leases. The computer's name is a setting and stays.
if [ "$COPY" = 1 ]; then
	status "Giving the copy its own identity"
	for f in "$MNT"/private/etc/ssh/ssh_host_*key "$MNT"/private/etc/ssh/ssh_host_*key.pub \
	    "$MNT/private/etc/hostid" "$MNT/private/etc/machine-id" \
	    "$MNT/private/var/db/hostid" "$MNT/boot/entropy" \
	    "$MNT/private/var/db/dhcpclient/DUID_IA.plist"; do
		if [ -e "$f" ]; then run rm -f "$f"; fi
	done
	for d in private/var/db/entropy private/var/db/dhcpclient/leases; do
		[ -d "$MNT/$d" ] && run find "$MNT/$d" -mindepth 1 -delete 2>/dev/null || true
	done
fi
progress 88

# --- 5a. Swap lives in files, not a partition --------------------------------
# NextBSD follows Darwin: there is no freebsd-swap partition and no swap sizing
# at install time (neither layout above creates one). Swapfiles and the sleep
# image live in /private/var/vm, managed at runtime by swapd
# (nextbsd-userland#178). Darwin ships it root:wheel 0755. Created after the
# clone so it exists on both a fresh install and an upgrade, under both the
# mbr-fat and gpt layouts.
#
# TODO(ZFS, nextbsd-userland#179): on a ZFS root this becomes datasets created
# early, while the pool is unfragmented (gang blocks break swapd's extent walk):
#   zroot/private         mountpoint=/private canmount=off
#   zroot/private/var     canmount=off
#   zroot/private/var/vm  checksum=off compression=off dedup=off copies=1
#                         recordsize=128K com.sun:auto-snapshot=false
#   zroot/private/var/crash  exec=off setuid=off
# The installer is UFS-only today, so only the directory is created here.
status "Creating /private/var/vm"
run install -d -o root -g wheel -m 0755 "$MNT/private/var/vm"

# --- 5b. Point the cloned system at its OWN root label -----------------------
# Root is found by vfs.root.mountfrom, never by an fstab line: the kernel's
# baked ROOTDEVNAME=ufs/ROOTFS is only a fallback, and the target is labelled
# $LABEL so it never collides with a ROOTFS medium. EFI gets the override in
# loader.conf below; a Pi gets it in cmdline.txt (step 6).
#
# Media built after nextbsd-overlays#5 ship no /etc/fstab, so there is normally
# nothing to fix. A medium built before that still clones an fstab whose root
# line names ufs/ROOTFS; repoint it so launchctl's boot-time mount -a doesn't
# chase the medium's label.
if [ "$FS" = zfs ]; then status "Setting boot config ($POOL/ROOT/default)"; else status "Setting root label + boot config ($LABEL)"; fi
[ -f "$MNT/etc/fstab" ] && run sed -i '' -e "s#/dev/ufs/ROOTFS#/dev/ufs/$LABEL#g" "$MNT/etc/fstab"
# A copied system's fstab, if it has one, names the running system's label.
[ "$COPY" = 1 ] && [ -f "$MNT/etc/fstab" ] && run sed -i '' -e "s#/dev/ufs/NEXTBSD[^[:space:]]*#/dev/ufs/$LABEL#g" "$MNT/etc/fstab"
# loader.conf only means something where there IS a loader. A Pi has none --
# the firmware enters the kernel directly -- so step 6 puts the same
# vfs.root.mountfrom on the cmdline.txt line instead.
#
# On ZFS there is no root line at all: the loader finds the pool on the disk
# it started from and passes its bootfs (vfs.root.mountfrom=zfs:NextBSD/ROOT/
# default), which bectl activate changes. A USB target gets a mount-root
# timeout, as cmdline.txt does on a Pi (is_usb_disk).
if [ "$PAYLOAD" = efi ]; then
	# One root line: a copied loader.conf already has the running
	# system's.
	[ -f "$MNT/boot/loader.conf" ] && run sed -i '' -e '/^vfs\.root\.mountfrom=/d' -e '/^vfs\.mountroot\.timeout=/d' "$MNT/boot/loader.conf"
	if [ "$FS" = ufs ]; then
		run sh -c "echo 'vfs.root.mountfrom=\"ufs:/dev/ufs/$LABEL\"' >> '$MNT/boot/loader.conf'"
	fi
	if is_usb_disk; then
		run sh -c "echo 'vfs.mountroot.timeout=\"30\"' >> '$MNT/boot/loader.conf'"
	fi
fi
progress 90

# --- 6. Make the disk bootable ----------------------------------------------
if [ "$PAYLOAD" = firmware ]; then
	# The firmware boot partition: config.txt, the board DTB, and the kernel
	# as kernel8.img. No bootcode, no ESP, no loader -- the EEPROM
	# bootloader reads config.txt and enters the kernel itself.
	status "Installing the Raspberry Pi boot partition"
	BOOTMNT=/tmp/nbi-boot
	run mkdir -p "$BOOTMNT"
	run mount -t msdosfs "/dev/$BOOTPART" "$BOOTMNT"

	# The live medium's boot partition, located by label without mounting
	# anything (see find_boot_source). We mount it read-only here, and only
	# here, because copying files requires it.
	if ! find_boot_source; then
		if [ "${NEXTBSD_DRYRUN:-0}" != 1 ]; then
			echo "do-install.sh: cannot find the boot partition we started from" >&2
			echo "  (looked for a FAT volume labelled NEXTBSD, excluding $DISK)" >&2
			exit 1
		fi
	fi

	# Reproduce the boot setup we are running from, rather than generating one.
	# The medium booted this machine, so its boot partition demonstrably works
	# on this hardware: config.txt, the DTB it names, any overlays, and the
	# firmware blobs a pre-2712 Pi needs. Copying it wholesale is correct by
	# construction and needs no per-board knowledge -- which also means a Pi 4,
	# or a board nobody has thought of yet, works with no changes here.
	status "Copying the boot setup from ${BOOTSRC:-<dryrun>}"
	if [ -n "$BOOTSRC" ]; then
		# diskarbitrationd has usually mounted the medium's boot partition
		# under /Volumes already, and msdosfs refuses a second mount of the
		# same device ("Device busy"). umount_disk_partitions only clears the
		# TARGET, so read the files through the existing mount instead of
		# unmounting a volume the live system owns.
		SRCDIR=$(mount | while read -r line; do
			[ "$(echo "$line" | cut -d' ' -f1)" = "$BOOTSRC" ] || continue
			echo "$line" | sed 's/^[^ ]* on \(.*\) (.*)/\1/'
			break
		done)
		if [ -z "$SRCDIR" ]; then
			SRCDIR=$SRCBOOT
			run mkdir -p "$SRCBOOT"
			run mount -o ro -t msdosfs "$BOOTSRC" "$SRCBOOT"
		fi
		if [ "${NEXTBSD_DRYRUN:-0}" != 1 ]; then
			for f in "$SRCDIR"/*; do
				[ -e "$f" ] || continue
				case "$(basename "$f")" in
				.Spotlight-V100|.fseventsd|.Trashes) continue ;;
				esac
				cp -R "$f" "$BOOTMNT/"
			done
			DTB=$(sed -n 's/^[[:space:]]*device_tree=[[:space:]]*//p' "$SRCDIR/config.txt" 2>/dev/null | tail -1)
			KERN=$(sed -n 's/^[[:space:]]*kernel=[[:space:]]*//p' "$SRCDIR/config.txt" 2>/dev/null | tail -1)
				status "Boot setup: kernel=${KERN:-<firmware default>} device_tree=${DTB:-<firmware default>}"

			[ "$SRCDIR" = "$SRCBOOT" ] && umount "$SRCBOOT" || true
		else
			echo "DRYRUN: cp -R $SRCDIR/* $BOOTMNT/"
			[ "$SRCDIR" = "$SRCBOOT" ] && echo "DRYRUN: umount $SRCBOOT" || true
		fi
	# Point the installed system at ITS OWN root.
	#
	# The copied cmdline.txt still names the medium's root, and both
	# volumes would otherwise be labelled ROOTFS -- two providers for
	# one label, and the kernel mounts whichever it finds. The old
	# mitigation was "do not leave the stick plugged in", which is not
	# a design.
	#
	# There is no loader here to write a loader.conf, but the firmware
	# does hand /chosen/bootargs to the kernel, and boot_parse_arg()
	# sets any name=value token into the kernel environment. So the
	# same tunable the generic path writes to loader.conf can be
	# passed on the command line instead:
	#
	#   FreeBSD: vfs.root.mountfrom=ufs:/dev/ufs/NEXTBSD
	#
	# This only works because the FreeBSD: guard is now found anywhere
	# on the line rather than only at position 0 -- the Pi firmware
	# prepends a dozen parameters of its own before it
	# (nextbsd-kernel#93).
	# cmdline.txt is ONE line. Raspberry Pi's boot-behaviour.adoc is
	# explicit: "All parameters in cmdline.txt must stay on the same single
	# line... the kernel ignores anything after the first line." Appending a
	# second line silently drops it -- which is exactly what happened the
	# first time this ran: the firmware passed only "FreeBSD: -v", the
	# tunable never arrived, and the kernel fell back to its compiled-in
	# ufs:/dev/ufs/ROOTFS and tried to mount the install medium.
	#
	# So rewrite the single line, carrying over the flags the medium had.
	#
	# On ZFS the line names the boot environment, and the kernel imports
	# the pool itself (spa_import_rootpool scans the disks; no loader, no
	# zpool.cache). vfs.mountroot.timeout always goes with it: measured on
	# a Pi 500+, the first mount try runs before a USB disk attaches, and
	# a ZFS root that isn't found drops to mountroot>. No fallback list:
	# the line is split on spaces, with no quoting.
	if [ "$FS" = zfs ]; then
		ROOTARG="vfs.root.mountfrom=zfs:$POOL/ROOT/default vfs.mountroot.timeout=30"
	else
		ROOTARG="vfs.root.mountfrom=ufs:/dev/ufs/$LABEL"
	fi
	status "Pointing cmdline.txt at ${ROOTARG%% *}"
	OLDCMD=""
	if [ -f "$BOOTMNT/cmdline.txt" ]; then
		OLDCMD=$(head -1 "$BOOTMNT/cmdline.txt" 2>/dev/null |
		    sed -e 's/[[:space:]]*vfs\.root\.mountfrom=[^[:space:]]*//g' \
		        -e 's/[[:space:]]*vfs\.mountroot\.timeout=[^[:space:]]*//g')
	fi
	[ -n "$OLDCMD" ] || OLDCMD="FreeBSD: -v"
	run sh -c "printf '%s %s\\n' '$OLDCMD' '$ROOTARG' > '$BOOTMNT/cmdline.txt'"
	fi

	run umount "$BOOTMNT"
	progress 94
else
	# gptzfsboot for a pool, gptboot for UFS.
	BOOTBLK=gptboot
	[ "$FS" = zfs ] && BOOTBLK=gptzfsboot
	status "Installing bootcode (UEFI loader + BIOS $BOOTBLK if present)"
	# BIOS bootcode only when the boot blocks exist (amd64 legacy). EFI-only
	# boxes and arm64 ship no pmbr/gptboot -- skip instead of failing; the ESP
	# loader below is what boots UEFI machines.
	if [ -f "$MNT/boot/pmbr" ] && [ -f "$MNT/boot/$BOOTBLK" ]; then
		run gpart bootcode -b "$MNT/boot/pmbr" -p "$MNT/boot/$BOOTBLK" -i 1 "$DISK"
	else
		status "No BIOS boot blocks present (EFI-only) — skipping $BOOTBLK"
	fi
	# Populate the ESP with the EFI loader at the firmware removable-media path
	# (works with no NVRAM entry). arm64 firmware looks for BOOTAA64.EFI, amd64
	# for BOOTX64.EFI.
	case "$(uname -m)" in
		arm64|aarch64) EFIFILE=BOOTAA64.EFI ;;
		*)             EFIFILE=BOOTX64.EFI  ;;
	esac
	EFIMNT=/tmp/nbi-efi
	run mkdir -p "$EFIMNT" "$MNT/boot/efi"
	run mount -t msdosfs "/dev/$BOOTPART" "$EFIMNT"
	run mkdir -p "$EFIMNT/EFI/BOOT"
	run cp "$MNT/boot/loader.efi" "$EFIMNT/EFI/BOOT/$EFIFILE"
	# Best-effort named UEFI boot entry (non-fatal: the removable path above
	# already boots if the firmware ignores or loses NVRAM entries). Not for
	# a copy, which goes on a second disk: this machine should go on
	# starting the system it runs now. And never for a test disk (md),
	# which isn't a disk the firmware could start at all.
	case "$DISK" in md*) NVRAM=0 ;; *) NVRAM=$((1 - COPY)) ;; esac
	if [ "$NVRAM" = 1 ] && [ "${NEXTBSD_DRYRUN:-0}" != 1 ] && command -v efibootmgr >/dev/null 2>&1; then
		efibootmgr 2>/dev/null \
			| sed -n 's/^[[:space:]+-]*Boot\([0-9A-Fa-f]\{4\}\)\*\{0,1\}[[:space:]]\{1,\}NextBSD$/\1/p' \
			| while read -r bootnum; do
				efibootmgr -B -b "$bootnum" >/dev/null 2>&1 || true
			done
		efibootmgr --create --activate --label NextBSD \
			--loader "$EFIMNT/EFI/BOOT/$EFIFILE" >/dev/null 2>&1 || \
			status "efibootmgr entry skipped (removable-path boot still installed)"
	fi
	run umount "$EFIMNT"
	progress 94
fi

# --- 7. Account + hostname: intentionally none ------------------------------
# No user is created and no hostname is written. The cloned base ships a
# passwordless root, and hostnamed synthesizes a default hostname at runtime,
# so the system is usable on first boot. The operator sets a root password,
# adds users, and changes the hostname after install.
progress 98

# --- 8. Done ----------------------------------------------------------------
phase finish
status "Finalizing"
if [ "$FS" = zfs ]; then
	# Exported, so the installed system's kernel (or loader) imports it
	# fresh under its own name, and this system keeps no record of it.
	run zpool export "$TPOOL"
	POOLMADE=""
else
	run umount "$MNT" || true
fi
progress 100
echo "DONE"
