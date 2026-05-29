#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)

cd "$REPO_ROOT"

# build-image.sh is the user-facing build entrypoint.  It reuses the prebuilt
# recipe library for the actual artifact work, then handles CLI validation and
# local stamp lifecycle around that build.
# The linter cannot resolve this runtime repository root.
# shellcheck disable=SC1090,SC1091
. "$REPO_ROOT/scripts/prebuilt/build-artifacts.sh"

STAMP_TARGETS=()

show_help() {
    local exit_code=${1:-0}
    local prog

    prog=$(basename "$0")
    cat << HELP
Usage: $prog <image|rootfs|test-tools|all>... [--x11] [--directfb2-test] [--no-ext4] [--clean-build] [--help]

Targets:
  image             Build the Linux Image. This prepares the Buildroot
                    toolchain if needed but does not publish rootfs.cpio or
                    test-tools.img as final outputs.
  rootfs            Build Buildroot rootfs.cpio and, unless --no-ext4 is
                    given, derive ext4.img from it.
  test-tools        Build the canonical test-tools.img with the X11 and
                    DirectFB2 smoke-test payload.
  all               Build image, rootfs, and canonical test-tools.

Options:
  --x11             Accepted for explicit canonical test-tools builds.
  --directfb2-test  Accepted for explicit canonical test-tools builds.
  --no-ext4         With rootfs, skip ext4.img generation and produce only
                    rootfs.cpio (matches the legacy ENABLE_EXTERNAL_ROOT=0 path).
  --clean-build     Remove buildroot/ and/or linux/ before building; with
                    test-tools, also remove DirectFB2 sources.
  --help            Show this message.
HELP
    exit "$exit_code"
}

BUILD_ROOTFS=0
BUILD_X11=0
BUILD_DIRECTFB_TEST=0
BUILD_TEST_TOOLS=0
BUILD_LINUX=0
NO_EXT4=0
CLEAN_BUILD=0
TARGET_SPECIFIED=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        image)
            BUILD_LINUX=1
            STAMP_TARGETS+=(image)
            TARGET_SPECIFIED=1
            ;;
        rootfs)
            BUILD_ROOTFS=1
            STAMP_TARGETS+=(rootfs)
            TARGET_SPECIFIED=1
            ;;
        test-tools)
            BUILD_TEST_TOOLS=1
            STAMP_TARGETS+=(test-tools)
            TARGET_SPECIFIED=1
            ;;
        all)
            BUILD_ROOTFS=1
            BUILD_TEST_TOOLS=1
            BUILD_LINUX=1
            STAMP_TARGETS+=(all)
            TARGET_SPECIFIED=1
            ;;
        --x11)
            BUILD_X11=1
            ;;
        --directfb2-test)
            BUILD_DIRECTFB_TEST=1
            ;;
        --no-ext4)
            NO_EXT4=1
            ;;
        --clean-build)
            CLEAN_BUILD=1
            ;;
        --help|-h)
            show_help 0
            ;;
        *)
            echo "Unknown option or target: $1"
            show_help 1
            ;;
    esac
    shift
done

if [[ $TARGET_SPECIFIED -eq 0 ]]; then
    echo "Error: No build target specified. Use image, rootfs, test-tools, or all."
    show_help 1
fi

if [[ ( $BUILD_DIRECTFB_TEST -eq 1 || $BUILD_X11 -eq 1 ) && $BUILD_TEST_TOOLS -eq 0 ]]; then
    echo "Error: --x11/--directfb2-test requires the test-tools or all target."
    show_help 1
fi

if [[ $NO_EXT4 -eq 1 && $BUILD_ROOTFS -eq 0 ]]; then
    echo "Error: --no-ext4 requires the rootfs or all target."
    show_help 1
fi

if [[ -x $REPO_ROOT/scripts/prebuilt/stamp-artifacts.sh ]]; then
    "$REPO_ROOT/scripts/prebuilt/stamp-artifacts.sh" --clear "${STAMP_TARGETS[@]}"
fi

if [[ $CLEAN_BUILD -eq 1 && ( $BUILD_ROOTFS -eq 1 || $BUILD_TEST_TOOLS -eq 1 || $BUILD_LINUX -eq 1 ) && -d buildroot ]]; then
    echo "Removing buildroot/ for clean build..."
    rm -rf buildroot
fi

if [[ $CLEAN_BUILD -eq 1 && $BUILD_LINUX -eq 1 && -d linux ]]; then
    echo "Removing linux/ for clean build..."
    rm -rf linux
fi

if [[ $CLEAN_BUILD -eq 1 && $BUILD_TEST_TOOLS -eq 1 ]]; then
    echo "Removing DirectFB2 sources for clean build..."
    rm -rf DirectFB2 DirectFB-examples directfb extra_packages
fi

if [[ $BUILD_LINUX -eq 1 ]]; then
    do_linux && OK
fi

if [[ $BUILD_ROOTFS -eq 1 ]]; then
    do_rootfs && OK
fi

if [[ $BUILD_TEST_TOOLS -eq 1 ]]; then
    do_test_tools && OK
fi

if [[ -x $REPO_ROOT/scripts/prebuilt/stamp-artifacts.sh ]]; then
    "$REPO_ROOT/scripts/prebuilt/stamp-artifacts.sh" "${STAMP_TARGETS[@]}"
fi
