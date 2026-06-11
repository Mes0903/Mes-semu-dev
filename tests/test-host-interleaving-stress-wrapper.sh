#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

WRAPPER="${REPO_ROOT}/.ci/device-smoke/test-host-interleaving-stress.sh"
MAKEFILE="${REPO_ROOT}/Makefile"

DEFAULT_TARGETS="test-virtio-actor test-vgpu-renderer test-vgpu-virgl-gate test-virtio-mmio"

get_value() {
    local key="$1"
    awk -F= -v key="$key" '$1 == key { print $2 }'
}

run_wrapper_config() {
    env "$@" SEMU_HOST_INTERLEAVING_PRINT_CONFIG=1 "${WRAPPER}"
}

expect_invalid_config() {
    local expected_message="$1"
    shift
    local invalid_output
    local invalid_status

    set +e
    invalid_output="$(run_wrapper_config "$@" 2>&1)"
    invalid_status=$?
    set -e

    if (( invalid_status == 0 )); then
        printf 'expected invalid host interleaving config to fail: %s\n' "$*"
        exit 1
    fi
    if ! grep -q "${expected_message}" <<<"${invalid_output}"; then
        printf 'expected validation message "%s" for config %s\n' \
            "${expected_message}" "$*"
        exit 1
    fi
}

output="$(run_wrapper_config)"
runs="$(get_value SEMU_HOST_INTERLEAVING_RUNS <<<"${output}")"
targets="$(get_value SEMU_HOST_INTERLEAVING_TARGETS <<<"${output}")"
repo_root="$(get_value SEMU_HOST_INTERLEAVING_REPO_ROOT <<<"${output}")"
make_cmd="$(get_value SEMU_HOST_INTERLEAVING_MAKE <<<"${output}")"

if [[ "${runs}" != 10 ]]; then
    printf 'expected host interleaving stress to default SEMU_HOST_INTERLEAVING_RUNS=10, got %s\n' \
        "${runs}"
    exit 1
fi
if [[ "${targets}" != "${DEFAULT_TARGETS}" ]]; then
    printf 'expected default host interleaving targets "%s", got "%s"\n' \
        "${DEFAULT_TARGETS}" "${targets}"
    exit 1
fi
if [[ "${repo_root}" != "${REPO_ROOT}" ]]; then
    printf 'expected repo root %s, got %s\n' "${REPO_ROOT}" "${repo_root}"
    exit 1
fi
if [[ "${make_cmd}" != "make" ]]; then
    printf 'expected default make command make, got %s\n' "${make_cmd}"
    exit 1
fi

output="$(
    run_wrapper_config \
        SEMU_HOST_INTERLEAVING_RUNS=3 \
        SEMU_HOST_INTERLEAVING_TARGETS="test-virtio-mmio test-vgpu-renderer" \
        SEMU_HOST_INTERLEAVING_MAKE=false
)"
runs="$(get_value SEMU_HOST_INTERLEAVING_RUNS <<<"${output}")"
targets="$(get_value SEMU_HOST_INTERLEAVING_TARGETS <<<"${output}")"
make_cmd="$(get_value SEMU_HOST_INTERLEAVING_MAKE <<<"${output}")"

if [[ "${runs}" != 3 ]]; then
    printf 'expected host interleaving stress to preserve SEMU_HOST_INTERLEAVING_RUNS=3, got %s\n' \
        "${runs}"
    exit 1
fi
if [[ "${targets}" != "test-virtio-mmio test-vgpu-renderer" ]]; then
    printf 'expected host interleaving stress to preserve custom targets, got "%s"\n' \
        "${targets}"
    exit 1
fi
if [[ "${make_cmd}" != false ]]; then
    printf 'expected host interleaving stress to preserve custom make command, got %s\n' \
        "${make_cmd}"
    exit 1
fi

expect_invalid_config \
    'SEMU_HOST_INTERLEAVING_RUNS must be a positive integer' \
    SEMU_HOST_INTERLEAVING_RUNS=0
expect_invalid_config \
    'SEMU_HOST_INTERLEAVING_RUNS must be a positive integer' \
    SEMU_HOST_INTERLEAVING_RUNS=
expect_invalid_config \
    'SEMU_HOST_INTERLEAVING_TARGETS must name at least one target' \
    SEMU_HOST_INTERLEAVING_TARGETS=

for target in ${DEFAULT_TARGETS}; do
    if ! grep -q "^${target}:" "${MAKEFILE}"; then
        printf 'expected Makefile target %s to exist\n' "${target}"
        exit 1
    fi
done

if ! grep -q '^test-host-interleaving-stress-wrapper:' "${MAKEFILE}"; then
    printf '%s\n' 'expected Makefile target test-host-interleaving-stress-wrapper'
    exit 1
fi
if ! grep -q '^test-host: .*test-host-interleaving-stress-wrapper' "${MAKEFILE}"; then
    printf '%s\n' 'expected test-host to include test-host-interleaving-stress-wrapper'
    exit 1
fi
