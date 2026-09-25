#!/bin/sh
# Host-side tests for adduser(8). They build a test binary whose paths point
# into a fixture directory and whose root check is bypassed, the same hook
# libds uses, so the whole flow runs on a build host as an ordinary user
# without touching anything real.
#
#   sh adduser_test.sh
#
# Run under sh, not zsh: the cases pass argument lists, and zsh does not
# word-split an unquoted variable, which silently turns a list into one
# argument and makes every refusal case look like a different failure.
set -u

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../../.." && pwd)	# the repository root
fix=${TMPDIR:-/tmp}/adduser_test.$$
trap 'rm -rf "$fix"' EXIT
mkdir -p "$fix"

# crypt(3) is in libc on Darwin and in libcrypt on FreeBSD. The shipped
# Makefile says LIBADD+= crypt for that reason; a host build here has to make
# the same distinction or it links on one platform and not the other.
case "$(uname -s)" in
Darwin)	cflib="-framework CoreFoundation" ;;
*)	cflib="-lCoreFoundation -lcrypt" ;;
esac

${CC:-cc} -O1 -g -Wall -Wextra -Wshadow -Wstrict-prototypes \
    -Wmissing-prototypes -fblocks -DLIBDS_TEST -DADDUSER_TEST \
    -DACCT_SHELLS="\"$fix/shells\"" \
    -DACCT_BINDING_PLIST="\"$fix/Binding.plist\"" \
    -DACCT_LOCAL_DOMAIN="\"$fix/LocalDomain.plist\"" \
    -DACCT_NETWORK_DOMAIN="\"$fix/NetworkDomain.plist\"" \
    -DACCT_CREATEHOMEDIR="\"$fix/createhomedir\"" \
    -I"$top/src/accounts/common" -I"$top/src/libds" \
    -o "$fix/adduser" \
    "$top/src/accounts/adduser/adduser.c" \
    "$top/src/accounts/common/acct.c" \
    "$top/src/libds/libds.c" $cflib || exit 2

printf '/bin/sh\n/bin/zsh\n/bin/csh\n' > "$fix/shells"
printf '#!/bin/sh\nexit 0\n' > "$fix/createhomedir"
chmod +x "$fix/createhomedir"

seed() {
	cat > "$fix/Users.plist" <<'P'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict><key>admin</key><dict>
<key>gid</key><integer>5000</integer><key>noPassword</key><true/>
<key>realName</key><string>Local Administrator</string>
<key>shell</key><string>/bin/zsh</string><key>uid</key><integer>5000</integer>
<key>username</key><string>admin</string></dict></dict></plist>
P
	cat > "$fix/Groups.plist" <<'P'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict><key>admin</key><dict>
<key>gid</key><integer>5000</integer><key>groupname</key><string>admin</string>
<key>members</key><array><string>admin</string></array></dict></dict></plist>
P
	rm -f "$fix/Binding.plist" "$fix/NetworkDomain.plist" "$fix/LocalDomain.plist"
}

cd "$fix" || exit 2
NEXTBSD_DS_DIR=$fix; export NEXTBSD_DS_DIR
pass=0; fail=0
ck() {
	if [ "$2" = "$3" ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		printf 'FAIL %-28s exit %s, wanted %s\n' "$1" "$2" "$3"
	fi
}
has() {	# has <plist> <needle> <expected count>
	n=$(grep -c "$2" "$fix/$1" 2>/dev/null || echo 0)
	if [ "$n" -ge "$3" ]; then pass=$((pass + 1)); else
		fail=$((fail + 1)); printf 'FAIL %s: %s appears %s times\n' "$1" "$2" "$n"
	fi
}

seed
printf 'joe\nJoe Maloney\n\nno\n\n\nyes\nno\n' | ./adduser >/dev/null 2>&1
ck "the six-question walk" "$?" 0
has Users.plist  '<string>joe</string>' 1
has Groups.plist '<string>joe</string>' 1

./adduser -q --admin -c Ann -p '$6$a$b' ann >/dev/null 2>&1
ck "--admin with a hash" "$?" 0
has Users.plist '\$6\$a\$b' 1

./adduser -q -w none kiosk >/dev/null 2>&1; ck "-w none" "$?" 0
has Users.plist '<key>noPassword</key>' 2
./adduser -q -w no locked >/dev/null 2>&1;  ck "-w no" "$?" 0
./adduser -q -w none -g admin ing >/dev/null 2>&1; ck "-g an existing group" "$?" 0

# Refusals. Every one must exit 2 and write nothing.
before=$(grep -c '<key>username</key>' "$fix/Users.plist")
./adduser -q -w none joe              >/dev/null 2>&1; ck "duplicate name" "$?" 2
./adduser -q -w none 9joe             >/dev/null 2>&1; ck "leading digit" "$?" 2
./adduser -q -w none 1234             >/dev/null 2>&1; ck "wholly numeric" "$?" 2
./adduser -q -w none _svc             >/dev/null 2>&1; ck "system account name" "$?" 2
./adduser -q -w none -s /bin/fish b1  >/dev/null 2>&1; ck "shell not in shells" "$?" 2
./adduser -q -w none -u 500 b2        >/dev/null 2>&1; ck "uid in system range" "$?" 2
./adduser -q -w none -u 5001 b3       >/dev/null 2>&1; ck "uid already used" "$?" 2
./adduser -q -w none -G nosuch b4     >/dev/null 2>&1; ck "-G missing group" "$?" 2
after=$(grep -c '<key>username</key>' "$fix/Users.plist")
ck "no refusal wrote a record" "$after" "$before"

./adduser -q b9 >/dev/null 2>&1; ck "no password and no terminal" "$?" 1

printf '<plist version="1.0"><dict><key>server</key><string>x.lan</string></dict></plist>\n' \
    > "$fix/Binding.plist"
# Joined is the DOMAIN marker arriving under /Network -- what dspromote wrote
# into the server's /Local and the export carries across. The binding only
# names the server for the message.
printf '<plist version="1.0"><dict/></plist>\n' > "$fix/NetworkDomain.plist"
./adduser -q -w none dave >/dev/null 2>&1; ck "a joined machine refuses" "$?" 2
# The server owns the accounts. Its own export can make the marker appear under
# /Network too, and asked in the wrong order that would brick the one machine
# that can manage them.
printf '<plist version="1.0"><dict/></plist>\n' > "$fix/LocalDomain.plist"
./adduser -q -w none dave2 >/dev/null 2>&1; ck "but a server does not" "$?" 0
has Users.plist '<string>dave2</string>' 1
rm -f "$fix/Binding.plist" "$fix/NetworkDomain.plist" "$fix/LocalDomain.plist"

printf '#format: nextbsd-1\neve::::/bin/sh:\n' > g.txt
./adduser -q -f g.txt >/dev/null 2>&1; ck "a batch file" "$?" 0
has Users.plist '<string>eve</string>' 1
printf '#format: nextbsd-9\nz:::::\n' > b.txt
./adduser -q -f b.txt >/dev/null 2>&1; ck "an unknown batch format" "$?" 1
printf '#format: nextbsd-1\nzed::::/bin/fish:\n' > c.txt
./adduser -q -f c.txt >/dev/null 2>&1; ck "a batch line that fails" "$?" 1

if command -v plutil >/dev/null 2>&1; then
	plutil -lint "$fix/Users.plist"  >/dev/null 2>&1; ck "Users.plist is a plist" "$?" 0
	plutil -lint "$fix/Groups.plist" >/dev/null 2>&1; ck "Groups.plist is a plist" "$?" 0
fi

printf '\n%d checks, %d failures\n' "$((pass + fail))" "$fail"
if [ "$fail" -eq 0 ]; then
	echo "ADDUSER-OK: the walk, the flags, the refusals and batch mode"
	exit 0
fi
echo "ADDUSER-FAIL"
exit 1
