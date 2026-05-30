#!/usr/bin/env bash

# Build-layer helper for selecting optional test-tools.img payloads.  CI recipe
# keys source this same helper so payload selection affects both local builds and
# CI cache/release decisions without making the build recipe depend on CI state.
prebuilt_test_tools_payloads() {
    local raw=${PREBUILT_TEST_TOOLS_PAYLOADS+x}
    local payloads=${PREBUILT_TEST_TOOLS_PAYLOADS-}
    local item
    local want_x11=false
    local want_directfb2=false

    if [ -z "$raw" ]; then
        printf '%s\n' x11 directfb2
        return 0
    fi

    payloads=${payloads//,/ }
    for item in $payloads; do
        case "$item" in
            minimal)
                ;;
            x11)
                want_x11=true
                ;;
            directfb2)
                want_directfb2=true
                ;;
            *)
                echo "[!] Unknown test-tools payload: $item" >&2
                echo "[!] Expected payloads: x11, directfb2, minimal" >&2
                return 1
                ;;
        esac
    done

    # The DirectFB2 smoke-test payload is built against the x11 Buildroot mode
    # today because that mode provides the C++ runtime support it needs.
    if [ "$want_directfb2" = true ]; then
        want_x11=true
    fi

    if [ "$want_x11" = true ]; then
        printf '%s\n' x11
    fi
    if [ "$want_directfb2" = true ]; then
        printf '%s\n' directfb2
    fi
}

prebuilt_test_tools_payload_is_enabled() {
    local want=$1
    local payload
    local payload_list

    payload_list=$(prebuilt_test_tools_payloads) || return 1
    while IFS= read -r payload; do
        if [ "$payload" = "$want" ]; then
            return 0
        fi
    done <<< "$payload_list"

    return 1
}

prebuilt_test_tools_payload_key() {
    local payload
    local payload_list
    local result=

    payload_list=$(prebuilt_test_tools_payloads) || return 1
    while IFS= read -r payload; do
        if [ -z "$payload" ]; then
            continue
        fi
        if [ -n "$result" ]; then
            result=$result,$payload
        else
            result=$payload
        fi
    done <<< "$payload_list"

    if [ -n "$result" ]; then
        printf '%s\n' "$result"
    else
        printf '%s\n' minimal
    fi
}
