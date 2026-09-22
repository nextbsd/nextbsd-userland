#!/bin/sh
#
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 The NextBSD Project
#
# adduser(8) for NextBSD (E18 U9): the familiar prompts, creating a
# directory user with dscli(8). System accounts are pw(8)'s business.

set -u

PATH=/bin:/usr/bin:/sbin:/usr/sbin
DSCLI=/usr/sbin/dscli
prog=${0##*/}
silent=no
name=

usage() {
	echo "usage: $prog [-S] [username]" >&2
	exit 64
}

while getopts "Sh" opt; do
	case $opt in
	S) silent=yes ;;
	*) usage ;;
	esac
done
shift $((OPTIND - 1))
[ $# -le 1 ] || usage
[ $# -eq 1 ] && name=$1

if [ "$(id -u)" -ne 0 ]; then
	echo "$prog: you must be root" >&2
	exit 77
fi
if [ ! -x "$DSCLI" ]; then
	echo "$prog: $DSCLI is missing" >&2
	exit 69
fi

# ask PROMPT DEFAULT -> $answer
ask() {
	if [ -n "$2" ]; then
		printf '%s [%s]: ' "$1" "$2"
	else
		printf '%s: ' "$1"
	fi
	if ! IFS= read -r answer; then
		echo
		exit 1
	fi
	[ -n "$answer" ] || answer=$2
}

# ask_password -> $password (empty means no password)
ask_password() {
	if [ -t 0 ]; then
		stty -echo
		trap 'stty echo' EXIT
	fi
	while :; do
		printf 'Password (empty for none): '
		IFS= read -r p1 || { echo; exit 1; }
		echo
		if [ -z "$p1" ]; then
			password=
			break
		fi
		printf 'Retype password: '
		IFS= read -r p2 || { echo; exit 1; }
		echo
		if [ "$p1" = "$p2" ]; then
			password=$p1
			break
		fi
		echo "Passwords do not match; try again."
	done
	if [ -t 0 ]; then
		stty echo
		trap - EXIT
	fi
}

shells=$(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' /etc/shells 2>/dev/null | tr '\n' ' ')
default_shell=/bin/zsh
[ -x "$default_shell" ] || default_shell=/bin/sh

[ "$silent" = yes ] || echo "Adding a user to the DirectoryServices database."
while :; do
	if [ -z "$name" ]; then
		ask "Username" ""
		name=$answer
	fi
	case $name in
	"" | -* | *[!A-Za-z0-9._-]*)
		echo "$prog: '$name' is not a valid user name" >&2
		name=
		continue
		;;
	esac
	if "$DSCLI" user show "$name" >/dev/null 2>&1 || getent passwd "$name" >/dev/null 2>&1; then
		echo "$prog: user '$name' already exists" >&2
		name=
		continue
	fi
	break
done

ask "Full name" ""
fullname=$answer
ask "Uid (leave empty for the next free)" ""
uid=$answer
case $uid in
"" | *[!0-9]*) uid= ;;
esac
while :; do
	ask "Shell ($shells)" "$default_shell"
	shell=$answer
	case " $shells " in
	*" $shell "*) break ;;
	esac
	echo "$shell is not listed in /etc/shells."
done
ask "Administrator (member of admin, may use sudo)? (yes/no)" "no"
admin=no
case $answer in
[Yy]*) admin=yes ;;
esac
ask_password

echo
echo "Username:   $name"
echo "Full name:  $fullname"
echo "Uid:        ${uid:-<next free>}"
echo "Shell:      $shell"
echo "Admin:      $admin"
echo "Password:   $([ -n "$password" ] && echo set || echo none)"
echo "Home:       /Local/Users/$name"
ask "OK? (yes/no)" "yes"
case $answer in
[Yy]*) ;;
*) echo "$prog: cancelled"; exit 1 ;;
esac

set -- user add "$name" --shell "$shell" --no-password
[ -n "$fullname" ] && set -- "$@" --real-name "$fullname"
[ -n "$uid" ] && set -- "$@" --uid "$uid"
[ "$admin" = yes ] && set -- "$@" --admin
"$DSCLI" "$@" || exit
if [ -n "$password" ]; then
	printf '%s\n' "$password" | "$DSCLI" user passwd "$name" || exit
fi
[ "$silent" = yes ] || echo "$prog: added user $name"
