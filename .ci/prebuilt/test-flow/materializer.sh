# shellcheck shell=bash
# Sourced by .ci/prebuilt/test-flow.sh.

test_materialize_downloads_release_and_writes_stamps() {
    local fixture=$tmp/materialize-release
    local release=$tmp/materialize-release-assets
    local output=$tmp/materialize.out
    local err=$tmp/materialize.err
    local image_key
    local rootfs_key
    local test_tools_key

    make_materializer_fixture "$fixture"
    (
        cd "$fixture"
        image_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)
        write_release_with_archives "$release" "$image_key" "$rootfs_key" "$test_tools_key"
        rm -f Image rootfs.cpio test-tools.img
        PREBUILT_URL="file://$release" scripts/prebuilt/plan-materialize.sh > "$output" 2> "$err"
        PREBUILT_VERBOSE=1 PREBUILT_URL="file://$release" scripts/prebuilt/plan-materialize.sh > "$tmp/materialize-verbose.out" 2> "$tmp/materialize-verbose.err"
    )

    assert_contains "$fixture/Image" '^release kernel$'
    assert_contains "$fixture/rootfs.cpio" '^release rootfs$'
    assert_contains "$fixture/test-tools.img" '^release tools$'
    assert_contains "$fixture/.prebuilt/Image.recipe-key" '^[0-9a-f]{40}$'
    assert_contains "$fixture/.prebuilt/rootfs.cpio.recipe-key" '^[0-9a-f]{40}$'
    assert_contains "$fixture/.prebuilt/test-tools.img.recipe-key" '^[0-9a-f]{40}$'
    assert_contains "$output" '^downloaded_any=true$'
    assert_contains "$output" '^built_any=false$'
    assert_not_contains "$output" '^image_action='
    assert_not_contains "$err" '^image_action='
    assert_contains "$tmp/materialize-verbose.err" '^image_action=use-local$'
}
test_materialize_consumes_supplied_plan_without_resolving_again() {
    local fixture=$tmp/materialize-plan-file
    local output=$tmp/materialize-plan-file.out
    local plan=$tmp/materialize-plan-file.env
    local image_key=1111111111111111111111111111111111111111
    local rootfs_key=2222222222222222222222222222222222222222
    local test_tools_key=3333333333333333333333333333333333333333

    make_materializer_fixture "$fixture"
    cat > "$fixture/scripts/prebuilt/build-plan-artifacts.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> build-plan-artifacts.args
case "$*" in
    image)
        printf 'built image\n' > Image
        ;;
    *)
        echo "unexpected internal builder args: $*" >&2
        exit 1
        ;;
esac
EOF
    chmod +x "$fixture/scripts/prebuilt/build-plan-artifacts.sh"
    cat > "$fixture/scripts/prebuilt/resolve-artifacts.sh" <<'EOF'
#!/usr/bin/env bash
echo "resolver should not run when PREBUILT_PLAN_FILE is set" >&2
exit 99
EOF
    chmod +x "$fixture/scripts/prebuilt/resolve-artifacts.sh"
    cat > "$plan" <<EOF
plan_version=1
image_action=build
rootfs_action=use-local
test_tools_action=use-local
current_image_recipe_key=$image_key
current_rootfs_recipe_key=$rootfs_key
current_test_tools_recipe_key=$test_tools_key
requires_build=true
blocked=false
EOF

    (
        cd "$fixture"
        PREBUILT_URL="file://$fixture/missing-release" \
            PREBUILT_PLAN_FILE="$plan" \
            scripts/prebuilt/materialize-artifacts.sh > "$output"
    )

    assert_contains "$fixture/build-plan-artifacts.args" '^image$'
    assert_contains "$output" '^built_any=true$'
}
test_materialize_rejects_class_args() {
    local fixture=$tmp/materialize-rejects-class-args
    local output=$tmp/materialize-rejects-class-args.out
    local plan=$tmp/materialize-rejects-class-args.env

    make_materializer_fixture "$fixture"
    cat > "$plan" <<'EOF'
plan_version=1
image_action=use-local
rootfs_action=use-local
test_tools_action=use-local
blocked=false
EOF

    if (
        cd "$fixture"
        PREBUILT_URL="file://$fixture/missing-release" \
            PREBUILT_PLAN_FILE="$plan" \
            scripts/prebuilt/materialize-artifacts.sh image > "$output" 2>&1
    ); then
        echo "[!] materializer unexpectedly accepted class arguments" >&2
        exit 1
    fi

    assert_contains "$output" 'materialize-artifacts.sh does not accept class arguments'
}
test_materialize_does_not_require_url_without_download() {
    local fixture=$tmp/materialize-no-url-no-download
    local output=$tmp/materialize-no-url-no-download.out
    local plan=$tmp/materialize-no-url-no-download.env

    make_materializer_fixture "$fixture"
    cat > "$plan" <<'EOF'
plan_version=1
image_action=use-local
rootfs_action=use-local
test_tools_action=skip
blocked=false
EOF

    (
        cd "$fixture"
        env -u PREBUILT_URL \
            PREBUILT_PLAN_FILE="$plan" \
            scripts/prebuilt/materialize-artifacts.sh > "$output"
    )

    assert_contains "$output" '^downloaded_any=false$'
    assert_contains "$output" '^built_any=false$'
}
test_materialize_requires_supplied_plan() {
    local fixture=$tmp/materialize-requires-plan
    local output=$tmp/materialize-requires-plan.out

    make_materializer_fixture "$fixture"
    if (
        cd "$fixture"
        PREBUILT_URL="file://$fixture/missing-release" \
            scripts/prebuilt/materialize-artifacts.sh > "$output" 2>&1
    ); then
        echo "[!] materializer unexpectedly resolved its own plan" >&2
        exit 1
    fi

    assert_contains "$output" 'PREBUILT_PLAN_FILE is required'
}
test_materialize_rejects_unknown_plan_keys() {
    local fixture=$tmp/materialize-unknown-plan-key
    local output=$tmp/materialize-unknown-plan-key.out
    local plan=$tmp/materialize-unknown-plan-key.env

    make_materializer_fixture "$fixture"
    cat > "$plan" <<'EOF'
plan_version=1
image_action=use-local
rootfs_action=use-local
test_tools_action=use-local
unexpected_future_key=true
blocked=false
EOF

    if (
        cd "$fixture"
        PREBUILT_URL="file://$fixture/missing-release" \
            PREBUILT_PLAN_FILE="$plan" \
            scripts/prebuilt/materialize-artifacts.sh > "$output" 2>&1
    ); then
        echo "[!] materializer unexpectedly accepted an unknown plan key" >&2
        exit 1
    fi

    assert_contains "$output" 'Unknown prebuilt plan key: unexpected_future_key'
}
test_materialize_blocked_message_lists_selected_class_actions() {
    local fixture=$tmp/materialize-blocked-message
    local output=$tmp/materialize-blocked-message.out
    local plan=$tmp/materialize-blocked-message.env

    make_materializer_fixture "$fixture"
    cat > "$plan" <<'EOF'
plan_version=1
image_action=use-local
rootfs_action=blocked-stale
test_tools_action=blocked-unmanaged
blocked=true
EOF

    if (
        cd "$fixture"
        PREBUILT_URL="file://$fixture/missing-release" \
            PREBUILT_PLAN_FILE="$plan" \
            scripts/prebuilt/materialize-artifacts.sh > "$output" 2>&1
    ); then
        echo "[!] materializer unexpectedly accepted a blocked plan" >&2
        exit 1
    fi

    assert_contains "$output" '^\[!\] Plan:$'
    assert_contains "$output" '^\[!\][[:space:]]+image:[[:space:]]+use-local$'
    assert_contains "$output" '^\[!\][[:space:]]+rootfs:[[:space:]]+blocked-stale$'
    assert_contains "$output" '^\[!\][[:space:]]+test-tools:[[:space:]]+blocked-unmanaged$'
}
test_materialize_build_uses_prebuilt_internal_builder() {
    local fixture=$tmp/materialize-internal-builder
    local output=$tmp/materialize-internal-builder.out
    local plan=$tmp/materialize-internal-builder.env
    local image_key=1111111111111111111111111111111111111111
    local rootfs_key=2222222222222222222222222222222222222222
    local test_tools_key=3333333333333333333333333333333333333333

    make_materializer_fixture "$fixture"
    cat > "$fixture/scripts/build-image.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
echo "build-image.sh should not be used by materialize-artifacts.sh" >&2
exit 99
EOF
    chmod +x "$fixture/scripts/build-image.sh"
    cat > "$fixture/scripts/prebuilt/build-plan-artifacts.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" > build-plan-artifacts.args
case "$*" in
    image)
        printf 'built image\n' > Image
        ;;
    *)
        echo "unexpected internal builder args: $*" >&2
        exit 1
        ;;
esac
EOF
    chmod +x "$fixture/scripts/prebuilt/build-plan-artifacts.sh"
    cat > "$plan" <<EOF
plan_version=1
image_action=build
rootfs_action=use-local
test_tools_action=use-local
current_image_recipe_key=$image_key
current_rootfs_recipe_key=$rootfs_key
current_test_tools_recipe_key=$test_tools_key
blocked=false
EOF

    (
        cd "$fixture"
        PREBUILT_URL="file://$fixture/missing-release" \
            PREBUILT_PLAN_FILE="$plan" \
            scripts/prebuilt/materialize-artifacts.sh > "$output"
    )

    assert_contains "$fixture/build-plan-artifacts.args" '^image$'
    assert_contains "$fixture/Image" '^built image$'
    assert_contains "$output" '^built_any=true$'
}
test_materialize_action_is_authoritative_for_builds() {
    local fixture=$tmp/materialize-action-authoritative
    local output=$tmp/materialize-action-authoritative.out
    local plan=$tmp/materialize-action-authoritative.env
    local image_key=1111111111111111111111111111111111111111
    local rootfs_key=2222222222222222222222222222222222222222
    local test_tools_key=3333333333333333333333333333333333333333

    make_materializer_fixture "$fixture"
    cat > "$fixture/scripts/prebuilt/build-plan-artifacts.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> build-plan-artifacts.args
case "$*" in
    image)
        printf 'built image\n' > Image
        ;;
    *)
        echo "unexpected internal builder args: $*" >&2
        exit 1
        ;;
esac
EOF
    chmod +x "$fixture/scripts/prebuilt/build-plan-artifacts.sh"
    cat > "$plan" <<EOF
plan_version=1
image_action=build
rootfs_action=use-local
test_tools_action=use-local
current_image_recipe_key=$image_key
current_rootfs_recipe_key=$rootfs_key
current_test_tools_recipe_key=$test_tools_key
requires_build=false
blocked=false
EOF

    (
        cd "$fixture"
        PREBUILT_URL="file://$fixture/missing-release" \
            PREBUILT_PLAN_FILE="$plan" \
            scripts/prebuilt/materialize-artifacts.sh > "$output"
    )

    assert_contains "$fixture/build-plan-artifacts.args" '^image$'
    assert_contains "$fixture/Image" '^built image$'
    assert_contains "$output" '^built_any=true$'
}
test_materialize_rejects_unsupported_plan_version() {
    local fixture=$tmp/materialize-plan-version
    local output=$tmp/materialize-plan-version.out
    local plan=$tmp/materialize-plan-version.env

    make_materializer_fixture "$fixture"
    cat > "$plan" <<'EOF'
plan_version=2
image_action=use-local
rootfs_action=use-local
test_tools_action=use-local
blocked=false
EOF

    if (
        cd "$fixture"
        PREBUILT_URL="file://$fixture/missing-release" \
            PREBUILT_PLAN_FILE="$plan" \
            scripts/prebuilt/materialize-artifacts.sh > "$output" 2>&1
    ); then
        echo "[!] materializer unexpectedly accepted unsupported plan version" >&2
        exit 1
    fi

    assert_contains "$output" 'Unsupported prebuilt plan version'
}
test_materialize_accepts_recipe_only_release_manifest() {
    local fixture=$tmp/materialize-recipe-only-release
    local release=$tmp/materialize-recipe-only-release-assets
    local output=$tmp/materialize-recipe-only.out
    local image_key
    local rootfs_key
    local test_tools_key

    make_materializer_fixture "$fixture"
    (
        cd "$fixture"
        image_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)
        write_release_with_archives "$release" "$image_key" "$rootfs_key" "$test_tools_key"
        assert_not_contains "$release/prebuilt.sha1" '^[0-9a-f]{40}  Image\.bz2$'
        rm -f Image rootfs.cpio test-tools.img
        PREBUILT_URL="file://$release" scripts/prebuilt/plan-materialize.sh > "$output"
    )

    assert_contains "$fixture/Image" '^release kernel$'
    assert_contains "$fixture/rootfs.cpio" '^release rootfs$'
    assert_contains "$fixture/test-tools.img" '^release tools$'
    assert_contains "$output" '^downloaded_any=true$'
    assert_contains "$fixture/.prebuilt/Image.recipe-key" '^[0-9a-f]{40}$'
}
test_plan_materialize_reuses_current_local_artifacts_without_url() {
    local fixture=$tmp/plan-materialize-no-url-local
    local output=$tmp/plan-materialize-no-url-local.out
    local image_key
    local rootfs_key
    local test_tools_key

    make_materializer_fixture "$fixture"
    (
        cd "$fixture"
        image_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)
        write_local_stamps "$PWD" "$image_key" "$rootfs_key" "$test_tools_key"
        env -u PREBUILT_URL scripts/prebuilt/plan-materialize.sh > "$output"
    )

    assert_contains "$output" '^downloaded_any=false$'
    assert_contains "$output" '^built_any=false$'
    assert_contains "$fixture/Image" '^kernel$'
    assert_contains "$fixture/rootfs.cpio" '^rootfs$'
    assert_contains "$fixture/test-tools.img" '^tools$'
}
test_materialize_requested_classes_only() {
    local fixture=$tmp/materialize-requested-classes
    local release=$tmp/materialize-requested-classes-assets
    local output=$tmp/materialize-requested-classes.out
    local image_key
    local rootfs_key
    local test_tools_key

    make_materializer_fixture "$fixture"
    (
        cd "$fixture"
        image_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)
        write_release_with_archives "$release" "$image_key" "$rootfs_key" "$test_tools_key"
        rm -f Image rootfs.cpio test-tools.img
        PREBUILT_URL="file://$release" scripts/prebuilt/plan-materialize.sh image rootfs > "$output"
    )

    assert_contains "$fixture/Image" '^release kernel$'
    assert_contains "$fixture/rootfs.cpio" '^release rootfs$'
    test ! -e "$fixture/test-tools.img"
    assert_contains "$fixture/.prebuilt/Image.recipe-key" '^[0-9a-f]{40}$'
    assert_contains "$fixture/.prebuilt/rootfs.cpio.recipe-key" '^[0-9a-f]{40}$'
    test ! -e "$fixture/.prebuilt/test-tools.img.recipe-key"
    assert_contains "$output" '^downloaded_any=true$'
}
test_materialize_requested_classes_reuse_local_without_fetching_unrequested() {
    local fixture=$tmp/materialize-requested-local-no-fetch
    local release=$tmp/materialize-requested-local-missing-release
    local output=$tmp/materialize-requested-local-no-fetch.out
    local err=$tmp/materialize-requested-local-no-fetch.err
    local image_key
    local rootfs_key

    make_materializer_fixture "$fixture"
    (
        cd "$fixture"
        image_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        rm -f test-tools.img
        mkdir -p .prebuilt
        stamp_artifact "$PWD" Image "$image_key"
        stamp_artifact "$PWD" rootfs.cpio "$rootfs_key"
        PREBUILT_URL="file://$release" \
            scripts/prebuilt/plan-materialize.sh image rootfs > "$output" 2> "$err"
    )

    assert_contains "$fixture/Image" '^kernel$'
    assert_contains "$fixture/rootfs.cpio" '^rootfs$'
    test ! -e "$fixture/test-tools.img"
    assert_contains "$output" '^downloaded_any=false$'
    assert_contains "$output" '^built_any=false$'
    assert_not_contains "$err" 'prebuilt manifest is unavailable'
}
test_materialize_strict_refreshes_stale_local_cache() {
    local fixture=$tmp/materialize-stale-cache
    local release=$tmp/materialize-stale-cache-assets
    local output=$tmp/materialize-stale.out
    local image_key
    local rootfs_key
    local test_tools_key

    make_materializer_fixture "$fixture"
    (
        cd "$fixture"
        image_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)
        write_release_with_archives "$release" "$image_key" "$rootfs_key" "$test_tools_key"
        write_local_stamps "$PWD" \
            0000000000000000000000000000000000000000 \
            8888888888888888888888888888888888888888 \
            9999999999999999999999999999999999999999
        PREBUILT_STRICT=1 PREBUILT_URL="file://$release" scripts/prebuilt/plan-materialize.sh > "$output"
    )

    assert_contains "$fixture/Image" '^release kernel$'
    assert_contains "$fixture/rootfs.cpio" '^release rootfs$'
    assert_contains "$fixture/test-tools.img" '^release tools$'
    assert_contains "$output" '^downloaded_any=true$'
}
test_materialize_image_build_preserves_valid_local_rootfs() {
    local fixture=$tmp/materialize-image-build
    local release=$tmp/materialize-image-build-release
    local output=$tmp/materialize-image-build.out
    local image_key
    local rootfs_key
    local test_tools_key

    make_materializer_fixture "$fixture"
    cat > "$fixture/scripts/prebuilt/build-plan-artifacts.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> build-plan-artifacts.args
case "$*" in
    image)
        printf 'built image\n' > Image
        ;;
    *)
        echo "unexpected internal builder args: $*" >&2
        exit 1
        ;;
esac
EOF
    chmod +x "$fixture/scripts/prebuilt/build-plan-artifacts.sh"
    (
        cd "$fixture"
        image_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)
        write_release_manifest "$release" \
            0000000000000000000000000000000000000000 \
            "$rootfs_key" \
            "$test_tools_key"
        rm -f Image .prebuilt/Image.recipe-key
        printf 'local rootfs\n' > rootfs.cpio
        printf 'local tools\n' > test-tools.img
        mkdir -p .prebuilt
        stamp_artifact "$PWD" rootfs.cpio "$rootfs_key"
        stamp_artifact "$PWD" test-tools.img "$test_tools_key"
        PREBUILT_STRICT=1 PREBUILT_URL="file://$release" scripts/prebuilt/plan-materialize.sh > "$output"
    )

    assert_contains "$fixture/build-plan-artifacts.args" '^image$'
    assert_contains "$fixture/Image" '^built image$'
    assert_contains "$fixture/rootfs.cpio" '^local rootfs$'
    assert_contains "$fixture/test-tools.img" '^local tools$'
    assert_contains "$output" '^built_any=true$'
}
test_resolver_and_materializer_run_from_outside_repo_root() {
    local fixture=$tmp/cwd-fixture
    local release=$tmp/cwd-release
    local output=$tmp/cwd.out
    local image_key
    local rootfs_key
    local test_tools_key

    make_materializer_fixture "$fixture"
    (
        cd "$fixture"
        image_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_key=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)
        write_release_with_archives "$release" "$image_key" "$rootfs_key" "$test_tools_key"
        rm -f Image rootfs.cpio test-tools.img
    )

    (cd "$tmp" && PREBUILT_URL="file://$release" "$fixture/scripts/prebuilt/resolve-artifacts.sh" > "$output")
    assert_contains "$output" '^image_action=download-release$'

    (cd "$tmp" && PREBUILT_URL="file://$release" "$fixture/scripts/prebuilt/plan-materialize.sh" > "$output")
    assert_contains "$fixture/Image" '^release kernel$'
    assert_contains "$fixture/rootfs.cpio" '^release rootfs$'
    assert_contains "$fixture/test-tools.img" '^release tools$'
}
