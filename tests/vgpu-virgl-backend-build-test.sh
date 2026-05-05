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

cp "${REPO_ROOT}/tests/fakes/virglrenderer.h" \
    "${TMP_DIR}/include/virglrenderer.h"

cat >"${TMP_DIR}/virglrenderer-stub.c" <<'SOURCE'
#include "virglrenderer.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

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

int virgl_renderer_context_create(uint32_t handle,
                                  uint32_t nlen,
                                  const char *name)
{
    (void) handle;
    (void) nlen;
    (void) name;
    return 0;
}

void virgl_renderer_context_destroy(uint32_t handle)
{
    (void) handle;
}

int virgl_renderer_context_create_with_flags(uint32_t ctx_id,
                                             uint32_t ctx_flags,
                                             uint32_t nlen,
                                             const char *name)
{
    (void) ctx_id;
    (void) ctx_flags;
    (void) nlen;
    (void) name;
    return 0;
}

void virgl_renderer_ctx_attach_resource(int ctx_id, int res_handle)
{
    (void) ctx_id;
    (void) res_handle;
}

void virgl_renderer_ctx_detach_resource(int ctx_id, int res_handle)
{
    (void) ctx_id;
    (void) res_handle;
}

int virgl_renderer_resource_create(
    struct virgl_renderer_resource_create_args *args,
    struct iovec *iov,
    uint32_t num_iovs)
{
    (void) args;
    (void) iov;
    (void) num_iovs;
    return 0;
}

void virgl_renderer_resource_unref(uint32_t res_handle)
{
    (void) res_handle;
}

int virgl_renderer_resource_attach_iov(int res_handle,
                                       struct iovec *iov,
                                       int num_iovs)
{
    (void) res_handle;
    (void) iov;
    (void) num_iovs;
    return 0;
}

void virgl_renderer_resource_detach_iov(int res_handle,
                                        struct iovec **iov,
                                        int *num_iovs)
{
    (void) res_handle;
    *iov = NULL;
    *num_iovs = 0;
}

int virgl_renderer_transfer_write_iov(uint32_t handle,
                                      uint32_t ctx_id,
                                      int level,
                                      uint32_t stride,
                                      uint32_t layer_stride,
                                      struct virgl_box *box,
                                      uint64_t offset,
                                      struct iovec *iovec,
                                      unsigned int iovec_cnt)
{
    (void) handle;
    (void) ctx_id;
    (void) level;
    (void) stride;
    (void) layer_stride;
    (void) box;
    (void) offset;
    (void) iovec;
    (void) iovec_cnt;
    return 0;
}

int virgl_renderer_transfer_read_iov(uint32_t handle,
                                     uint32_t ctx_id,
                                     uint32_t level,
                                     uint32_t stride,
                                     uint32_t layer_stride,
                                     struct virgl_box *box,
                                     uint64_t offset,
                                     struct iovec *iov,
                                     int iovec_cnt)
{
    (void) handle;
    (void) ctx_id;
    (void) level;
    (void) stride;
    (void) layer_stride;
    (void) box;
    (void) offset;
    (void) iov;
    (void) iovec_cnt;
    return 0;
}

int virgl_renderer_submit_cmd(void *buffer, int ctx_id, int ndw)
{
    (void) buffer;
    (void) ctx_id;
    (void) ndw;
    return 0;
}
SOURCE

"${CC:-cc}" -c "${TMP_DIR}/virglrenderer-stub.c" \
    -I"${TMP_DIR}/include" \
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
