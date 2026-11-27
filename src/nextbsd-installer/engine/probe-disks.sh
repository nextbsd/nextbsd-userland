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
# what you booted. Two sources: the disk under the live ISO's cd9660, AND the
# physical disk backing the live root (/).
#
# The disk a provider sits on: a label (iso9660/NEXTBSD, ufs/ROOTFS) resolved
# through glabel to the provider it labels, then any partition suffix dropped
# (da0p3 -> da0, da0s1a -> da0).
disk_of() {
	_p="${1#/dev/}"
	_l="$(glabel status -s 2>/dev/null | awk -v n="$_p" '$1 == n {print $3; exit}')"
	[ -n "$_l" ] && _p="$_l"
	printf '%s\n' "$_p" | sed -E 's/[ps][0-9]+[a-h]?$//'
}
# The live ISO's /init mounts the boot medium, found by its volume label, as
# cd9660 and runs the system from rootfs.uzip on it. Find that mount by its
# file system and device, not its mount point (init mounted it in the
# miniroot, before the pivot to the union root). Discs and images a user
# opens later mount under /Volumes and are skipped. Then take the disk under
# the label: da0 for an image written to a USB stick, cd0 for a burned disc.
# Before this, media was the label's own name ("NEXTBSD"), which matched no
# disk, so the stick was offered as a target.
media=""
for d in $(mount -p 2>/dev/null | awk '$3 == "cd9660" && $2 !~ /^\/Volumes\// {print $1}'); do
	m="$(disk_of "$d")"
	case "$m" in iso9660/*) continue ;; esac
	media="$m"
	break
done
# Resolve the disk under / through glabel (e.g. ufs/ROOTFS -> da0p3 -> da0).
# A ZFS root (NextBSD/ROOT/27.0.0-192) is on every disk of its pool.
#
# The pools this system has imported, as "pool<TAB>provider" lines, a label
# (gpt/zfs0) resolved to its partition: for the root pool here and for the
# VOL notes below. zpool list reads the kernel's view, no privilege needed.
pool_provs="$(zpool list -vHP 2>/dev/null | awk -F'\t' '
	$1 != "" { pool = $1; next }
	{ for (i = 2; i <= NF; i++) if ($i ~ /^ *\/dev\//) {
		v = $i; sub(/^ *\/dev\//, "", v); print pool "\t" v } }' |
while IFS="$(printf '\t')" read -r pool prov; do
	dev="$(glabel status -s 2>/dev/null | awk -v n="$prov" '$1 == n {print $3; exit}')"
	printf '%s\t%s\n' "$pool" "${dev:-$prov}"
done)"
root_type="$(mount -p 2>/dev/null | awk '$2=="/"{print $3; exit}')"
root_prov="$(mount -p 2>/dev/null | awk '$2=="/"{print $1; exit}')"
root_prov="${root_prov#/dev/}"
root_pool=""
if [ "$root_type" = zfs ]; then
	root_pool="${root_prov%%/*}"
	root_provs="$(printf '%s\n' "$pool_provs" | awk -F'\t' -v p="$root_pool" '$1 == p {print $2}')"
else
	root_provs="$(glabel status 2>/dev/null | awk -v L="$root_prov" '$1==L{print $3; exit}')"
	[ -z "$root_provs" ] && root_provs="$root_prov"
fi
boot_disks="$(printf '%s\n' $root_provs | sed -E 's/[ps][0-9]+[a-h]?$//' | sort -u)"
# The live ISO's root is a union over a tmpfs (/cow), on no disk at all: its
# medium is the cd9660 one above.
[ "$root_type" = unionfs ] && boot_disks=""
[ -n "$media" ] && printf 'MEDIA\t%s\n' "$media"
for d in $boot_disks; do printf 'MEDIA\t%s\n' "$d"; done

# Running from the install image, or from an installed NextBSD? The live ISO's
# root is a union over the compressed image; the disk image's root is labelled
# ROOTFS (build.sh), which do-install.sh deliberately never gives an installed
# system (it labels NEXTBSD).
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
	if [ -n "$root_pool" ]; then
		# The whole pool, not only the boot environment: a copy
		# takes the shared datasets too (logical size, as a copy
		# onto UFS would need it).
		used_k="$(zfs get -Hp -o value logicalused "$root_pool" 2>/dev/null |
		    awk '{printf "%d", $1 / 1024}')"
	else
		used_k="$(df -k / 2>/dev/null | awk 'NR==2{print $3}')"
	fi
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
#
# Only disks something can be installed on. kern.disks lists every GEOM disk,
# which includes what isn't one (#663):
#   cd*, acd*         optical drives (read-only, and the medium a live ISO
#                     was burned to is already marked MEDIA above)
#   mmcsd*boot*,      an eMMC's boot and replay-protected areas, which the
#   *rpmb*            mmc driver publishes beside the user area
# and, below, any disk with no medium in it (an empty card reader reports a
# size of 0).
for dev in $disks; do
	case "$dev" in
	cd[0-9]*|acd[0-9]*|mmcsd*boot*|*rpmb*) continue ;;
	esac
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
	[ "${bytes:-0}" -gt 0 ] 2>/dev/null || continue
	size="$(printf '%s' "${bytes:-0}" | awk '{
		b=$1; u="B"; s=1
		if (b>=1099511627776){u="TB";s=1099511627776}
		else if (b>=1073741824){u="GB";s=1073741824}
		else if (b>=1048576){u="MB";s=1048576}
		printf "%.0f %s", b/s, u }')"
	is_media=0
	[ "$dev" = "$media" ] && is_media=1
	for d in $boot_disks; do [ "$dev" = "$d" ] && is_media=1; done
	printf 'DISK\t%s\t%s\t%s\t%d\t%s\n' "$dev" "${size:-?}" "${model:-disk}" "$is_media" "${bytes:-0}"

	# Volumes: each partition with its label, size and file system. A
	# partition holding a ZFS pool says which in the note -- "(zpool:
	# NextBSD)" is an existing NextBSD install -- from the imported pools,
	# or from the label on the partition itself when the pool isn't
	# imported (zdb needs read access to the device: root only).
	gpart list "$dev" 2>/dev/null | awk '
		/^Consumers:/ { done = 1 }
		done { next }
		/^[0-9]+\. Name: / { if (n != "") print n "|" l "|" z "|" t; n = $3; l = ""; z = ""; t = "" }
		/^   label: / { l = $2; if (l == "(null)") l = "" }
		/^   Mediasize: / { z = $3; gsub(/[()]/, "", z) }
		/^   type: / { t = $2 }
		END { if (n != "") print n "|" l "|" z "|" t }' |
	while IFS='|' read -r prov label psize ptype; do
		fstype="$ptype" note="" pool=""
		case "$ptype" in
		freebsd-zfs|freebsd)
			pool="$(printf '%s\n' "$pool_provs" | awk -F'\t' -v v="$prov" '$2 == v {print $1; exit}')"
			[ -n "$pool" ] || pool="$(zdb -l "/dev/$prov" 2>/dev/null |
			    awk -F"'" '/^ *name:/ {print $2; exit}')"
			if [ -n "$pool" ]; then fstype=zfs; note="(zpool: $pool)"
			elif [ "$ptype" = freebsd-zfs ]; then fstype=zfs; fi
			;;
		freebsd-ufs) fstype=ufs ;;
		efi|fat32*|fat16|ms-basic-data) fstype=msdosfs ;;
		esac
		printf 'VOL\t%s\t%s\t%s\t%s\t%s\n' "$dev" "${label:-(unlabeled)}" "$psize" "$fstype" "$note"
	done || true
done
