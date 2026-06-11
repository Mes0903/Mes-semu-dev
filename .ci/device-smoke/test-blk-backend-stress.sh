#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

: "${SEMU_BLK_BACKEND_STRESS_RUNS=20}"
: "${SEMU_BLK_BACKEND_STRESS_TARGET=test-virtio-blk-common}"
: "${SEMU_BLK_BACKEND_STRESS_MAKE=make}"

case "${SEMU_BLK_BACKEND_STRESS_RUNS}" in
    ''|*[!0-9]*)
        echo "FAIL: SEMU_BLK_BACKEND_STRESS_RUNS must be a positive integer" >&2
        exit 1
        ;;
esac
if (( 10#${SEMU_BLK_BACKEND_STRESS_RUNS} < 1 )); then
    echo "FAIL: SEMU_BLK_BACKEND_STRESS_RUNS must be a positive integer" >&2
    exit 1
fi

if [[ -z "${SEMU_BLK_BACKEND_STRESS_TARGET}" ]]; then
    echo "FAIL: SEMU_BLK_BACKEND_STRESS_TARGET must name a target" >&2
    exit 1
fi
if [[ -z "${SEMU_BLK_BACKEND_STRESS_MAKE}" ]]; then
    echo "FAIL: SEMU_BLK_BACKEND_STRESS_MAKE must name a command" >&2
    exit 1
fi

read -r -a BLK_BACKEND_STRESS_MAKE_CMD <<< "${SEMU_BLK_BACKEND_STRESS_MAKE}"

if [[ "${SEMU_BLK_BACKEND_STRESS_PRINT_CONFIG:-0}" == 1 ]]; then
    printf 'SEMU_BLK_BACKEND_STRESS_RUNS=%s\n' "${SEMU_BLK_BACKEND_STRESS_RUNS}"
    printf 'SEMU_BLK_BACKEND_STRESS_TARGET=%s\n' "${SEMU_BLK_BACKEND_STRESS_TARGET}"
    printf 'SEMU_BLK_BACKEND_STRESS_REPO_ROOT=%s\n' "${REPO_ROOT}"
    printf 'SEMU_BLK_BACKEND_STRESS_MAKE=%s\n' "${SEMU_BLK_BACKEND_STRESS_MAKE}"
    exit 0
fi

cd "${REPO_ROOT}"

for (( run = 1; run <= 10#${SEMU_BLK_BACKEND_STRESS_RUNS}; run++ )); do
    printf '\n=== virtio-blk backend stress run %d/%d: %s ===\n' \
        "${run}" "${SEMU_BLK_BACKEND_STRESS_RUNS}" \
        "${SEMU_BLK_BACKEND_STRESS_TARGET}"
    "${BLK_BACKEND_STRESS_MAKE_CMD[@]}" "${SEMU_BLK_BACKEND_STRESS_TARGET}"
done
