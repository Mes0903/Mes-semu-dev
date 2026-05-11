#!/usr/bin/env bash
# Interactive VirGL/3D demo: launch semu against the -3D kernel and disk image
# stored in ../blob/. For automated smoke testing use .ci/test-virgl.sh; for
# crash collection use scripts/run-vgpu-crash-debug.sh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

BLOB_DIR="${REPO_ROOT}/../blob"
KERNEL="${BLOB_DIR}/Image-3D"
DISK="${BLOB_DIR}/test-tools-3D.img"
DTB="minimal.dtb"

# Mes0903/semu blob branch holds the published prebuilt kernel; the raw URL
# 302-redirects to raw.githubusercontent.com, which curl -L follows.
KERNEL_FALLBACK_URL="https://github.com/Mes0903/semu/raw/blob/Image.bz2"

if [[ ! -x ./semu ]] || ! grep -q 'VIRGL=1' .build-config.stamp 2>/dev/null; then
    make ENABLE_VIRGL=1 semu minimal.dtb
elif [[ ! -f "${DTB}" ]]; then
    make minimal.dtb
fi

if [[ ! -f "${KERNEL}" ]]; then
    echo "missing ${KERNEL}; downloading from Mes0903/semu blob branch..." >&2
    mkdir -p "${BLOB_DIR}"
    curl --fail --retry 3 --retry-delay 1 -L \
        -o "${KERNEL}.bz2.part" "${KERNEL_FALLBACK_URL}"
    mv "${KERNEL}.bz2.part" "${KERNEL}.bz2"
    bunzip2 -f "${KERNEL}.bz2"
fi

if [[ ! -f "${DISK}" ]]; then
    echo "missing ${DISK}" >&2
    echo "rebuild with: scripts/build-image.sh --virgl" >&2
    exit 1
fi

exec ./semu -k "${KERNEL}" -c 1 -b "${DTB}" -d "${DISK}"
