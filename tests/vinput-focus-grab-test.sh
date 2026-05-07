#!/usr/bin/env bash

set -euo pipefail

fail()
{
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

grep -Fq 'vinput_suspend_keyboard_until_regrab();' virtio-input-event.c ||
    fail "SDL focus loss must suspend keyboard forwarding until regrab"

grep -Fq 'vinput_release_pressed_keys();' virtio-input-event.c ||
    fail "SDL focus loss must release guest-visible pressed keys"

grep -Fq 'vinput_keyboard_forwarding_enabled = false;' virtio-input-event.c ||
    fail "focus loss must disable keyboard forwarding"

grep -Fq 'vinput_keyboard_forwarding_enabled = true;' virtio-input-event.c ||
    fail "mouse regrab must re-enable keyboard forwarding"

grep -Fq '!vinput_keyboard_forwarding_enabled' virtio-input-event.c ||
    fail "keyboard events must be dropped while forwarding is suspended"

grep -Fq 'Host Alt+Tab' virtio-input-event.c ||
    fail "focus-grab keyboard suppression needs an explanatory comment"
