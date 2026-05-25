#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$SCRIPT_DIR/prebuilt-inputs.sh"

for f in Image.bz2 rootfs.cpio.bz2 test-tools.img.bz2 prebuilt.sha1; do
    if [ ! -f "$f" ]; then
        echo "[!] Missing $f" >&2
        exit 1
    fi
done

read -r -a SHA1 <<< "$(prebuilt_sha1_tool)"
awk '$2 != "inputs"' prebuilt.sha1 | "${SHA1[@]}" -c -
