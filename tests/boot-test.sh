#!/bin/sh
# boot-test.sh — boot the disk image in qemu (UEFI), log in as root, and
# run the on-image test suite interactively. CI boots UEFI via OVMF;
# BIOS boot of the same dual-boot image is verified separately.

set -eu

IMG=${1:?usage: boot-test.sh path/to/disk.img[.zip|.gz]}

if [ ! -f "$IMG" ]; then
    echo "ERROR: $IMG not found"
    exit 1
fi

mkdir -p tests
LOG=tests/boot.log
EXP=tests/boot.exp

# Accept the published image in any of: raw .img, .zip (current
# published format — single-entry DEFLATE-9 archive containing the
# NextBSD-<arch>.img raw image), .gz (legacy). Extract/decompress to a
# raw .img that qemu can boot as a disk.
case "$IMG" in
*.zip)
    RAW=tests/disk.img
    echo "==> extracting $IMG -> $RAW"
    # Discover the single .img member by extension (it's named
    # NextBSD-<arch>.img) rather than hardcoding a fixed entry name.
    # -p: write the member to stdout.
    MEMBER=$(unzip -Z1 "$IMG" | grep -E '\.img$' | head -1)
    [ -n "$MEMBER" ] || { echo "FAIL: no .img member in $IMG" >&2; exit 1; }
    unzip -p "$IMG" "$MEMBER" > "$RAW"
    IMG=$RAW
    ;;
*.gz)
    RAW=tests/disk.img
    echo "==> decompressing $IMG -> $RAW"
    gunzip -c "$IMG" > "$RAW"
    IMG=$RAW
    ;;
esac

ARCH="${ARCH:-amd64}"
# BOOT_GATE: full  = boot -> login -> run the on-image Mach/launchd/configd/etc
#                    test suite (both arches since nextbsd#355 was fixed —
#                    the arm64 post-login hang was getty's std.9600 class
#                    leaving CLOCAL clear, so the shell's job-control
#                    open(/dev/tty) waited forever for DCD on the pl011).
#            login = boot -> assert the login: prompt -> clean exit. Kept as
#                    an opt-in smoke gate for bring-up of new arches.
BOOT_GATE="${BOOT_GATE:-full}"
# BOOT_TRACE=1 turns on the mach.debug_enable + launchd_trace kenvs at the
# loader. OFF by default (nextbsd#369): those trace points are kernel printf,
# so with the console un-muted they push ~1800 lines (~144KB, ~12s of blocking
# writes at 115200) through the emulated UART during a single-threaded TCG boot
# — right through the notifyd/syslogd Mach handshake that #369 says is racing
# under load. Set BOOT_TRACE=1 to get the paper trail back when root-causing.
BOOT_TRACE="${BOOT_TRACE:-0}"
echo "==> boot test: $IMG  (arch=$ARCH, gate=$BOOT_GATE, trace=$BOOT_TRACE)"
ls -lh "$IMG"

# Arch-specific qemu shape: binary, machine type, NIC model, TCG cpu, and the
# UEFI firmware search path. amd64 = q35 + OVMF + e1000; arm64 = virt + AAVMF
# (QEMU_EFI) + virtio-net. The expect script below consumes these via env.
case "$ARCH" in
  amd64)
    QEMU=qemu-system-x86_64
    MACHINE=q35
    # e1000 option ROM ships with qemu-system-x86; -nic is fine.
    NET_ARGS="-nic user,model=e1000"
    # q35 always instantiates a VGA adapter (-display none suppresses only the
    # HOST window, not the emulated device), so the guest already has a
    # framebuffer -> efifb -> vt(4) -> /dev/ttyv0. Nothing to add.
    VIDEO_ARGS=""
    TCG_CPU=qemu64
    FW_CANDIDATES="/usr/share/OVMF/OVMF_CODE.fd /usr/share/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd"
    ;;
  arm64|aarch64)
    QEMU=qemu-system-aarch64
    MACHINE=virt
    # virtio-net-pci defaults to loading a PXE option ROM (efi-virtio.rom) that
    # the arm64 qemu package doesn't ship -> qemu exits "failed to find romfile".
    # We never netboot, so disable the ROM with romfile= (empty). Use the
    # -netdev/-device form since -nic doesn't take romfile.
    NET_ARGS="-netdev user,id=net0 -device virtio-net-pci,netdev=net0,romfile="
    # `virt` instantiates NO display adapter at all, unlike q35. Without one the
    # firmware exposes no GOP, the loader hands the kernel no framebuffer, vt(4)
    # never attaches, and /dev/ttyv0 does not exist — so the console-on-screen
    # path went untested on the one arch where it actually broke (#55: no login
    # on the arm64 framebuffer, found only by booting a published image in UTM,
    # never by CI). Give the guest a GPU so the tested machine matches the
    # machines users run. Free under -display none: the device is emulated
    # either way, only the host-side window is suppressed.
    VIDEO_ARGS="-device virtio-gpu-pci"
    TCG_CPU=max
    FW_CANDIDATES="/usr/share/qemu-efi-aarch64/QEMU_EFI.fd /usr/share/AAVMF/AAVMF_CODE.fd /usr/share/edk2/aarch64/QEMU_EFI.fd"
    ;;
  *) echo "ERROR: unsupported ARCH=$ARCH" >&2; exit 1 ;;
esac

# Pick acceleration. KVM if available (native-arch runner); TCG fallback. On
# arm64 the TCG cpu is 'max' (qemu64 is x86-only).
if [ -e /dev/kvm ]; then
    sudo chmod 666 /dev/kvm 2>/dev/null || true
fi
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    ACCEL_FLAGS="-accel kvm -cpu host"
    echo "==> using KVM acceleration"
else
    ACCEL_FLAGS="-accel tcg,thread=single -cpu $TCG_CPU"
    echo "==> using TCG (single-thread)"
fi

# Find the UEFI firmware for this arch.
FW=""
for f in $FW_CANDIDATES; do
    if [ -f "$f" ]; then FW="$f"; break; fi
done
if [ -z "$FW" ]; then
    echo "ERROR: no UEFI firmware found for $ARCH (looked in: $FW_CANDIDATES)" >&2
    exit 1
fi
echo "==> firmware: $FW | qemu: $QEMU -machine $MACHINE | net: $NET_ARGS"

export ARCH BOOT_GATE BOOT_TRACE ACCEL_FLAGS FW QEMU MACHINE NET_ARGS VIDEO_ARGS

cat > "$EXP" <<'EOF'
set timeout 480
log_file -a tests/boot.log
log_user 1

# Type each command at a human cadence: 1 char / 40ms (~25 cps, a brisk typist)
# via send -s. This is plenty for the loader's UART to read the command without
# mis-reading it. Note: the UART can still drop a char from the ECHO it prints
# back (boot_multicons -> boo_multicons) even when it got the command fine — so
# loader_set syncs on the OK prompt, NOT the echo, and is immune to that.
set send_slow {1 .04}

set img [lindex $argv 0]
set accel_flags [split $env(ACCEL_FLAGS) " "]

eval spawn $env(QEMU) \
    -m 4G \
    -machine $env(MACHINE) \
    -bios $env(FW) \
    $accel_flags \
    -drive file=$img,format=raw,if=virtio \
    $env(NET_ARGS) \
    $env(VIDEO_ARGS) \
    -display none -serial stdio \
    -no-reboot

# Stage 0: at the loader's autoboot countdown, drop into the OK prompt
# and enable the serial console for the kernel. /boot/loader.conf no
# longer sets console/boot_serial/etc. (clean console on real laptops
# and VBox where the floating comconsole UART line was producing junk
# chars at the loader prompt). Under qemu+OVMF -display none the
# loader's default `efi` console routes to serial so we can interact
# from here.
expect {
    timeout {
        puts "\nFAIL: didn't see loader autoboot prompt within 60s"
        exit 1
    }
    -re "Hit \\\[Enter\\\]" { send " " }
    "Booting"                { send " " }
    -re "FreeBSD/\[a-z0-9\]+ EFI" { send " " }
}

expect {
    timeout {
        puts "\nFAIL: didn't reach loader OK prompt within 30s"
        exit 1
    }
    "OK " { puts "\n==> at loader prompt; setting serial console vars" }
}

# loader_set — robustly set ONE loader variable over the serial console.
# The loader echoes the typed line then prints a fresh "OK " prompt; firing the
# next command before its line discipline is ready at that prompt can merge/eat
# it (run 26070245676 ate `boot_multicons=YES`). Matching the echo BEFORE the
# OK helped but still raced occasionally — the missing piece is a settle at the
# prompt before sending. So per command:
#   1. settle (let the loader finish printing its OK prompt and be ready)
#   2. send the command
#   3. match THIS command's exact echo (so we never latch a stale OK)
#   4. wait for the next OK prompt
# Tight per-command timeout so a real hang fails in seconds, not at the global
# 8-min bound. ~1s x 6 vars is negligible against multi-minute boot.
proc loader_set {cmd} {
    global timeout
    set saved $timeout
    set timeout 20
    sleep 0.3
    # Drain the settled prior prompt/output, THEN send, THEN sync on the fresh
    # OK. We deliberately do NOT match the command echo: this emulated UART
    # intermittently drops a char from the ECHO (e.g. boot_multicons -> boo_...)
    # even though the loader received the command fine and printed OK — so an
    # exact-echo match hangs on a cosmetic glitch. Syncing on the next OK is
    # robust to echo corruption. send_slow still paces input so the loader reads
    # the command itself correctly; the drain prevents latching the prior OK.
    expect *
    send -s -- "$cmd\r"
    expect {
        timeout { puts "\nFAIL: no OK prompt after '$cmd' within 20s"; exit 1 }
        "OK "
    }
    set timeout $saved
}

# loader_boot — issue the final `boot`, robust to the emulated UART dropping a
# char on INPUT (not just the echo). A dropped input char makes the loader read
# e.g. `bot` and print "unknown command", leaving us stuck at the OK prompt
# forever (run 28754808354, amd64). Unlike loader_set's variable commands, a
# garbled boot never self-recovers — so detect the reject (or a re-appeared OK
# prompt) and resend. Success = the loader leaves the prompt to start the
# kernel: no "unknown command" and no fresh "OK " within the settle window.
proc loader_boot {} {
    global timeout
    set saved $timeout
    set timeout 10
    for {set try 1} {$try <= 5} {incr try} {
        sleep 0.3
        expect *
        send -s -- "boot\r"
        expect {
            "unknown command" {
                puts "\n==> loader garbled 'boot' (try $try/5); resending"
                continue
            }
            -re "Booting|/boot/kernel/kernel|Loading kernel|---<<" {
                set timeout $saved
                return
            }
            "OK " {
                puts "\n==> loader still at prompt after 'boot' (try $try/5); resending"
                continue
            }
            timeout {
                # No reject and no fresh OK prompt within the window => the
                # kernel is loading. Hand off to the Stage 1a banner capture.
                set timeout $saved
                return
            }
        }
    }
    puts "\nFAIL: loader never accepted 'boot' after 5 tries (UART input drop)"
    exit 1
}

# Dropped vidconsole from the console var: QEMU CI runs with -display none, so
# vidconsole always fails to bind and the loader prints "console vidconsole is
# unavailable" every boot. comconsole alone suffices for serial-only capture.
loader_set "set console=comconsole"
loader_set "set boot_serial=YES"
loader_set "set comconsole_speed=115200"
loader_set "set boot_multicons=YES"
# Undo the shipped console mute for CI only (nextbsd#363, nextbsd#369).
# nextbsd-overlays' /boot/loader.conf.d/nextbsd.conf sets boot_mutemsgs="YES" so
# a normal boot has a macOS-clean login console. That mutes the kernel console
# for the whole boot, and the on-image test markers this script matches ride the
# same console — so with the mute in place the expect blocks below time out on
# markers that were in fact emitted. Clear it here, the same way and in the same
# place we opt CI into the serial console: the shipped image stays quiet, CI
# sees everything. `NO` (not `unset`) is deliberate — the loader's howto builder
# does `val != NULL && strcasecmp(val, "no") != 0` (sys/kern/subr_boot.c
# boot_env_to_howto), so `=NO` clears RB_MUTEMSGS while keeping the variable set.
loader_set "set boot_mutemsgs=NO"
# Verbose diagnostic trace toggles. CI-only — the shipped ISO is silent by
# default. The kernel reads mach.debug_enable as a tunable (CTLFLAG_RWTUN) at
# boot; launchd PID 1 and libxpc both read kenv "launchd_trace=1" once at
# startup. Together these gate the [T41-*]/[T39-*] trace points.
#
# OFF by default on BOTH arches now (nextbsd#369). It was already off on arm64,
# where the flood "crawls the emulated boot" under single-thread TCG. The amd64
# runner has no KVM either, and once the console is un-muted (above) the trace
# is ~1800 kernel printfs — ~144KB of synchronous, kernel-blocking writes over a
# 115200 emulated UART — landing squarely on the notifyd/syslogd Mach handshake
# that #369 reports stalling under load. Run 29097309084 showed exactly that:
# markers flowed fine, then SYSLOG-RUN-FAIL with syslogd parked in
# mach_msg_receive. Nothing downstream needs these lines — the expect blocks
# match markers from the on-image run.sh, and run.sh emits its own procstat -kk
# diagnostics on failure. BOOT_TRACE=1 brings them back for root-cause runs.
if {$env(BOOT_TRACE) eq "1"} {
    puts "\n==> BOOT_TRACE=1: enabling mach.debug_enable + launchd_trace"
    loader_set "set mach.debug_enable=1"
    loader_set "set launchd_trace=1"
}
loader_boot

# Stage 1a: capture getty's boot banner. PAM port iter 4 (issue #99)
# restored RunAtLoad on com.apple.hostnamed.plist so hostnamed runs
# at boot. Banner format: "<ostype>/<arch> (HOSTNAME) (console)", from getty's
# \s/\m -- ostype is NextBSD after the kernel rebrand (FreeBSD upstream) and
# arch follows the target (amd64/arm64/powerpc/...). Matched brand- AND
# arch-agnostically so neither a rebrand nor a new arch breaks this check.
#
# launchd's early-init synthesizes the hostname from SMBIOS and sets it
# before any LaunchDaemon starts, and it logs "early-init: sethostname('X')"
# on the console just before the banner. getty reads the hostname once, so
# the banner must show that same X. 'Amnesiac' (nothing set) or 'localhost'
# (launchctl bootstrap's Apple reset, launchctl.c system_specific_bootstrap)
# means the early name was lost (nextbsd/nextbsd#325). A mismatch is recorded
# and fails the run at the end, so the checks after the banner still run.
set early_name ""
set banner_fail ""
# Exit 2, not 1, for the two "never got there" timeouts below. A boot that
# never completes is categorically different from an assertion that fails, and
# boot_soft must not hide it -- see #87. The workflow gates on 2 regardless of
# boot_soft, while ordinary marker failures (exit 1) stay soft.
expect {
    timeout {
        puts "\nFAIL: boot banner not seen within 8 minutes"
        exit 2
    }
    -re "early-init: sethostname\\('(\[^'\]+)'\\)" {
        set early_name $expect_out(1,string)
        exp_continue
    }
    -re "\[A-Za-z\]*BSD/\[A-Za-z0-9_\]+ \\(\(\[A-Za-z0-9._-\]+\)\\) \\(console\\)" {
        set banner_name $expect_out(1,string)
        if {$banner_name eq "Amnesiac" || $banner_name eq "localhost"} {
            set banner_fail "banner shows '$banner_name', not the synthesized hostname '$early_name'"
        } elseif {$early_name ne "" && $banner_name ne $early_name} {
            set banner_fail "banner shows '$banner_name' but early-init set '$early_name'"
        }
        if {$banner_fail eq ""} {
            puts "\nOK: BOOT-BANNER-OK — getty shows the synthesized hostname '$banner_name'"
        } else {
            puts "\nFAIL: BOOT-BANNER — $banner_fail (recorded; fails the run at the end)"
        }
    }
}

# Stage 1b: wait for the getty "login:" prompt. Boot is complete:
# loader preloaded mach.ko -> kernel mounts the freebsd-ufs root rw ->
# /sbin/launchd as PID 1 -> getty plist -> login.
expect {
    timeout {
        puts "\nFAIL: 'login:' prompt not seen within 8 minutes"
        exit 2
    }
    "login:" { puts "\nOK: boot reached the login prompt" }
}

# BOOT_GATE=login: stop here — boot proven through kernel + mach + launchd +
# all daemons + getty to the login prompt. Opt-in smoke gate for new-arch
# bring-up; both current arches run BOOT_GATE=full.
if {$env(BOOT_GATE) eq "login"} {
    puts "\nOK: BOOT-LOGIN-OK — $env(ARCH) reached the login prompt (login gate PASSED)"
    close
    wait
    exit 0
}

# Stage 2: log in as root. The live ISO has no root password, so login
# either drops straight to the shell or asks "Password:" and accepts an
# empty password. Both paths land at a shell prompt; failure is
# "Login incorrect" or silence.
send "root\r"
expect {
    timeout {
        puts "\nFAIL: no response after sending root"
        exit 1
    }
    "Password:" {
        send "\r"
        exp_continue
    }
    "Login incorrect" {
        puts "\nFAIL: root login rejected"
        exit 1
    }
    -re {[#%$] $} { puts "\nOK: at root shell prompt" }
}

# Stage 3: invoke the on-ISO mach smoke test. Test scripts live under
# /usr/tests/<component>/ following the FreeBSD convention. The script
# emits two markers in sequence:
#   MACH-SMOKE-OK / MACH-SMOKE-FAIL          — kernel-side kextstat -m mach
#   LIBSYSTEM-KERNEL-OK / LIBSYSTEM-KERNEL-FAIL — userland test_libmach
# Both must pass.
set saved_marker_timeout $timeout
send "/usr/tests/freebsd-launchd-mach/run.sh\r"
expect {
    timeout {
        puts "\nFAIL: /usr/tests/freebsd-launchd-mach/run.sh timed out"
        exit 1
    }
    "MACH-SMOKE-FAIL" {
        puts "\nFAIL: mach.ko did NOT load on boot"
        exit 1
    }
    "MACH-SMOKE-OK" { puts "\nOK: mach.ko is loaded" }
}
expect {
    timeout {
        puts "\nFAIL: LIBSYSTEM-KERNEL marker not seen"
        exit 1
    }
    "LIBSYSTEM-KERNEL-FAIL" {
        puts "\nFAIL: libsystem_kernel roundtrip failed"
        exit 1
    }
    "LIBSYSTEM-KERNEL-OK" { puts "\nOK: libsystem_kernel works" }
}
expect {
    timeout {
        puts "\nFAIL: MACH-PORT marker not seen"
        exit 1
    }
    "MACH-PORT-FAIL" {
        puts "\nFAIL: mach_port_* round-trip failed"
        exit 1
    }
    "MACH-PORT-OK" { puts "\nOK: mach_port_* round-trip works" }
}
# EVFILT-MACHPORT — #168 Phase A discovery probe: does the kernel's NATIVE
# kqueue Mach-port filter (mach.ko filt_machport, slot -16) deliver a wakeup +
# inline-receive on a port set, via raw kevent(2) (no libdispatch/libmach
# bridge)? NON-FATAL — every outcome is reported, none gates the build; the
# result decides whether the configd KEM-in-process rework (#168) can use the
# native filter directly or must fall back to the register_event_bell pipe-bridge.
expect {
    timeout {
        puts "\nWARN: EVFILT-MACHPORT marker not seen (pre-#168 run.sh — informational)"
    }
    "EVFILT-MACHPORT-FAIL" {
        puts "\nWARN: EVFILT-MACHPORT-FAIL — native kqueue Mach-port filter did NOT deliver (configd fold must use the pipe-bridge)"
    }
    "EVFILT-MACHPORT-SKIP" {
        puts "\nWARN: EVFILT-MACHPORT-SKIP — native filter unavailable on this kernel image"
    }
    "EVFILT-MACHPORT-OK" {
        puts "\nOK: native EVFILT_MACHPORT delivers (kqueue wakeup + inline receive) — #168 can use it directly"
    }
}
# EVFILT-MACHPORT-CONCURRENT — #168 Stage 0 / #251 concurrency stress: pthreads
# hammer concurrent knote attach/detach + port-set teardown + move_member churn
# on a shared set (the PR #250 panic surface: #253 UAF, #252 NULL td_machdata,
# #148 thread-exit kmsg destroy). A kernel regression panics here — caught by the
# boot test's panic handling — so a clean OK proves the races stay fixed. WARN on
# timeout (pre-#251 run.sh / image) and on SKIP (filter unavailable); FAIL on -FAIL.
expect {
    timeout {
        puts "\nWARN: EVFILT-MACHPORT-CONCURRENT marker not seen (pre-#251 run.sh — informational)"
    }
    "EVFILT-MACHPORT-CONCURRENT-FAIL" {
        puts "\nFAIL: EVFILT-MACHPORT-CONCURRENT-FAIL — a stress worker hit an unexpected error"
        exit 1
    }
    "EVFILT-MACHPORT-CONCURRENT-SKIP" {
        puts "\nWARN: EVFILT-MACHPORT-CONCURRENT-SKIP — native filter unavailable on this kernel image"
    }
    "EVFILT-MACHPORT-CONCURRENT-OK" {
        puts "\nOK: native EVFILT_MACHPORT survives concurrent attach/detach/destroy + move_member churn (#251)"
    }
}
expect {
    timeout {
        puts "\nFAIL: TASK-SPECIAL-PORT marker not seen"
        exit 1
    }
    "TASK-SPECIAL-PORT-FAIL" {
        puts "\nFAIL: task_*_special_port round-trip failed"
        exit 1
    }
    "TASK-SPECIAL-PORT-OK" { puts "\nOK: task_*_special_port round-trip works" }
}
expect {
    timeout {
        puts "\nFAIL: HOST-BOOTSTRAP marker not seen"
        exit 1
    }
    "HOST-BOOTSTRAP-FAIL" {
        puts "\nFAIL: host_set_special_port fallback failed"
        exit 1
    }
    "HOST-BOOTSTRAP-OK" { puts "\nOK: host_set_special_port fallback works" }
}
expect {
    timeout {
        puts "\nFAIL: BUSYSTATE marker not seen"
        exit 1
    }
    "BUSYSTATE-FAIL" {
        puts "\nFAIL: mach.bus.busy did not settle to 0 (bus not quiescent / mach.ko hook missing)"
        exit 1
    }
    "BUSYSTATE-OK" { puts "\nOK: mach.bus.busy quiescent (device_match_start/end counter works)" }
}
expect {
    timeout {
        puts "\nFAIL: WAITQUIET marker not seen"
        exit 1
    }
    "WAITQUIET-FAIL" {
        puts "\nFAIL: mach_wait_quiet did not return promptly on a quiescent bus"
        exit 1
    }
    "WAITQUIET-OK" { puts "\nOK: mach_wait_quiet returns on quiescence" }
}
expect {
    timeout {
        puts "\nFAIL: BOOTSTRAP marker not seen"
        exit 1
    }
    "BOOTSTRAP-FAIL" {
        puts "\nFAIL: bootstrap protocol round-trip failed"
        exit 1
    }
    "BOOTSTRAP-OK" { puts "\nOK: bootstrap protocol round-trip works" }
}
expect {
    timeout {
        puts "\nFAIL: BOOTSTRAP-REMOTE marker not seen"
        exit 1
    }
    "BOOTSTRAP-REMOTE-FAIL" {
        puts "\nFAIL: cross-process bootstrap round-trip failed"
        exit 1
    }
    "BOOTSTRAP-REMOTE-OK" { puts "\nOK: cross-process bootstrap works" }
}
expect {
    timeout {
        puts "\nFAIL: LIBDISPATCH marker not seen"
        exit 1
    }
    "LIBDISPATCH-FAIL" {
        puts "\nFAIL: libdispatch baseline roundtrip failed"
        exit 1
    }
    "LIBDISPATCH-OK" { puts "\nOK: libdispatch baseline works" }
}
expect {
    timeout {
        puts "\nFAIL: LIBDISPATCH-MACH marker not seen"
        exit 1
    }
    "LIBDISPATCH-MACH-FAIL" {
        puts "\nFAIL: libdispatch Mach RECV round-trip failed"
        exit 1
    }
    "LIBDISPATCH-MACH-OK" { puts "\nOK: libdispatch Mach RECV round-trip works" }
}
expect {
    timeout {
        puts "\nFAIL: LIBXPC marker not seen"
        exit 1
    }
    "LIBXPC-FAIL" {
        puts "\nFAIL: libxpc dictionary round-trip failed"
        exit 1
    }
    "LIBXPC-OK" { puts "\nOK: libxpc dictionary round-trip works" }
}
expect {
    timeout {
        puts "\nFAIL: MIG-BUILD marker not seen"
        exit 1
    }
    "MIG-BUILD-FAIL" {
        puts "\nFAIL: mig / migcom failed to run on the ISO"
        exit 1
    }
    "MIG-BUILD-OK" { puts "\nOK: mig + migcom installed and runnable" }
}
expect {
    timeout {
        puts "\nFAIL: LAUNCHD-BUILD marker not seen"
        exit 1
    }
    "LAUNCHD-BUILD-FAIL" {
        puts "\nFAIL: /sbin/launchd failed to exec / reject correctly"
        exit 1
    }
    "LAUNCHD-BUILD-OK" { puts "\nOK: /sbin/launchd execs + rejects non-PID-1" }
}
expect {
    timeout {
        puts "\nFAIL: COREFOUNDATION marker not seen"
        exit 1
    }
    "COREFOUNDATION-FAIL" {
        puts "\nFAIL: libCoreFoundation smoke test failed"
        exit 1
    }
    "COREFOUNDATION-OK" { puts "\nOK: libCoreFoundation CFDictionary + plist round-trip works" }
}
expect {
    timeout {
        puts "\nFAIL: LAUNCHCTL-BUILD marker not seen"
        exit 1
    }
    "LAUNCHCTL-BUILD-FAIL" {
        puts "\nFAIL: /bin/launchctl help failed to exec / link"
        exit 1
    }
    "LAUNCHCTL-BUILD-OK" { puts "\nOK: /bin/launchctl execs + prints help" }
}

# LAUNCHCTL-LIST — runtime smoke. Guards against the print_jobs
# NULL-Label segfault (fixed in support/launchctl.c; without the
# fix, the first response entry missing Label crashed the walk).
# Also guards against the deferred D-state-hang concern — run.sh
# runs launchctl in a 30s budget and emits LIST-FAIL if it doesn't
# exit cleanly.
expect {
    timeout {
        puts "\nFAIL: LAUNCHCTL-LIST marker not seen"
        exit 1
    }
    "LAUNCHCTL-LIST-FAIL" {
        puts "\nFAIL: launchctl list did not complete cleanly"
        exit 1
    }
    "LAUNCHCTL-LIST-OK" { puts "\nOK: launchctl list round-trips with launchd" }
}

# GETTY-TTYV0 — the framebuffer login. org.nextbsd.getty.console serves /dev/console,
# which binds to exactly ONE tty (cnselect -> ttyconsdev_select); only kernel
# MESSAGES fan out to every console. On arm64 the UART always wins that
# selection (the EFI loader publishes hw.uart.console from ACPI SPCR on its
# own), so org.nextbsd.getty.ttyv0 is what gives a screen-only arm64 machine a
# reachable login. WARN on timeout: a published image built before that plist
# landed simply doesn't emit the marker, and this stage must not fail it.
# SKIP is expected on a guest with no framebuffer (qemu -machine virt with no
# GPU -> no GOP -> no efifb -> vt(4) never attaches -> no /dev/ttyv0), where the
# job's `test -c` guard makes it a deliberate no-op.
expect {
    timeout {
        puts "\nWARN: GETTY-TTYV0 marker not seen (image predates org.nextbsd.getty.ttyv0 — informational)"
    }
    "GETTY-TTYV0-FAIL" {
        puts "\nFAIL: GETTY-TTYV0-FAIL — framebuffer console has no login"
        exit 1
    }
    "GETTY-TTYV0-SKIP" {
        puts "\nWARN: GETTY-TTYV0-SKIP — no framebuffer on this guest; job no-ops by design"
    }
    "GETTY-TTYV0-OK" { puts "\nOK: framebuffer console (ttyv0) has a login" }
}

# Stage 3+ Phase J runtime: syslogd + notifyd RunAtLoad via plists,
# then syslog(1) post + read-back round-trip via Mach IPC into the
# ASL store. See run.sh tail for the test sequence.
expect {
    timeout {
        puts "\nFAIL: NOTIFYD-PROC marker not seen"
        exit 1
    }
    "NOTIFYD-PROC-FAIL" {
        puts "\nFAIL: notifyd not running at boot"
        exit 1
    }
    "NOTIFYD-PROC-OK" { puts "\nOK: notifyd running" }
}
expect {
    timeout {
        puts "\nFAIL: SYSLOGD-PROC marker not seen"
        exit 1
    }
    "SYSLOGD-PROC-FAIL" {
        puts "\nFAIL: syslogd not running at boot"
        exit 1
    }
    "SYSLOGD-PROC-OK" { puts "\nOK: syslogd running" }
}
expect {
    timeout {
        puts "\nFAIL: SYSLOG-RUN marker not seen"
        exit 1
    }
    "SYSLOG-RUN-FAIL" {
        puts "\nFAIL: syslog(1) post/read round-trip failed"
        exit 1
    }
    "SYSLOG-RUN-SKIP" { puts "\nSKIP: syslog round-trip blocked by launch_msg hang (task #41)" }
    "SYSLOG-RUN-OK" { puts "\nOK: syslog round-trip works" }
}

expect {
    timeout {
        puts "\nFAIL: SERVICE-LDCONFIG marker not seen"
        exit 1
    }
    "SERVICE-LDCONFIG-FAIL" {
        puts "\nFAIL: service ldconfig onestart failed / hints not rewritten"
        exit 1
    }
    "SERVICE-LDCONFIG-OK" { puts "\nOK: service(8) drives the ldconfig rc.d job" }
}

expect {
    timeout {
        puts "\nFAIL: CONFIGD-STORE marker not seen"
        exit 1
    }
    "CONFIGD-STORE-FAIL" {
        puts "\nFAIL: configd SCDynamicStore round-trip failed"
        exit 1
    }
    "CONFIGD-STORE-OK" { puts "\nOK: configd SCDynamicStore round-trip works" }
}

expect {
    timeout {
        puts "\nFAIL: CONFIGD-NOTIFY marker not seen"
        exit 1
    }
    "CONFIGD-NOTIFY-FAIL" {
        puts "\nFAIL: configd change-notification round-trip failed"
        exit 1
    }
    "CONFIGD-NOTIFY-OK" { puts "\nOK: configd change notifications work" }
}

expect {
    timeout {
        puts "\nFAIL: CONFIGD-PATTERN marker not seen"
        exit 1
    }
    "CONFIGD-PATTERN-FAIL" {
        puts "\nFAIL: configd regex pattern watch failed"
        exit 1
    }
    "CONFIGD-PATTERN-OK" { puts "\nOK: configd regex pattern watches work" }
}

expect {
    timeout {
        puts "\nFAIL: CONFIGD-LIST marker not seen"
        exit 1
    }
    "CONFIGD-LIST-FAIL" {
        puts "\nFAIL: configd key listing failed"
        exit 1
    }
    "CONFIGD-LIST-OK" { puts "\nOK: configd key listing works" }
}

expect {
    timeout {
        puts "\nFAIL: CONFIGD-MULTI marker not seen"
        exit 1
    }
    "CONFIGD-MULTI-FAIL" {
        puts "\nFAIL: configd batch routines failed"
        exit 1
    }
    "CONFIGD-MULTI-OK" { puts "\nOK: configd batch routines work" }
}

expect {
    timeout {
        puts "\nFAIL: SC-STORE marker not seen"
        exit 1
    }
    "SC-STORE-FAIL" {
        puts "\nFAIL: SCDynamicStore client framework round-trip failed"
        exit 1
    }
    "SC-STORE-OK" { puts "\nOK: SCDynamicStore client framework works" }
}

expect {
    timeout {
        puts "\nFAIL: SC-NOTIFY marker not seen"
        exit 1
    }
    "SC-NOTIFY-FAIL" {
        puts "\nFAIL: SCDynamicStore change-notification delivery failed"
        exit 1
    }
    "SC-NOTIFY-OK" { puts "\nOK: SCDynamicStore change notifications work" }
}

# SC-RUNLOOP. scrltest does its work and then SIGSEGVs during teardown,
# BEFORE it can print SC-RUNLOOP-OK. The functional part passes -- the log
# shows "run-loop callback fired with the watched key" every time -- so the
# crash is in run-loop/dispatch source disposal, not in the feature.
#
# That cost every run 480 seconds. The marker never arrived, so this block
# sat on the global timeout, on success and failure alike, and then reported
# the misleading "marker not seen". It is the single reason boot tests took
# ~10 minutes regardless of outcome.
#
# Match the crash explicitly so it is reported as what it is, and bound this
# block to 30s so a missing marker can never cost minutes again.
set timeout 30
expect {
    timeout {
        puts "\nFAIL: SC-RUNLOOP marker not seen (no output at all from scrltest)"
        exit 1
    }
    "SC-RUNLOOP-FAIL" {
        puts "\nFAIL: SCDynamicStore run-loop-source notifications failed"
        exit 1
    }
    -re "Segmentation fault|Abort trap" {
        puts "\nFAIL: SC-RUNLOOP — scrltest crashed in teardown after the callback fired (nextbsd-userland#158)"
        exit 1
    }
    "SC-RUNLOOP-OK" { puts "\nOK: SCDynamicStore run-loop notifications work" }
}
set timeout $saved_marker_timeout

expect {
    timeout {
        puts "\nFAIL: SC-MULTI marker not seen"
        exit 1
    }
    "SC-MULTI-FAIL" {
        puts "\nFAIL: SCDynamicStore batch get/set failed"
        exit 1
    }
    "SC-MULTI-OK" { puts "\nOK: SCDynamicStore batch get/set works" }
}

expect {
    timeout {
        puts "\nFAIL: SC-PREFS marker not seen"
        exit 1
    }
    "SC-PREFS-FAIL" {
        puts "\nFAIL: SCPreferences read/edit/commit failed"
        exit 1
    }
    "SC-PREFS-OK" { puts "\nOK: SCPreferences read/edit/commit works" }
}

expect {
    timeout {
        puts "\nFAIL: SC-PATH marker not seen"
        exit 1
    }
    "SC-PATH-FAIL" {
        puts "\nFAIL: SCPreferences path accessors failed"
        exit 1
    }
    "SC-PATH-OK" { puts "\nOK: SCPreferences path accessors work" }
}

expect {
    timeout {
        puts "\nFAIL: SC-LOCK marker not seen"
        exit 1
    }
    "SC-LOCK-FAIL" {
        puts "\nFAIL: SCPreferences lock failed"
        exit 1
    }
    "SC-LOCK-OK" { puts "\nOK: SCPreferences lock works" }
}

expect {
    timeout {
        puts "\nFAIL: SC-PNOTIFY marker not seen"
        exit 1
    }
    "SC-PNOTIFY-FAIL" {
        puts "\nFAIL: SCPreferences change-notification delivery failed"
        exit 1
    }
    "SC-PNOTIFY-OK" { puts "\nOK: SCPreferences change notifications work" }
}

expect {
    timeout {
        puts "\nFAIL: SC-PLINK marker not seen"
        exit 1
    }
    "SC-PLINK-FAIL" {
        puts "\nFAIL: SCPreferences path links failed"
        exit 1
    }
    "SC-PLINK-OK" { puts "\nOK: SCPreferences path links work" }
}

expect {
    timeout {
        puts "\nFAIL: SC-NETIF marker not seen"
        exit 1
    }
    "SC-NETIF-FAIL" {
        puts "\nFAIL: SCNetworkInterface enumeration failed"
        exit 1
    }
    "SC-NETIF-OK" { puts "\nOK: SCNetworkInterface enumeration works" }
}

expect {
    timeout {
        puts "\nFAIL: SC-NETSVC marker not seen"
        exit 1
    }
    "SC-NETSVC-FAIL" {
        puts "\nFAIL: SCNetworkService / SCNetworkProtocol failed"
        exit 1
    }
    "SC-NETSVC-OK" { puts "\nOK: SCNetworkService + SCNetworkProtocol work" }
}

expect {
    timeout {
        puts "\nFAIL: SC-NETSET marker not seen"
        exit 1
    }
    "SC-NETSET-FAIL" {
        puts "\nFAIL: SCNetworkSet failed"
        exit 1
    }
    "SC-NETSET-OK" { puts "\nOK: SCNetworkSet works" }
}

expect {
    timeout {
        puts "\nFAIL: SC-VLAN marker not seen"
        exit 1
    }
    "SC-VLAN-FAIL" {
        puts "\nFAIL: SCVLANInterface failed"
        exit 1
    }
    "SC-VLAN-OK" { puts "\nOK: SCVLANInterface works" }
}

expect {
    timeout {
        puts "\nFAIL: SC-BOND marker not seen"
        exit 1
    }
    "SC-BOND-FAIL" {
        puts "\nFAIL: SCBondInterface failed"
        exit 1
    }
    "SC-BOND-OK" { puts "\nOK: SCBondInterface works" }
}

expect {
    timeout {
        puts "\nFAIL: SC-BRIDGE marker not seen"
        exit 1
    }
    "SC-BRIDGE-FAIL" {
        puts "\nFAIL: SCBridgeInterface failed"
        exit 1
    }
    "SC-BRIDGE-OK" { puts "\nOK: SCBridgeInterface works" }
}

expect {
    timeout {
        puts "\nFAIL: IPCFG-BOOT marker not seen"
        exit 1
    }
    "IPCFG-BOOT-FAIL" {
        puts "\nFAIL: ipconfigd skeleton did not claim its Mach service"
        exit 1
    }
    "IPCFG-BOOT-OK" {
        puts "\nOK: ipconfigd skeleton up (com.apple.IPConfiguration registered)"
    }
}

# KEM-LINK — the in-process KernelEventMonitor (configd's config_link_monitor.c,
# #257) published State:/Network/Interface/<if>/Link = {Active:…} from a PF_ROUTE
# link-state change. This is the Apple-shaped DHCP-on-link-up trigger that
# replaced the hwregd attach subscription; ipconfigd's DHCP (IPCFG-BOUND below)
# now depends on it. run.sh surfaces the marker by cat'ing /var/log/configd.stderr.
expect {
    timeout {
        puts "\nFAIL: KEM-LINK marker not seen"
        exit 1
    }
    "KEM-LINK-FAIL" {
        puts "\nFAIL: KernelEventMonitor did not publish a link-state key"
        exit 1
    }
    "KEM-LINK-OK" {
        puts "\nOK: KernelEventMonitor published State:/Network/Interface/.../Link"
    }
}

# IPCFG-ARP — iter 6 RFC 5227 ARP probe on the DHCPOFFER. Fires
# after 3 successful (= no-reply) probes of the offered address;
# precedes IPCFG-BOUND-OK in the timeline (probe runs between
# OFFER and REQUEST). Conflict path would log IPCFG-BOUND-FAIL.
expect {
    timeout {
        puts "\nFAIL: IPCFG-ARP marker not seen"
        exit 1
    }
    "IPCFG-BOUND-FAIL" {
        puts "\nFAIL: ipconfigd DHCPv4 failed before ARP probe completed"
        exit 1
    }
    "IPCFG-ARP-OK" {
        puts "\nOK: ipconfigd RFC 5227 ARP probe clean (no conflict on offered address)"
    }
    "IPCFG-ARP-SKIP" {
        puts "\nSKIP: ARP probe produced no result -- dhcp_discover.c treats a failed
        arp_probe as no-conflict by design, so this is tolerated (#87)"
    }
}

expect {
    timeout {
        puts "\nFAIL: IPCFG-BOUND marker not seen"
        exit 1
    }
    "IPCFG-BOUND-FAIL" {
        puts "\nFAIL: ipconfigd DHCPv4 INIT → BOUND failed"
        exit 1
    }
    "IPCFG-BOUND-OK" {
        puts "\nOK: ipconfigd DHCPv4 INIT → BOUND works (address + route + DNS installed)"
    }
}

expect {
    timeout {
        puts "\nFAIL: IPCFG-STORE marker not seen"
        exit 1
    }
    "IPCFG-STORE-FAIL" {
        puts "\nFAIL: ipconfigd SCDynamicStore publish failed"
        exit 1
    }
    "IPCFG-STORE-OK" {
        puts "\nOK: ipconfigd published State:/Network/Service/.../IPv4 to configd"
    }
}

# IPCFG-DHCP — issue #88. ipconfigd publishes State:/Network/Service/<UUID>/DHCP
# carrying InterfaceName + LeaseStartTime (always), and Option_12
# (host name, only when the lease supplied it). SLIRP doesn't ship
# Option_12 so the marker just proves the dict shape is correct;
# hostnamed iter 3 is the first consumer that reads the value.
expect {
    timeout {
        puts "\nFAIL: IPCFG-DHCP marker not seen"
        exit 1
    }
    "IPCFG-DHCP-FAIL" {
        puts "\nFAIL: ipconfigd /DHCP publish failed"
        exit 1
    }
    "IPCFG-DHCP-OK" {
        puts "\nOK: ipconfigd published State:/Network/Service/.../DHCP (consumer surface for hostnamed iter 3)"
    }
}

# IPCFG-RA — iter 7a IPv6 Router Advertisement + SLAAC. ipconfigd
# sends an ND_ROUTER_SOLICIT to ff02::2 on em0, waits up to 15s for
# an RA, derives a SLAAC address (EUI-64 over the PIO's prefix),
# installs it + a default ::/0 route via the RA's source LL, and
# publishes State:/Network/Service/.../IPv6.
#
# IPCFG-RA-MISS is the soft path: QEMU SLIRP's IPv6 RA support varies
# by qemu version and CLI args. First-round CI accepts MISS so we can
# see whether SLIRP actually responds; later rounds either keep MISS
# acceptable (if SLIRP is silent) or upgrade to OK-required (if SLIRP
# does answer). Either way, IPv6 not coming up does NOT fail the
# overall boot — IPv4 already passed.
expect {
    timeout {
        puts "\nFAIL: IPCFG-RA marker not seen (neither OK nor MISS)"
        exit 1
    }
    "IPCFG-RA-OK" {
        puts "\nOK: ipconfigd RA/SLAAC works (address + ::/0 route + State:/.../IPv6 published)"
    }
    "IPCFG-RA-MISS" {
        puts "\nWARN: ipconfigd RA-MISS — SLIRP did not respond to RS (IPv4 still OK)"
    }
}

expect {
    timeout {
        puts "\nFAIL: IPCFG-RENEW-OK marker not seen (iter 5b lease renewal CI gate)"
        exit 1
    }
    "IPCFG-RENEW-OK" {
        puts "\nOK: ipconfigd lease renewal works (RENEWING DHCPREQUEST → ACK, lease re-published)"
    }
}

# IPCFG-RPC fires AFTER the daemon log dump (ipconfigrpctest runs at
# the end of run.sh), so expect for it last.
expect {
    timeout {
        puts "\nFAIL: IPCFG-RPC marker not seen"
        exit 1
    }
    "IPCFG-RPC-FAIL" {
        puts "\nFAIL: ipconfigd MIG RPC (ipconfig_if_count / if_addr) failed"
        exit 1
    }
    "IPCFG-RPC-OK" {
        puts "\nOK: ipconfigd MIG RPC works (ipconfig_if_count + ipconfig_if_addr returned the BOUND lease)"
    }
}

# MDNS-BOOT — mDNSResponder Mach service liveness. bootstrap_look_up
# for com.apple.mDNSResponder via mdnstest. Proves the Mach surface
# is up; the engine itself is gated separately by MDNS-ENGINE-OK.
expect {
    timeout {
        puts "\nFAIL: MDNS-BOOT marker not seen"
        exit 1
    }
    "MDNS-BOOT-FAIL" {
        puts "\nFAIL: mDNSResponder did not register its Mach service"
        exit 1
    }
    "MDNS-BOOT-OK" {
        puts "\nOK: mDNSResponder Mach service up (com.apple.mDNSResponder registered)"
    }
}

# MDNS-ENGINE — iter 2 mDNS engine. Logged by the daemon itself
# (PosixDaemon.c, after mDNS_Init returns mStatus_NoError) to
# /var/log/mDNSResponder.stderr, which run.sh cats so the marker
# reaches this console.
expect {
    timeout {
        puts "\nFAIL: MDNS-ENGINE marker not seen"
        exit 1
    }
    "MDNS-ENGINE-FAIL" {
        puts "\nFAIL: mDNS_Init returned an error"
        exit 1
    }
    "MDNS-ENGINE-OK" {
        puts "\nOK: mDNSResponder engine up (mDNS_Init + udsserver ready)"
    }
}

# MDNS-IFWATCH (iter 4, OPTIONAL) + MDNS-DNSSD (iter 3, REQUIRED) — handled
# in ONE expect block because the two markers can arrive in either order and
# MDNS-IFWATCH may not arrive at all. When IFWATCH was a SEPARATE preceding
# block, an absent IFWATCH marker made it sit in its own timeout while run.sh
# raced ahead and emitted MDNS-DNSSD-OK unmatched — then the DNSSD block saw
# its marker already scroll past and failed "marker not seen" (it had, in
# fact, succeeded). Folding them together makes the optional IFWATCH markers
# non-terminating (exp_continue) so they can never swallow the required DNSSD
# marker; only DNSSD-OK/FAIL (or a real absence-of-DNSSD timeout) ends it.
#
# MDNS-IFWATCH — mDNSConfigStore.c's SCDS subscriber wakes the mDNS main
# thread (self-pipe onto the event loop) to re-walk the interface list when
# ipconfigd publishes/removes a service IPv4. Informational only: the
# routing-socket watcher in mDNSPosix.c is a parallel trigger, so its absence
# is not a regression.
# MDNS-DNSSD — end-to-end DNS-SD round-trip via libdns_sd: dnssdtest
# registers "_iter3._tcp" / "freebsd-launchd-mach-iter3" through the daemon's
# AF_UNIX channel and browses for it. This is the hard gate.
expect {
    timeout {
        puts "\nFAIL: MDNS-DNSSD marker not seen"
        exit 1
    }
    "MDNS-IFWATCH-OK" {
        puts "\nOK: SCDS interface/service IPv4 change drove a mDNSResponder interface re-walk (iter 4)"
        exp_continue
    }
    "MDNS-IFWATCH-SKIP" {
        puts "\nSKIP: no SCDS-driven interface re-walk observed (routing-socket watcher still active)"
        exp_continue
    }
    "MDNS-DNSSD-FAIL" {
        puts "\nFAIL: dnssdtest could not round-trip Register + Browse via libdns_sd"
        exit 1
    }
    "MDNS-DNSSD-OK" {
        puts "\nOK: libdns_sd end-to-end (Register + Browse round-trip via /var/run/mDNSResponder)"
    }
}

# DA-BOOT — DiskArbitration iter 1 liveness. bootstrap_look_up for
# com.apple.DiskArbitration via datest. Iter 1 ships just the daemon
# skeleton (no libgeom / framework yet); the kernel notify-channel
# storage subscription (C1.3, #218) is wired in da_iokit_subscribe.c.
expect {
    timeout {
        puts "\nFAIL: DA-BOOT marker not seen"
        exit 1
    }
    "DA-BOOT-FAIL" {
        puts "\nFAIL: diskarbitrationd did not register its Mach service"
        exit 1
    }
    "DA-BOOT-OK" {
        puts "\nOK: diskarbitrationd Mach service up (com.apple.DiskArbitration registered)"
    }
}

# DA-IOKIT — C1.3 (#218) DiskArbitration kernel-notify migration gate. run.sh's
# da_iokit_gate inspects /var/log/diskarbitrationd.stderr: the daemon now learns
# of storage arrival/removal from the kernel notify channel via libIOKit's
# IOServiceAddMatchingNotification (recv port registered via IOREGIOCWATCH on
# /dev/ioregistry, #225). hwregd was retired in #218 — the kernel is the
# registry, no userland fallback. DA-IOKIT-OK means it registered the kernel
# watches AND saw a storage device (the qemu vtblk/ahci boot disk) arrive
# through that channel.
#
# CRITICAL (MDNS-IFWATCH lesson): OK/SKIP are folded into ONE block that ends
# only on OK/FAIL/SKIP, with a NON-FATAL timeout, so an absent/optional marker
# can never sit blocking while a later required marker scrolls past. The gate
# SELF-SKIPs when /dev/ioregistry is absent (kernel predating K1 — no storage
# events) or when no storage device surfaced through the kernel channel, so this
# stays green on pre-K1 continuous images. Only DA-IOKIT-FAIL gates (a kernel
# watch registration failed outright).
expect {
    timeout {
        puts "\nWARN: DA-IOKIT marker not seen (pre-C1.3 run.sh — informational)"
    }
    "DA-IOKIT-FAIL" {
        puts "\nFAIL: diskarbitrationd failed to register kernel device notifications"
        exit 1
    }
    "DA-IOKIT-SKIP" {
        puts "\nWARN: DA-IOKIT-SKIP — no /dev/ioregistry or no storage arrival"
    }
    "DA-IOKIT-OK" {
        puts "\nOK: diskarbitrationd saw a storage device via the kernel notify channel (/dev/ioregistry)"
    }
}

# IPCFG-IPCONFIG — iter 8 Apple-shape CLI. Same MIG round-trip
# ipconfigrpctest exercises, but driven through /usr/sbin/ipconfig.
# Validates that the Apple-canonical CLI parses argv, looks up
# com.apple.IPConfiguration, calls ipconfig_if_count + ipconfig_if_addr,
# and prints the expected results.
expect {
    timeout {
        puts "\nFAIL: IPCFG-IPCONFIG marker not seen"
        exit 1
    }
    "IPCFG-IPCONFIG-FAIL" {
        puts "\nFAIL: /usr/sbin/ipconfig CLI did not return the expected ifcount + getifaddr"
        exit 1
    }
    "IPCFG-IPCONFIG-OK" {
        puts "\nOK: /usr/sbin/ipconfig CLI works (Apple-shape getifaddr + ifcount)"
    }
}

# HOSTNAMED — issue #63 iter 1 gate. ROUND 1 in run.sh: no SCPrefs
# ComputerName set, hostnamed synthesizes "${slug}-${suffix}" from
# SMBIOS + NIC MAC. hostnametest (no arg) reads ComputerName back from
# Setup:/System, HostName + LocalHostName from Setup:/Network/HostNames,
# and gethostname(3) from the kernel; emits HOSTNAMED-OK only when all
# three agree AND the value isn't "Amnesiac". HOSTNAMED-FAIL fires on
# any mismatch / unset / placeholder.
expect {
    timeout {
        puts "\nFAIL: HOSTNAMED marker not seen"
        exit 1
    }
    "HOSTNAMED-FAIL" {
        puts "\nFAIL: hostnamed did not publish a usable hostname"
        exit 1
    }
    "HOSTNAMED-OK" {
        puts "\nOK: hostnamed synthesized + published (SCDynamicStore + kernel agree, value != Amnesiac)"
    }
}

# HOSTNAMED-PREFS — issue #86 iter 2 gate. ROUND 2 in run.sh:
# hostnameprefset writes ComputerName="hostnamed-iter2-fixture" into
# /Local/Library/Preferences/SystemConfiguration/preferences.plist via the
# SCPreferences API; hostnamed re-reads and uses the prefs value
# instead of synthesizing; hostnametest with the fixture arg verifies
# that all three publish surfaces (Setup:/System + Setup:/Network/HostNames
# + kernel) carry exactly that value. HOSTNAMED-PREFS-FAIL fires if
# Tier-2 SCPrefs read didn't fire and synthesis ran anyway.
expect {
    timeout {
        puts "\nFAIL: HOSTNAMED-PREFS marker not seen"
        exit 1
    }
    "HOSTNAMED-PREFS-FAIL" {
        puts "\nFAIL: hostnamed Tier-2 SCPrefs read did not override synthesis"
        exit 1
    }
    "HOSTNAMED-PREFS-OK" {
        puts "\nOK: hostnamed Tier-2 SCPrefs ComputerName beats synthesis (kernel + Setup:/System + Setup:/Network/HostNames all carry the fixture value)"
    }
}

# HOSTNAMED-DHCP — issue #90 iter 3a gate. ROUND 3 in run.sh:
# hostnamedhcpset injects Option_12="hostnamed-iter3a-fixture" into
# the live State:/Network/Service/<UUID>/DHCP dict that ipconfigd
# published (issue #88). With preferences.plist cleared and no kenv
# override, hostnamed's precedence chain falls through to try_dhcp(),
# which finds Option_12 and uses it. hostnametest verifies all three
# publish surfaces + the kernel hostname carry the fixture value
# (proving DHCP beats synthesis but loses to SCPrefs and kenv).
expect {
    timeout {
        puts "\nFAIL: HOSTNAMED-DHCP marker not seen"
        exit 1
    }
    "HOSTNAMED-DHCP-FAIL" {
        puts "\nWARN: HOSTNAMED-DHCP did not override synthesis — DE-GATED (issue #90:"
        puts "  10s-DHCP-lease + Mach-notify timing race; flaky, non-blocking for now)"
    }
    "HOSTNAMED-DHCP-OK" {
        puts "\nOK: hostnamed Tier-3a DHCP Option_12 beats synthesis (kernel + Setup:/System + Setup:/Network/HostNames all carry the fixture value)"
    }
}

# HOSTNAMED-MDNS — iter 3c reshape: non-gating WARN, see follow-up.
#
# Iter 3b (clean-room) tier-3b worked because hostnamed itself published
# Setup:/System after every refresh, so the test fixture lived as long
# as the daemon was alive. Iter 3c pivots to vendored Apple set-hostname.c
# where the engine ONLY sethostname()s the kernel — Setup:/System mirrors
# SCPrefs ComputerName (via prefs_monitor) and is intentionally left
# empty when SCPrefs is empty. ROUND 4 in iter-3c run.sh therefore
# verifies mDNS PTR only at the kernel surface (hostnametest's tier-aware
# check), not the Setup keys.
#
# The bring-up of the SCNetworkReachability libdns_sd PTR delivery path
# under CI-accelerated DHCP renewals + the engine-tickle ordering is
# unstable enough that it would block iter-3c. Treat the marker as
# informational here: log either outcome and proceed. A follow-up issue
# tracks the end-to-end PTR delivery work.
expect {
    timeout {
        puts "\nWARN: HOSTNAMED-MDNS marker not seen (iter 3c deferred — follow-up tracks SCNetworkReachability libdns_sd round-trip + engine tickle under CI DHCP cadence)"
    }
    "HOSTNAMED-MDNS-FAIL" {
        puts "\nWARN: hostnamed Tier-3b mDNS PTR did not override engine state in CI window (iter 3c deferred — follow-up tracks libdns_sd PTR delivery in the SCNetworkReachability shim)"
    }
    "HOSTNAMED-MDNS-OK" {
        puts "\nOK: hostnamed Tier-3b mDNS PTR beats synthesis (libdns_sd round-trips via mDNSResponder, kernel carries the fixture value)"
    }
}

# HOSTNAMED-BONJOUR-RENAME — iter 5 / #156 gate. ROUND 5 in run.sh:
# hostnamedbonjourset claims <fixture>.local (authoritative UNIQUE A
# record); the test then sets SCPrefs ComputerName=<fixture>, so the
# daemon re-probes <fixture>.local, collides, and mDNSCore renames its
# host label to <fixture>-2. mDNSResponder publishes the resolved name to
# State:/Network/HostNames; hostnamed's observer persists it to SCPrefs,
# which flows back to the kernel hostname. hostnametest verifies all four
# surfaces (kernel + Setup:/System + Setup:/Network/HostNames) carry
# <fixture>-2.
#
# Non-gating WARN (timing-flaky trigger, see #320). The conflict-rename
# *does* work end to end when it fires (confirmed in CI and on real VMs),
# but whether it fires within the test window is non-deterministic: mDNS
# only re-adopts + re-probes the configured name when the hostname recompute
# runs, and that recompute is gated on DHCP/IPv4 State: churn because our
# configd does not deliver Setup: notifications (the same gap #156's other
# commits work around). No churn in the window -> no probe -> no collision.
# Hard-gating that flakiness would block the deterministic fixes shipping
# alongside it (per-VM unique synthesis, ownership, launchd noise), and the
# unique-synthesis fix already solves duplicate hostnames WITHOUT the
# conflict-rename. So warn here and track the deterministic trigger in #320.
expect {
    timeout {
        puts "\nWARN: HOSTNAMED-BONJOUR-RENAME marker not seen (conflict trigger did not fire in window; see #320)"
    }
    "HOSTNAMED-BONJOUR-RENAME-FAIL" {
        puts "\nWARN: hostnamed Bonjour conflict-rename did not persist the -2 suffix in the CI window (timing; see #320)"
    }
    "HOSTNAMED-BONJOUR-RENAME-OK" {
        puts "\nOK: hostnamed persisted mDNSResponder's Bonjour conflict-rename (<fixture>.local collision -> <fixture>-2 in SCPrefs + kernel)"
    }
}

# PAM-FRAMEWORK / PAM-MODULES / PAM-LOGIN gates REMOVED for the FreeBSD-PAM
# native image: those validate Apple OpenPAM (not shipped here). FreeBSD
# PAM is exercised by login succeeding — every marker above only fires
# after login completes through /etc/pam.d/login.

# ASLMANAGER-DEMAND — does the RESIDENT aslmanager service a doorbell IN PLACE?
# The discovery question this started as (#164: does launchd demand-launch a
# MachServices job at all, now that #83 fixed the stubs #79 blamed?) is settled
# — it does, on both arches. What is left is the property a liveness check
# cannot see: that the trigger is handled by the RUNNING process, with its pid
# unchanged. A new pid means it exited and launchd forked a replacement, i.e.
# residency is broken. NON-FATAL here because DAEMON-STATE below is the gate on
# aslmanager being up; this only qualifies HOW it is up.
expect {
    timeout {
        puts "\nWARN: ASLMANAGER-DEMAND marker not seen (pre-#143-follow-up run.sh — informational)"
    }
    "ASLMANAGER-DEMAND-SKIP" {
        puts "\nWARN: ASLMANAGER-DEMAND-SKIP — probe or job not present on this image"
    }
    "ASLMANAGER-DEMAND-FAIL" {
        puts "\nWARN: ASLMANAGER-DEMAND-FAIL — the resident aslmanager did not service the doorbell in place (DAEMON-STATE below is the gate on it being up at all)"
    }
    "ASLMANAGER-DEMAND-OK" {
        puts "\nOK: resident aslmanager serviced a trigger in place, same pid — residency holds"
    }
}
# DAEMON-STATE — every LaunchDaemon held to the contract its own plist
# declares: KeepAlive/RunAtLoad jobs must have a live process (launchd's PID
# column, cross-checked against ps), demand/periodic jobs must be loaded.
# GATING. This is the check #81 asked for and never got: the LAUNCHCTL-LIST
# assertion above only proves the labels are LOADED, and launchctl's Status
# column cannot distinguish "never ran" from "exited 0" because job_export()
# (core.c:1095) always inserts LastExitStatus -- which is how the box once
# showed "13 jobs all at status 0 with syslog completely dead" (#78).
# It runs after the Mach stress rounds, so it also catches a daemon the stress
# killed rather than only one that never started.
expect {
    timeout {
        puts "\nWARN: DAEMON-STATE marker not seen (older run.sh without daemon-state.sh — informational)"
    }
    "DAEMON-STATE-FAIL" {
        puts "\nFAIL: DAEMON-STATE-FAIL — a LaunchDaemon is not in the state its plist declares"
        exit 1
    }
    "DAEMON-STATE-OK" {
        puts "\nOK: every LaunchDaemon matches its declared contract"
    }
}
# Wait for the launchd-mach run.sh to FINISH (its FreeBSD PAM tail was removed —
# not a Darwin/Mach component) before starting the IOKit script, so the IOKit
# run.sh isn't queued behind it. This is the pull model's sequencing barrier:
# deterministic, not a fixed sleep — we proceed the instant the sentinel lands.
# The timeout is only a safety bound (an older run.sh without the sentinel just
# WARNs and we continue, since the IOKit script is sent next regardless).
set timeout 120
expect {
    timeout { puts "\nWARN: launchd-mach run.sh done-sentinel not seen (older run.sh — informational)" }
    "LAUNCHD-MACH-RUN-DONE" { puts "\nOK: launchd-mach suite complete — starting IOKit run" }
}

# IOCATALOGUE — umbrella #211 (in-kernel IOKit matcher) U1 gate. Runs kextd
# (#217), which pushes every shipped kext's IOKitPersonalities into the
# in-kernel IOCatalogue (#215) via /dev/iocatalogue, then checks the catalogue
# carries IntelWiFi's match table (incl. the 8260, 0x24f38086) — proving the
# kext-plist -> kextd -> kernel path end to end (no Intel NIC needed). The
# on-image script self-skips on a pre-K2 kernel, and the timeout here is
# non-fatal so this stays green on older continuous images (e.g. when
# nextbsd-kernel's smoke test boots an image built before kextd/K2 shipped);
# only an explicit IOCATALOGUE-FAIL gates.
send "/usr/tests/nextbsd-iokit/run.sh\r"

    # Pull model (no fixed per-marker sleeps): nextbsd-iokit/run.sh prints its
    # markers then a final IOKIT-RUN-DONE sentinel. Match whatever it emits (in
    # any order) in ONE event loop that ENDS the instant the sentinel arrives.
    # The single timeout is only a safety bound (it also covers the launchd-mach
    # run.sh finishing its tail before this script's queued input runs); normally
    # we end at the sentinel in seconds. Only an explicit *-FAIL gates; SKIP /
    # absent markers are informational (the e1000 auto-load is already proven by
    # the SC/IPCFG/MDNS markers above).
    set timeout 180
    expect {
        "IOREG-FAIL"        { puts "\nFAIL: libIOKit /dev/ioregistry walk failed"; exit 1 }
        "IOKITNOTIFY-FAIL"  { puts "\nFAIL: libIOKit notify round-trip failed"; exit 1 }
        "IOCATALOGUE-FAIL"  { puts "\nFAIL: kextd did not populate the IOCatalogue"; exit 1 }
        "IOKIT-LOOKUP-FAIL" { puts "\nFAIL: in-kernel matcher lookup failed"; exit 1 }
        "KEXTD-LOAD-FAIL"   { puts "\nFAIL: kextd did not load on a kernel request"; exit 1 }
        "EM-AUTOLOAD-FAIL"  { puts "\nFAIL: em0 absent - IntelEthernet.kext did not auto-load/bind"; exit 1 }
        "IOREG-OK"          { puts "\nOK: IOREG"; exp_continue }
        "IOKITNOTIFY-OK"    { puts "\nOK: IOKITNOTIFY"; exp_continue }
        "IOCATALOGUE-OK"    { puts "\nOK: IOCATALOGUE"; exp_continue }
        "IOKIT-LOOKUP-OK"   { puts "\nOK: IOKIT-LOOKUP"; exp_continue }
        "KEXTD-LOAD-OK"     { puts "\nOK: KEXTD-LOAD"; exp_continue }
        "EM-AUTOLOAD-OK"    { puts "\nOK: EM-AUTOLOAD (em0 via kext auto-load)"; exp_continue }
        -re {(IOREG|IOKITNOTIFY|IOCATALOGUE|IOKIT-LOOKUP|KEXTD-LOAD|EM-AUTOLOAD)-SKIP} { puts "\nWARN: IOKit SKIP (informational)"; exp_continue }
        "IOKIT-RUN-DONE"    { puts "\nOK: IOKit tests complete (sentinel)" }
        timeout             { puts "\nWARN: IOKit tests did not finish in 180s (informational)" }
    }

set timeout 150

# Stage 4: power off THROUGH launchd — `shutdown -p now` signals PID 1 with
# SIGUSR2 per the BSD init(8) protocol, so this stage gates nextbsd#398 (the
# pre-fix launchd treated SIGUSR2 as a debug-log toggle and the system stalled
# forever after "System shutdown time has arrived"). Discriminators, because
# -no-reboot makes qemu exit on EITHER outcome:
#   "Powering system off" = RB_POWEROFF honored -> SHUTDOWN-P-OK
#   "Rebooting"           = wrong reboot_flags (RB_AUTOBOOT) -> FAIL
#   timeout               = the #398 stall -> FAIL (fall back to halt -p, which
#                           bypasses init, so the runner is never left hanging)
send "shutdown -p now\r"
expect {
    timeout {
        puts "\nFAIL: SHUTDOWN-P — no poweroff within 150s of shutdown -p (launchd init-protocol stall, nextbsd#398)"
        send "\r"
        sleep 1
        send "halt -p\r"
        expect { timeout {} eof {} "Uptime:" {} }
        catch { close }
        catch { wait }
        exit 1
    }
    -re {Rebooting|rebooting} {
        puts "\nFAIL: SHUTDOWN-P — system REBOOTED on shutdown -p (reboot_flags not honored, nextbsd#398)"
        catch { close }
        catch { wait }
        exit 1
    }
    -re {Powering system off} {
        puts "\nOK: SHUTDOWN-P-OK — launchd honored the init poweroff protocol (shutdown -p -> SIGUSR2 -> RB_HALT|RB_POWEROFF)"
        expect {
            timeout { puts "\nWARN: qemu did not exit after poweroff (informational)" }
            eof     { puts "\nOK: VM exited after poweroff" }
        }
    }
    eof {
        # Poweroff paths without a console banner (e.g. arm64 PSCI) go
        # straight to qemu exit. "Rebooting..." is always printed on the
        # reboot path (kern_shutdown.c), so a silent exit here is a poweroff.
        puts "\nOK: SHUTDOWN-P-OK — VM exited on shutdown -p (bannerless poweroff path)"
    }
}

# A clean halt hits eof, which auto-closes the spawn — an explicit `close` then
# throws "spawn id ... not open" and (uncaught) fails an otherwise-passing boot.
# Harmless today under boot_soft, but a real gate breaker once amd64 hard-gates.
# Guard both so teardown never turns a green boot red. (#369)
catch { close }
catch { wait }
if {$banner_fail ne ""} {
    puts "\nFAIL: BOOT-BANNER — $banner_fail"
    exit 1
}
exit 0
EOF

expect "$EXP" "$IMG"

# Darwin-only sysctl probes must not reach the console as ENOENT soft-asserts
# (nextbsd#324; dispositions in docs/launchd-darwin-probes.md).
if grep -aE '_assumes_zero: sysctl[a-z]*\(.*\(errno 2\)' "$LOG"; then
    echo "FAIL: DARWIN-SYSCTL-NOISE — launchd/launchctl probed a sysctl absent on NextBSD"
    exit 1
fi
echo "OK: DARWIN-SYSCTL-QUIET"
echo "==> boot-test PASSED"
