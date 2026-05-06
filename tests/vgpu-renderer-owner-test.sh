#!/usr/bin/env bash

set -euo pipefail

fail()
{
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

callback_body="$(
    awk '
        /^static void vgpu_virgl_write_fence\(/ {
            in_callback = 1
        }
        /^static void vgpu_virgl_write_context_fence\(/ {
            in_callback = 1
        }
        /^static / &&
            $0 !~ /^static void vgpu_virgl_write_fence\(/ &&
            $0 !~ /^static void vgpu_virgl_write_context_fence\(/ {
            in_callback = 0
        }
        in_callback {
            print
        }
    ' virtio-gpu-virgl.c
)"

[ -n "${callback_body}" ] ||
    fail "missing VirGL fence callback implementations"

if grep -Fq 'virtio_gpu_complete_fence' <<<"${callback_body}"; then
    fail "VirGL fence callbacks must enqueue renderer completions, not write virtqueue state directly"
fi

grep -Fq 'VGPU_RENDERER_DONE_FENCE' <<<"${callback_body}" ||
    fail "VirGL fence callbacks must emit VGPU_RENDERER_DONE_FENCE"

grep -Fq 'vgpu_renderer_complete' <<<"${callback_body}" ||
    fail "VirGL fence callbacks must publish through the renderer completion queue"

num_capsets_body="$(
    awk '
        /case offsetof\(struct virtio_gpu_config, num_capsets\):/ {
            in_case = 1
        }
        in_case {
            print
        }
        in_case && /return true;/ {
            exit
        }
    ' virtio-gpu.c
)"

[ -n "${num_capsets_body}" ] ||
    fail "missing virtio-gpu num_capsets config read case"

if grep -Fq 'virtio_gpu_backend_get_num_capsets' <<<"${num_capsets_body}"; then
    fail "num_capsets config reads must use the emulator-owned cache, not the backend"
fi

grep -Fq 'PRIV(vgpu)->num_capsets' <<<"${num_capsets_body}" ||
    fail "num_capsets config reads must return PRIV(vgpu)->num_capsets"

main_loop_body="$(
    awk '
        /^static void window_main_loop_sw\(void\)/ {
            in_fn = 1
        }
        /^static / && in_fn &&
            $0 !~ /^static void window_main_loop_sw\(void\)/ {
            in_fn = 0
        }
        in_fn {
            print
        }
    ' window-sw.c
)"

[ -n "${main_loop_body}" ] ||
    fail "missing window_main_loop_sw implementation"

renderer_drain_line="$(
    awk 'index($0, "window_drain_renderer_queue();") { print NR; exit }' \
        <<<"${main_loop_body}"
)"
display_drain_line="$(
    awk 'index($0, "window_drain_display_queue();") { print NR; exit }' \
        <<<"${main_loop_body}"
)"

[ -n "${renderer_drain_line}" ] ||
    fail "SDL main loop must drain renderer requests"
[ -n "${display_drain_line}" ] ||
    fail "SDL main loop must still drain display requests"

if [ "${renderer_drain_line}" -ge "${display_drain_line}" ]; then
    fail "SDL main loop must drain renderer requests before display requests"
fi
