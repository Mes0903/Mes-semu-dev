# shellcheck shell=bash
# Sourced by .ci/prebuilt-flow/test-flow.sh.

test_resolve_artifacts_decisions() {
    local fixture
    local release
    local output=$tmp/resolve.out
    local image_key
    local rootfs_key
    local test_tools_key

    fixture=$tmp/resolve-release-match
    release=$tmp/resolve-release
    make_resolver_fixture "$fixture"
    (
        cd "$fixture"
        image_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)
        write_release_manifest "$release" "$image_key" "$rootfs_key" "$test_tools_key"
        rm -f Image rootfs.cpio test-tools.img
        PREBUILT_URL="file://$release" .ci/prebuilt/resolve-artifacts.sh > "$output"
    )
    assert_contains "$output" '^plan_version=1$'
    assert_contains "$output" '^image_action=download-release$'
    assert_contains "$output" '^rootfs_action=download-release$'
    assert_contains "$output" '^test_tools_action=download-release$'
    assert_contains "$output" '^requires_build=false$'
    assert_contains "$output" '^release_needs_update=false$'
    assert_contains "$output" '^platform_action_cache_tag=[0-9a-f]{40}$'
    assert_not_contains "$output" '^current_recipe_classes_sha1='
    assert_not_contains "$output" '^release_manifest_sha1='

    fixture=$tmp/resolve-release-miss
    release=$tmp/resolve-release-miss-release
    make_resolver_fixture "$fixture"
    (
        cd "$fixture"
        write_release_manifest "$release"             0000000000000000000000000000000000000000             8888888888888888888888888888888888888888             9999999999999999999999999999999999999999
        rm -f Image rootfs.cpio test-tools.img
        PREBUILT_URL="file://$release" .ci/prebuilt/resolve-artifacts.sh > "$output"
    )
    assert_contains "$output" '^image_action=build$'
    assert_contains "$output" '^rootfs_action=build$'
    assert_contains "$output" '^test_tools_action=build$'
    assert_contains "$output" '^requires_build=true$'
    assert_contains "$output" '^release_needs_update=true$'

    fixture=$tmp/resolve-cache-match
    release=$tmp/resolve-cache-match-release
    make_resolver_fixture "$fixture"
    (
        cd "$fixture"
        image_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)
        write_release_manifest "$release"             0000000000000000000000000000000000000000             8888888888888888888888888888888888888888             9999999999999999999999999999999999999999
        write_local_stamps "$PWD" "$image_key" "$rootfs_key" "$test_tools_key"
        PREBUILT_URL="file://$release" .ci/prebuilt/resolve-artifacts.sh > "$output"
    )
    assert_contains "$output" '^image_action=use-local$'
    assert_contains "$output" '^rootfs_action=use-local$'
    assert_contains "$output" '^test_tools_action=use-local$'
    assert_contains "$output" '^requires_build=false$'
    assert_contains "$output" '^release_needs_update=true$'

    fixture=$tmp/resolve-cache-stale-release-match
    release=$tmp/resolve-cache-stale-release-match-release
    make_resolver_fixture "$fixture"
    (
        cd "$fixture"
        image_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)
        write_release_manifest "$release" "$image_key" "$rootfs_key" "$test_tools_key"
        write_local_stamps "$PWD"             0000000000000000000000000000000000000000             8888888888888888888888888888888888888888             9999999999999999999999999999999999999999
        PREBUILT_URL="file://$release" .ci/prebuilt/resolve-artifacts.sh > "$output"
    )
    assert_contains "$output" '^image_action=download-release$'
    assert_contains "$output" '^rootfs_action=download-release$'
    assert_contains "$output" '^test_tools_action=download-release$'
    assert_contains "$output" '^requires_build=false$'

    fixture=$tmp/resolve-cache-unmanaged-release-match
    release=$tmp/resolve-cache-unmanaged-release-match-release
    make_resolver_fixture "$fixture"
    (
        cd "$fixture"
        image_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)
        write_release_manifest "$release" "$image_key" "$rootfs_key" "$test_tools_key"
        rm -rf .prebuilt
        PREBUILT_URL="file://$release" .ci/prebuilt/resolve-artifacts.sh > "$output"
    )
    assert_contains "$output" '^image_action=download-release$'
    assert_contains "$output" '^rootfs_action=download-release$'
    assert_contains "$output" '^test_tools_action=download-release$'
}

test_resolve_splits_rootfs_and_test_tools_classes() {
    local fixture=$tmp/resolve-split-classes
    local release=$tmp/resolve-split-classes-release
    local output=$tmp/resolve-split-classes.out
    local image_key
    local rootfs_key
    local test_tools_key

    make_resolver_fixture "$fixture"
    (
        cd "$fixture"
        image_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)
        write_release_manifest "$release" "$image_key" "$rootfs_key" "$test_tools_key"
        rm -f test-tools.img
        mkdir -p .prebuilt
        stamp_artifact "$PWD" Image "$image_key"
        stamp_artifact "$PWD" rootfs.cpio "$rootfs_key"
        PREBUILT_URL="file://$release" .ci/prebuilt/resolve-artifacts.sh > "$output"
    )

    assert_contains "$output" '^image_action=use-local$'
    assert_contains "$output" '^rootfs_action=use-local$'
    assert_contains "$output" '^test_tools_action=download-release$'
}

test_resolve_requested_classes_ignore_unrequested_missing_artifacts() {
    local fixture=$tmp/resolve-requested-classes
    local release=$tmp/resolve-requested-classes-release
    local output=$tmp/resolve-requested-classes.out
    local image_key
    local rootfs_key
    local test_tools_key

    make_resolver_fixture "$fixture"
    (
        cd "$fixture"
        image_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)
        write_release_manifest "$release" "$image_key" "$rootfs_key" "$test_tools_key"
        rm -f test-tools.img
        mkdir -p .prebuilt
        stamp_artifact "$PWD" Image "$image_key"
        stamp_artifact "$PWD" rootfs.cpio "$rootfs_key"
        PREBUILT_URL="file://$release" .ci/prebuilt/resolve-artifacts.sh image rootfs > "$output"
    )

    assert_contains "$output" '^image_action=use-local$'
    assert_contains "$output" '^rootfs_action=use-local$'
    assert_contains "$output" '^test_tools_action=skip$'
    assert_contains "$output" '^requires_build=false$'
}

test_resolve_uses_recipe_stamped_local_artifact_without_content_check() {
    local fixture=$tmp/resolve-recipe-stamped-local
    local release=$tmp/resolve-recipe-stamped-local-release
    local output=$tmp/resolve-recipe-stamped-local.out
    local image_key
    local rootfs_key
    local test_tools_key

    make_resolver_fixture "$fixture"
    (
        cd "$fixture"
        image_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_key=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)
        write_release_manifest "$release" "$image_key" "$rootfs_key" "$test_tools_key"
        mkdir -p .prebuilt
        stamp_artifact "$PWD" Image "$image_key"
        stamp_artifact "$PWD" rootfs.cpio "$rootfs_key"
        stamp_artifact "$PWD" test-tools.img "$test_tools_key"
        printf 'manual kernel
' > Image
        PREBUILT_URL="file://$release" .ci/prebuilt/resolve-artifacts.sh > "$output"
    )

    assert_contains "$output" '^image_action=use-local$'
    assert_contains "$output" '^rootfs_action=use-local$'
    assert_contains "$output" '^test_tools_action=use-local$'
}

test_resolver_fails_when_release_manifest_is_unavailable() {
    local fixture=$tmp/resolve-missing-release
    local release=$tmp/resolve-missing-release-dir
    local output=$tmp/resolve-missing-release.out
    local err=$tmp/resolve-missing-release.err

    make_resolver_fixture "$fixture"
    rm -rf "$release"
    (
        cd "$fixture"
        rm -f Image rootfs.cpio test-tools.img
        if PREBUILT_URL="file://$release" .ci/prebuilt/resolve-artifacts.sh > "$output" 2> "$err"; then
            echo "[!] resolve-artifacts unexpectedly accepted an unavailable release manifest" >&2
            exit 1
        fi
    )

    assert_contains "$err" 'prebuilt manifest is unavailable'

    (
        cd "$fixture"
        if PREBUILT_BOOTSTRAP_ON_404=1 PREBUILT_URL="file://$release"             .ci/prebuilt/resolve-artifacts.sh > "$output" 2> "$err"; then
            echo "[!] resolve-artifacts unexpectedly accepted a non-HTTP missing manifest" >&2
            exit 1
        fi
    )

    assert_contains "$err" 'prebuilt manifest is unavailable'
}

test_resolver_bootstraps_missing_http_release_when_enabled() {
    local fixture=$tmp/resolve-missing-http-release-bootstrap
    local webroot=$tmp/resolve-missing-http-release-bootstrap-web
    local port_file=$tmp/resolve-missing-http-release-bootstrap.port
    local output=$tmp/resolve-missing-http-release-bootstrap.out
    local err=$tmp/resolve-missing-http-release-bootstrap.err
    local server_pid
    local port

    make_resolver_fixture "$fixture"
    mkdir -p "$webroot"
    python3 - "$webroot" "$port_file" <<'PYHTTP' &
import functools
import http.server
import os
import socketserver
import sys

root, port_file = sys.argv[1], sys.argv[2]
os.chdir(root)
class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

handler = functools.partial(QuietHandler)
with socketserver.TCPServer(("127.0.0.1", 0), handler) as httpd:
    with open(port_file, "w", encoding="utf-8") as f:
        print(httpd.server_address[1], file=f)
    httpd.serve_forever()
PYHTTP
    # shellcheck disable=SC2031  # $! is intentionally read after starting the fixture server.
    server_pid=$!
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        if [ -s "$port_file" ]; then
            break
        fi
        sleep 0.1
    done
    if [ ! -s "$port_file" ]; then
        kill "$server_pid" 2>/dev/null || true
        echo "[!] HTTP fixture server did not start" >&2
        exit 1
    fi
    port=$(cat "$port_file")

    (
        cd "$fixture"
        rm -f Image rootfs.cpio test-tools.img
        PREBUILT_BOOTSTRAP_ON_404=1 PREBUILT_URL="http://127.0.0.1:$port"             .ci/prebuilt/resolve-artifacts.sh > "$output" 2> "$err"
    )
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true

    assert_contains "$output" '^image_action=build$'
    assert_contains "$output" '^rootfs_action=build$'
    assert_contains "$output" '^test_tools_action=build$'
    assert_contains "$output" '^requires_build=true$'
    assert_contains "$output" '^release_manifest_available=false$'
    assert_contains "$output" '^release_needs_update=true$'
    assert_contains "$err" 'prebuilt manifest is unavailable'
    assert_contains "$err" 'bootstrapping from source'
}
