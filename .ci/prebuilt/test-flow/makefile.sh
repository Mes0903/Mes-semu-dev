# shellcheck shell=bash
# Sourced by .ci/prebuilt/test-flow.sh.

write_external_makefile() {
    local dst=$1

    mkdir -p "$dst/scripts/prebuilt"
    cp "$SCRIPT_DIR/artifact-inputs.sh" \
       "$SCRIPT_DIR/resolve-artifacts.sh" \
       "$SCRIPT_DIR/stamp-artifacts.sh" \
       "$SCRIPT_DIR/materialize-artifacts.sh" \
       "$SCRIPT_DIR/plan-materialize.sh" \
       "$dst/scripts/prebuilt/"

    cat > "$dst/Makefile" <<EOF
Q := @
VECHO = printf '%b'
PRINTF = printf
SHA1SUM = sha1sum
.PHONY: FORCE
FORCE:

include $REPO_ROOT/mk/external.mk

PREBUILT_SCRIPT_DIR := \$(CURDIR)/scripts/prebuilt
PREBUILT_INPUTS_SCRIPT := \$(PREBUILT_SCRIPT_DIR)/artifact-inputs.sh
PREBUILT_RESOLVER := \$(PREBUILT_SCRIPT_DIR)/resolve-artifacts.sh
PREBUILT_PLAN_MATERIALIZER := \$(PREBUILT_SCRIPT_DIR)/plan-materialize.sh

smoke: Image rootfs.cpio test-tools.img
	@test -s Image
	@test -s rootfs.cpio
	@test -s test-tools.img

smoke-check: Image rootfs.cpio
	@test -s Image
	@test -s rootfs.cpio
	@test ! -e test-tools.img
EOF
}
test_make_uses_valid_local_artifacts_without_release() {
    local fixture=$tmp/make-local-valid
    local release=$tmp/make-local-valid-missing-release
    local image_key
    local rootfs_key
    local test_tools_key

    mkdir -p "$fixture"
    copy_prebuilt_inputs "$fixture"
    write_external_makefile "$fixture"

    printf 'kernel\n' > "$fixture/Image"
    printf 'rootfs\n' > "$fixture/rootfs.cpio"
    printf 'tools\n' > "$fixture/test-tools.img"
    (
        cd "$fixture"
        image_key=$(. "$REPO_ROOT/scripts/prebuilt/artifact-inputs.sh"; prebuilt_class_recipe_key image)
        rootfs_key=$(. "$REPO_ROOT/scripts/prebuilt/artifact-inputs.sh"; prebuilt_class_recipe_key rootfs)
        test_tools_key=$(. "$REPO_ROOT/scripts/prebuilt/artifact-inputs.sh"; prebuilt_class_recipe_key test-tools)
        write_local_stamps "$PWD" "$image_key" "$rootfs_key" "$test_tools_key"
    )

    PREBUILT_URL="file://$release" make -C "$fixture" smoke > "$tmp/make-local-valid.out" 2>&1
    assert_contains "$fixture/Image" '^kernel$'
    assert_contains "$fixture/rootfs.cpio" '^rootfs$'
    assert_contains "$fixture/test-tools.img" '^tools$'
}
test_make_prebuilt_plan_uses_local_first() {
    local fixture=$tmp/make-prebuilt-plan-local-first
    local release=$tmp/make-prebuilt-plan-missing-release
    local output=$tmp/make-prebuilt-plan-local-first.out
    local err=$tmp/make-prebuilt-plan-local-first.err
    local image_key
    local rootfs_key
    local test_tools_key

    mkdir -p "$fixture"
    copy_prebuilt_inputs "$fixture"
    write_external_makefile "$fixture"
    printf 'kernel\n' > "$fixture/Image"
    printf 'rootfs\n' > "$fixture/rootfs.cpio"
    printf 'tools\n' > "$fixture/test-tools.img"
    (
        cd "$fixture"
        image_key=$( . "$REPO_ROOT/scripts/prebuilt/artifact-inputs.sh"; prebuilt_class_recipe_key image )
        rootfs_key=$( . "$REPO_ROOT/scripts/prebuilt/artifact-inputs.sh"; prebuilt_class_recipe_key rootfs )
        test_tools_key=$( . "$REPO_ROOT/scripts/prebuilt/artifact-inputs.sh"; prebuilt_class_recipe_key test-tools )
        write_local_stamps "$PWD" "$image_key" "$rootfs_key" "$test_tools_key"
    )

    PREBUILT_URL="file://$release" make -C "$fixture" prebuilt-plan > "$output" 2> "$err"
    assert_contains "$output" '^image_action=use-local$'
    assert_contains "$output" '^rootfs_action=use-local$'
    assert_contains "$output" '^test_tools_action=use-local$'
    assert_not_contains "$err" 'prebuilt manifest is unavailable'
}
test_make_downloads_release_artifacts_through_materializer() {
    local fixture=$tmp/make-download
    local release=$tmp/make-download-release
    local image_key
    local rootfs_key
    local test_tools_key

    mkdir -p "$fixture" "$release"
    copy_prebuilt_inputs "$fixture"
    write_external_makefile "$fixture"
    (
        cd "$fixture"
        image_key=$(. "$REPO_ROOT/scripts/prebuilt/artifact-inputs.sh"; prebuilt_class_recipe_key image)
        rootfs_key=$(. "$REPO_ROOT/scripts/prebuilt/artifact-inputs.sh"; prebuilt_class_recipe_key rootfs)
        test_tools_key=$(. "$REPO_ROOT/scripts/prebuilt/artifact-inputs.sh"; prebuilt_class_recipe_key test-tools)
        write_release_with_archives "$release" "$image_key" "$rootfs_key" "$test_tools_key"
    )

    PREBUILT_URL="file://$release" make -C "$fixture" smoke > "$tmp/make-download.out" 2>&1
    assert_contains "$fixture/Image" '^release kernel$'
    assert_contains "$fixture/rootfs.cpio" '^release rootfs$'
    assert_contains "$fixture/test-tools.img" '^release tools$'
    assert_contains "$fixture/.prebuilt/Image.recipe-key" '^[0-9a-f]{40}$'
    assert_not_contains "$tmp/make-download.out" '^downloaded_any='
    assert_not_contains "$tmp/make-download.out" '^built_any='
}
test_make_check_materializes_only_default_guest_artifacts() {
    local fixture=$tmp/make-check-default-artifacts
    local release=$tmp/make-check-default-artifacts-release
    local image_key
    local rootfs_key
    local test_tools_key

    mkdir -p "$fixture" "$release"
    copy_prebuilt_inputs "$fixture"
    write_external_makefile "$fixture"
    (
        cd "$fixture"
        image_key=$(. "$REPO_ROOT/scripts/prebuilt/artifact-inputs.sh"; prebuilt_class_recipe_key image)
        rootfs_key=$(. "$REPO_ROOT/scripts/prebuilt/artifact-inputs.sh"; prebuilt_class_recipe_key rootfs)
        test_tools_key=$(. "$REPO_ROOT/scripts/prebuilt/artifact-inputs.sh"; prebuilt_class_recipe_key test-tools)
        write_release_with_archives "$release" "$image_key" "$rootfs_key" "$test_tools_key"
    )

    PREBUILT_URL="file://$release" make -C "$fixture" smoke-check > "$tmp/make-check-default-artifacts.out" 2>&1
    assert_contains "$fixture/Image" '^release kernel$'
    assert_contains "$fixture/rootfs.cpio" '^release rootfs$'
    test ! -e "$fixture/test-tools.img"
    test ! -e "$fixture/.prebuilt/test-tools.img.recipe-key"
}
test_make_blocks_unmanaged_local_artifacts_in_non_strict_mode() {
    local fixture=$tmp/make-unmanaged
    local release=$tmp/make-unmanaged-release
    local output=$tmp/make-unmanaged.out
    local image_key
    local rootfs_key
    local test_tools_key

    mkdir -p "$fixture" "$release"
    copy_prebuilt_inputs "$fixture"
    write_external_makefile "$fixture"
    printf 'kernel\n' > "$fixture/Image"
    printf 'rootfs\n' > "$fixture/rootfs.cpio"
    printf 'tools\n' > "$fixture/test-tools.img"
    (
        cd "$fixture"
        image_key=$(. "$REPO_ROOT/scripts/prebuilt/artifact-inputs.sh"; prebuilt_class_recipe_key image)
        rootfs_key=$(. "$REPO_ROOT/scripts/prebuilt/artifact-inputs.sh"; prebuilt_class_recipe_key rootfs)
        test_tools_key=$(. "$REPO_ROOT/scripts/prebuilt/artifact-inputs.sh"; prebuilt_class_recipe_key test-tools)
        write_release_with_archives "$release" "$image_key" "$rootfs_key" "$test_tools_key"
    )

    if PREBUILT_URL="file://$release" make -C "$fixture" smoke > "$output" 2>&1; then
        echo "[!] make unexpectedly overwrote unmanaged local artifacts" >&2
        cat "$output" >&2
        exit 1
    fi

    assert_contains "$output" 'unmanaged'
    assert_contains "$output" '^\[!\] Plan:$'
    assert_contains "$output" '^\[!\][[:space:]]+image:[[:space:]]+blocked-unmanaged$'
    build_line=$(grep -n 'make build-image' "$output" | cut -d: -f1)
    strict_line=$(grep -n 'PREBUILT_STRICT=1' "$output" | cut -d: -f1)
    ignore_line=$(grep -n 'PREBUILT_IGNORE_SHA=1' "$output" | cut -d: -f1)
    [ "$build_line" -lt "$strict_line" ]
    [ "$strict_line" -lt "$ignore_line" ]
    assert_contains "$fixture/Image" '^kernel$'
}
test_make_uses_unstamped_local_artifacts_when_sha_ignored() {
    local fixture=$tmp/make-ignore-sha-local
    local release=$tmp/make-ignore-sha-missing-release

    mkdir -p "$fixture"
    copy_prebuilt_inputs "$fixture"
    write_external_makefile "$fixture"
    printf 'manual kernel\n' > "$fixture/Image"
    printf 'manual rootfs\n' > "$fixture/rootfs.cpio"
    printf 'manual tools\n' > "$fixture/test-tools.img"

    PREBUILT_IGNORE_SHA=1 PREBUILT_URL="file://$release" make -C "$fixture" smoke > "$tmp/make-ignore-sha.out" 2>&1
    assert_contains "$fixture/Image" '^manual kernel$'
    assert_contains "$fixture/rootfs.cpio" '^manual rootfs$'
    assert_contains "$fixture/test-tools.img" '^manual tools$'
}
test_make_strict_refreshes_unmanaged_local_artifacts() {
    local fixture=$tmp/make-strict-unmanaged
    local release=$tmp/make-strict-unmanaged-release
    local image_key
    local rootfs_key
    local test_tools_key

    mkdir -p "$fixture" "$release"
    copy_prebuilt_inputs "$fixture"
    write_external_makefile "$fixture"
    printf 'kernel\n' > "$fixture/Image"
    printf 'rootfs\n' > "$fixture/rootfs.cpio"
    printf 'tools\n' > "$fixture/test-tools.img"
    (
        cd "$fixture"
        image_key=$(. "$REPO_ROOT/scripts/prebuilt/artifact-inputs.sh"; prebuilt_class_recipe_key image)
        rootfs_key=$(. "$REPO_ROOT/scripts/prebuilt/artifact-inputs.sh"; prebuilt_class_recipe_key rootfs)
        test_tools_key=$(. "$REPO_ROOT/scripts/prebuilt/artifact-inputs.sh"; prebuilt_class_recipe_key test-tools)
        write_release_with_archives "$release" "$image_key" "$rootfs_key" "$test_tools_key"
    )

    PREBUILT_STRICT=1 PREBUILT_URL="file://$release" make -C "$fixture" smoke > "$tmp/make-strict-unmanaged.out" 2>&1
    assert_contains "$fixture/Image" '^release kernel$'
    assert_contains "$fixture/rootfs.cpio" '^release rootfs$'
    assert_contains "$fixture/test-tools.img" '^release tools$'
}
test_make_strict_builds_when_release_inputs_do_not_match() {
    local fixture=$tmp/make-strict-build
    local release=$tmp/make-strict-build-release

    mkdir -p "$fixture" "$release"
    copy_prebuilt_inputs "$fixture"
    write_external_makefile "$fixture"
    cat > "$fixture/scripts/prebuilt/build-plan-artifacts.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> build-plan-artifacts.args
case "$*" in
    image)
        printf 'built image\n' > Image
        ;;
    rootfs)
        printf 'built rootfs\n' > rootfs.cpio
        ;;
    test-tools)
        printf 'built tools\n' > test-tools.img
        ;;
    *)
        echo "unexpected internal builder args: $*" >&2
        exit 1
        ;;
esac
EOF
    chmod +x "$fixture/scripts/prebuilt/build-plan-artifacts.sh"
    write_release_manifest "$release" \
        0000000000000000000000000000000000000000 \
        8888888888888888888888888888888888888888 \
        9999999999999999999999999999999999999999

    PREBUILT_STRICT=1 PREBUILT_URL="file://$release" make -C "$fixture" smoke > "$tmp/make-strict-build.out" 2>&1
    assert_contains "$fixture/build-plan-artifacts.args" '^image$'
    assert_contains "$fixture/build-plan-artifacts.args" '^rootfs$'
    assert_contains "$fixture/build-plan-artifacts.args" '^test-tools$'
    assert_contains "$fixture/Image" '^built image$'
    assert_contains "$fixture/rootfs.cpio" '^built rootfs$'
    assert_contains "$fixture/test-tools.img" '^built tools$'
    assert_contains "$fixture/.prebuilt/Image.recipe-key" '^[0-9a-f]{40}$'
    assert_contains "$fixture/.prebuilt/rootfs.cpio.recipe-key" '^[0-9a-f]{40}$'
    assert_contains "$fixture/.prebuilt/test-tools.img.recipe-key" '^[0-9a-f]{40}$'
}
test_make_build_image_stamps_built_outputs() {
    local fixture=$tmp/make-build-image-stamps

    mkdir -p "$fixture/scripts" "$fixture/scripts/prebuilt"
    copy_prebuilt_inputs "$fixture"
    cp "$REPO_ROOT/scripts/build-image.sh" "$fixture/scripts/"
    cp "$SCRIPT_DIR/artifact-inputs.sh" \
       "$SCRIPT_DIR/stamp-artifacts.sh" \
       "$fixture/scripts/prebuilt/"
    cat > "$fixture/scripts/prebuilt/build-artifacts.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
OK() { :; }
do_linux() { printf 'built image\n' > Image; }
do_rootfs() { printf 'built rootfs\n' > rootfs.cpio; }
do_test_tools() { printf 'built tools\n' > test-tools.img; }
EOF
    cat > "$fixture/Makefile" <<'EOF'
BUILD_IMAGE_ARGS ?= all
build-image:
	scripts/build-image.sh $(BUILD_IMAGE_ARGS)
EOF

    make -C "$fixture" build-image > "$tmp/make-build-image-stamps.out" 2>&1
    assert_contains "$fixture/.prebuilt/Image.recipe-key" '^[0-9a-f]{40}$'
    assert_contains "$fixture/.prebuilt/rootfs.cpio.recipe-key" '^[0-9a-f]{40}$'
    assert_contains "$fixture/.prebuilt/test-tools.img.recipe-key" '^[0-9a-f]{40}$'
}
test_make_distclean_removes_packaged_artifacts() {
    local output=$tmp/make-distclean.out

    (
        cd "$REPO_ROOT"
        make --no-print-directory -n distclean > "$output"
    )

    assert_contains "$output" 'Image\.bz2'
    assert_contains "$output" 'rootfs\.cpio\.bz2'
    assert_contains "$output" 'test-tools\.img\.bz2'
}
test_make_check_accepts_run_flags() {
    local default_output=$tmp/make-check-default.out
    local custom_output=$tmp/make-check-custom.out
    local -a make_args=(
        --no-print-directory
        -n
        check
        OBJS=
        GDBSTUB_LIB=
        ENABLE_VIRTIOSND=0
        ENABLE_VIRTIONET=0
    )

    (
        cd "$REPO_ROOT"
        make "${make_args[@]}" > "$default_output"
        make "${make_args[@]}" RUN_HEADLESS=0 RUN_DISK=test-tools.img > "$custom_output"
    )

    assert_not_contains "$default_output" 'mini-gdbstub|portaudio|minislirp'
    assert_contains "$default_output" '^\./semu .* -H .* -d ext4\.img'
    assert_contains "$custom_output" '^\./semu .* -d test-tools\.img'
    assert_not_contains "$custom_output" '^\./semu .* -H'
    assert_not_contains "$custom_output" 'rootfs_ext4\.sh rootfs\.cpio ext4\.img'
}
