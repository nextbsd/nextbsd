#!/bin/sh
# img-boot-test.sh — thin entry point onto the shared nextbsd-ci boot harness
# (T4). A full image boot is expensive, so this is a LIGHT smoke gate: boot the
# freshly-built NextBSD-*.img to the getty login prompt, confirm a clean
# end-state, power off. It is a login-only run (NB_LOGIN_ONLY=1) — no on-image
# marker suite — because the image gate's job is "did this kernel boot," not
# "do the userland markers pass" (that is the kernel/userland lanes' job).
#
# The loader un-mute dance, the arch-aware qemu argv (OVMF/q35/amd64,
# AAVMF/virt/arm64), login detection and teardown all come from the shared
# harness now, not the in-repo loader.exp.inc / qemu-arch.sh those deleted.
set -eu
IMG=${1:?usage: [ARCH=amd64|arm64] img-boot-test.sh path/to/NextBSD-*.img[.zip]}
[ -f "$IMG" ] || { echo "ERROR: $IMG not found"; exit 1; }

mkdir -p tests
# A zipped image is extracted to a raw disk.img first (the harness takes a raw IMG).
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

# Fetch the shared harness at the pinned lockstep tag (absent = first run).
[ -d nextbsd-ci/.git ] || git clone --depth 1 --branch v0.3.4 \
  https://github.com/nextbsd/nextbsd-ci.git nextbsd-ci

echo "==> img boot test: $IMG (arch=${ARCH:-amd64}) — shared harness, login-only"
ls -lh "$IMG"

# Login-only: the harness boots -> logs in -> confirms a clean end-state ->
# powers off, and runs no on-image suite. NB_BOOT_VERBOSE=1: the serial
# transcript is the diagnostic. The exit class is the gate.
ARCH=${ARCH:-amd64} NB_LOGIN_ONLY=1 NB_BOOT_VERBOSE=1 NB_LOG=boot-test.log \
  sh nextbsd-ci/harness/boot-test.sh "$IMG"
