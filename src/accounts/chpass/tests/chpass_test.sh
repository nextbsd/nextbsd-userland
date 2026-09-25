#!/bin/sh
# Host-side tests for chpass(1), chfn and chsh.
#
# Covered: the name-based field restriction, authorisation, shell validation
# and its root/user asymmetry, the joined-machine refusal, the yp* stubs, and
# printing a record without changing it.
#
# Not covered here: the password prompt a non-root user gets before an edit,
# which reads /dev/tty and cannot be fed from a pipe. The on-image lifecycle
# drives it through pty_run. Every case below is therefore either root, which
# is not asked, or a refusal that happens before the prompt.
#
#   sh chpass_test.sh
set -u

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../../.." && pwd)
fix=${TMPDIR:-/tmp}/chpass_test.$$
trap 'rm -rf "$fix"' EXIT
mkdir -p "$fix"

case "$(uname -s)" in
Darwin)	cflib="-framework CoreFoundation" ;;
*)	cflib="-lCoreFoundation -lcrypt" ;;
esac

${CC:-cc} -O1 -g -Wall -Wextra -Wshadow -Wstrict-prototypes -Wpointer-arith \
    -Wmissing-prototypes -fblocks -DLIBDS_TEST -DCHPASS_TEST \
    -DACCT_BINDING_PLIST="\"$fix/Binding.plist\"" \
    -DACCT_LOCAL_DOMAIN="\"$fix/LocalDomain.plist\"" \
    -DACCT_NETWORK_DOMAIN="\"$fix/NetworkDomain.plist\"" \
    -DACCT_SHELLS="\"$fix/shells\"" \
    -I"$top/src/accounts/common" -I"$top/src/libds" \
    -o "$fix/chpass" \
    "$top/src/accounts/chpass/chpass.c" \
    "$top/src/accounts/common/acct.c" \
    "$top/src/libds/libds.c" $cflib || exit 2

# The links, as the Makefile installs them.
for l in chfn chsh ypchpass ypchfn ypchsh; do ln -sf chpass "$fix/$l"; done
printf '/bin/sh\n/bin/zsh\n' > "$fix/shells"

seed() {
	cat > "$fix/Users.plist" <<'P'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
<key>joe</key><dict>
<key>gid</key><integer>5001</integer><key>noPassword</key><true/>
<key>realName</key><string>Joe Original</string>
<key>shell</key><string>/bin/zsh</string><key>uid</key><integer>5001</integer>
<key>username</key><string>joe</string></dict>
<key>kate</key><dict>
<key>gid</key><integer>5002</integer><key>noPassword</key><true/>
<key>realName</key><string>Kate Original</string>
<key>shell</key><string>/bin/zsh</string><key>uid</key><integer>5002</integer>
<key>username</key><string>kate</string></dict>
</dict></plist>
P
	printf '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict/></plist>\n' > "$fix/Groups.plist"
	rm -f "$fix/Binding.plist" "$fix/NetworkDomain.plist" "$fix/LocalDomain.plist"
}

cd "$fix" || exit 2
NEXTBSD_DS_DIR=$fix; export NEXTBSD_DS_DIR
pass=0; fail=0
ck() {
	if [ "$2" = "$3" ]; then pass=$((pass + 1)); else
		fail=$((fail + 1)); printf 'FAIL %-40s exit %s, wanted %s\n' "$1" "$2" "$3"
	fi
}
cksay() {	# cksay <label> <expected-substring> <command...>
	_lbl=$1; _want=$2; shift 2
	_out=$("$@" 2>&1 </dev/null)
	if printf '%s' "$_out" | grep -q "$_want"; then pass=$((pass + 1)); else
		fail=$((fail + 1))
		printf 'FAIL %-40s wanted [%s], said [%s]\n' "$_lbl" "$_want" \
		    "$(printf '%s' "$_out" | head -1)"
	fi
}
cksay_as() {	# cksay_as <uid> <label> <expected> <command...>
	_uid=$1; _lbl=$2; _want=$3; shift 3
	_out=$(NEXTBSD_TEST_UID=$_uid "$@" 2>&1 </dev/null)
	if printf '%s' "$_out" | grep -q "$_want"; then pass=$((pass + 1)); else
		fail=$((fail + 1))
		printf 'FAIL %-40s wanted [%s], said [%s]\n' "$_lbl" "$_want" \
		    "$(printf '%s' "$_out" | head -1)"
	fi
}

field() {	# field <user> <key>
	awk -v u="$1" -v k="$2" '
	    index($0, "<key>" u "</key>") { f = 1 }
	    f && match($0, "<key>" k "</key>[ \t]*<string>[^<]*") {
	        s = substr($0, RSTART, RLENGTH); sub(/.*<string>/, "", s)
	        print s; exit
	    }
	    f && index($0, "<key>" k "</key>") { want = 1; next }
	    want && match($0, /<string>[^<]*/) {
	        print substr($0, RSTART + 8, RLENGTH - 8); exit
	    }
	' "$fix/Users.plist" 2>/dev/null
}
ckfield() {	# ckfield <label> <user> <key> <expected>
	got=$(field "$2" "$3")
	if [ "$got" = "$4" ]; then pass=$((pass + 1)); else
		fail=$((fail + 1)); printf 'FAIL %-40s %s.%s is [%s], wanted [%s]\n' "$1" "$2" "$3" "$got" "$4"
	fi
}

# ---- the names are not synonyms
seed
cksay "chfn refuses -s"  "use chsh"  ./chfn -s /bin/sh joe
cksay "chsh refuses -f"  "use chfn"  ./chsh -f "Nope" joe
ckfield "and neither changed anything" joe shell "/bin/zsh"
ckfield "nor the name"                 joe realName "Joe Original"

# ---- each name edits its own field
seed
./chfn -f "Joe Edited" joe >/dev/null 2>&1 </dev/null; ck "chfn sets the name" "$?" 0
ckfield "the name changed" joe realName "Joe Edited"
ckfield "the shell did not" joe shell "/bin/zsh"
./chsh -s /bin/sh joe >/dev/null 2>&1 </dev/null; ck "chsh sets the shell" "$?" 0
ckfield "the shell changed" joe shell "/bin/sh"
ckfield "the name did not"  joe realName "Joe Edited"

# ---- chpass does both at once
seed
./chpass -f "Both Changed" -s /bin/sh joe >/dev/null 2>&1 </dev/null
ck "chpass sets both" "$?" 0
ckfield "name"  joe realName "Both Changed"
ckfield "shell" joe shell "/bin/sh"

# ---- printing a record changes nothing and succeeds
seed
out=$(./chpass joe 2>&1 </dev/null); ck "a bare chpass succeeds" "$?" 0
if printf '%s' "$out" | grep -q 'Joe Original'; then pass=$((pass+1)); else
	fail=$((fail+1)); printf 'FAIL the record was not printed\n'; fi
ckfield "and nothing changed" joe realName "Joe Original"

# ---- shell validation
seed
cksay "a relative shell is refused" "absolute path" ./chsh -s zsh joe
# root gets a warning for an unlisted shell and the change happens
./chsh -s /usr/local/bin/fish joe >/dev/null 2>&1 </dev/null
ck "root may set an unlisted shell" "$?" 0
ckfield "and it took" joe shell "/usr/local/bin/fish"
# a user may not
seed
cksay_as 5001 "a user may not set an unlisted shell" \
    "not a listed shell" ./chsh -s /usr/local/bin/fish joe
ckfield "and it did not take" joe shell "/bin/zsh"

# ---- authorisation
seed
cksay_as 5001 "a user may not edit another" \
    "only change your own" ./chfn -f "Hacked" kate
ckfield "kate is untouched" kate realName "Kate Original"
cksay_as 5002 "nor the other way round" \
    "only change your own" ./chsh -s /bin/sh joe
ckfield "joe is untouched" joe shell "/bin/zsh"

# ---- a colon would corrupt a passwd-style line elsewhere
seed
cksay "a colon in the name is refused" "may not contain a colon" \
    ./chfn -f "Bad:Name" joe
ckfield "and nothing changed" joe realName "Joe Original"

# ---- the yp names
for y in ypchpass ypchfn ypchsh; do
	cksay "$y says NIS is unsupported" "NIS is not supported" ./$y joe
done

# ---- a joined machine
seed
printf '<plist version="1.0"><dict><key>server</key><string>ds.example.lan</string></dict></plist>\n' \
    > "$fix/Binding.plist"
# Joined is the DOMAIN marker arriving under /Network -- what dspromote wrote
# into the server's /Local and the export carries across. The binding only
# names the server for the message.
printf '<plist version="1.0"><dict/></plist>\n' > "$fix/NetworkDomain.plist"
cksay "a joined machine refuses" "joined to ds.example.lan" ./chfn -f "X" joe
ckfield "and changed nothing" joe realName "Joe Original"
# ...and the server is not a client of itself, whatever its export shows.
printf '<plist version="1.0"><dict/></plist>\n' > "$fix/LocalDomain.plist"
./chfn -f "Joe Server" joe >/dev/null 2>&1 </dev/null
ckfield "but a server does not" joe realName "Joe Server"
rm -f "$fix/Binding.plist" "$fix/NetworkDomain.plist" "$fix/LocalDomain.plist"

# ---- a user who is not there
seed
cksay "an unknown user" "no such user" ./chfn -f "X" nosuch

if command -v plutil >/dev/null 2>&1; then
	seed; ./chfn -f "Lint Check" joe >/dev/null 2>&1 </dev/null
	plutil -lint "$fix/Users.plist" >/dev/null 2>&1; ck "Users.plist still lints" "$?" 0
fi

printf '\n%d checks, %d failures\n' "$((pass + fail))" "$fail"
if [ "$fail" -eq 0 ]; then
	echo "CHPASS-OK: field restriction, authorisation, shells, yp stubs"
	exit 0
fi
echo "CHPASS-FAIL"
exit 1
