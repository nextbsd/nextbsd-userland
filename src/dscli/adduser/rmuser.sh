#!/bin/sh
#
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 The NextBSD Project
#
# rmuser(8) for NextBSD (E18 U9): remove a directory user with dscli(8),
# after confirming. System accounts are pw(8)'s business.

set -u

PATH=/bin:/usr/bin:/sbin:/usr/sbin
DSCLI=/usr/sbin/dscli
prog=${0##*/}
yes=no

usage() {
	echo "usage: $prog [-y] username ..." >&2
	exit 64
}

while getopts "yh" opt; do
	case $opt in
	y) yes=yes ;;
	*) usage ;;
	esac
done
shift $((OPTIND - 1))
[ $# -ge 1 ] || usage

if [ "$(id -u)" -ne 0 ]; then
	echo "$prog: you must be root" >&2
	exit 77
fi

ask() {
	printf '%s [%s]: ' "$1" "$2"
	if ! IFS= read -r answer; then
		echo
		exit 1
	fi
	[ -n "$answer" ] || answer=$2
}

status=0
for name; do
	if ! "$DSCLI" user show "$name" >/dev/null 2>&1; then
		if getent passwd "$name" >/dev/null 2>&1; then
			echo "$prog: $name is a system account; use pw userdel" >&2
		else
			echo "$prog: user '$name' does not exist" >&2
		fi
		status=1
		continue
	fi
	"$DSCLI" user show "$name"
	remove_home=yes
	if [ "$yes" != yes ]; then
		ask "Remove user $name? (yes/no)" "no"
		case $answer in
		[Yy]*) ;;
		*) continue ;;
		esac
		ask "Remove the home /Local/Users/$name too? (yes/no)" "yes"
		case $answer in
		[Yy]*) ;;
		*) remove_home=no ;;
		esac
	fi
	if [ "$remove_home" = yes ]; then
		"$DSCLI" user delete "$name" --remove-home || status=1
	else
		"$DSCLI" user delete "$name" || status=1
	fi
done
exit $status
