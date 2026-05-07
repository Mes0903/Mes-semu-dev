#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

OUT_DIR="${SEMU_CRASH_LOG_DIR:-vgpu-crash-logs/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "${OUT_DIR}"
RESOURCE_LOG="${OUT_DIR}/host-resources.log"
PROGRESS_LOG="${OUT_DIR}/vgpu-progress.log"
RESOURCE_MONITOR_PID=""
RESOURCE_MONITOR_INTERVAL="${SEMU_RESOURCE_MONITOR_INTERVAL:-1}"
if ! awk -v v="${RESOURCE_MONITOR_INTERVAL}" \
    'BEGIN { exit !(v ~ /^[0-9]+([.][0-9]+)?$/ && v + 0 > 0) }'; then
    RESOURCE_MONITOR_INTERVAL=1
fi
RESOURCE_MONITOR_ENABLED="${SEMU_RESOURCE_MONITOR:-1}"
VGPU_PROGRESS_LOG_ENABLED="${SEMU_VGPU_PROGRESS_LOG:-1}"
VGPU_PROGRESS_LOG_INTERVAL="${SEMU_VGPU_PROGRESS_LOG_INTERVAL:-2}"
if [[ "${VGPU_PROGRESS_LOG_ENABLED}" != "0" ]]; then
    export SEMU_VGPU_PROGRESS_LOG="${VGPU_PROGRESS_LOG_ENABLED}"
    export SEMU_VGPU_PROGRESS_LOG_FILE="${SEMU_VGPU_PROGRESS_LOG_FILE:-${PROGRESS_LOG}}"
    export SEMU_VGPU_PROGRESS_LOG_INTERVAL="${VGPU_PROGRESS_LOG_INTERVAL}"
fi

sample_host_resources() {
    {
        echo "timestamp: $(date -Is)"

        if command -v free >/dev/null 2>&1; then
            echo "--- free -h ---"
            free -h
        elif [[ -r /proc/meminfo ]]; then
            echo "--- /proc/meminfo ---"
            sed -n '1,8p' /proc/meminfo
        fi

        if [[ -r /sys/fs/cgroup/memory.current ]]; then
            echo "cgroup_memory_current_bytes: $(< /sys/fs/cgroup/memory.current)"
        fi
        if [[ -r /sys/fs/cgroup/memory.max ]]; then
            echo "cgroup_memory_max_bytes: $(< /sys/fs/cgroup/memory.max)"
        fi
        if [[ -r /sys/fs/cgroup/memory.events ]]; then
            echo "--- cgroup memory.events ---"
            cat /sys/fs/cgroup/memory.events || true
        fi

        echo "--- semu ps ---"
        ps -C semu -o pid,ppid,stat,rss,vsz,etime,cmd 2>/dev/null || echo "(no semu process)"

        if command -v pgrep >/dev/null 2>&1; then
            for pid in $(pgrep -x semu 2>/dev/null || true); do
                if [[ -r "/proc/${pid}/status" ]]; then
                    echo "--- /proc/${pid}/status ---"
                    awk '/^(Name|State|Pid|PPid|VmPeak|VmSize|VmRSS|RssAnon|RssFile|Threads):/ { print }' \
                        "/proc/${pid}/status" || true
                fi
            done
        fi

        echo
    } >>"${RESOURCE_LOG}"
}

start_resource_monitor() {
    if [[ "${RESOURCE_MONITOR_ENABLED}" == "0" ]]; then
        return
    fi

    {
        echo "host resource monitor started: $(date -Is)"
        echo "interval_seconds: ${RESOURCE_MONITOR_INTERVAL}"
        echo
    } >"${RESOURCE_LOG}"

    (
        while :; do
            sample_host_resources
            sleep "${RESOURCE_MONITOR_INTERVAL}"
        done
    ) &
    RESOURCE_MONITOR_PID="$!"
}

stop_resource_monitor() {
    if [[ -z "${RESOURCE_MONITOR_PID}" ]]; then
        return
    fi

    kill "${RESOURCE_MONITOR_PID}" 2>/dev/null || true
    wait "${RESOURCE_MONITOR_PID}" 2>/dev/null || true
    RESOURCE_MONITOR_PID=""
    sample_host_resources
    echo "host resource monitor stopped: $(date -Is)" >>"${RESOURCE_LOG}"
}

trap stop_resource_monitor EXIT

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
    echo "resource log: ${RESOURCE_LOG}"
    echo "resource monitor enabled: ${RESOURCE_MONITOR_ENABLED}"
    echo "resource monitor interval: ${RESOURCE_MONITOR_INTERVAL}s"
    echo "vgpu progress log enabled: ${VGPU_PROGRESS_LOG_ENABLED}"
    if [[ "${VGPU_PROGRESS_LOG_ENABLED}" != "0" ]]; then
        echo "vgpu progress log: ${SEMU_VGPU_PROGRESS_LOG_FILE}"
        echo "vgpu progress log interval: ${SEMU_VGPU_PROGRESS_LOG_INTERVAL}s"
    fi
    git rev-parse --abbrev-ref HEAD 2>/dev/null || true
    git rev-parse HEAD 2>/dev/null || true
} >"${OUT_DIR}/run-info.txt"

ulimit -c unlimited 2>/dev/null || true

echo "Writing crash capture to ${OUT_DIR}/gdb.log"
if [[ "${RESOURCE_MONITOR_ENABLED}" == "0" ]]; then
    echo "Host resource monitor disabled by SEMU_RESOURCE_MONITOR=0"
else
    echo "Writing host resource samples to ${RESOURCE_LOG}"
fi
if [[ "${VGPU_PROGRESS_LOG_ENABLED}" == "0" ]]; then
    echo "VGPU progress monitor disabled by SEMU_VGPU_PROGRESS_LOG=0"
else
    echo "Writing VGPU progress samples to ${SEMU_VGPU_PROGRESS_LOG_FILE}"
fi
echo "Run the guest normally here. If semu segfaults, gdb will append a host backtrace."
echo "VirGL test-tools note: Xorg usually starts as :0 during boot."
echo "For normal smoke, use '. /root/local-env.sh' and DISPLAY=:0 twm/xterm/glxgears."
echo "Run startx only for a plan/manual gate that explicitly asks for startx stress."

set +e
GDB_BIN=""
if command -v gdb >/dev/null 2>&1; then
    GDB_BIN="gdb"
elif command -v gdb-multiarch >/dev/null 2>&1; then
    GDB_BIN="gdb-multiarch"
fi

start_resource_monitor

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
stop_resource_monitor

echo "semu/gdb exit code: ${ret}" | tee -a "${OUT_DIR}/gdb.log"
echo "Saved: ${OUT_DIR}/run-info.txt"
echo "Saved: ${OUT_DIR}/gdb.log"
if [[ -f "${RESOURCE_LOG}" ]]; then
    echo "Saved: ${RESOURCE_LOG}"
fi
if [[ -f "${SEMU_VGPU_PROGRESS_LOG_FILE:-}" ]]; then
    echo "Saved: ${SEMU_VGPU_PROGRESS_LOG_FILE}"
fi
exit "${ret}"
