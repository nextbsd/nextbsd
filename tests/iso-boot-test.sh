#!/bin/sh
# iso-boot-test.sh — thin entry point onto the shared nextbsd-ci boot harness
# (T4). Boots the freshly-built NextBSD-*.iso LIVE image (from the CD device:
# NB_MEDIA=cd) to the getty login prompt, confirms a clean end-state, powers
# off. Login-only (NB_LOGIN_ONLY=1): the live-ISO gate is "did this kernel
# boot," not "do the userland markers pass."
#
# The loader un-mute dance, the arch-aware qemu argv, login detection and
# teardown all come from the shared harness now, not the in-repo loader.exp.inc
# / qemu-arch.sh (deleted).
set -eu
ISO=${1:?usage: [ARCH=amd64|arm64] iso-boot-test.sh path/to/NextBSD-*.iso[.zip]}
[ -f "$ISO" ] || { echo "ERROR: $ISO not found"; exit 1; }

mkdir -p tests
# A zipped ISO is extracted to a raw live.iso first (the harness takes a raw image).
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

# Fetch the shared harness at the pinned lockstep tag (absent = first run).
[ -d nextbsd-ci/.git ] || git clone --depth 1 --branch v0.3.5 \
  https://github.com/nextbsd/nextbsd-ci.git nextbsd-ci

# NB_DESKTOP=1 (Desktop images, nextbsd/nextbsd#678): also check, on the
# booted system, that launchd loaded WindowServer and loginwindow, that the
# WindowServer is ready on QEMU's virtio-gpu, and that loginwindow connected. The harness has no hook for extra commands after login,
# so tests/desktop-check.exp.inc goes in front of its suite-tail.exp.inc,
# which login-only boots reach straight after the login (once per checkout).
if [ -n "${NB_DESKTOP:-}" ]; then
  T=nextbsd-ci/harness/suite-tail.exp.inc
  if ! grep -q 'desktop-check.exp.inc' "$T"; then
    cat tests/desktop-check.exp.inc "$T" > "$T.new"
    mv "$T.new" "$T"
  fi
  echo "==> desktop image: tests/desktop-check.exp.inc runs after the login"
fi

echo "==> iso boot test: $ISO (arch=${ARCH:-amd64}) — shared harness, login-only, cd"
ls -lh "$ISO"

# The live ISO has no virtio disk; the harness's NB_MEDIA=cd attaches it as the
# virtio-scsi CD the virt machine boots from.
ARCH=${ARCH:-amd64} NB_MEDIA=cd NB_LOGIN_ONLY=1 NB_BOOT_VERBOSE=1 NB_LOG=boot-test.log \
  sh nextbsd-ci/harness/boot-test.sh "$ISO"
