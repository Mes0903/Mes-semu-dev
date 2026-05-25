#!/usr/bin/env bash
#
# Compress raw prebuilt guest artifacts, write prebuilt.sha1, and print
# checksums. This script does not publish anything to a remote service.
#
# Inputs (in cwd):
#   Image
#   rootfs.cpio
#   test-tools.img
#   plus the source inputs listed by .ci/prebuilt-inputs.sh
#
# Outputs (in cwd):
#   Image.bz2
#   rootfs.cpio.bz2
#   test-tools.img.bz2
#   prebuilt.sha1   -- four-line manifest in sha1sum format. The
#                      first three lines verify the published archives;
#                      the fourth uses the virtual name 'inputs' to
#                      publish the SHA-1 of the concatenated input
#                      files so drift-detection consumers can read it
#                      directly from the release.
#
# Stdout (machine-readable, one assignment per line):
#   kernel_sha1=<sha1 of Image.bz2>
#   initrd_sha1=<sha1 of rootfs.cpio.bz2>
#   test_tools_sha1=<sha1 of test-tools.img.bz2>
#   inputs_sha1=<sha1 of the concatenated input files>

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$SCRIPT_DIR/prebuilt-inputs.sh"

read -r -a SHA1 <<< "$(prebuilt_sha1_tool)"
mapfile -t INPUTS < <(prebuilt_inputs)

for f in Image rootfs.cpio test-tools.img "${INPUTS[@]}"; do
    if [ ! -f "$f" ]; then
        echo "[!] Missing $f -- run scripts/build-image.sh --all --x11 --directfb2-test first" >&2
        exit 1
    fi
done

bzip2 -k -f Image
bzip2 -k -f rootfs.cpio
bzip2 -k -f test-tools.img

KERNEL_SHA1=$("${SHA1[@]}" Image.bz2 | awk '{print $1}')
INITRD_SHA1=$("${SHA1[@]}" rootfs.cpio.bz2 | awk '{print $1}')
TEST_TOOLS_SHA1=$("${SHA1[@]}" test-tools.img.bz2 | awk '{print $1}')
INPUTS_SHA1=$(prebuilt_inputs_sha1)

{
    echo "$KERNEL_SHA1  Image.bz2"
    echo "$INITRD_SHA1  rootfs.cpio.bz2"
    echo "$TEST_TOOLS_SHA1  test-tools.img.bz2"
    echo "$INPUTS_SHA1  inputs"
} > prebuilt.sha1

{
    cat prebuilt.sha1
    echo "inputs_sha1: $INPUTS_SHA1"
} >&2

echo "kernel_sha1=$KERNEL_SHA1"
echo "initrd_sha1=$INITRD_SHA1"
echo "test_tools_sha1=$TEST_TOOLS_SHA1"
echo "inputs_sha1=$INPUTS_SHA1"
