#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)

cd "$REPO_ROOT"

# build-image.sh is the user-facing build entrypoint. It reuses the shared
# artifact build recipe for local builds; CI cache/stamp decisions live under
# .ci/prebuilt and are intentionally outside this CLI.
# The linter cannot resolve this runtime repository root.
# shellcheck disable=SC1090,SC1091
. "$REPO_ROOT/scripts/prebuilt/build-artifacts.sh"


TEST_TOOLS_PAYLOADS_EXPLICIT=0
TEST_TOOLS_PAYLOAD_OPTION_SEEN=0
TEST_TOOLS_PAYLOADS=

add_test_tools_payload() {
    local payload=$1

    if [[ $TEST_TOOLS_PAYLOADS_EXPLICIT -eq 0 || \
          $TEST_TOOLS_PAYLOADS = minimal ]]; then
        TEST_TOOLS_PAYLOADS=
        TEST_TOOLS_PAYLOADS_EXPLICIT=1
    fi

    case ",$TEST_TOOLS_PAYLOADS," in
        *,"$payload",*)
            ;;
        ,,)
            TEST_TOOLS_PAYLOADS=$payload
            ;;
        *)
            TEST_TOOLS_PAYLOADS=$TEST_TOOLS_PAYLOADS,$payload
            ;;
    esac
}

show_help() {
    local exit_code=${1:-0}
    local prog

    prog=$(basename "$0")
    cat << HELP
Usage: $prog <image|rootfs|test-tools|all>... [--x11] [--directfb2-test] [--minimal-test-tools] [--test-tools-payloads=list] [--no-ext4] [--clean-build] [--help]

Targets:
  image             Build the Linux Image. This prepares the Buildroot
                    toolchain if needed but does not publish rootfs.cpio or
                    test-tools.img as final outputs.
  rootfs            Build Buildroot rootfs.cpio and, unless --no-ext4 is
                    given, derive ext4.img from it.
  test-tools        Build test-tools.img. By default this uses the canonical
                    X11 + DirectFB2 smoke-test payload.
  all               Build image, rootfs, and canonical test-tools.

Options:
  --x11             Select the X11/C++ runtime payload for test-tools. When
                    any test-tools payload option is given, only selected
                    payloads are included.
  --directfb2-test  Select the DirectFB2 smoke-test payload for test-tools;
                    this implies --x11.
  --minimal-test-tools
                    Build test-tools.img without optional payloads.
  --test-tools-payloads=list
                    Set test-tools payloads explicitly. Use comma-separated
                    values from: x11, directfb2, minimal.
  --no-ext4         With rootfs, skip ext4.img generation and produce only
                    rootfs.cpio (matches the legacy ENABLE_EXTERNAL_ROOT=0 path).
  --clean-build     Remove buildroot/ and/or linux/ before building; with
                    test-tools, also remove DirectFB2 sources.
  --help            Show this message.
HELP
    exit "$exit_code"
}

BUILD_ROOTFS=0
BUILD_TEST_TOOLS=0
BUILD_LINUX=0
NO_EXT4=0
CLEAN_BUILD=0
TARGET_SPECIFIED=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        image)
            BUILD_LINUX=1
            TARGET_SPECIFIED=1
            ;;
        rootfs)
            BUILD_ROOTFS=1
            TARGET_SPECIFIED=1
            ;;
        test-tools)
            BUILD_TEST_TOOLS=1
            TARGET_SPECIFIED=1
            ;;
        all)
            BUILD_ROOTFS=1
            BUILD_TEST_TOOLS=1
            BUILD_LINUX=1
            TARGET_SPECIFIED=1
            ;;
        --x11)
            TEST_TOOLS_PAYLOAD_OPTION_SEEN=1
            add_test_tools_payload x11
            ;;
        --directfb2-test)
            TEST_TOOLS_PAYLOAD_OPTION_SEEN=1
            add_test_tools_payload directfb2
            ;;
        --minimal-test-tools)
            TEST_TOOLS_PAYLOAD_OPTION_SEEN=1
            TEST_TOOLS_PAYLOADS=minimal
            TEST_TOOLS_PAYLOADS_EXPLICIT=1
            ;;
        --test-tools-payloads=*)
            TEST_TOOLS_PAYLOAD_OPTION_SEEN=1
            TEST_TOOLS_PAYLOADS=${1#*=}
            TEST_TOOLS_PAYLOADS_EXPLICIT=1
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

if [[ $TEST_TOOLS_PAYLOAD_OPTION_SEEN -eq 1 && $BUILD_TEST_TOOLS -eq 0 ]]; then
    echo "Error: test-tools payload options require the test-tools or all target."
    show_help 1
fi

if [[ $NO_EXT4 -eq 1 && $BUILD_ROOTFS -eq 0 ]]; then
    echo "Error: --no-ext4 requires the rootfs or all target."
    show_help 1
fi

if [[ $BUILD_TEST_TOOLS -eq 1 ]]; then
    if [[ $TEST_TOOLS_PAYLOADS_EXPLICIT -eq 1 ]]; then
        export PREBUILT_TEST_TOOLS_PAYLOADS=$TEST_TOOLS_PAYLOADS
    fi
    prebuilt_test_tools_payload_key >/dev/null
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
