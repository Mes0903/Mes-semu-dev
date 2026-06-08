#!/usr/bin/env bash
set -euo pipefail

failed=0

check_absent() {
    local file=$1
    local pattern=$2

    if grep -En "$pattern" "$file"; then
        printf 'threaded-only source contract failed: %s still matches /%s/\n' \
            "$file" "$pattern" >&2
        failed=1
    fi
}

check_absent Makefile 'ENABLE_THREADED|phase2-coroutine|coro\.o'
check_absent main.c '#include "coro\.h"|SEMU_HAS\(THREADED\)|coro_|hart_exec_loop|static void wfi_handler\('
check_absent uart.c '#include "coro\.h"|SEMU_HAS\(THREADED\)|coro_'
check_absent riscv.c 'SEMU_HAS\(THREADED\)'
check_absent tests/phase2-threading-contract-test.c 'ENABLE_THREADED|default device lock helper remains a no-op'

exit "$failed"
