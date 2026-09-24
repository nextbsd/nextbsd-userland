#!/bin/sh
# Host-side tests for autologin-user.
#
# The program prints "admin" when the console may log in with no password, and
# prints nothing otherwise. Every clause is checked by mutation: start from the
# one state that should print, break exactly one thing, and require silence.
# A clause that can be deleted without a test failing is not a clause.
#
#   sh autologin_test.sh
#
# Run under sh, not zsh.
set -u

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../../.." && pwd)
fix=${TMPDIR:-/tmp}/autologin_test.$$
trap 'rm -rf "$fix"' EXIT
mkdir -p "$fix"

${CC:-cc} -O1 -g -Wall -Wextra -Wshadow -Wstrict-prototypes \
    -Wmissing-prototypes \
    -DLOCAL_USERS="\"$fix/Users.plist\"" \
    -DNETWORK_USERS="\"$fix/Network-Users.plist\"" \
    -DLOGINWINDOW_PLIST="\"$fix/loginwindow.plist\"" \
    -I"$top/src/nss_directory_services" \
    -o "$fix/autologin-user" \
    "$top/src/accounts/autologin-user/autologin.c" \
    "$top/src/nss_directory_services/plist.c" || exit 2

pass=0; fail=0

# The one state that permits automatic login: a single user, named admin,
# with noPassword, no network node, no login window.
seed() {
	rm -f "$fix/Network-Users.plist" "$fix/loginwindow.plist"
	cat > "$fix/Users.plist" <<'P'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
<key>admin</key><dict>
<key>username</key><string>admin</string>
<key>uid</key><integer>5000</integer>
<key>gid</key><integer>5000</integer>
<key>realName</key><string>Administrator</string>
<key>shell</key><string>/usr/local/bin/zsh</string>
<key>noPassword</key><true/>
</dict>
</dict></plist>
P
}

cksays() {	# cksays <label> <expected stdout>
	_lbl=$1; _want=$2
	_got=$("$fix/autologin-user" 2>/dev/null </dev/null)
	if [ "$_got" = "$_want" ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		printf 'FAIL %-52s said [%s], wanted [%s]\n' "$_lbl" "$_got" "$_want"
	fi
}

# ---- the permitting state
seed
cksays "one admin with noPassword logs in automatically" "admin"

# ---- clause: a login window is installed
seed
: > "$fix/loginwindow.plist"
cksays "a login window suppresses it" ""
rm -f "$fix/loginwindow.plist"
cksays "and removing the login window restores it" "admin"

# An empty file is still installed; so is one with content.
seed
echo '<plist version="1.0"><dict/></plist>' > "$fix/loginwindow.plist"
cksays "the login window's contents do not matter" ""

# ---- clause: a joined directory client
seed
: > "$fix/Network-Users.plist"
cksays "a joined client suppresses it" ""

# ---- clause: exactly one user
seed
sed -e 's#</dict></plist>#<key>joe</key><dict><key>username</key><string>joe</string><key>uid</key><integer>5001</integer></dict></dict></plist>#' \
    "$fix/Users.plist" > "$fix/Users.plist.new" && mv "$fix/Users.plist.new" "$fix/Users.plist"
cksays "a second account suppresses it" ""

# ---- clause: the user is named admin
seed
sed -e 's/>admin</>joe</g' -e 's/<key>admin<\/key>/<key>joe<\/key>/' \
    "$fix/Users.plist" > "$fix/Users.plist.new" && mv "$fix/Users.plist.new" "$fix/Users.plist"
cksays "a sole user not named admin suppresses it" ""

# ---- clause: noPassword
seed
sed -e 's#<key>noPassword</key><true/>##' \
    "$fix/Users.plist" > "$fix/Users.plist.new" && mv "$fix/Users.plist.new" "$fix/Users.plist"
cksays "no noPassword key suppresses it" ""

seed
sed -e 's#<key>noPassword</key><true/>#<key>noPassword</key><false/>#' \
    "$fix/Users.plist" > "$fix/Users.plist.new" && mv "$fix/Users.plist.new" "$fix/Users.plist"
cksays "noPassword false suppresses it" ""

# ---- a missing or unreadable database is not an invitation
seed
rm -f "$fix/Users.plist"
cksays "a missing Users.plist suppresses it" ""

seed
echo 'not a plist at all' > "$fix/Users.plist"
cksays "an unparsable Users.plist suppresses it" ""

seed
: > "$fix/Users.plist"
cksays "an empty Users.plist suppresses it" ""

printf '\n%d checks, %d failures\n' "$((pass + fail))" "$fail"
if [ "$fail" -eq 0 ]; then
	echo "AUTOLOGIN-OK: every clause suppresses automatic login on its own"
	exit 0
fi
echo "AUTOLOGIN-FAIL"
exit 1
