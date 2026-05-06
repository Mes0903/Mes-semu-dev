#!/usr/bin/env bash

set -euo pipefail

fail()
{
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

grep -Fq 'VIRTIO_GPU_F_EDID | (SEMU_HAS(VIRGL) ? VIRTIO_GPU_F_VIRGL : 0)' \
    virtio-gpu.c ||
    fail "device features must advertise only EDID plus optional VIRGL"

grep -Fq '.resource_create_blob = VIRTIO_GPU_CMD_UNDEF' virtio-gpu-virgl.c ||
    fail "OpenGL-only VirGL backend must not implement blob resource creation"
grep -Fq '.resource_map_blob = VIRTIO_GPU_CMD_UNDEF' virtio-gpu-virgl.c ||
    fail "OpenGL-only VirGL backend must not implement blob mapping"
grep -Fq '.resource_unmap_blob = VIRTIO_GPU_CMD_UNDEF' virtio-gpu-virgl.c ||
    fail "OpenGL-only VirGL backend must not implement blob unmapping"

grep -Fq 'if (request->context_init)' virtio-gpu-virgl.c ||
    fail "CTX_CREATE must reject context_init while CONTEXT_INIT is not advertised"

if grep -Eq 'VIRTIO_GPU_F_RESOURCE_BLOB|VIRTIO_GPU_F_CONTEXT_INIT' \
    <(sed -n '970,990p' virtio-gpu.c); then
    fail "DeviceFeatures must not advertise blob or context_init"
fi

if rg -n 'CAPSET_(VENUS|DRM)|VIRTIO_GPU_CAPSET_VENUS|VIRTIO_GPU_CAPSET_DRM' \
    virtio-gpu.c virtio-gpu-virgl.c >/dev/null; then
    fail "OpenGL-only plan must not expose Venus or native DRM capsets"
fi
