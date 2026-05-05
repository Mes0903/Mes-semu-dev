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

grep -Fq 'gl_cursor_texture' window-sw.c ||
    fail "VirGL window path should retain a GL cursor texture"

grep -Fq 'sdl_scanout_apply_gl_cursor_frame' window-sw.c ||
    fail "VirGL window path should upload cursor frames without SDL_Renderer"

grep -Fq 'SDL_ConvertPixels' window-sw.c ||
    fail "VirGL cursor upload should convert SDL pixel formats before GL upload"

grep -Fq 'GL_RGBA' window-sw.c ||
    fail "VirGL cursor upload should use an explicit RGBA GL upload format"

grep -Fq 'glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)' window-sw.c ||
    fail "VirGL cursor overlay should alpha-blend the cursor plane"

grep -Fq 'scanout->gl_context' window-sw.c ||
    fail "VirGL cursor handling should branch on the GL context"
