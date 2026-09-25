#!/bin/sh
# Host-side tests for passwd(1).
#
# What is covered: everything that does not prompt, which is where the
# security logic lives. Authorisation, -d, -l, -u and their refusals, store
# routing, the joined-machine refusal, and the constant-time verifier.
#
# What is NOT covered, stated plainly rather than implied: the interactive
# change path. readpassphrase(3) is called with RPP_REQUIRE_TTY and reads
# /dev/tty, so no pipe can feed it, on a build host or on an image. Changing
# a password by hand, or an expect harness, is the only way to exercise it.
# That is a property of the tool being interactive by design; the scripted
# way to set a password on a BSD is pw(8) -h, which reads a hash from a
# descriptor, and that will be tested when pw lands.
#
#   sh passwd_test.sh
#
# Run under sh, not zsh: the cases pass argument lists, and zsh does not
# word-split an unquoted variable.
set -u

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../../.." && pwd)
fix=${TMPDIR:-/tmp}/passwd_test.$$
trap 'rm -rf "$fix"' EXIT
mkdir -p "$fix"

case "$(uname -s)" in
Darwin)	cflib="-framework CoreFoundation" ;;
*)	cflib="-lCoreFoundation -lcrypt" ;;
esac

${CC:-cc} -O1 -g -Wall -Wextra -Wshadow -Wstrict-prototypes \
    -Wmissing-prototypes -fblocks -DLIBDS_TEST -DPASSWD_TEST \
    -DACCT_BINDING_PLIST="\"$fix/Binding.plist\"" \
    -DACCT_NETWORK_USERS="\"$fix/NetworkUsers.plist\"" \
    -I"$top/src/accounts/common" -I"$top/src/libds" \
    -o "$fix/passwd" \
    "$top/src/accounts/passwd/passwd.c" \
    "$top/src/accounts/common/acct.c" \
    "$top/src/libds/libds.c" $cflib || exit 2

# The shared helpers, including hashing, are covered by
# src/accounts/common/tests/acct_test.c, which is also built for the image.
# They are not duplicated here: this host cannot verify hashing at all, since
# its crypt(3) may not implement the format login.conf requires.

seed() {
	cat > "$fix/Users.plist" <<'P'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
<key>admin</key><dict>
<key>gid</key><integer>5000</integer><key>noPassword</key><true/>
<key>realName</key><string>Local Administrator</string>
<key>shell</key><string>/bin/zsh</string><key>uid</key><integer>5000</integer>
<key>username</key><string>admin</string></dict>
<key>joe</key><dict>
<key>gid</key><integer>5001</integer>
<key>passwordHash</key><string>$6$abcdefgh$notarealhashbutwellformed</string>
<key>realName</key><string>Joe</string>
<key>shell</key><string>/bin/zsh</string><key>uid</key><integer>5001</integer>
<key>username</key><string>joe</string></dict>
</dict></plist>
P
	cat > "$fix/Groups.plist" <<'P'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict><key>admin</key><dict>
<key>gid</key><integer>5000</integer><key>groupname</key><string>admin</string>
<key>members</key><array><string>admin</string></array></dict></dict></plist>
P
	rm -f "$fix/Binding.plist" "$fix/NetworkUsers.plist"
}

# The stored hash for a user, whatever the layout. The seeded file puts a key
# and its value on one line; CoreFoundation rewrites them onto two, so a raw
# grep comparison would be comparing formatting rather than the value.
hash_of() {
	tr -d '\t' < "$fix/Users.plist" | tr '>' '>\n' |
	    grep -A2 '<key>passwordHash</key>' | grep -m1 '^\$\|^\*LOCKED\*' ||
	    awk 'BEGIN{RS="<key>"} /^passwordHash<\/key>/{
	        match($0, /<string>[^<]*/); if (RLENGTH > 0)
	            print substr($0, RSTART + 8, RLENGTH - 8); exit }' "$fix/Users.plist"
}

# Assert the diagnostic, not only the exit status. A refusal and a failure for
# some other reason both exit 1, so checking the status alone cannot tell them
# apart: with the authorisation check deleted, every one of these still exited
# 1 because the prompt then failed for want of a terminal, and the suite stayed
# green. Match the message.
cksay() {	# cksay <label> <expected-substring> <command...>
	_lbl=$1; _want=$2; shift 2
	_out=$("$@" 2>&1)
	if printf '%s' "$_out" | grep -q "$_want"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		printf 'FAIL %-34s wanted [%s], said [%s]\n' "$_lbl" "$_want" \
		    "$(printf '%s' "$_out" | head -1)"
	fi
}

cd "$fix" || exit 2
NEXTBSD_DS_DIR=$fix; export NEXTBSD_DS_DIR
pass=0; fail=0
ck() {
	if [ "$2" = "$3" ]; then pass=$((pass + 1)); else
		fail=$((fail + 1)); printf 'FAIL %-34s exit %s, wanted %s\n' "$1" "$2" "$3"
	fi
}
inplist() {	# inplist <needle> <expected-count-at-least>
	# grep -c prints a count and exits 1 when it is zero, so no "|| echo 0":
	# that appended a second line and every comparison then failed.
	n=$(grep -c "$1" "$fix/Users.plist" 2>/dev/null)
	n=${n:-0}
	if [ "$n" -ge "$2" ]; then pass=$((pass + 1)); else
		fail=$((fail + 1)); printf 'FAIL Users.plist: %s appears %s times\n' "$1" "$n"
	fi
}
notinplist() {
	n=$(grep -c "$1" "$fix/Users.plist" 2>/dev/null)
	n=${n:-0}
	if [ "$n" -eq 0 ]; then pass=$((pass + 1)); else
		fail=$((fail + 1)); printf 'FAIL Users.plist still has %s\n' "$1"
	fi
}

seed
# -l then -u is a round trip that must restore the original hash exactly.
orig=$(hash_of)
./passwd -l joe >/dev/null 2>&1;  ck "lock an account" "$?" 0
inplist '\*LOCKED\*' 1
./passwd -l joe >/dev/null 2>&1;  ck "locking twice is refused" "$?" 1
./passwd -u joe >/dev/null 2>&1;  ck "unlock it" "$?" 0
notinplist '\*LOCKED\*'
now=$(hash_of)
if [ "$orig" = "$now" ]; then pass=$((pass + 1)); else
	fail=$((fail + 1)); printf 'FAIL the lock round trip changed the hash\n'
fi
./passwd -u joe >/dev/null 2>&1;  ck "unlocking twice is refused" "$?" 1

# -d clears the password outright.
./passwd -d joe >/dev/null 2>&1;  ck "clear a password" "$?" 0
inplist '<key>noPassword</key>' 2
notinplist 'notarealhash'

# Locking an account that has no password, then unlocking, must not invent one.
./passwd -l joe >/dev/null 2>&1;  ck "lock a passwordless account" "$?" 0
./passwd -u joe >/dev/null 2>&1;  ck "unlock it again" "$?" 0
inplist '<key>noPassword</key>' 2

seed
# Routing and refusals.
./passwd -l nosuch >/dev/null 2>&1; ck "no such user" "$?" 1
./passwd -i nonsense joe >/dev/null 2>&1; ck "-i takes files or directory" "$?" 1
./passwd -l -u joe >/dev/null 2>&1; ck "two actions at once is usage" "$?" 1

# A joined machine refuses, because the records belong to the server.
printf '<plist version="1.0"><dict><key>server</key><string>ds.example.lan</string></dict></plist>\n' \
    > "$fix/Binding.plist"
# Joined is decided by the NETWORK plist being present, not by the binding --
# the binding only supplies the server name for the message.
printf '<plist version="1.0"><dict/></plist>\n' > "$fix/NetworkUsers.plist"
./passwd -d joe >/dev/null 2>&1; ck "a joined machine refuses" "$?" 2
# The shape that was broken: on a real client the account is in the /Network
# plists, so it is NOT in the local ones. find_store() then chose STORE_FILES
# and the refusal -- which lived inside do_directory() -- never ran, and passwd
# rewrote a local hash while login kept using /Network. Staging the user only
# locally, as this suite used to, is the one case where the old code worked.
./passwd -d notlocal >/dev/null 2>&1
ck "joined + account not in the local plist still refuses" "$?" 2
inplist 'notarealhash' 1
rm -f "$fix/Binding.plist" "$fix/NetworkUsers.plist"

# -i files on a directory account reaches the master.passwd path. On a host
# build that path has no libutil and says so; what matters is that -i routed
# there rather than silently editing the plists.
./passwd -i files joe >/dev/null 2>&1; ck "-i files routes away from the plists" "$?" 1
inplist 'notarealhash' 1

# Authorisation, which is the most important thing here and was untested until
# a mutation showed it: with the hook always pretending to be root, disabling
# the check broke nothing. NEXTBSD_TEST_UID now sets the pretended uid, so the
# non-root paths are reachable. These refusals all happen before any prompt.
seed
before=$(hash_of)
NEXTBSD_TEST_UID=5001 cksay "a user may not change another's" \
    "permission denied" ./passwd admin
NEXTBSD_TEST_UID=9999 cksay "an unrelated uid is refused" \
    "permission denied" ./passwd joe
NEXTBSD_TEST_UID=5001 cksay "a user may not lock" \
    "only root" ./passwd -l joe
NEXTBSD_TEST_UID=5001 cksay "a user may not clear" \
    "only root" ./passwd -d joe
NEXTBSD_TEST_UID=5001 cksay "a user may not unlock" \
    "only root" ./passwd -u joe
if [ "$(hash_of)" = "$before" ]; then pass=$((pass + 1)); else
	fail=$((fail + 1)); printf 'FAIL an unauthorised attempt changed the hash\n'
fi

if command -v plutil >/dev/null 2>&1; then
	plutil -lint "$fix/Users.plist" >/dev/null 2>&1; ck "Users.plist is still a plist" "$?" 0
fi

printf '\n%d checks, %d failures\n' "$((pass + fail))" "$fail"
if [ "$fail" -eq 0 ]; then
	echo "PASSWD-OK: lock, unlock, clear, routing and refusals"
	exit 0
fi
echo "PASSWD-FAIL"
exit 1
