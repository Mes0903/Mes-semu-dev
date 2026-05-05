#!/usr/bin/env bash

set -euo pipefail

fail()
{
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

if grep -Fq 'vinput_flush_pending_mouse_motion' virtio-input-event.c ||
    grep -Fq 'pending_mouse_motion_dx' virtio-input-event.c ||
    grep -Fq 'pending_mouse_motion_dy' virtio-input-event.c ||
    grep -Fq 'vinput_accumulate_mouse_motion' virtio-input-event.c; then
    fail "virtio-input SDL path should not coalesce mouse motion"
fi

grep -Fq 'vinput_publish_mouse_motion(e.motion.xrel, e.motion.yrel)' \
    virtio-input-event.c ||
    fail "virtio-input SDL path should publish each SDL mouse motion"

if grep -Fq 'vinput_flush_mouse_motion_cmd' virtio-input.c ||
    grep -Fq 'pending_mouse_motion_cmd' virtio-input.c ||
    grep -Fq 'virtio_input_accumulate_mouse_motion_cmd' virtio-input.c; then
    fail "virtio-input emulator drain should not coalesce mouse motion commands"
fi

grep -Fq 'virtio_input_update_mouse_motion(event.u.mouse_motion.dx,' \
    virtio-input.c ||
    fail "virtio-input emulator drain should forward each mouse motion command"
