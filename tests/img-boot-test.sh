#!/bin/sh
# img-boot-test.sh — LIGHT boot smoke test for the installed disk image.
# Boots the raw .img in qemu (UEFI via OVMF, virtio disk) and verifies the
# ASSEMBLED image reaches a usable system: loader -> kernel -> launchd PID 1
# -> getty login -> root shell on a UFS root.
#
# It deliberately does NOT run the deep functional suite
# (/usr/tests/freebsd-launchd-mach/run.sh — the LEAF command-suite + launchd /
# CoreFoundation / launchctl + syslog + network-daemon gates). Those exercise
# the Darwin userland and belong in nextbsd-userland's ci-image-boot, closest
# to the source; re-running them here (on the same pkg-built rootfs the ISO
# shares) was redundant and the syslog/network round-trips race under qemu. The
# ISO builder only needs to prove the two boot PATHS reach a shell: this
# direct-UFS disk image and the live ISO (iso-boot-test.sh).
#
# Success = "login:" prompt reached (launchd PID 1 got getty up).
#
# Arch-agnostic: the qemu shape (binary, machine, UEFI firmware, NIC, accel)
# comes from tests/qemu-arch.sh, which takes ARCH from the environment or infers
# it from the NextBSD-<arch>-<date> image name. amd64 boots on q35+OVMF, arm64 on
# virt+AAVMF.

set -eu

IMG=${1:?usage: [ARCH=amd64|arm64] img-boot-test.sh path/to/NextBSD-*.img[.zip]}
[ -f "$IMG" ] || { echo "ERROR: $IMG not found"; exit 1; }
# The as-published name (NextBSD-<arch>-<date>.img.zip) is what carries the arch;
# $IMG is rewritten to the extracted scratch copy below.
ARTIFACT=$IMG

mkdir -p tests
LOG=tests/img-boot.log
EXP=tests/img-boot.exp
: > "$LOG"

case "$IMG" in
*.zip)
    RAW=tests/disk.img
    echo "==> extracting $IMG -> $RAW"
    MEMBER=$(unzip -Z1 "$IMG" | grep -E '\.img$' | head -1)
    [ -n "$MEMBER" ] || { echo "FAIL: no .img member in $IMG" >&2; exit 1; }
    unzip -p "$IMG" "$MEMBER" > "$RAW"
    IMG=$RAW
    ;;
esac

. "$(dirname "$0")/qemu-arch.sh"
qemu_arch_setup "$IMG" "$ARTIFACT"

echo "==> img boot test: $IMG (arch=$ARCH)"
ls -lh "$IMG"

cat > "$EXP" <<'EOF'
set timeout 600
log_file -a tests/img-boot.log
log_user 1

set accel_flags [split $env(ACCEL_FLAGS) " "]
set net_args    [split $env(NET_ARGS) " "]
set video_args  [split $env(VIDEO_ARGS) " "]
set disk_args   [split $env(DISK_ARGS) " "]

eval spawn $env(QEMU) \
    -m 4G \
    -machine $env(MACHINE) \
    -bios $env(FW) \
    $accel_flags \
    $disk_args \
    $net_args \
    $video_args \
    -display none -serial stdio \
    -no-reboot

source tests/loader.exp.inc

# Stage 0: drop into the loader OK prompt and enable the serial console for
# the kernel (/boot/loader.conf leaves console unset for a clean console on
# real hardware).
loader_prompt 60
loader_set "set console=comconsole"
loader_set "set boot_serial=YES"
loader_set "set comconsole_speed=115200"
loader_set "set boot_multicons=YES"
# boot VERBOSE: the shipped image sets boot_mutemsgs="YES" (nextbsd#363), which
# mutes kernel console output. RB_VERBOSE (boot -v) bypasses the mute so CI sees
# full boot output; shipped images (booted normally) stay quiet.
loader_boot "boot -v"

# Stage 2: get a shell as admin and confirm a UFS root (direct disk boot, no
# live union pivot).
#
# This used to send "root" with an empty password. nextbsd-overlays f9dcd5b
# (#278) disabled root the way Darwin does -- its password field went from
# empty to "*" -- so login rejects every password including the empty one,
# and this stage failed with "Login incorrect" on the first image built
# afterwards. The image was right; the test described an older one.
#
# admin is the way in: nss_directory_services gives an account carrying
# noPassword an EMPTY passwd field for a privileged caller
# (dsdb_pack_passwd), and login is privileged. Where automatic login is
# configured there is no prompt at all, so both paths are handled.
# Boot completes and we get a shell, in one block. This was two -- a wait for
# "login:" and then a decision that handled either a prompt or an automatic
# login -- and on an image where automatic login works there is no prompt, so
# the first block waited out eight minutes and the second never ran.
expect {
    timeout { puts "\nFAIL: neither a login prompt nor an automatic login in 8 minutes"; exit 1 }
    -re "panic|Fatal trap" { puts "\nFAIL: kernel panic during boot"; exit 1 }
    -re {login on console as admin} {
        puts "\nOK: LOGIN-OK — launchd reached getty, which logged admin in"
    }
    "login:" {
        send "admin\r"
        expect {
            timeout { puts "\nFAIL: no response after sending admin"; exit 1 }
            "Login incorrect" { puts "\nFAIL: admin login rejected"; exit 1 }
            "Password:" { send "\r"; exp_continue }
            -re {[#%$] $} { puts "\nOK: LOGIN-OK — logged in as admin" }
        }
    }
}
send "\r"
send "echo NB-SHELL-READY\r"
expect {
    timeout { puts "\nFAIL: no shell after login"; exit 1 }
    "NB-SHELL-READY" { puts "\nOK: shell is responding" }
}
# Bracket the output with a sentinel and read until it arrives. Matching a bare
# shell prompt here was a race: the previous command's prompt is still sitting in
# expect's buffer, so on a slow arm64 guest the prompt matched before mount had
# printed anything, the "on / (...)" line never reached the transcript, and the
# noatime check below failed on an image that was in fact mounted correctly. The
# quotes keep the echoed command line from matching the sentinel.
send "mount | grep ' / '; echo MOUNT'-'REPORTED\r"
set saw_root 0
expect {
    timeout { puts "\nFAIL: mount printed nothing before the sentinel"; exit 1 }
    -re {panic|Fatal trap|Fatal data abort} {
        puts "\nFAIL: kernel panic after login, before mount reported"
        exit 1
    }
    -re { on / \((ufs[^)]*)\)} {
        set saw_root 1
        puts "\nOK: ROOT-IS-UFS — / is a ufs mount ($expect_out(1,string))"
        exp_continue
    }
    "MOUNT-REPORTED" { }
}
if {$saw_root == 0} {
    puts "\nFAIL: ROOT-IS-UFS — mount printed no ' on / (ufs...)' line"
    exit 1
}
# admin is not root, and halt is root's to run.
send "sudo halt -p\r"
expect { timeout { } eof { } }
puts "\nIMG-BOOT-DONE"
EOF

set +e
expect -f "$EXP"
rc=$?
set -e

echo "==> verdict"
# The OK `puts` lines go to expect's stdout, not the serial transcript ($LOG).
# Assert against the getty login prompt in the transcript (launchd PID 1 reached
# getty on the installed image).
# Either a login prompt or an automatic login proves launchd got getty up.
# Asserting only on "login:" would fail an image that logs admin in
# automatically, which is the configured behaviour on a seeded image.
if ! grep -qE "login:|login on console as admin" "$LOG"; then
    echo "FAIL: $ARCH disk image did not reach a login (rc=$rc)"
    exit 1
fi
# / must carry noatime from launchd's own remount (nextbsd-userland#185), not
# from an fstab root line: the overlay stops shipping fstab
# (nextbsd-overlays#5). Matched on the serial transcript, where the whole
# "on / (...)" line is intact.
if ! grep -aqE ' on / \(ufs, local, noatime' "$LOG"; then
    echo "FAIL: ROOT-NOATIME -- / is mounted without noatime"
    grep -aE ' on / \(' "$LOG" | tail -2
    exit 1
fi
echo "OK: ROOT-NOATIME"
# launchctl's boot-time `mount -vat nonfs` must never touch / (#467). The
# overlay ships no fstab (nextbsd-overlays#5), so the step is skipped; the
# fwexec half also catches any other failed mount -a.
if grep -aE 'Cannot union mount root filesystem|fwexec\(mount_tool' "$LOG"; then
    echo "FAIL: FSTAB-ROOT-REMOUNT -- launchctl mount -a failed or tried to remount / (#467)"
    exit 1
fi
echo "OK: FSTAB-ROOT-QUIET"
echo "PASS: $ARCH disk image booted — launchd reached the login prompt on a UFS root"
exit 0
