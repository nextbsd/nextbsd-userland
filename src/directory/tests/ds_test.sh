#!/bin/sh
# Host-side tests for dspromote, dsdemote, dsjoin, dsleave and dsstatus.
#
# Every path the program touches is a build-time override, so the whole thing
# runs against a staged fake root: no real /etc is written, no real launchctl
# runs, and the suite needs no privilege. launchctl is a stub that records its
# arguments, and DS_LAUNCHD_DIR points at the staged LaunchDaemons tree, so the
# job assertions genuinely fire -- previously the program looked for the real
# /System path, every job was skipped as "not installed", and the log the stub
# wrote was never read by any check.
#
# What is covered: the role transitions in both directions, every refusal, that
# promotion SHARES /Local rather than moving anything to /Network, the role
# marker and its rejection of a malformed one, the exports grouping, the
# managed NTP block, and argv[0] dispatch.
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
DOMAIN=$LOCAL_DIR/Domain.plist
NET_DOMAIN=$NET_DIR/Domain.plist
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

# The same warnings src/directory/Makefile gets from WARNS=6, -Werror included.
# Built with less than that, this suite once passed on both arches while the
# real build had not compiled ds.c at all: a const cast in unmount_network()
# was an error under -Wcast-qual, which was missing here.
WARNS6="-Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith
-Wreturn-type -Wcast-qual -Wwrite-strings -Wswitch -Wshadow -Wcast-align
-Wchar-subscripts -Wnested-externs -Wold-style-definition -Wno-pointer-sign
-Wno-format-y2k -Wno-unused-parameter -Werror"

# shellcheck disable=SC2086
${CC:-cc} -O1 -g $WARNS6 \
    -DDS_TEST \
    -DDS_LOCAL_DIR="\"$LOCAL_DIR\"" \
    -DDS_NETWORK_DIR="\"$NET_DIR\"" \
    -DDS_LOCAL_USERS="\"$LOCAL_USERS\"" \
    -DDS_NETWORK_USERS="\"$NET_USERS\"" \
    -DDS_BINDING="\"$BINDING\"" \
    -DDS_DOMAIN="\"$DOMAIN\"" \
    -DDS_NETWORK_DOMAIN="\"$NET_DOMAIN\"" \
    -DDS_LAUNCHD_DIR="\"$R/System/Library/LaunchDaemons\"" \
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
ckrc()   { if [ "$2" = "$3" ]; then pass=$((pass+1)); else fail=$((fail+1))
           printf 'FAIL %-46s exit %s want %s\n' "$1" "$2" "$3"; fi; }
cklc()   { if grep -q "$2" "$DS_LAUNCHCTL_LOG" 2>/dev/null; then pass=$((pass+1))
           else fail=$((fail+1))
           printf 'FAIL %-46s launchctl log has no /%s/\n' "$1" "$2"; fi; }
cknolc() { if grep -q "$2" "$DS_LAUNCHCTL_LOG" 2>/dev/null; then fail=$((fail+1))
           printf 'FAIL %-46s launchctl log still has /%s/\n' "$1" "$2"
           else pass=$((pass+1)); fi; }
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
# Nothing moves and nothing under /Network is created. These are the checks
# that would have caught the old move-and-symlink layout: the plists stay put,
# /Local stays a real directory, and /Network is left entirely alone for the
# client's mount to land on.
ck       "Local stays a real directory" \
         "$( [ -d "$LOCAL_DIR" ] && [ ! -L "$LOCAL_DIR" ] && echo yes )" "yes"
ckfile   "the plists stayed in /Local"     "$LOCAL_DIR/Users.plist"
ckfile   "and so did Groups.plist"         "$LOCAL_DIR/Groups.plist"
cknofile "nothing was put in /Network"     "$NET_DIR"
cknofile "and no /Network/Users was made"  "$NET_USERS"
ckfile   "the domain marker is written"    "$DOMAIN"
ckgrep   "and it is an empty dict"         "$DOMAIN" "<dict/>"
ckfile   "exports written"                 "$EXPORTS"
ckfile   "the Bonjour record written"      "$SVC"
ckgrep   "exports names the homes"         "$EXPORTS" "$LOCAL_USERS"
ckgrep   "exports shares /Local accounts"  "$EXPORTS" "$LOCAL_DIR"
cknogrep "and exports no /Network path"    "$EXPORTS" "/Network"
ckgrep   "the record advertises the type"  "$SVC" "_nextbsd-ds._tcp"
ckgrep   "and nfs's port as an integer"    "$SVC" "<integer>2049</integer>"
cksay    "status now reports server"       "directory server" "$fix/dsstatus"
"$fix/dsstatus" >/dev/null 2>&1; ckrc "dsstatus succeeds on a server" "$?" 0
# The jobs: asserted, not assumed. These could not fire before DS_LAUNCHD_DIR.
cklc     "promote loads rpcbind"           "load -w .*org.nextbsd.rpcbind"
cklc     "promote loads mountd"            "load -w .*org.nextbsd.mountd"
cklc     "promote loads nfsd"              "load -w .*org.nextbsd.nfsd"
cknolc   "and does not load network-mount" "org.nextbsd.network-mount"

# One line, not two, when both paths share a filesystem -- which they do here.
ck "exports is one line on one filesystem" \
   "$(grep -c -v '^#' "$EXPORTS")" "1"

# ---- promote refusals
cksay "promote refuses an existing server" "already a directory server" "$fix/dspromote"
"$fix/dspromote" >/dev/null 2>&1; ckrc "and exits nonzero" "$?" 1
cksay "join refuses on a server"           "run dsdemote first"         "$fix/dsjoin" other.local

# ---- demote reverses it
"$fix/dsdemote" >/dev/null 2>&1
ck       "demote leaves a real directory"  "$( [ -d "$LOCAL_DIR" ] && [ ! -L "$LOCAL_DIR" ] && echo yes )" "yes"
ckfile   "the plists were never disturbed" "$LOCAL_DIR/Users.plist"
ckfile   "nor were the groups"             "$LOCAL_DIR/Groups.plist"
cknofile "the domain marker is removed"    "$DOMAIN"
cknofile "exports removed"                 "$EXPORTS"
cknofile "the Bonjour record withdrawn"    "$SVC"
cksay    "status is standalone again"      "standalone" "$fix/dsstatus"

# ---- the marker is presence, not contents
# dscli writes an empty dict, so matching on a value would fail to recognise a
# machine Gershwin promoted. Any contents at that path make this a server.
seed
mkdir -p "$LOCAL_DIR"
printf 'not a plist at all\n' > "$DOMAIN"
cksay "any Domain.plist IS a server"       "directory server" "$fix/dsstatus"
cksay "and demote accepts it"              "standalone again" "$fix/dsdemote"

# ---- the order: /Local wins, and /Network is not consulted
# The whole of /Local is exported, so a server that ever had /Network mounted
# would see its own marker arrive back under it. Asked in order, it stays a
# server; asked unordered, it would report itself somebody else's client.
seed
mkdir -p "$LOCAL_DIR" "$NET_DIR"
printf '<plist><dict/></plist>\n' > "$DOMAIN"
printf '<plist><dict/></plist>\n' > "$NET_DOMAIN"
cksay "both markers: still a server"       "directory server" "$fix/dsstatus"
cksay "join still refuses it"              "run dsdemote first" "$fix/dsjoin" other.local
cksay "leave sends it to dsdemote"         "run dsdemote"       "$fix/dsleave"

# ---- join
# A join writes the binding and loads the job; the MOUNT is what makes this
# machine a client, so the marker arriving under /Network is staged here the way
# a successful mount would deliver it.
seed
"$fix/dsjoin" server.local >/dev/null 2>&1
mkdir -p "$NET_DIR"
printf '<plist><dict/></plist>\n' > "$NET_DOMAIN"
ckfile   "join writes the binding"         "$BINDING"
ckgrep   "the binding names the server"    "$BINDING" "<string>server.local</string>"
ckgrep   "and carries a version"           "$BINDING" "<integer>1</integer>"
ckgrep   "the NTP block names the server"  "$NTP" "^server server.local iburst$"
ckgrep   "the markers are still there"     "$NTP" "^# END directory server$"
cknogrep "the pool line is untouched"      "$NTP" "^#pool"
cksay    "status reports the binding"      "bound to server.local" "$fix/dsstatus"
cklc     "join loads network-mount"        "load -w .*org.nextbsd.network-mount"

# ---- join refusals
cksay "join refuses when already bound"    "already bound to server.local" "$fix/dsjoin" other.local
"$fix/dsjoin" other.local >/dev/null 2>&1; ckrc "and exits nonzero" "$?" 1
cksay "promote refuses on a client"        "run dsleave first"             "$fix/dspromote"
# Leaving unmounts, so the staged mount goes with the binding.
"$fix/dsleave" >/dev/null 2>&1; rm -f "$NET_DOMAIN"
cksay "join refuses a bad host name"       "not a usable host name" "$fix/dsjoin" 'evil;rm -rf /'
cknofile "and wrote no binding for it"     "$BINDING"

# ---- leave, with the mount up
seed
"$fix/dsjoin" server.local >/dev/null 2>&1
mkdir -p "$NET_DIR"; printf '<plist><dict/></plist>\n' > "$NET_DOMAIN"
"$fix/dsleave" >/dev/null 2>&1
cknofile "leave removes the binding"       "$BINDING"
cklc     "leave unloads network-mount"     "unload -w .*org.nextbsd.network-mount"
cknogrep "and empties the NTP block"       "$NTP" "^server server.local"
ckgrep   "leaving the markers behind"      "$NTP" "^# BEGIN directory server"
rm -f "$NET_DOMAIN"
cksay    "status is standalone again"      "standalone" "$fix/dsstatus"

# ---- leave, with the mount down
# An unmounted /Network reads as standalone everywhere else, on purpose. dsleave
# is the exception: it must still be able to undo a join, or a machine that
# cannot reach its server could never be unbound and nothing would be left to
# clear the binding, the job or the managed ntp.conf block.
seed
"$fix/dsjoin" server.local >/dev/null 2>&1
cksay    "status is standalone unmounted"  "standalone" "$fix/dsstatus"
cksay    "but leave clears it anyway"      "Left server.local" "$fix/dsleave"
cknofile "and the binding is gone"         "$BINDING"
cknogrep "with the NTP block emptied"      "$NTP" "^server server.local"
cksay    "a second leave then refuses"     "not bound to a directory server" "$fix/dsleave"

# ---- and a stale binding does not let a rebind through
seed
"$fix/dsjoin" server.local >/dev/null 2>&1
cksay "join refuses an unmounted client"   "already bound to server.local" "$fix/dsjoin" other.local

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
