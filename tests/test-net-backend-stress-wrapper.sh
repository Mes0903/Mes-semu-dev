#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

WRAPPER="${REPO_ROOT}/.ci/device-smoke/test-net-backend-stress.sh"
MAKEFILE="${REPO_ROOT}/Makefile"

get_value() {
    local key="$1"
    awk -F= -v key="$key" '$1 == key { print $2 }'
}

run_wrapper_config() {
    env "$@" SEMU_NET_BACKEND_STRESS_PRINT_CONFIG=1 "${WRAPPER}"
}

expect_invalid_runs() {
    local value="$1"
    local output status

    set +e
    output="$(run_wrapper_config SEMU_NET_BACKEND_STRESS_RUNS="${value}" 2>&1)"
    status=$?
    set -e

    if (( status == 0 )); then
        printf 'expected SEMU_NET_BACKEND_STRESS_RUNS=%s to fail\n' "${value}"
        exit 1
    fi
    if ! grep -q 'SEMU_NET_BACKEND_STRESS_RUNS must be a positive integer' <<<"${output}"; then
        printf 'expected positive integer validation for SEMU_NET_BACKEND_STRESS_RUNS=%s\n' \
            "${value}"
        exit 1
    fi
}

output="$(run_wrapper_config)"
runs="$(get_value SEMU_NET_BACKEND_STRESS_RUNS <<<"${output}")"
target="$(get_value SEMU_NET_BACKEND_STRESS_TARGET <<<"${output}")"
repo_root="$(get_value SEMU_NET_BACKEND_STRESS_REPO_ROOT <<<"${output}")"
make_cmd="$(get_value SEMU_NET_BACKEND_STRESS_MAKE <<<"${output}")"

if [[ "${runs}" != 20 ]]; then
    printf 'expected net backend stress to default SEMU_NET_BACKEND_STRESS_RUNS=20, got %s\n' \
        "${runs}"
    exit 1
fi
if [[ "${target}" != "test-virtio-net-common" ]]; then
    printf 'expected net backend stress default target test-virtio-net-common, got %s\n' \
        "${target}"
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
        SEMU_NET_BACKEND_STRESS_RUNS=3 \
        SEMU_NET_BACKEND_STRESS_TARGET=test-virtio-mmio \
        SEMU_NET_BACKEND_STRESS_MAKE=false
)"
runs="$(get_value SEMU_NET_BACKEND_STRESS_RUNS <<<"${output}")"
target="$(get_value SEMU_NET_BACKEND_STRESS_TARGET <<<"${output}")"
make_cmd="$(get_value SEMU_NET_BACKEND_STRESS_MAKE <<<"${output}")"

if [[ "${runs}" != 3 ]]; then
    printf 'expected net backend stress to preserve SEMU_NET_BACKEND_STRESS_RUNS=3, got %s\n' \
        "${runs}"
    exit 1
fi
if [[ "${target}" != "test-virtio-mmio" ]]; then
    printf 'expected net backend stress to preserve custom target, got %s\n' "${target}"
    exit 1
fi
if [[ "${make_cmd}" != "false" ]]; then
    printf 'expected net backend stress to preserve custom make=false, got %s\n' \
        "${make_cmd}"
    exit 1
fi

expect_invalid_runs 0
expect_invalid_runs ''
expect_invalid_runs abc

set +e
target_output="$(run_wrapper_config SEMU_NET_BACKEND_STRESS_TARGET= 2>&1)"
target_status=$?
set -e
if (( target_status == 0 )); then
    printf '%s\n' 'expected empty SEMU_NET_BACKEND_STRESS_TARGET to fail'
    exit 1
fi
if ! grep -q 'SEMU_NET_BACKEND_STRESS_TARGET must name a target' <<<"${target_output}"; then
    printf '%s\n' 'expected validation message for empty SEMU_NET_BACKEND_STRESS_TARGET'
    exit 1
fi

set +e
make_output="$(run_wrapper_config SEMU_NET_BACKEND_STRESS_MAKE= 2>&1)"
make_status=$?
set -e
if (( make_status == 0 )); then
    printf '%s\n' 'expected empty SEMU_NET_BACKEND_STRESS_MAKE to fail'
    exit 1
fi
if ! grep -q 'SEMU_NET_BACKEND_STRESS_MAKE must name a command' <<<"${make_output}"; then
    printf '%s\n' 'expected validation message for empty SEMU_NET_BACKEND_STRESS_MAKE'
    exit 1
fi

if ! grep -q '^test-net-backend-stress-wrapper:' "${MAKEFILE}"; then
    printf '%s\n' 'expected Makefile target test-net-backend-stress-wrapper'
    exit 1
fi
if ! grep -q '^test-host: .*test-net-backend-stress-wrapper' "${MAKEFILE}"; then
    printf '%s\n' 'expected test-host to include test-net-backend-stress-wrapper'
    exit 1
fi
