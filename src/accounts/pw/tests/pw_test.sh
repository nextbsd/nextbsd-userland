#!/bin/sh
# Host-side tests for pw(8).
#
# The acceptance test here is not the flag tables, it is the packaging
# contract: the five forms a ports install script emits, generated from the
# framework's do-users-groups.sh. Each of those has to reach the base copy of
# pw with its argv intact, because that is the binary that has always served
# them and the only way to be sure they keep working.
#
# The routing point records what it would have handed to the vendored
# handed, so routing can be asserted exactly rather than inferred.
#
#   sh pw_test.sh
set -u

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../../.." && pwd)
fix=${TMPDIR:-/tmp}/pw_test.$$
trap 'rm -rf "$fix"' EXIT
mkdir -p "$fix"

case "$(uname -s)" in
Darwin)	cflib="-framework CoreFoundation" ;;
*)	cflib="-lCoreFoundation -lcrypt" ;;
esac

${CC:-cc} -O1 -g -Wall -Wextra -Wshadow -Wstrict-prototypes \
    -Wmissing-prototypes -fblocks -DLIBDS_TEST -DPW_TEST \
    -DACCT_BINDING_PLIST="\"$fix/Binding.plist\"" \
    -DACCT_NETWORK_USERS="\"$fix/NetworkUsers.plist\"" \
    -I"$top/src/accounts/common" -I"$top/src/libds" \
    -o "$fix/pw" \
    "$top/src/accounts/pw/pw.c" \
    "$top/src/accounts/common/acct.c" \
    "$top/src/libds/libds.c" $cflib || exit 2

# The base pw is vendored into the binary now rather than exec'd, and it needs
# libutil, so it is not compiled on the host. Under -DPW_TEST the routing point
# records the command line it would have handed over to $PW_DELEGATED_TO, which
# is what these tests assert: that we routed, and with what.

seed() {
	cat > "$fix/Users.plist" <<'P'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
<key>joe</key><dict>
<key>gid</key><integer>5001</integer><key>noPassword</key><true/>
<key>realName</key><string>Joe Tester</string>
<key>shell</key><string>/bin/zsh</string><key>uid</key><integer>5001</integer>
<key>username</key><string>joe</string></dict>
</dict></plist>
P
	cat > "$fix/Groups.plist" <<'P'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
<key>staff</key><dict>
<key>gid</key><integer>5100</integer><key>groupname</key><string>staff</string>
<key>members</key><array><string>joe</string></array></dict>
</dict></plist>
P
	rm -f "$fix/Binding.plist" "$fix/NetworkUsers.plist" "$fix/delegated"
}

cd "$fix" || exit 2
NEXTBSD_DS_DIR=$fix; export NEXTBSD_DS_DIR
PW_DELEGATED_TO=$fix/delegated; export PW_DELEGATED_TO
pass=0; fail=0
ck() {
	if [ "$2" = "$3" ]; then pass=$((pass + 1)); else
		fail=$((fail + 1)); printf 'FAIL %-44s exit %s, wanted %s\n' "$1" "$2" "$3"
	fi
}
# Did we hand this to the base copy, and with what?
ckdelegated() {	# ckdelegated <label> <expected-args-substring> <command...>
	_lbl=$1; _want=$2; shift 2
	rm -f "$fix/delegated"
	"$@" >/dev/null 2>&1 </dev/null
	if [ ! -f "$fix/delegated" ]; then
		fail=$((fail + 1)); printf 'FAIL %-44s was not delegated at all\n' "$_lbl"
		return
	fi
	if grep -q -- "$_want" "$fix/delegated"; then pass=$((pass + 1)); else
		fail=$((fail + 1))
		printf 'FAIL %-44s delegated [%s], wanted [%s]\n' "$_lbl" \
		    "$(cat "$fix/delegated")" "$_want"
	fi
}
cklocal() {	# cklocal <label> <command...>  — must NOT be delegated
	_lbl=$1; shift
	rm -f "$fix/delegated"
	"$@" >/dev/null 2>&1 </dev/null
	if [ -f "$fix/delegated" ]; then
		fail=$((fail + 1))
		printf 'FAIL %-44s was delegated: [%s]\n' "$_lbl" "$(cat "$fix/delegated")"
	else pass=$((pass + 1)); fi
}
cksay() {
	_lbl=$1; _want=$2; shift 2
	_out=$("$@" 2>&1 </dev/null)
	if printf '%s' "$_out" | grep -q "$_want"; then pass=$((pass + 1)); else
		fail=$((fail + 1))
		printf 'FAIL %-44s wanted [%s], said [%s]\n' "$_lbl" "$_want" \
		    "$(printf '%s' "$_out" | head -1)"
	fi
}

# ---- the packaging contract: the five forms a port emits ----------------
# Every account a port creates is a system account, so each of these must be
# handed to the base copy with its argv intact. The name is positional.
seed
ckdelegated "ports: useradd with a system uid" \
    'useradd www -u 80 -g 80' \
    ./pw useradd www -u 80 -g 80 -c "World Wide Web Owner" -d /nonexistent -s /usr/sbin/nologin
ckdelegated "ports: useradd tolerating -L" \
    '\-L daemon' \
    ./pw useradd pgsql -u 70 -g 70 -L daemon -c "PostgreSQL Daemon" -d /var/db/postgres -s /bin/sh
ckdelegated "ports: groupadd with a system gid" \
    'groupadd www -g 80' \
    ./pw groupadd www -g 80
ckdelegated "ports: usershow of a system account" \
    'usershow root' \
    ./pw usershow root
ckdelegated "ports: groupshow of a system group" \
    'groupshow wheel' \
    ./pw groupshow wheel
ckdelegated "ports: groupmod adding a member" \
    'groupmod wheel -m www' \
    ./pw groupmod wheel -m www

# The twenty ports users that escape a naive uid rule: a high uid with a real
# shell and a real home. These are the ones a 499 threshold would have put in
# the directory with a computed home their package does not use.
seed
ckdelegated "postgres (uid 770, real shell and home)" \
    'useradd postgres' \
    ./pw useradd postgres -u 770 -g 770 -c "PostgreSQL Daemon" -d /var/db/postgres -s /bin/sh
ckdelegated "jenkins (uid 818)" 'useradd jenkins' \
    ./pw useradd jenkins -u 818 -g 818 -d /usr/local/jenkins -s /bin/sh
ckdelegated "ejabberd (uid 543)" 'useradd ejabberd' \
    ./pw useradd ejabberd -u 543 -g 543 -d /var/spool/ejabberd -s /bin/sh

# ---- routing for accounts that are ours ---------------------------------
seed
cklocal "an existing directory user stays here"  ./pw usershow joe
cklocal "an existing directory group stays here" ./pw groupshow staff
cklocal "a new regular user stays here"          ./pw useradd kate -u 5002 -g 5002
cklocal "a new regular group stays here"         ./pw groupadd project -g 5101

# An underscore name is a service account wherever its uid sits.
seed
ckdelegated "an underscore name goes to the base" 'useradd _svc' \
    ./pw useradd _svc -u 5500 -g 5500

# nologin with nowhere to live is a service account too.
seed
ckdelegated "nologin with /nonexistent goes to the base" 'useradd daemonish' \
    ./pw useradd daemonish -u 5600 -g 5600 -d /nonexistent -s /usr/sbin/nologin

# ---- the show verbs, whose output and status are load bearing -----------
seed
out=$(./pw usershow joe 2>&1 </dev/null); ck "usershow of a directory user" "$?" 0
case "$out" in
	joe:*:5001:5001:*) pass=$((pass+1)) ;;
	*) fail=$((fail+1)); printf 'FAIL usershow format was [%s]\n' "$out" ;;
esac
out=$(./pw groupshow staff 2>&1 </dev/null); ck "groupshow of a directory group" "$?" 0
case "$out" in
	staff:*:5100:*joe*) pass=$((pass+1)) ;;
	*) fail=$((fail+1)); printf 'FAIL groupshow format was [%s]\n' "$out" ;;
esac
# The existence test: a missing name must be non-zero.
./pw usershow nosuchuser >/dev/null 2>&1 </dev/null
if [ $? -ne 0 ]; then pass=$((pass+1)); else
	fail=$((fail+1)); printf 'FAIL usershow of a missing user returned 0\n'; fi

# ---- what we cannot represent is refused, not silently dropped ----------
seed
cksay "-L on a directory account is refused" "no class field" \
    ./pw usermod joe -L staff
cksay "-d on a directory account is refused" "always" \
    ./pw usermod joe -d /home/joe
cksay "renaming is refused"                  "not supported" \
    ./pw usermod joe -l joseph

# ---- lock and unlock agree with passwd(1) -------------------------------
seed
./pw lock joe >/dev/null 2>&1 </dev/null; ck "pw lock" "$?" 0
if grep -q '\*LOCKED\*' Users.plist; then pass=$((pass+1)); else
	fail=$((fail+1)); printf 'FAIL pw lock did not set the sentinel\n'; fi
./pw unlock joe >/dev/null 2>&1 </dev/null; ck "pw unlock" "$?" 0
if grep -q '\*LOCKED\*' Users.plist; then
	fail=$((fail+1)); printf 'FAIL pw unlock left the sentinel\n'
else pass=$((pass+1)); fi

# ---- group membership ---------------------------------------------------
seed
./pw groupmod staff -m kate >/dev/null 2>&1 </dev/null; ck "groupmod -m" "$?" 0
if grep -q '<string>kate</string>' Groups.plist; then pass=$((pass+1)); else
	fail=$((fail+1)); printf 'FAIL kate was not added\n'; fi

# ---- a joined machine ---------------------------------------------------
seed
printf '<plist version="1.0"><dict><key>server</key><string>ds.example.lan</string></dict></plist>\n' \
    > "$fix/Binding.plist"
# Joined is decided by the NETWORK plist being present, not by the binding --
# the binding only supplies the server name for the message.
printf '<plist version="1.0"><dict/></plist>\n' > "$fix/NetworkUsers.plist"
cksay "a joined machine refuses a write" "joined to ds.example.lan" \
    ./pw useradd newbie -u 5300 -g 5300
cklocal "and does not hand it to the base either" ./pw useradd newbie -u 5300 -g 5300
rm -f "$fix/Binding.plist"

# The check that used to sit here asserted the message pw gave when the base
# copy was missing from /usr/libexec/bsd. That copy is vendored into this
# binary now, so the state it tested cannot arise: there is nothing to be
# missing. Removed rather than reworded.

if command -v plutil >/dev/null 2>&1; then
	seed; ./pw useradd kate -u 5002 -g 5002 >/dev/null 2>&1 </dev/null
	plutil -lint "$fix/Users.plist" >/dev/null 2>&1; ck "Users.plist still lints" "$?" 0
fi

printf '\n%d checks, %d failures\n' "$((pass + fail))" "$fail"
if [ "$fail" -eq 0 ]; then
	echo "PW-OK: the packaging contract, routing, the show verbs and refusals"
	exit 0
fi
echo "PW-FAIL"
exit 1
