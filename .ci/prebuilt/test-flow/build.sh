# shellcheck shell=bash
# Sourced by .ci/prebuilt/test-flow.sh.

test_recipe_uses_immutable_external_revisions() {
    assert_contains "$REPO_ROOT/scripts/prebuilt/artifact-recipe-buildroot.env" '^BUILDROOT_REV=[0-9a-f]{40}$'
    assert_contains "$REPO_ROOT/scripts/prebuilt/artifact-recipe-linux.env" '^LINUX_REV=[0-9a-f]{40}$'
    assert_contains "$REPO_ROOT/scripts/prebuilt/artifact-recipe-test-tools.env" '^DIRECTFB2_REV=[0-9a-f]{40}$'
    assert_contains "$REPO_ROOT/scripts/prebuilt/artifact-recipe-test-tools.env" '^DIRECTFB_EXAMPLES_REV=[0-9a-f]{40}$'
    assert_not_contains "$REPO_ROOT/scripts/prebuilt/artifact-recipe-buildroot.env" '^BUILDROOT_REF='
    assert_not_contains "$REPO_ROOT/scripts/prebuilt/artifact-recipe-linux.env" '^LINUX_REF='
}
test_build_artifacts_library_enables_fail_fast() {
    local output=$tmp/build-artifacts-options.out

    (
        cd "$REPO_ROOT"
        bash -c '. scripts/prebuilt/build-artifacts.sh; set -o' > "$output"
    )

    assert_contains "$output" '^errexit[[:space:]]+on$'
    assert_contains "$output" '^nounset[[:space:]]+on$'
    assert_contains "$output" '^pipefail[[:space:]]+on$'
}
test_buildroot_rootfs_mode_change_cleans_target_state() {
    local fixture=$tmp/buildroot-mode-clean

    mkdir -p "$fixture/scripts/prebuilt"
    cp "$SCRIPT_DIR/artifact-inputs.sh" \
       "$SCRIPT_DIR/build-artifacts.sh" \
       "$SCRIPT_DIR"/artifact-recipe-*.env \
       "$fixture/scripts/prebuilt/"
    copy_prebuilt_inputs "$fixture"

    mkdir -p "$fixture/buildroot/output/target/usr/bin"
    mkdir -p "$fixture/buildroot/output/images"
    mkdir -p "$fixture/buildroot/output/build/pkg"
    mkdir -p "$fixture/buildroot/output/build/buildroot-fs"
    printf 'stale x11 payload\n' > "$fixture/buildroot/output/target/usr/bin/Xorg"
    printf 'stale image\n' > "$fixture/buildroot/output/images/rootfs.cpio"
    printf 'mode=x11\nclass=test-tools\nrecipe_key=old\n' > \
        "$fixture/buildroot/output/.semu-rootfs-state"
    touch "$fixture/buildroot/output/build/pkg/.stamp_target_installed"
    touch "$fixture/buildroot/output/build/pkg/.stamp_images_installed"

    (
        cd "$fixture"
        bash -c '. scripts/prebuilt/build-artifacts.sh; prepare_buildroot_rootfs_output default rootfs'
    )

    test ! -d "$fixture/buildroot/output/target"
    test ! -d "$fixture/buildroot/output/images"
    test ! -d "$fixture/buildroot/output/build/buildroot-fs"
    test ! -e "$fixture/buildroot/output/build/pkg/.stamp_target_installed"
    test ! -e "$fixture/buildroot/output/build/pkg/.stamp_images_installed"
    test ! -e "$fixture/buildroot/output/.semu-rootfs-state"
}
test_buildroot_rootfs_same_state_keeps_target_state() {
    local fixture=$tmp/buildroot-state-keep

    mkdir -p "$fixture/scripts/prebuilt"
    cp "$SCRIPT_DIR/artifact-inputs.sh" \
       "$SCRIPT_DIR/build-artifacts.sh" \
       "$SCRIPT_DIR"/artifact-recipe-*.env \
       "$fixture/scripts/prebuilt/"
    copy_prebuilt_inputs "$fixture"

    mkdir -p "$fixture/buildroot/output/target/usr/bin"
    mkdir -p "$fixture/buildroot/output/images"
    printf 'default payload\n' > "$fixture/buildroot/output/target/usr/bin/busybox"
    (
        cd "$fixture"
        bash -c '. scripts/prebuilt/build-artifacts.sh; record_buildroot_rootfs_output_state default rootfs'
        bash -c '. scripts/prebuilt/build-artifacts.sh; prepare_buildroot_rootfs_output default rootfs'
    )

    assert_contains "$fixture/buildroot/output/target/usr/bin/busybox" '^default payload$'
    assert_contains "$fixture/buildroot/output/.semu-rootfs-state" '^mode=default$'
    assert_contains "$fixture/buildroot/output/.semu-rootfs-state" '^class=rootfs$'
    assert_contains "$fixture/buildroot/output/.semu-rootfs-state" '^recipe_key=[0-9a-f]{40}$'
}
test_build_artifacts_test_tools_target_is_canonical() {
    local fixture=$tmp/build-artifacts-canonical-test-tools

    mkdir -p "$fixture/scripts/prebuilt" "$fixture/scripts" "$fixture/buildroot/output/images"
    cp "$SCRIPT_DIR/artifact-inputs.sh" \
       "$SCRIPT_DIR/build-artifacts.sh" \
       "$SCRIPT_DIR"/artifact-recipe-*.env \
       "$fixture/scripts/prebuilt/"
    cat > "$fixture/scripts/rootfs_ext4.sh" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" > test-tools.args
printf 'tools\n' > test-tools.img
EOF
    chmod +x "$fixture/scripts/rootfs_ext4.sh"

    (
        cd "$fixture"
        bash -c '
            . scripts/prebuilt/build-artifacts.sh
            build_buildroot_rootfs() {
                printf "%s %s\n" "$1" "$2" > buildroot.call
                mkdir -p buildroot/output/images
                printf "rootfs\n" > buildroot/output/images/rootfs.cpio
            }
            do_extra_packages() {
                mkdir -p extra_packages
                printf "directfb\n" > extra_packages.called
            }
            stage_cxx_runtime() { printf "runtime\n" > runtime.called; }
            BUILD_X11=0
            BUILD_DIRECTFB_TEST=0
            do_test_tools
        '
    )

    assert_contains "$fixture/buildroot.call" '^x11 test-tools$'
    assert_contains "$fixture/extra_packages.called" '^directfb$'
    assert_contains "$fixture/runtime.called" '^runtime$'
    assert_contains "$fixture/test-tools.args" '192 ./extra_packages$'
}
test_build_image_enables_fail_fast_before_sourcing_recipe() {
    local fixture=$tmp/build-image-fail-fast-before-source
    local output=$tmp/build-image-fail-fast-before-source.out

    mkdir -p "$fixture/scripts/prebuilt"
    cp "$REPO_ROOT/scripts/build-image.sh" "$fixture/scripts/"
    cat > "$fixture/scripts/prebuilt/build-artifacts.sh" <<'EOF'
#!/usr/bin/env bash
if ! set -o | awk '$1 == "errexit" && $2 == "on" { found = 1 } END { exit !found }'; then
    echo "errexit was not enabled before sourcing build-artifacts.sh" >&2
    exit 23
fi
if ! set -o | awk '$1 == "nounset" && $2 == "on" { found = 1 } END { exit !found }'; then
    echo "nounset was not enabled before sourcing build-artifacts.sh" >&2
    exit 24
fi
if ! set -o | awk '$1 == "pipefail" && $2 == "on" { found = 1 } END { exit !found }'; then
    echo "pipefail was not enabled before sourcing build-artifacts.sh" >&2
    exit 25
fi
OK() { :; }
do_linux() { printf 'built image\n' > Image; }
do_rootfs() { :; }
do_test_tools() { :; }
EOF
    chmod +x "$fixture/scripts/prebuilt/build-artifacts.sh"

    (cd "$fixture" && scripts/build-image.sh image > "$output" 2>&1)
    assert_contains "$fixture/Image" '^built image$'
}
test_build_image_failure_invalidates_selected_stamps() {
    local fixture=$tmp/build-image-failure-invalidates-stamps
    local output=$tmp/build-image-failure-invalidates-stamps.out

    mkdir -p "$fixture/scripts/prebuilt"
    mkdir -p "$fixture/.prebuilt"
    cp "$REPO_ROOT/scripts/build-image.sh" "$fixture/scripts/"
    cp "$SCRIPT_DIR/artifact-inputs.sh" \
       "$SCRIPT_DIR/stamp-artifacts.sh" \
       "$fixture/scripts/prebuilt/"
    cat > "$fixture/scripts/prebuilt/build-artifacts.sh" <<'EOF'
#!/usr/bin/env bash
OK() { :; }
do_linux() { printf 'new image\n' > Image; }
do_rootfs() {
    printf 'partial rootfs\n' > rootfs.cpio
    exit 7
}
do_test_tools() { printf 'new tools\n' > test-tools.img; }
EOF
    chmod +x "$fixture/scripts/prebuilt/build-artifacts.sh"
    printf 'old image\n' > "$fixture/Image"
    printf 'old rootfs\n' > "$fixture/rootfs.cpio"
    printf 'old tools\n' > "$fixture/test-tools.img"
    printf 'old\n' > "$fixture/.prebuilt/Image.recipe-key"
    printf 'old\n' > "$fixture/.prebuilt/rootfs.cpio.recipe-key"
    printf 'old\n' > "$fixture/.prebuilt/test-tools.img.recipe-key"

    if (cd "$fixture" && scripts/build-image.sh all --no-ext4 > "$output" 2>&1); then
        echo "[!] build-image unexpectedly succeeded" >&2
        exit 1
    fi

    test ! -e "$fixture/.prebuilt/Image.recipe-key"
    test ! -e "$fixture/.prebuilt/rootfs.cpio.recipe-key"
    test ! -e "$fixture/.prebuilt/test-tools.img.recipe-key"
    assert_contains "$fixture/Image" '^new image$'
    assert_contains "$fixture/rootfs.cpio" '^partial rootfs$'
    assert_contains "$fixture/test-tools.img" '^old tools$'
}
test_build_image_test_tool_flags_require_test_tools_target() {
    local output=$tmp/build-image-flag-target.out

    if scripts/build-image.sh image --x11 > "$output" 2>&1; then
        echo "[!] build-image image --x11 unexpectedly returned success" >&2
        exit 1
    fi

    assert_contains "$output" 'requires the test-tools or all target'
}
test_build_image_help_lists_class_targets() {
    local output=$tmp/build-image-help.out

    "$REPO_ROOT/scripts/build-image.sh" --help > "$output" 2>&1

    assert_contains "$output" '^Usage: build-image.sh '
    assert_contains "$output" 'image\|rootfs\|test-tools\|all'
    assert_not_contains "$output" "$REPO_ROOT/scripts/build-image.sh"
}
test_stamp_artifacts_fails_when_stamp_dir_cannot_be_created() {
    local fixture=$tmp/stamp-dir-blocked
    local output=$tmp/stamp-dir-blocked.out

    make_guest_fixture "$fixture"
    cp "$SCRIPT_DIR/stamp-artifacts.sh" "$fixture/scripts/prebuilt/"
    rm -rf "$fixture/.prebuilt"
    printf 'not a directory\n' > "$fixture/.prebuilt"

    if (cd "$fixture" && scripts/prebuilt/stamp-artifacts.sh image > "$output" 2>&1); then
        echo "[!] stamp-artifacts unexpectedly succeeded with a blocked stamp directory" >&2
        exit 1
    fi
}
test_stamp_artifacts_rejects_unknown_flags() {
    local fixture=$tmp/stamp-unknown-flag
    local output=$tmp/stamp-unknown-flag.out

    make_guest_fixture "$fixture"
    cp "$SCRIPT_DIR/stamp-artifacts.sh" "$fixture/scripts/prebuilt/"

    (cd "$fixture" && scripts/prebuilt/stamp-artifacts.sh --help > "$output")
    assert_contains "$output" 'Build-system helper for make build-image'
    assert_contains "$output" 'Usage: stamp-artifacts.sh \[--clear\] <image\|rootfs\|test-tools\|all>\.\.\.'

    if (cd "$fixture" && scripts/prebuilt/stamp-artifacts.sh all --no-ext > "$output" 2>&1); then
        echo "[!] stamp-artifacts unexpectedly accepted an unknown flag" >&2
        exit 1
    fi
    assert_contains "$output" 'stamp-artifacts\.sh is a build-system helper; unsupported helper option: --no-ext'

    if (cd "$fixture" && scripts/prebuilt/stamp-artifacts.sh > "$output" 2>&1); then
        echo "[!] stamp-artifacts unexpectedly accepted missing target" >&2
        exit 1
    fi
    assert_contains "$output" 'requires an artifact class to stamp'
}
