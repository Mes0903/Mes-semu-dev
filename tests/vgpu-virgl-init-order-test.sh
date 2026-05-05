#!/usr/bin/env bash

set -euo pipefail

fail()
{
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

line_of()
{
    local pattern="$1"
    local path="$2"

    awk -v pattern="${pattern}" 'index($0, pattern) { print NR; exit }' \
        "${path}"
}

window_init_line="$(line_of 'g_window.window_init(headless' main.c)"
gpu_init_line="$(line_of 'virtio_gpu_init(&(emu->vgpu))' main.c)"

[ -n "${window_init_line}" ] || fail "main.c is missing g_window.window_init"
[ -n "${gpu_init_line}" ] || fail "main.c is missing virtio_gpu_init"

if [ "${window_init_line}" -ge "${gpu_init_line}" ]; then
    fail "VirGL needs SDL/OpenGL window initialization before virtio_gpu_init"
fi

grep -Fq 'SDL_GL_MakeCurrent(NULL, NULL)' window-sw.c ||
    fail "window-sw.c should detach GL contexts with SDL_GL_MakeCurrent(NULL, NULL)"

grep -Fq 'if (!frame->y_0_top)' window-sw.c ||
    fail "VirGL GL scanout blit should only flip non-Y_0_TOP resources"

if grep -Fq 'frame->height - frame->src_y' window-sw.c; then
    fail "VirGL GL scanout blit should not invert Y_0_TOP source rectangles"
fi
