#!/bin/sh
# do-install.sh — the NextBSD whole-disk install engine.
#
# Driven by the front-end (which parses the PROGRESS/STATUS lines below), but
# also runnable standalone. Honors NEXTBSD_DRYRUN=1 (print every destructive
# command instead of running it) so it's safe to exercise off-target.
#
# Design notes baked in:
#   * Whole-disk GPT: freebsd-boot (BIOS) + EFI (ESP) + freebsd-ufs root.
#   * ...EXCEPT on a Raspberry Pi 5-family board, which cannot boot that at
#     all. There is no UEFI and no loader(8): the EEPROM bootloader reads
#     config.txt off a FAT partition and enters the kernel directly. That
#     needs MBR + FAT32 + UFS, selected automatically -- see LAYOUT/PAYLOAD.
#   * UFS root labeled NEXTBSD (not the medium's ROOTFS; see LABEL below),
#     found by vfs.root.mountfrom -- loader.conf on EFI, cmdline.txt on a
#     Pi -- so the install is disk-path agnostic (ada0/nvd0/vtbd0 all just
#     work). No /etc/fstab is needed for root (nextbsd-overlays#5).
#   * Clone with cpdup from the live / (union ISO or plain image alike), then
#     SCRUB the volatile dirs — NextBSD has no rc.d cleanvar/cleartmp, so the
#     installer must hand off a boot-clean /var and /tmp itself.
#
# Usage: do-install.sh -d <disk> [-U]
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
cleanup() {
	umount "$SRCBOOT" 2>/dev/null || true
	rmdir "$SRCBOOT" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

DISK="" UPGRADE=0
MNT=/tmp/nbi-mnt
SRC=/
# Root label for the INSTALLED system — deliberately NOT "ROOTFS" (the live
# install medium's label). A shared label makes a leftover/failed install a
# boot-hijacker: two ufs/ROOTFS providers and the kernel mounts the wrong one.
# vfs.root.mountfrom in loader.conf (EFI, step 5b) or cmdline.txt (Pi, step 6)
# points the target here.
LABEL=NEXTBSD

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

while getopts "d:U" o; do
	case "$o" in
		d) DISK=$OPTARG ;;
		U) UPGRADE=1 ;;
		*) echo "usage: do-install.sh -d disk [-U]" >&2; exit 2 ;;
	esac
done
[ -n "$DISK" ] || { echo "do-install.sh: missing -d <disk>" >&2; exit 2; }

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
run() {
	if [ "${NEXTBSD_DRYRUN:-0}" = 1 ]; then echo "DRYRUN: $*"; else "$@"; fi
}

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
		# The UFS root lives in a BSD label inside the freebsd slice.
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
		run gpart create -s bsd "/dev/${DISK}s2"
		run gpart add -t freebsd-ufs "/dev/${DISK}s2"
		progress 8

		status "Creating UFS root (label $LABEL)"
		run newfs -U -L "$LABEL" "/dev/${DISK}s2a"
		# -c 1: one sector per cluster. Without it newfs_msdos picks a
		# larger cluster and a 100 MB partition yields ~12,700 clusters,
		# below the 65,525 that *defines* FAT32 -- so it refuses. This is
		# the same choice build.sh makes (sectors_per_cluster=1) and that
		# Raspberry Pi OS makes (mkdosfs -F 32 -s 1).
		run newfs_msdos -F 32 -c 1 -L NEXTBSD "/dev/${DISK}s1"
		progress 14
	else
		status "Partitioning $DISK (GPT: freebsd-boot + EFI + freebsd-ufs)"
		progress 4
		run gpart destroy -F "$DISK" 2>/dev/null || true
		run gpart create -s gpt "$DISK"
		run gpart add -a 4k -s 512k -t freebsd-boot -l bootcode "$DISK"
		run gpart add -a 1m   -s 260m -t efi          -l EFI      "$DISK"
		run gpart add -a 1m            -t freebsd-ufs  -l ROOTFS   "$DISK"
		progress 8

		# --- 2. Filesystems -------------------------------------------
		status "Creating UFS root (label $LABEL)"
		run newfs -U -L "$LABEL" "/dev/${DISK}p3"
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
status "Mounting target"
run mkdir -p "$MNT"
run mount "/dev/$ROOTPART" "$MNT"
progress 16

# --- 4. Clone the running system (cpdup) ------------------------------------
# Works the same on the union ISO and a plain image: cpdup reads / through the
# VFS and (don't-cross-mounts) skips /dev and $MNT automatically. cpdup vendored
# alongside this engine.
status "Cloning base with cpdup"
if [ "$UPGRADE" = 1 ]; then
	# Upgrade: replace base, KEEP data. -o = don't delete extras in the
	# preserved trees; we clone over the top but spare /etc /var /home /Users.
	for keep in etc var home Users usr/local; do
		[ -e "$MNT/$keep" ] && run cpdup -o -i0 "$SRC/$keep" "$MNT/$keep"
	done
	run cpdup -i0 -X /dev/null "$SRC" "$MNT"
else
	run cpdup -i0 "$SRC" "$MNT"
fi
progress 86

# --- 5. Boot-clean the volatile dirs (no rc.d cleanvar/cleartmp on NextBSD) --
status "Scrubbing volatile state"
for d in tmp var/run var/tmp var/log; do
	[ -d "$MNT/$d" ] && run find "$MNT/$d" -mindepth 1 -delete 2>/dev/null || true
done
progress 88

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
status "Setting root label + boot config ($LABEL)"
[ -f "$MNT/etc/fstab" ] && run sed -i '' -e "s#/dev/ufs/ROOTFS#/dev/ufs/$LABEL#g" "$MNT/etc/fstab"
# loader.conf only means something where there IS a loader. A Pi has none --
# the firmware enters the kernel directly -- so step 6 puts the same
# vfs.root.mountfrom on the cmdline.txt line instead.
if [ "$PAYLOAD" = efi ]; then
	run sh -c "echo 'vfs.root.mountfrom=\"ufs:/dev/ufs/$LABEL\"' >> '$MNT/boot/loader.conf'"
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
		run mkdir -p "$SRCBOOT"
		run mount -o ro -t msdosfs "$BOOTSRC" "$SRCBOOT"
		if [ "${NEXTBSD_DRYRUN:-0}" != 1 ]; then
			for f in "$SRCBOOT"/*; do
				[ -e "$f" ] || continue
				case "$(basename "$f")" in
				.Spotlight-V100|.fseventsd|.Trashes) continue ;;
				esac
				cp -R "$f" "$BOOTMNT/"
			done
			DTB=$(sed -n 's/^[[:space:]]*device_tree=[[:space:]]*//p' "$SRCBOOT/config.txt" 2>/dev/null | tail -1)
			KERN=$(sed -n 's/^[[:space:]]*kernel=[[:space:]]*//p' "$SRCBOOT/config.txt" 2>/dev/null | tail -1)
				status "Boot setup: kernel=${KERN:-<firmware default>} device_tree=${DTB:-<firmware default>}"

			umount "$SRCBOOT" || true
		else
			echo "DRYRUN: cp -R $SRCBOOT/* $BOOTMNT/"
			echo "DRYRUN: umount $SRCBOOT"
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
	status "Pointing cmdline.txt at ufs/$LABEL"
	OLDCMD=""
	if [ -f "$BOOTMNT/cmdline.txt" ]; then
		OLDCMD=$(head -1 "$BOOTMNT/cmdline.txt" 2>/dev/null |
		    sed -e 's/[[:space:]]*vfs\.root\.mountfrom=[^[:space:]]*//g')
	fi
	[ -n "$OLDCMD" ] || OLDCMD="FreeBSD: -v"
	run sh -c "printf '%s vfs.root.mountfrom=ufs:/dev/ufs/%s\\n' '$OLDCMD' '$LABEL' > '$BOOTMNT/cmdline.txt'"
	fi

	run umount "$BOOTMNT"
	progress 94
else
	status "Installing bootcode (UEFI loader + BIOS gptboot if present)"
	# BIOS bootcode only when the boot blocks exist (amd64 legacy). EFI-only
	# boxes and arm64 ship no pmbr/gptboot -- skip instead of failing; the ESP
	# loader below is what boots UEFI machines.
	if [ -f "$MNT/boot/pmbr" ] && [ -f "$MNT/boot/gptboot" ]; then
		run gpart bootcode -b "$MNT/boot/pmbr" -p "$MNT/boot/gptboot" -i 1 "$DISK"
	else
		status "No BIOS boot blocks present (EFI-only) — skipping gptboot"
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
	# already boots if the firmware ignores or loses NVRAM entries).
	if [ "${NEXTBSD_DRYRUN:-0}" != 1 ] && command -v efibootmgr >/dev/null 2>&1; then
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
status "Finalizing"
run umount "$MNT" || true
progress 100
echo "DONE"
