#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TMP_DIR="$(mktemp -d)"
cleanup()
{
    rm -rf "${TMP_DIR}"
    rm -f "${REPO_ROOT}/virtio-gpu-virgl.o" \
        "${REPO_ROOT}/.virtio-gpu-virgl.o.d"
}
trap cleanup EXIT

mkdir -p "${TMP_DIR}/include" "${TMP_DIR}/lib"

cat >"${TMP_DIR}/include/virglrenderer.h" <<'HEADER'
#pragma once

#include <stdint.h>
#include <string.h>

#define VIRGL_RENDERER_CALLBACKS_VERSION 4
#define VIRGL_RENDERER_THREAD_SYNC 2

struct virgl_renderer_callbacks {
    int version;
};

void virgl_renderer_get_cap_set(uint32_t set,
                                uint32_t *max_ver,
                                uint32_t *max_size);
void virgl_renderer_fill_caps(uint32_t set, uint32_t version, void *caps);
void virgl_renderer_reset(void);
HEADER

cat >"${TMP_DIR}/virglrenderer-stub.c" <<'SOURCE'
#include <stdint.h>
#include <string.h>

void virgl_renderer_get_cap_set(uint32_t set,
                                uint32_t *max_ver,
                                uint32_t *max_size)
{
    if (set == 1) {
        *max_ver = 1;
        *max_size = 64;
    } else if (set == 2) {
        *max_ver = 2;
        *max_size = 128;
    } else {
        *max_ver = 0;
        *max_size = 0;
    }
}

void virgl_renderer_fill_caps(uint32_t set, uint32_t version, void *caps)
{
    memset(caps, (int) (0xa0u + set + version), set == 2 ? 128 : 64);
}

void virgl_renderer_reset(void)
{
}
SOURCE

"${CC:-cc}" -c "${TMP_DIR}/virglrenderer-stub.c" \
    -o "${TMP_DIR}/virglrenderer-stub.o"
ar rcs "${TMP_DIR}/lib/libvirglrenderer.a" \
    "${TMP_DIR}/virglrenderer-stub.o"

fake_pkg_config="${TMP_DIR}/pkg-config"
cat >"${fake_pkg_config}" <<SCRIPT
#!/usr/bin/env bash
case "\$1" in
    --exists)
        exit 0
        ;;
    --cflags)
        printf '%s\n' '-I${TMP_DIR}/include'
        ;;
    --libs)
        printf '%s\n' '${TMP_DIR}/lib/libvirglrenderer.a'
        ;;
    *)
        exit 2
        ;;
esac
SCRIPT
chmod +x "${fake_pkg_config}"

rm -f "${REPO_ROOT}/virtio-gpu-virgl.o" \
    "${REPO_ROOT}/.virtio-gpu-virgl.o.d"
make -s -C "${REPO_ROOT}" ENABLE_VIRGL=1 PKG_CONFIG="${fake_pkg_config}" semu
strings -a "${REPO_ROOT}/semu" >"${TMP_DIR}/semu.strings"
grep -q 'SEMU_VIRGL_BACKEND_LINKED' "${TMP_DIR}/semu.strings"
