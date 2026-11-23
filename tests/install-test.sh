#!/bin/sh
# install-test.sh — install the live ISO onto a blank disk, then boot the
# installed disk and check it (nextbsd/nextbsd#686, Phase 1 of E31,
# nextbsd/nextbsd#600).
#
#   1. Boot the live ISO in QEMU with a second, blank virtio disk (vtbd0).
#      Log in through the shared harness, ask the installer's probe
#      (probe-disks.sh) whether vtbd0 is offered, and run the engine on it
#      the way Installer.app's helper does: do-install.sh -d vtbd0, stdin on
#      /dev/null, PATH=/sbin:/usr/sbin:/bin:/usr/bin
#      (tests/install-run.exp.inc). Power off through launchd.
#   2. Boot the installed disk alone (UEFI on both arches), log in through
#      the harness and check the result (tests/install-check.exp.inc): the
#      file system the engine made is read off the booted system, so the same
#      test checks today's UFS installs and, once the ISO's engine installs
#      ZFS (nextbsd/nextbsd-userland#658), the full ZFS layout, strictly.
#      Desktop images (NB_DESKTOP=1) also run tests/desktop-check.exp.inc:
#      WindowServer and loginwindow start on the installed system.
#
# The shared nextbsd-ci harness (pinned tag, as in img-boot-test.sh) has no
# way to attach a second disk or to run commands after the login, so this
# script assembles the expect script from the harness's own pieces (the
# same order boot-test.sh uses: patterns, spawn, loader, contract, login,
# suite-tail, teardown) with this repo's steps between login and suite-tail,
# and adds the target disk to the qemu command line the harness's
# qemu-arch.sh describes. No harness change, no new harness tag.
#
# usage: [ARCH=amd64|arm64] [NB_DESKTOP=1] install-test.sh NextBSD-*.iso[.zip]
# exit:  0 pass, 1 an assertion failed, 2 a hang (the harness's classes)
set -eu
ISO=${1:?usage: [ARCH=amd64|arm64] [NB_DESKTOP=1] install-test.sh path/to/NextBSD-*.iso[.zip]}
[ -f "$ISO" ] || { echo "ERROR: $ISO not found"; exit 1; }
ARTIFACT=$ISO

mkdir -p tests
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

[ -d nextbsd-ci/.git ] || git clone --depth 1 --branch v0.3.5 \
  https://github.com/nextbsd/nextbsd-ci.git nextbsd-ci
H=nextbsd-ci/harness

# The target: a blank, sparse 32 GB raw disk. 32 GB, not less: probe-disks.sh
# offers only disks of 30,000,000,000 bytes and up (its MIN_BYTES), the same
# rule Installer.app shows. Truncated to nothing first, so a re-run on the
# same runner starts from a blank disk too.
TARGET=tests/install-target.img
: > "$TARGET"
truncate -s 32G "$TARGET"
# cache=unsafe: ignore the guest's flushes (ZFS issues many). The file is a
# throwaway on the runner; qemu still writes it all out when it exits.
TARGET_ARGS="-drive file=$TARGET,format=raw,if=virtio,cache=unsafe"

# login.exp.inc's second stage waits the whole global budget (480 s) for a
# shell prompt that the first stage has, on an admin login, already
# consumed, then probes the shell and goes on (8 minutes of every Desktop
# boot in iso-test/img-test). Cap that wait at 30 s here; the probe that
# follows it is unchanged. If the harness's text ever moves, awk inserts
# nothing and the boot is only slower.
awk '/^# Stage 2: the shell prompt/ { print "if {$timeout > 30} { set timeout 30 }" } { print }' \
  "$H/login.exp.inc" > tests/install-login.exp.inc

# The harness's environment, as boot-test.sh exports it. The banner verdict
# stays a warning, as in every nextbsd boot (no overlay says otherwise).
export NB_BOOT_TRACE=0 NB_BANNER_POLICY=warn NB_QEMU_ARGV="" NB_DEBUG=""
export NB_TIMEOUT_GLOBAL=${NB_TIMEOUT_GLOBAL:-480}

# qemu_arch_setup exports QEMU MACHINE FW ACCEL_FLAGS NET_ARGS DISK_ARGS
# CD_ARGS VIDEO_ARGS for one medium.
# shellcheck disable=SC1091
. "$H/qemu-arch.sh"

# assemble <exp> <step>... — the harness's boot, then the given steps after the
# login, then its end-state hold and poweroff.
assemble() {
  out=$1; shift
  {
    cat "$H/patterns.tcl" "$H/spawn.exp.inc" "$H/loader.exp.inc" \
        "$H/contract.exp.inc" tests/install-login.exp.inc
    for s in "$@"; do cat "$s"; done
    cat "$H/suite-tail.exp.inc" "$H/teardown.exp.inc"
  } > "$out"
}

summary() {
  [ -n "${GITHUB_STEP_SUMMARY:-}" ] && printf '%s\n' "$@" >> "$GITHUB_STEP_SUMMARY"
  return 0
}
VARIANT=Server
[ -n "${NB_DESKTOP:-}" ] && VARIANT=Desktop
summary "### install-test: $VARIANT ${ARCH:-amd64}" ""

# ---- 1. the live ISO, with the blank disk: install -------------------------
qemu_arch_setup "$ISO" "$ARTIFACT"
export NB_MEDIA=cd CD_ARGS="$CD_ARGS $TARGET_ARGS"
export NB_LOG=tests/install-live.log
: > "$NB_LOG"
assemble tests/install-live.exp tests/install-run.exp.inc
echo "==> install-test 1/2: live ISO $ISO + blank $TARGET (arch=$ARCH)"
t0=$(date +%s)
set +e
expect -f tests/install-live.exp
rc=$?
set -e
t1=$(date +%s)
echo "==> live ISO + install: rc=$rc, $((t1 - t0)) s"
summary "- live ISO boot + install + poweroff: rc=$rc, $((t1 - t0)) s"
grep -a -E '^(OK|FAIL|WARN|INFO): INSTALL' "$NB_LOG" | tr -d '\r' | sed 's/^/  - /' >> "${GITHUB_STEP_SUMMARY:-/dev/null}" || true
if [ "$rc" -ne 0 ]; then
  echo "==> install-test FAILED in the install boot (rc=$rc); serial transcript: $NB_LOG"
  exit "$rc"
fi
ls -ls "$TARGET"

# ---- 2. the installed disk alone: boot and check --------------------------
qemu_arch_setup "$TARGET" "$ARTIFACT"
export NB_MEDIA=disk DISK_ARGS="$TARGET_ARGS"
export NB_LOG=tests/install-disk.log
: > "$NB_LOG"
if [ -n "${NB_DESKTOP:-}" ]; then
  assemble tests/install-disk.exp tests/install-check.exp.inc tests/desktop-check.exp.inc
else
  assemble tests/install-disk.exp tests/install-check.exp.inc
fi
echo "==> install-test 2/2: boot the installed disk $TARGET alone (arch=$ARCH)"
set +e
expect -f tests/install-disk.exp
rc=$?
set -e
t2=$(date +%s)
echo "==> installed-disk boot + checks: rc=$rc, $((t2 - t1)) s"
summary "- installed disk boot + checks + poweroff: rc=$rc, $((t2 - t1)) s"
grep -a -E '^(OK|FAIL|WARN|INFO): (INSTALLED|DESKTOP)' "$NB_LOG" | tr -d '\r' | sed 's/^/  - /' >> "${GITHUB_STEP_SUMMARY:-/dev/null}" || true
if [ "$rc" -ne 0 ]; then
  echo "==> install-test FAILED on the installed disk (rc=$rc); serial transcript: $NB_LOG"
  exit "$rc"
fi
echo "==> install-test PASSED ($VARIANT $ARCH, $((t2 - t0)) s)"
