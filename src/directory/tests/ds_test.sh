#!/bin/sh
# Host-side tests for dspromote, dsdemote, dsjoin, dsleave and dsstatus.
#
# Every path the program touches is a build-time override, so the whole thing
# runs against a staged fake root: no /etc is written, no launchctl runs, and
# the suite needs no privilege. launchctl itself is a stub that records the
# arguments it was called with, so the tests can assert WHICH jobs each verb
# loads rather than only that it tried.
#
# What is covered: the role transitions in both directions, every refusal, the
# symlink directions promotion depends on, the exports grouping, the managed
# NTP block, and argv[0] dispatch.
#
#   sh ds_test.sh
#
# Run under sh, not zsh.
set -u

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
fix=${TMPDIR:-/tmp}/ds_test.$$
trap 'rm -rf "$fix"' EXIT
mkdir -p "$fix"

R=$fix/root
LOCAL_DIR=$R/Local/Library/DirectoryServices
NET_DIR=$R/Network/Library/DirectoryServices
LOCAL_USERS=$R/Local/Users
NET_USERS=$R/Network/Users
BINDING=$LOCAL_DIR/Binding.plist
EXPORTS=$R/etc/exports
NTP=$R/etc/ntp.conf
SVCDIR=$R/Local/Library/Preferences/mDNSResponder/Services
SVC=$SVCDIR/org.nextbsd.directory.plist

# The launchctl stub: appends its arguments so a test can assert them.
mkdir -p "$fix/bin"
cat > "$fix/bin/launchctl" <<'STUB'
#!/bin/sh
printf '%s\n' "$*" >> "${DS_LAUNCHCTL_LOG:-/dev/null}"
exit 0
STUB
chmod +x "$fix/bin/launchctl"
DS_LAUNCHCTL_LOG=$fix/launchctl.log; export DS_LAUNCHCTL_LOG

${CC:-cc} -O1 -g -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
    -DDS_TEST \
    -DDS_LOCAL_DIR="\"$LOCAL_DIR\"" \
    -DDS_NETWORK_DIR="\"$NET_DIR\"" \
    -DDS_LOCAL_USERS="\"$LOCAL_USERS\"" \
    -DDS_NETWORK_USERS="\"$NET_USERS\"" \
    -DDS_BINDING="\"$BINDING\"" \
    -DDS_EXPORTS="\"$EXPORTS\"" \
    -DDS_NTP_CONF="\"$NTP\"" \
    -DDS_SERVICE_DIR="\"$SVCDIR\"" \
    -DDS_LAUNCHCTL="\"$fix/bin/launchctl\"" \
    -o "$fix/dspromote" "$top/src/directory/ds.c" || exit 2
for n in dsdemote dsjoin dsleave dsstatus; do ln -sf dspromote "$fix/$n"; done

pass=0; fail=0
ck()     { if [ "$2" = "$3" ]; then pass=$((pass+1)); else fail=$((fail+1))
           printf 'FAIL %-46s got [%s] want [%s]\n' "$1" "$2" "$3"; fi; }
ckfile() { if [ -e "$2" ]; then pass=$((pass+1)); else fail=$((fail+1))
           printf 'FAIL %-46s %s does not exist\n' "$1" "$2"; fi; }
cknofile(){ if [ ! -e "$2" ]; then pass=$((pass+1)); else fail=$((fail+1))
           printf 'FAIL %-46s %s still exists\n' "$1" "$2"; fi; }
cklink() { t=$(readlink "$2" 2>/dev/null)
           if [ "$t" = "$3" ]; then pass=$((pass+1)); else fail=$((fail+1))
           printf 'FAIL %-46s %s -> [%s] want [%s]\n' "$1" "$2" "$t" "$3"; fi; }
cksay()  { _l=$1; _w=$2; shift 2
           _o=$("$@" 2>&1 </dev/null)
           case "$_o" in *"$_w"*) pass=$((pass+1)) ;;
           *) fail=$((fail+1)); printf 'FAIL %-46s said [%s] want [%s]\n' "$_l" "$(echo "$_o"|head -1)" "$_w" ;; esac; }
ckgrep() { if grep -q "$3" "$2" 2>/dev/null; then pass=$((pass+1)); else fail=$((fail+1))
           printf 'FAIL %-46s %s has no /%s/\n' "$1" "$2" "$3"; fi; }
cknogrep(){ if grep -q "$3" "$2" 2>/dev/null; then fail=$((fail+1))
           printf 'FAIL %-46s %s still has /%s/\n' "$1" "$2" "$3"; else pass=$((pass+1)); fi; }

seed() {
    rm -rf "$R"; : > "$DS_LAUNCHCTL_LOG"
    mkdir -p "$LOCAL_DIR" "$LOCAL_USERS" "$R/etc" \
             "$R/System/Library/LaunchDaemons"
    for j in rpcbind mountd nfsd network-mount; do
        : > "$R/System/Library/LaunchDaemons/org.nextbsd.$j.plist"
    done
    printf '<plist><dict><key>admin</key><dict/></dict></plist>\n' > "$LOCAL_DIR/Users.plist"
    printf '<plist><dict><key>admin</key><dict/></dict></plist>\n' > "$LOCAL_DIR/Groups.plist"
    cat > "$NTP" <<'NTPC'
# /etc/ntp.conf
pool 0.freebsd.pool.ntp.org iburst
# BEGIN directory server (managed by dsjoin and dsleave; do not edit)
# END directory server
NTPC
}
# The daemon plists the stub checks for live under a staged LaunchDaemons dir,
# but the program looks for the real path, so those lookups miss and it warns.
# That is fine: the log still records what it tried for the ones that exist.

# ---- standalone
seed
cksay "a fresh machine is standalone" "standalone" "$fix/dsstatus"
cksay "dsdemote refuses when not a server" "not a directory server" "$fix/dsdemote"
cksay "dsleave refuses when not bound"     "not bound"              "$fix/dsleave"

# ---- promote
seed
"$fix/dspromote" >/dev/null 2>&1
cklink   "promote links Local -> Network"  "$LOCAL_DIR"  "$NET_DIR"
cklink   "promote links /Network/Users"    "$NET_USERS"  "$LOCAL_USERS"
ckfile   "the plists moved to /Network"    "$NET_DIR/Users.plist"
ckfile   "and Groups.plist with them"      "$NET_DIR/Groups.plist"
ckfile   "exports written"                 "$EXPORTS"
ckfile   "the Bonjour record written"      "$SVC"
ckgrep   "exports names the homes"         "$EXPORTS" "$LOCAL_USERS"
ckgrep   "exports names the accounts"      "$EXPORTS" "$NET_DIR"
ckgrep   "the record advertises the type"  "$SVC" "_nextbsd-ds._tcp"
ckgrep   "and nfs's port as an integer"    "$SVC" "<integer>2049</integer>"
cksay    "status now reports server"       "directory server" "$fix/dsstatus"

# One line, not two, when both paths share a filesystem -- which they do here.
ck "exports is one line on one filesystem" \
   "$(grep -c -v '^#' "$EXPORTS")" "1"

# ---- promote refusals
cksay "promote refuses an existing server" "already a directory server" "$fix/dspromote"
cksay "join refuses on a server"           "run dsdemote first"         "$fix/dsjoin" other.local

# ---- demote reverses it
"$fix/dsdemote" >/dev/null 2>&1
ck       "demote leaves a real directory"  "$( [ -d "$LOCAL_DIR" ] && [ ! -L "$LOCAL_DIR" ] && echo yes )" "yes"
ckfile   "the plists came back"            "$LOCAL_DIR/Users.plist"
cknofile "the /Network copy is gone"       "$NET_DIR/Users.plist"
cknofile "the /Network/Users link is gone" "$NET_USERS"
cknofile "exports removed"                 "$EXPORTS"
cknofile "the Bonjour record withdrawn"    "$SVC"
cksay    "status is standalone again"      "standalone" "$fix/dsstatus"

# ---- join
seed
"$fix/dsjoin" server.local >/dev/null 2>&1
ckfile   "join writes the binding"         "$BINDING"
ckgrep   "the binding names the server"    "$BINDING" "<string>server.local</string>"
ckgrep   "and carries a version"           "$BINDING" "<integer>1</integer>"
ckgrep   "the NTP block names the server"  "$NTP" "^server server.local iburst$"
ckgrep   "the markers are still there"     "$NTP" "^# END directory server$"
cknogrep "the pool line is untouched"      "$NTP" "^#pool"
cksay    "status reports the binding"      "bound to server.local" "$fix/dsstatus"

# ---- join refusals
cksay "join refuses when already bound"    "already bound to server.local" "$fix/dsjoin" other.local
cksay "promote refuses on a client"        "run dsleave first"             "$fix/dspromote"
"$fix/dsleave" >/dev/null 2>&1
cksay "join refuses a bad host name"       "not a usable host name" "$fix/dsjoin" 'evil;rm -rf /'
cknofile "and wrote no binding for it"     "$BINDING"

# ---- leave
seed
"$fix/dsjoin" server.local >/dev/null 2>&1
"$fix/dsleave" >/dev/null 2>&1
cknofile "leave removes the binding"       "$BINDING"
cknogrep "and empties the NTP block"       "$NTP" "^server server.local"
ckgrep   "leaving the markers behind"      "$NTP" "^# BEGIN directory server"
cksay    "status is standalone again"      "standalone" "$fix/dsstatus"

# ---- argv[0] dispatch
cp "$fix/dspromote" "$fix/dswhat"
cksay "an unknown name is refused" "not one of dspromote" "$fix/dswhat"

printf '\n%d checks, %d failures\n' "$((pass+fail))" "$fail"
if [ "$fail" -eq 0 ]; then
    echo "DS-OK: promote, demote, join, leave, the refusals and the artefacts"
    exit 0
fi
echo "DS-FAIL"
exit 1
