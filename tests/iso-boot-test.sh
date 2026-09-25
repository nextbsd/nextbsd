#!/bin/sh
# iso-boot-test.sh — boot the LIVE ISO in qemu (UEFI via OVMF, -cdrom) and
# verify the on-demand live-root assembly:
#   loader preloads the mfsroot -> /rescue/init mounts the cd9660, vnode-mds
#   rootfs.uzip (geom_uzip), unions a tmpfs over it, `sysctl vfs.pivot` adopts
#   the union as / -> exec launchd -> getty login prompt.
#
# Success = we see the pivot marker ("vfs.pivot: / is now unionfs") AND the
# login prompt. The full serial log is always dumped for diagnosis — this is
# the feedback loop for iterating the live-root pipeline.
#
# Arch-agnostic: the qemu shape (binary, machine, UEFI firmware, NIC, CD
# attachment, accel) comes from tests/qemu-arch.sh, which takes ARCH from the
# environment or infers it from the NextBSD-<arch>-<date> ISO name.

set -eu

ISO=${1:?usage: [ARCH=amd64|arm64] iso-boot-test.sh path/to/NextBSD-*.iso[.zip]}
[ -f "$ISO" ] || { echo "ERROR: $ISO not found"; exit 1; }
# The as-published name (NextBSD-<arch>-<date>.iso.zip) is what carries the arch;
# $ISO is rewritten to the extracted scratch copy below.
ARTIFACT=$ISO

mkdir -p tests
LOG=tests/iso-boot.log
EXP=tests/iso-boot.exp
: > "$LOG"

case "$ISO" in
*.zip)
    RAW=tests/live.iso
    echo "==> extracting $ISO -> $RAW"
    MEMBER=$(unzip -Z1 "$ISO" | grep -E '\.iso$' | head -1)
    [ -n "$MEMBER" ] || { echo "FAIL: no .iso member in $ISO" >&2; exit 1; }
    unzip -p "$ISO" "$MEMBER" > "$RAW"
    ISO=$RAW
    ;;
esac

. "$(dirname "$0")/qemu-arch.sh"
qemu_arch_setup "$ISO" "$ARTIFACT"

echo "==> iso boot test: $ISO (arch=$ARCH)"
ls -lh "$ISO"

cat > "$EXP" <<'EOF'
set timeout 600
log_file -a tests/iso-boot.log
log_user 1

set accel_flags [split $env(ACCEL_FLAGS) " "]
set net_args    [split $env(NET_ARGS) " "]
set video_args  [split $env(VIDEO_ARGS) " "]
set cd_args     [split $env(CD_ARGS) " "]

eval spawn $env(QEMU) \
    -m 4G \
    -machine $env(MACHINE) \
    -bios $env(FW) \
    $accel_flags \
    $cd_args \
    $net_args \
    $video_args \
    -display none -serial stdio \
    -no-reboot

source tests/loader.exp.inc

# Stage 0: loader autoboot -> OK prompt; enable serial console.
loader_prompt 90
loader_set "set console=comconsole"
loader_set "set boot_serial=YES"
loader_set "set comconsole_speed=115200"
loader_set "set boot_multicons=YES"
# Keep base virtio_gpu(4) attached. On qemu virt, vtgpu0 IS the live console
# this harness reads and types at, and VirtIOGraphics' takeover detaches it --
# the next console write then faults (esr 0x96000047). That is nextbsd-kernel#170,
# and it predates this harness: it was only ever hidden because the old test had
# a passwordless root shell and powered off about a second after the kext load
# was requested, before the ~7s graphics chain finished.
#
# Set here at the loader rather than in the image, because the default has to
# stay on: under UTM and Virtualization.framework base vtgpu displays nothing,
# so video is blind from the bootloader until this kext loads, and shipping it
# off would leave those machines blind for good. CI is the environment that
# cannot tolerate the takeover, so CI is what opts out.
#
# This does mean the arm64 lanes do not exercise the DRM handoff. #170 option 3
# -- a vt_simplefb-style placeholder for arm64 to hand off from -- is what would
# let them, and until then there is nothing here to test that does not panic.
loader_set "set hw.virtio_gpu_drm.takeover=0"
# boot VERBOSE: the shipped image sets boot_mutemsgs="YES" (nextbsd#363) which
# mutes kernel console output — including the "vfs.pivot: / is now unionfs"
# marker this harness sequences on. RB_VERBOSE (boot -v) bypasses the mute in
# the kernel, so CI sees all markers while shipped images stay quiet.
loader_boot "boot -v"

# Stage 1: the live-root assembly markers from /rescue/init + vfs.pivot.
set saw_init 0
set saw_pivot 0
expect {
    timeout { puts "\nFAIL: live-root assembly markers not seen within 8 minutes"; exit 1 }
    -re "init\\] NextBSD live root" { set saw_init 1; exp_continue }
    "vfs.pivot: / is now unionfs" {
        set saw_pivot 1
        puts "\nOK: PIVOT-OK — / is now the writable unionfs (on-demand uzip + tmpfs)"
    }
    -re "panic|Fatal trap|vfs.pivot:.*not|mount_unionfs:.*fail|mdconfig:.*" {
        puts "\nWARN: assembly diagnostic: $expect_out(0,string)"
        exp_continue
    }
    "login:" {
        if {$saw_pivot == 0} { puts "\nWARN: reached login WITHOUT a pivot marker (booted mfsroot or fell through?)" }
    }
}

# Stage 2 and 3, in one block: launchd PID 1 came up on the union and we have
# a shell.
#
# The third copy of the same fix. nextbsd-overlays f9dcd5b (#278) disabled root
# the way Darwin does -- its password field went from empty to "*" -- so
# sending "root" with an empty password is rejected. admin is the way in:
# nss_directory_services gives an account carrying noPassword an EMPTY passwd
# field for a privileged caller (dsdb_pack_passwd), and login is privileged.
#
# Waiting for "login:" and then deciding was two blocks, and that was the
# second bug: where automatic login works there is no prompt, so the first
# block waited out eight minutes and the second never ran. One block does both.
expect {
    timeout { puts "\nFAIL: neither a login prompt nor an automatic login in 8 minutes"; exit 1 }
    -re "panic|Fatal trap" { puts "\nFAIL: kernel panic during boot"; exit 1 }
    -re {login on console as admin} {
        puts "\nOK: LOGIN-OK — launchd reached getty on the live union, which logged admin in"
    }
    "login:" {
        puts "\nOK: LOGIN-OK — launchd reached getty on the live union"
        send "admin\r"
        expect {
            timeout { puts "\nFAIL: no response after sending admin"; exit 1 }
            "Login incorrect" { puts "\nFAIL: admin login rejected"; exit 1 }
            "Password:" { send "\r"; exp_continue }
            -re {[#%$] $} { puts "\nOK: logged in as admin" }
        }
    }
}
send "\r"
send "echo NB-SHELL-READY\r"
expect {
    timeout { puts "\nFAIL: no shell after login"; exit 1 }
    "NB-SHELL-READY" { puts "\nOK: shell is responding" }
}
# Same sentinel as img-boot-test.sh, and for the same reason: a bare prompt match
# here raced the previous command's leftover prompt.
send "df / ; mount | grep ' / '; echo MOUNT'-'REPORTED\r"
set saw_union 0
expect {
    timeout { puts "\nFAIL: df/mount printed nothing before the sentinel"; exit 1 }
    -re {panic|Fatal trap|Fatal data abort} {
        puts "\nFAIL: kernel panic after login, before df/mount reported"
        exit 1
    }
    -re "unionfs" {
        set saw_union 1
        puts "\nOK: ROOT-IS-UNION — / is a unionfs mount"
        exp_continue
    }
    "MOUNT-REPORTED" { }
}
if {$saw_union == 0} {
    puts "\nFAIL: ROOT-IS-UNION — mount printed no unionfs line for /"
    exit 1
}
send "sudo halt -p\r"
expect { timeout { } eof { } }
puts "\nISO-BOOT-DONE"
EOF

set +e
expect -f "$EXP"
rc=$?
set -e

echo "==> verdict"
# The PIVOT-OK/LOGIN-OK `puts` lines go to expect's stdout (captured by CI), not
# the spawn transcript ($LOG). Assert against the markers that ARE in the serial
# transcript: the kernel's vfs.pivot adoption + the getty login prompt (launchd
# PID 1 reached getty on the union).
if ! { grep -q "vfs.pivot: / is now unionfs" "$LOG" && grep -qE "login:|login on console as admin" "$LOG"; }; then
    echo "FAIL: $ARCH live ISO did not complete the pivot+login sequence (rc=$rc)"
    exit 1
fi
# launchctl's boot-time `mount -vat nonfs` must never touch / (#467). The
# overlay ships no fstab (nextbsd-overlays#5), so the step is skipped; the
# fwexec half also catches any other failed mount -a.
if grep -aE 'Cannot union mount root filesystem|fwexec\(mount_tool' "$LOG"; then
    echo "FAIL: FSTAB-ROOT-REMOUNT -- launchctl mount -a failed or tried to remount / (#467)"
    exit 1
fi
echo "OK: FSTAB-ROOT-QUIET"
echo "PASS: $ARCH live ISO booted — vfs.pivot to writable union + launchd reached the login prompt"
exit 0
