#!/usr/bin/env bash

set -euo pipefail

fail()
{
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

if rg -n 'pthread_mutex|vgpu_gl_lock|vgpu_gl_unlock' \
    window-sw.c vgpu-gl.h virtio-gpu-virgl.c virtio-gpu.c virtio-gpu.h; then
    fail "final OpenGL path must not use a global GL mutex"
fi

if rg -n 'command_enter|command_leave|thread_enter|virtio_gpu_thread_enter' \
    virtio-gpu.c virtio-gpu.h virtio-gpu-virgl.c main.c; then
    fail "VirGL ownership must not rely on backend command/thread lock hooks"
fi
