#!/bin/sh
# probe-disks.sh — enumerate candidate install disks for the front-end.
#
# Emits one record per line, tab-separated, in a stable machine-readable form
# the C++ front-end parses:
#
#   DISK<TAB>dev<TAB>size<TAB>model<TAB>is_media(0|1)<TAB>bytes
#   VOL<TAB>dev<TAB>label<TAB>size<TAB>fstype<TAB>note
#   MEDIA<TAB>dev            # the booted install medium (locked)
#   DMI<TAB>model
#   SYSTEM<TAB>image|installed
#   NEED<TAB>bytes           # the smallest disk this system installs onto
#
# is_media marks the disk the running system booted from: the install
# medium in image mode, the current system's own disk once installed.
# SYSTEM says which: "image" when running from the install image (the live
# ISO's union root, or the disk image's ROOTFS), "installed" otherwise, where
# do-install.sh -c copies the running system. Installer.app reads both.
#
# Read-only: this only inspects GEOM/CAM, it never writes. FreeBSD-only.
# Sizes and models come from GEOM's configuration (geom disk list), which any
# user can read, so Installer.app can probe without privilege; diskinfo(8)
# needs read access to the device and is only the fallback.
#
# TEST DISKS: memory disks (md) are never listed, except while the file
# /var/run/nextbsd-installer.test-disks exists. Only root can create it, and
# /var/run starts empty at every boot. Then vnode-backed md units are listed
# too, so an install can be tried on a file (mdconfig -a -t vnode -f FILE)
# without touching a real disk.
set -eu

TEST_DISKS=/var/run/nextbsd-installer.test-disks

# The smallest disk worth installing on: 30,000,000,000 bytes, so a stick
# sold as "32 GB" (31 or so billion bytes) qualifies.
MIN_BYTES=30000000000

# The booted install medium — locked in the front-end so you can't install onto
# what you booted. Two sources: a cd9660/cd node, AND the physical disk backing
# the live root (/). The common case is a USB the image was written to (e.g.
# da0), which the old cd-only check missed — so it was offered as a target.
media=""
for d in /dev/iso9660/* /dev/cd0; do
	[ -e "$d" ] || continue
	media="${d##*/}"
	break
done
# Resolve the disk under / through glabel (e.g. ufs/ROOTFS -> da0p3 -> da0).
root_prov="$(mount -p 2>/dev/null | awk '$2=="/"{print $1; exit}')"
root_prov="${root_prov#/dev/}"
boot_disk="$(glabel status 2>/dev/null | awk -v L="$root_prov" '$1==L{print $3; exit}')"
[ -z "$boot_disk" ] && boot_disk="$root_prov"
boot_disk="$(printf '%s' "$boot_disk" | sed -E 's/[ps][0-9]+$//')"
[ -n "$media" ]     && printf 'MEDIA\t%s\n' "$media"
[ -n "$boot_disk" ] && printf 'MEDIA\t%s\n' "$boot_disk"

# Running from the install image, or from an installed NextBSD? The live ISO's
# root is a union over the compressed image; the disk image's root is labelled
# ROOTFS (build.sh), which do-install.sh deliberately never gives an installed
# system (it labels NEXTBSD).
root_type="$(mount -p 2>/dev/null | awk '$2=="/"{print $3; exit}')"
if [ "$root_type" = unionfs ] || [ "$root_prov" = ufs/ROOTFS ]; then
	system=image
else
	system=installed
fi
printf 'SYSTEM\t%s\n' "$system"

# The smallest usable disk. A copy of an installed system also has to hold
# what's on it now, with a tenth to spare -- less what a copy leaves out
# (do-install.sh -c), which may be most of it: a file-backed disk in /tmp.
need=$MIN_BYTES
if [ "$system" = installed ]; then
	used_k="$(df -k / 2>/dev/null | awk 'NR==2{print $3}')"
	skip_k="$(du -skx /private/tmp /private/var/tmp /private/var/vm 2>/dev/null |
	    awk '{s += $1} END {print s + 0}')"
	used_k=$((${used_k:-0} - skip_k))
	need="$(printf '%s %s' "${used_k:-0}" "$MIN_BYTES" | awk '{
		n = $1 * 1024 * 1.1 + 1073741824
		if (n < $2) n = $2
		printf "%.0f", n }')"
fi
printf 'NEED\t%s\n' "$need"

# DMI model for the hostname suggestion. Lenovo puts the friendly name
# ("ThinkPad T460s") in smbios.system.version and the bare MTM code
# ("20FAS0GL00") in system.product; most other vendors use system.product.
dmi_maker="$(kenv -q smbios.system.maker 2>/dev/null || true)"
case "$dmi_maker" in
	*[Ll][Ee][Nn][Oo][Vv][Oo]*) model="$(kenv -q smbios.system.version 2>/dev/null || true)" ;;
	*)                            model="$(kenv -q smbios.system.product 2>/dev/null || true)" ;;
esac
[ -z "$model" ] && model="$(kenv -q smbios.system.product 2>/dev/null || true)"
[ -n "$model" ] && printf 'DMI\t%s\n' "$model"

# The disks: kern.disks, plus the test disks while they're allowed.
disks="$(sysctl -n kern.disks 2>/dev/null || true)"
if [ -e "$TEST_DISKS" ]; then
	disks="$disks $(geom md list 2>/dev/null | awk '
		/^Geom name:/ { name = $3 }
		/^ *type: vnode/ { print name }')"
fi

# One DISK line per disk, with size + model, then its volumes.
for dev in $disks; do
	case "$dev" in
	md*)
		bytes="$(geom md list "$dev" 2>/dev/null | awk '/Mediasize:/{print $2; exit}')"
		model="Test disk ($dev)"
		;;
	*)
		bytes="$(geom disk list "$dev" 2>/dev/null | awk '/Mediasize:/{print $2; exit}')"
		model="$(geom disk list "$dev" 2>/dev/null | sed -n 's/^ *descr: *//p' | head -1)"
		[ -n "$bytes" ] || bytes="$(diskinfo "$dev" 2>/dev/null | awk '{print $3}')"
		[ -n "$model" ] || model="$(diskinfo -v "$dev" 2>/dev/null | awk -F'#' '/Disk descr/{gsub(/^[ \t]+|[ \t]+$/,"",$1); print $1}')"
		;;
	esac
	size="$(printf '%s' "${bytes:-0}" | awk '{
		b=$1; u="B"; s=1
		if (b>=1099511627776){u="TB";s=1099511627776}
		else if (b>=1073741824){u="GB";s=1073741824}
		else if (b>=1048576){u="MB";s=1048576}
		printf "%.0f %s", b/s, u }')"
	is_media=0
	{ [ "$dev" = "$media" ] || [ "$dev" = "$boot_disk" ]; } && is_media=1
	printf 'DISK\t%s\t%s\t%s\t%d\t%s\n' "$dev" "${size:-?}" "${model:-disk}" "$is_media" "${bytes:-0}"

	# Volumes: GPT partitions with their labels + fs type.
	gpart show -l "$dev" 2>/dev/null | awk -v d="$dev" '
		/freebsd-|efi|ms-basic|fat|linux/ {
			label=$4; if (label=="-") label="(unlabeled)"
			print "VOL\t" d "\t" label "\t" $2 "\t" $5 "\t"
		}' 2>/dev/null || true
done
