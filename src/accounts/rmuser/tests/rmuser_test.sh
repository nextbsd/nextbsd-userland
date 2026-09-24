#!/bin/sh
# Host-side tests for rmuser(8).
#
# What is covered: the refusals, the last-administrator guard, group cleanup,
# --keep-home, batch mode, and that a refusal removes nothing.
#
# What is NOT covered here: killing processes and removing a real home. Both
# act on the live system rather than the fixture, so they belong on the image,
# where the lifecycle test creates a user and removes them for real.
#
#   sh rmuser_test.sh
#
# Run under sh, not zsh: the cases pass argument lists.
set -u

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../../.." && pwd)
fix=${TMPDIR:-/tmp}/rmuser_test.$$
trap 'rm -rf "$fix"' EXIT
mkdir -p "$fix"

case "$(uname -s)" in
Darwin)	cflib="-framework CoreFoundation" ;;
*)	cflib="-lCoreFoundation -lcrypt" ;;
esac

${CC:-cc} -O1 -g -Wall -Wextra -Wshadow -Wstrict-prototypes \
    -Wmissing-prototypes -fblocks -DLIBDS_TEST -DRMUSER_TEST \
    -DACCT_BINDING_PLIST="\"$fix/Binding.plist\"" \
    -DACCT_LOCAL_USERS="\"$fix/Users\"" \
    -DACCT_CRON_TABS="\"$fix/crontabs\"" \
    -DACCT_AT_JOBS="\"$fix/atjobs\"" \
    -I"$top/src/accounts/common" -I"$top/src/libds" \
    -o "$fix/rmuser" \
    "$top/src/accounts/rmuser/rmuser.c" \
    "$top/src/accounts/common/acct.c" \
    "$top/src/libds/libds.c" $cflib || exit 2

seed() {
	cat > "$fix/Users.plist" <<'P'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
<key>admin</key><dict>
<key>gid</key><integer>5000</integer><key>noPassword</key><true/>
<key>shell</key><string>/bin/zsh</string><key>uid</key><integer>5000</integer>
<key>username</key><string>admin</string></dict>
<key>joe</key><dict>
<key>gid</key><integer>5001</integer><key>realName</key><string>Joe</string>
<key>shell</key><string>/bin/zsh</string><key>uid</key><integer>5001</integer>
<key>username</key><string>joe</string></dict>
<key>kate</key><dict>
<key>gid</key><integer>5002</integer><key>realName</key><string>Kate</string>
<key>shell</key><string>/bin/zsh</string><key>uid</key><integer>5002</integer>
<key>username</key><string>kate</string></dict>
<key>_svc</key><dict>
<key>gid</key><integer>200</integer>
<key>shell</key><string>/usr/sbin/nologin</string><key>uid</key><integer>200</integer>
<key>username</key><string>_svc</string></dict>
<key>_odd</key><dict>
<key>gid</key><integer>5003</integer>
<key>shell</key><string>/bin/zsh</string><key>uid</key><integer>5003</integer>
<key>username</key><string>_odd</string></dict>
<key>lowuid</key><dict>
<key>gid</key><integer>200</integer>
<key>shell</key><string>/bin/zsh</string><key>uid</key><integer>200</integer>
<key>username</key><string>lowuid</string></dict>
</dict></plist>
P
	cat > "$fix/Groups.plist" <<'P'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
<key>admin</key><dict>
<key>gid</key><integer>5000</integer><key>groupname</key><string>admin</string>
<key>members</key><array><string>admin</string><string>joe</string></array></dict>
<key>project</key><dict>
<key>gid</key><integer>5100</integer><key>groupname</key><string>project</string>
<key>members</key><array><string>joe</string><string>kate</string></array></dict>
</dict></plist>
P
	rm -rf "$fix/Binding.plist" "$fix/Users" "$fix/crontabs" "$fix/atjobs"
	mkdir -p "$fix/Users/joe/Documents" "$fix/crontabs" "$fix/atjobs"
	: > "$fix/Users/joe/.zshrc"
	: > "$fix/crontabs/joe"
}

cd "$fix" || exit 2
NEXTBSD_DS_DIR=$fix; export NEXTBSD_DS_DIR
pass=0; fail=0
# All invocations read stdin from /dev/null. Any prompt then gets EOF and the
# tool gives up, rather than blocking: a suite that can hang is worse than one
# that fails, and a mutation which deletes a guard reaches a prompt.
exec 3</dev/null

ck() {
	if [ "$2" = "$3" ]; then pass=$((pass + 1)); else
		fail=$((fail + 1)); printf 'FAIL %-38s exit %s, wanted %s\n' "$1" "$2" "$3"
	fi
}
cksay() {	# cksay <label> <expected-substring> <command...>
	_lbl=$1; _want=$2; shift 2
	_out=$("$@" 2>&1 </dev/null)
	if printf '%s' "$_out" | grep -q "$_want"; then pass=$((pass + 1)); else
		fail=$((fail + 1))
		printf 'FAIL %-38s wanted [%s], said [%s]\n' "$_lbl" "$_want" \
		    "$(printf '%s' "$_out" | head -1)"
	fi
}
has()    { grep -q "$1" "$fix/$2" 2>/dev/null; }
ckhas()  { if has "$2" "$3"; then pass=$((pass+1)); else fail=$((fail+1)); printf 'FAIL %s: %s missing from %s\n' "$1" "$2" "$3"; fi; }
cknot()  { if has "$2" "$3"; then fail=$((fail+1)); printf 'FAIL %s: %s still in %s\n' "$1" "$2" "$3"; else pass=$((pass+1)); fi; }

# ---- refusals, none of which may remove anything
seed
cksay "root is refused"            "cannot remove root"       ./rmuser -y root
# Two different guards can refuse an underscore name: the name itself, and a
# uid in the system range. Assert them apart, or deleting one is invisible:
# _svc has a system uid too, so the uid check used to cover for the name check.
cksay "a system NAME is refused"   "^rmuser: _odd: system account" ./rmuser -y _odd
# lowuid has an ordinary name and a system uid, so only the uid check can
# refuse it. _svc would be caught by the name check first and never reach it.
cksay "a system UID is refused"    "uid 200 is a system account"   ./rmuser -y lowuid
cksay "a missing user is refused"  "no such user"             ./rmuser -y nosuch
ckhas "refusals kept the records" '<string>joe</string>'  Users.plist
ckhas "refusals kept _svc"        '<string>_svc</string>' Users.plist
ckhas "refusals kept _odd"        '<string>_odd</string>' Users.plist
ckhas "refusals kept lowuid"      '<string>lowuid</string>' Users.plist

# ---- the last administrator
seed
# joe and admin are both in admin, so removing joe is allowed.
./rmuser -y --keep-home joe >/dev/null 2>&1 </dev/null; ck "a non-last admin can go" "$?" 0
cknot "joe is gone"               '<string>joe</string>'  Users.plist
cknot "joe left the admin group"  '<string>joe</string>'  Groups.plist
ckhas "admin is still there"      '<string>admin</string>' Users.plist
# now admin is the only member, so it must be refused without -y
cksay "the last admin is refused" "last member" ./rmuser --keep-home admin
ckhas "and admin survived"        '<string>admin</string>' Users.plist
# and allowed with -y
./rmuser -y --keep-home admin >/dev/null 2>&1 </dev/null; ck "-y forces it" "$?" 0
cknot "admin is gone"             '<key>admin</key>' Users.plist

# ---- the private group goes, a shared one does not
seed
# joe's own group: same name, same gid, no other members.
python3 - "$fix" <<'PYEOF' 2>/dev/null || true
import sys, re
p = sys.argv[1] + "/Groups.plist"
t = open(p).read()
t = t.replace("<dict>\n<key>admin</key>", """<dict>
<key>joe</key><dict>
<key>gid</key><integer>5001</integer><key>groupname</key><string>joe</string>
<key>members</key><array/></dict>
<key>admin</key>""", 1)
open(p, "w").write(t)
PYEOF
./rmuser -y --keep-home joe >/dev/null 2>&1 </dev/null
ck "remove a user with a private group" "$?" 0
cknot "the private group went too" '<key>joe</key>' Groups.plist
ckhas "the shared group stayed"    '<key>project</key>' Groups.plist

# A group sharing the name but with a different gid is somebody else's.
seed
python3 - "$fix" <<'PYEOF' 2>/dev/null || true
import sys
p = sys.argv[1] + "/Groups.plist"
t = open(p).read()
t = t.replace("<dict>\n<key>admin</key>", """<dict>
<key>joe</key><dict>
<key>gid</key><integer>9999</integer><key>groupname</key><string>joe</string>
<key>members</key><array/></dict>
<key>admin</key>""", 1)
open(p, "w").write(t)
PYEOF
./rmuser -y --keep-home joe >/dev/null 2>&1 </dev/null
ckhas "a same-name group with another gid stays" '<key>joe</key>' Groups.plist

# ---- group cleanup across several groups
seed
./rmuser -y --keep-home joe >/dev/null 2>&1 </dev/null; ck "remove a user in two groups" "$?" 0
cknot "dropped from admin and project" '<string>joe</string>' Groups.plist
ckhas "kate is untouched"              '<string>kate</string>' Groups.plist

# ---- the home, and --keep-home
seed
./rmuser -y --keep-home joe >/dev/null 2>&1 </dev/null
if [ -d "$fix/Users/joe" ]; then pass=$((pass+1)); else
	fail=$((fail+1)); printf 'FAIL --keep-home removed the home anyway\n'
fi
seed
./rmuser -y joe >/dev/null 2>&1 </dev/null; ck "remove with the home" "$?" 0
if [ -d "$fix/Users/joe" ]; then
	fail=$((fail+1)); printf 'FAIL the home survived a plain removal\n'
else pass=$((pass+1)); fi
# the crontab went too
if [ -f "$fix/crontabs/joe" ]; then
	fail=$((fail+1)); printf 'FAIL the crontab survived\n'
else pass=$((pass+1)); fi

# ---- a joined machine refuses
seed
printf '<plist version="1.0"><dict><key>server</key><string>ds.example.lan</string></dict></plist>\n' \
    > "$fix/Binding.plist"
cksay "a joined machine refuses" "joined to ds.example.lan" ./rmuser -y joe
ckhas "and changed nothing" '<string>joe</string>' Users.plist
rm -f "$fix/Binding.plist"

# ---- batch mode
seed
printf '# leavers\njoe\nkate\n' > list
./rmuser -f list >/dev/null 2>&1 </dev/null; ck "a batch file" "$?" 0
cknot "joe removed by batch"  '<string>joe</string>'  Users.plist
cknot "kate removed by batch" '<string>kate</string>' Users.plist
seed
printf 'joe\nnosuch\n' > list2
./rmuser -f list2 >/dev/null 2>&1 </dev/null; ck "a batch line that fails" "$?" 1
cknot "the good line still ran" '<string>joe</string>' Users.plist

# ---- usage
seed
./rmuser >/dev/null 2>&1 </dev/null; ck "no arguments is usage" "$?" 1
./rmuser -f list -y joe >/dev/null 2>&1 </dev/null; ck "-f with names is usage" "$?" 1

if command -v plutil >/dev/null 2>&1; then
	seed; ./rmuser -y joe >/dev/null 2>&1 </dev/null
	plutil -lint "$fix/Users.plist"  >/dev/null 2>&1; ck "Users.plist still lints" "$?" 0
	plutil -lint "$fix/Groups.plist" >/dev/null 2>&1; ck "Groups.plist still lints" "$?" 0
fi

printf '\n%d checks, %d failures\n' "$((pass + fail))" "$fail"
if [ "$fail" -eq 0 ]; then
	echo "RMUSER-OK: refusals, the last-admin guard, group cleanup, homes and batch mode"
	exit 0
fi
echo "RMUSER-FAIL"
exit 1
