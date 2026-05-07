#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

OUT_DIR="${SEMU_CRASH_LOG_DIR:-vgpu-crash-logs/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "${OUT_DIR}"

if [[ "${SEMU_SKIP_BUILD:-0}" != "1" ]]; then
    make ENABLE_VIRGL=1 semu minimal.dtb
fi

if [[ ! -f Image ]]; then
    echo "missing Image; build or download it before running this script" >&2
    exit 1
fi

if [[ ! -f test-tools.img ]]; then
    echo "missing test-tools.img; run 'make test-tools.img' or scripts/build-image.sh --virgl" >&2
    exit 1
fi

if [[ "$#" -gt 0 ]]; then
    SEMU_ARGS=("$@")
else
    SEMU_ARGS=(
        -k "${SEMU_KERNEL:-Image}"
        -c "${SEMU_CPUS:-1}"
        -b "${SEMU_DTB:-minimal.dtb}"
        -d "${SEMU_DISK:-test-tools.img}"
    )
fi

{
    echo "timestamp: $(date -Is)"
    echo "cwd: ${REPO_ROOT}"
    echo "command: ./semu ${SEMU_ARGS[*]}"
    echo "log dir: ${OUT_DIR}"
    git rev-parse --abbrev-ref HEAD 2>/dev/null || true
    git rev-parse HEAD 2>/dev/null || true
} >"${OUT_DIR}/run-info.txt"

ulimit -c unlimited 2>/dev/null || true

echo "Writing crash capture to ${OUT_DIR}/gdb.log"
echo "Run the guest normally here. If semu segfaults, gdb will append a host backtrace."
echo "VirGL test-tools note: Xorg usually starts as :0 during boot. Do not run startx;"
echo "use '. /root/local-env.sh', then DISPLAY=:0 twm/xterm/glxgears on the existing server."

set +e
GDB_BIN=""
if command -v gdb >/dev/null 2>&1; then
    GDB_BIN="gdb"
elif command -v gdb-multiarch >/dev/null 2>&1; then
    GDB_BIN="gdb-multiarch"
fi

if [[ -n "${GDB_BIN}" ]]; then
    "${GDB_BIN}" -q \
        -ex "set pagination off" \
        -ex "set print thread-events off" \
        -ex "set confirm off" \
        -ex "run" \
        -ex "printf \"\\n--- semu stopped; collecting host backtrace ---\\n\"" \
        -ex "bt full" \
        -ex "info threads" \
        -ex "thread apply all bt full" \
        -ex "quit" \
        --args ./semu "${SEMU_ARGS[@]}" 2>&1 | tee "${OUT_DIR}/gdb.log"
    ret="${PIPESTATUS[0]}"
else
    echo "gdb not found; recording console only" | tee "${OUT_DIR}/gdb.log"
    ./semu "${SEMU_ARGS[@]}" 2>&1 | tee -a "${OUT_DIR}/gdb.log"
    ret="${PIPESTATUS[0]}"
fi
set -e

echo "semu/gdb exit code: ${ret}" | tee -a "${OUT_DIR}/gdb.log"
echo "Saved: ${OUT_DIR}/run-info.txt"
echo "Saved: ${OUT_DIR}/gdb.log"
exit "${ret}"
