#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

: "${SEMU_HOST_INTERLEAVING_RUNS=10}"
: "${SEMU_HOST_INTERLEAVING_TARGETS=test-virtio-actor test-vgpu-renderer test-vgpu-virgl-gate test-virtio-mmio}"
: "${SEMU_HOST_INTERLEAVING_MAKE:=make}"

case "${SEMU_HOST_INTERLEAVING_RUNS}" in
    ''|*[!0-9]*)
        echo "FAIL: SEMU_HOST_INTERLEAVING_RUNS must be a positive integer" >&2
        exit 1
        ;;
esac
if (( 10#${SEMU_HOST_INTERLEAVING_RUNS} < 1 )); then
    echo "FAIL: SEMU_HOST_INTERLEAVING_RUNS must be a positive integer" >&2
    exit 1
fi

read -r -a HOST_INTERLEAVING_TARGETS <<< "${SEMU_HOST_INTERLEAVING_TARGETS}"
read -r -a HOST_INTERLEAVING_MAKE_CMD <<< "${SEMU_HOST_INTERLEAVING_MAKE}"
if (( ${#HOST_INTERLEAVING_TARGETS[@]} == 0 )); then
    echo "FAIL: SEMU_HOST_INTERLEAVING_TARGETS must name at least one target" >&2
    exit 1
fi

if [[ "${SEMU_HOST_INTERLEAVING_PRINT_CONFIG:-0}" == 1 ]]; then
    printf 'SEMU_HOST_INTERLEAVING_RUNS=%s\n' "${SEMU_HOST_INTERLEAVING_RUNS}"
    printf 'SEMU_HOST_INTERLEAVING_TARGETS=%s\n' "${SEMU_HOST_INTERLEAVING_TARGETS}"
    printf 'SEMU_HOST_INTERLEAVING_REPO_ROOT=%s\n' "${REPO_ROOT}"
    printf 'SEMU_HOST_INTERLEAVING_MAKE=%s\n' "${SEMU_HOST_INTERLEAVING_MAKE}"
    exit 0
fi

cd "${REPO_ROOT}"

for (( run = 1; run <= 10#${SEMU_HOST_INTERLEAVING_RUNS}; run++ )); do
    for target in "${HOST_INTERLEAVING_TARGETS[@]}"; do
        printf '\n=== host interleaving stress run %d/%d: %s ===\n' \
            "${run}" "${SEMU_HOST_INTERLEAVING_RUNS}" "${target}"
        "${HOST_INTERLEAVING_MAKE_CMD[@]}" "${target}"
    done
done
