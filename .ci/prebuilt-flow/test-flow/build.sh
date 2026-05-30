# shellcheck shell=bash
# Sourced by .ci/prebuilt-flow/test-flow.sh.

test_recipe_uses_immutable_external_revisions() {
    assert_contains "$BUILD_SCRIPT_DIR/artifact-recipe.env" '^BUILDROOT_REV=[0-9a-f]{40}$'
    assert_contains "$BUILD_SCRIPT_DIR/artifact-recipe.env" '^LINUX_REV=[0-9a-f]{40}$'
    assert_contains "$BUILD_SCRIPT_DIR/artifact-recipe.env" '^DIRECTFB2_REV=[0-9a-f]{40}$'
    assert_contains "$BUILD_SCRIPT_DIR/artifact-recipe.env" '^DIRECTFB_EXAMPLES_REV=[0-9a-f]{40}$'
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

test_build_artifacts_ok_uses_escape_colors() {
    local fixture=$tmp/build-artifacts-ok-color
    local output=$tmp/build-artifacts-ok-color.out
    local expected

    mkdir -p "$fixture/scripts/prebuilt"
    cp "$SCRIPT_DIR/artifact-inputs.sh" \
       "$BUILD_SCRIPT_DIR/build-artifacts.sh" \
       "$BUILD_SCRIPT_DIR/artifact-recipe.env" \
       "$BUILD_SCRIPT_DIR/test-tools-payloads.sh" \
       "$fixture/scripts/prebuilt/"

    (
        cd "$fixture"
        bash -c '. scripts/prebuilt/build-artifacts.sh; OK' > "$output"
    )

    expected=$(printf '\033[32;01m OK \033[0m')
    if ! grep -Fq "$expected" "$output"; then
        echo "[!] Expected OK output to contain ANSI color escapes" >&2
        echo "--- $output ---" >&2
        cat "$output" >&2
        exit 1
    fi
    if grep -Fq '\e[32;01m' "$output"; then
        printf '%s\n' '[!] Expected OK output not to contain literal \e color text' >&2
        echo "--- $output ---" >&2
        cat "$output" >&2
        exit 1
    fi
}

test_buildroot_lock_serializes_parallel_sections() {
    local fixture=$tmp/buildroot-lock-serializes
    local output=$fixture/lock.log

    mkdir -p "$fixture/scripts/prebuilt"
    cp "$SCRIPT_DIR/artifact-inputs.sh" \
       "$BUILD_SCRIPT_DIR/build-artifacts.sh" \
       "$BUILD_SCRIPT_DIR/artifact-recipe.env" \
       "$BUILD_SCRIPT_DIR/test-tools-payloads.sh" \
       "$fixture/scripts/prebuilt/"

    (
        cd "$fixture"
        bash -c '. scripts/prebuilt/build-artifacts.sh; with_buildroot_lock bash -c '\''printf "one start\n" >> lock.log; sleep 1; printf "one end\n" >> lock.log'\''' &
        bash -c '. scripts/prebuilt/build-artifacts.sh; with_buildroot_lock bash -c '\''printf "two start\n" >> lock.log; sleep 1; printf "two end\n" >> lock.log'\''' &
        wait
    )

    awk '
        NR == 1 { first = $1; if ($2 != "start") exit 1; next }
        NR == 2 { if ($1 != first || $2 != "end") exit 1; next }
        NR == 3 { second = $1; if (second == first || $2 != "start") exit 1; next }
        NR == 4 { if ($1 != second || $2 != "end") exit 1; next }
        END { exit NR != 4 }
    ' "$output"
}
test_buildroot_lock_recovers_pidless_stale_lock() {
    local fixture=$tmp/buildroot-lock-pidless
    local output=$fixture/lock.log

    mkdir -p "$fixture/scripts/prebuilt" "$fixture/.semu-buildroot.lock"
    cp "$SCRIPT_DIR/artifact-inputs.sh" \
       "$BUILD_SCRIPT_DIR/build-artifacts.sh" \
       "$BUILD_SCRIPT_DIR/artifact-recipe.env" \
       "$BUILD_SCRIPT_DIR/test-tools-payloads.sh" \
       "$fixture/scripts/prebuilt/"

    (
        cd "$fixture"
        timeout 3 bash -c '
            . scripts/prebuilt/build-artifacts.sh
            with_buildroot_lock sh -c '\''printf "recovered\n" > lock.log'\''
        '
    )

    assert_contains "$output" '^recovered$'
    test ! -d "$fixture/.semu-buildroot.lock"
}
test_buildroot_lock_keeps_replaced_fresh_lock() {
    local fixture=$tmp/buildroot-lock-replaced-fresh

    mkdir -p "$fixture/scripts/prebuilt" "$fixture/.semu-buildroot.lock"
    cp "$SCRIPT_DIR/artifact-inputs.sh" \
       "$BUILD_SCRIPT_DIR/build-artifacts.sh" \
       "$BUILD_SCRIPT_DIR/artifact-recipe.env" \
       "$BUILD_SCRIPT_DIR/test-tools-payloads.sh" \
       "$fixture/scripts/prebuilt/"
    printf '999999\n' > "$fixture/.semu-buildroot.lock/pid"

    (
        cd "$fixture"
        # shellcheck disable=SC2016  # Inner bash expands the fixture-local lock variables.
        timeout 4 bash -c '
            . scripts/prebuilt/build-artifacts.sh
            first_stale_check=1
            buildroot_lock_is_stale() {
                if [ "$first_stale_check" -eq 1 ]; then
                    first_stale_check=0
                    rm -rf "$BUILDROOT_LOCK_DIR"
                    mkdir "$BUILDROOT_LOCK_DIR"
                    printf "%s\n" "$$" > "$BUILDROOT_LOCK_DIR/pid"
                    (
                        sleep 1
                        if [ -d "$BUILDROOT_LOCK_DIR" ] && grep -qx "$$" "$BUILDROOT_LOCK_DIR/pid"; then
                            printf "fresh survived\n" > fresh-survived.log
                            rm -rf "$BUILDROOT_LOCK_DIR"
                        fi
                    ) &
                    return 0
                fi
                return 1
            }
            with_buildroot_lock sh -c '\''printf "acquired\n" > acquired.log'\''
        '
    )

    assert_contains "$fixture/fresh-survived.log" '^fresh survived$'
    assert_contains "$fixture/acquired.log" '^acquired$'
}
test_buildroot_lock_waits_for_fresh_pidless_lock() {
    local fixture=$tmp/buildroot-lock-fresh-pidless

    mkdir -p "$fixture/scripts/prebuilt" "$fixture/.semu-buildroot.lock"
    cp "$SCRIPT_DIR/artifact-inputs.sh" \
       "$BUILD_SCRIPT_DIR/build-artifacts.sh" \
       "$BUILD_SCRIPT_DIR/artifact-recipe.env" \
       "$BUILD_SCRIPT_DIR/test-tools-payloads.sh" \
       "$fixture/scripts/prebuilt/"

    (
        cd "$fixture"
        # shellcheck disable=SC2016  # Inner bash owns the fixture-local lock lifecycle.
        timeout 5 bash -c '
            . scripts/prebuilt/build-artifacts.sh
            (
                sleep 0.2
                printf "%s\n" "$$" > "$BUILDROOT_LOCK_DIR/pid"
                printf "published\n" > published.log
                sleep 0.5
                if [ -d "$BUILDROOT_LOCK_DIR" ] && grep -qx "$$" "$BUILDROOT_LOCK_DIR/pid"; then
                    printf "fresh held\n" > fresh-held.log
                    rm -rf "$BUILDROOT_LOCK_DIR"
                fi
            ) &
            with_buildroot_lock sh -c '\''printf "acquired\n" > acquired.log'\''
        '
    )

    assert_contains "$fixture/published.log" '^published$'
    assert_contains "$fixture/fresh-held.log" '^fresh held$'
    assert_contains "$fixture/acquired.log" '^acquired$'
}
test_do_rootfs_unlocked_defaults_no_ext4_flag() {
    local fixture=$tmp/rootfs-no-ext4-default

    mkdir -p "$fixture/scripts/prebuilt" "$fixture/scripts"
    cp "$SCRIPT_DIR/artifact-inputs.sh" \
       "$BUILD_SCRIPT_DIR/build-artifacts.sh" \
       "$BUILD_SCRIPT_DIR/artifact-recipe.env" \
       "$BUILD_SCRIPT_DIR/test-tools-payloads.sh" \
       "$fixture/scripts/prebuilt/"
    cat > "$fixture/scripts/rootfs_ext4.sh" <<'EOF'
#!/usr/bin/env bash
printf 'ext4 from %s to %s\n' "$1" "$2" > ext4.called
printf 'ext4\n' > "$2"
EOF
    chmod +x "$fixture/scripts/rootfs_ext4.sh"

    (
        cd "$fixture"
        env -u NO_EXT4 bash -c '
            . scripts/prebuilt/build-artifacts.sh
            build_buildroot_rootfs() {
                mkdir -p buildroot/output/images
                printf "rootfs\n" > buildroot/output/images/rootfs.cpio
            }
            do_rootfs_unlocked
        '
    )

    assert_contains "$fixture/rootfs.cpio" '^rootfs$'
    assert_contains "$fixture/ext4.img" '^ext4$'
    assert_contains "$fixture/ext4.called" '^ext4 from ./rootfs.cpio to ./ext4.img$'
}
test_build_artifacts_test_tools_target_is_canonical() {
    local fixture=$tmp/build-artifacts-canonical-test-tools

    mkdir -p "$fixture/scripts/prebuilt" "$fixture/scripts" "$fixture/buildroot/output/images"
    cp "$SCRIPT_DIR/artifact-inputs.sh" \
       "$BUILD_SCRIPT_DIR/build-artifacts.sh" \
       "$BUILD_SCRIPT_DIR/artifact-recipe.env" \
       "$BUILD_SCRIPT_DIR/test-tools-payloads.sh" \
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
                printf "%s\n" "$1" > buildroot.call
                mkdir -p buildroot/output/images
                printf "rootfs\n" > buildroot/output/images/rootfs.cpio
            }
            do_extra_packages() {
                mkdir -p extra_packages
                printf "directfb\n" > extra_packages.called
            }
            stage_cxx_runtime() { printf "runtime\n" > runtime.called; }
            do_test_tools
        '
    )

    assert_contains "$fixture/buildroot.call" '^x11$'
    assert_contains "$fixture/extra_packages.called" '^directfb$'
    assert_contains "$fixture/runtime.called" '^runtime$'
    assert_contains "$fixture/test-tools.args" '192 ./extra_packages$'
}
test_build_artifacts_test_tools_payload_selection_omits_directfb() {
    local fixture=$tmp/build-artifacts-x11-test-tools

    mkdir -p "$fixture/scripts/prebuilt" "$fixture/scripts" "$fixture/buildroot/output/images"
    cp "$SCRIPT_DIR/artifact-inputs.sh" \
       "$BUILD_SCRIPT_DIR/build-artifacts.sh" \
       "$BUILD_SCRIPT_DIR/artifact-recipe.env" \
       "$BUILD_SCRIPT_DIR/test-tools-payloads.sh" \
       "$fixture/scripts/prebuilt/"
    cat > "$fixture/scripts/rootfs_ext4.sh" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" > test-tools.args
printf 'tools\n' > test-tools.img
EOF
    chmod +x "$fixture/scripts/rootfs_ext4.sh"

    (
        cd "$fixture"
        PREBUILT_TEST_TOOLS_PAYLOADS=x11 bash -c '
            . scripts/prebuilt/build-artifacts.sh
            build_buildroot_rootfs() {
                printf "%s\n" "$1" > buildroot.call
                mkdir -p buildroot/output/images
                printf "rootfs\n" > buildroot/output/images/rootfs.cpio
            }
            do_extra_packages() {
                echo "DirectFB payload should not be staged for x11-only test-tools" >&2
                exit 41
            }
            stage_cxx_runtime() {
                mkdir -p extra_packages/lib
                printf "runtime\n" > runtime.called
            }
            do_test_tools
        '
    )

    assert_contains "$fixture/buildroot.call" '^x11$'
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
test_build_image_passes_test_tools_payload_selection() {
    local fixture=$tmp/build-image-test-tools-payloads
    local output=$tmp/build-image-test-tools-payloads.out

    mkdir -p "$fixture/scripts/prebuilt"
    cp "$REPO_ROOT/scripts/build-image.sh" "$fixture/scripts/"
    cat > "$fixture/scripts/prebuilt/build-artifacts.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
OK() { :; }
prebuilt_test_tools_payload_key() {
    printf '%s\n' "${PREBUILT_TEST_TOOLS_PAYLOADS-unset}" > payload-key.seen
}
do_linux() { :; }
do_rootfs() { :; }
do_test_tools() {
    printf '%s\n' "${PREBUILT_TEST_TOOLS_PAYLOADS-unset}" > do-test-tools.seen
    printf 'tools\n' > test-tools.img
}
EOF
    chmod +x "$fixture/scripts/prebuilt/build-artifacts.sh"

    (cd "$fixture" && scripts/build-image.sh test-tools --x11 > "$output" 2>&1)

    assert_contains "$fixture/payload-key.seen" '^x11$'
    assert_contains "$fixture/do-test-tools.seen" '^x11$'
}
test_build_image_test_tool_flags_require_test_tools_target() {
    local fixture=$tmp/build-image-flag-target
    local output=$tmp/build-image-flag-target.out
    local args

    mkdir -p "$fixture/scripts/prebuilt"
    cp "$REPO_ROOT/scripts/build-image.sh" "$fixture/scripts/"
    cat > "$fixture/scripts/prebuilt/build-artifacts.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
OK() { :; }
prebuilt_test_tools_payload_key() { :; }
do_linux() { printf 'image\n' > Image; }
do_rootfs() { :; }
do_test_tools() { :; }
EOF
    chmod +x "$fixture/scripts/prebuilt/build-artifacts.sh"

    for args in \
        'image --x11' \
        'image --directfb2-test' \
        'image --minimal-test-tools' \
        'image --test-tools-payloads=x11'
    do
        # shellcheck disable=SC2086  # Split the fixture argument string into CLI words.
        if (cd "$fixture" && scripts/build-image.sh $args > "$output" 2>&1); then
            echo "[!] build-image $args unexpectedly returned success" >&2
            exit 1
        fi
        assert_contains "$output" 'payload options require the test-tools or all target'
    done
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
    cp "$SCRIPT_DIR/stamp-artifacts.sh" "$fixture/.ci/prebuilt/"
    rm -rf "$fixture/.prebuilt"
    printf 'not a directory\n' > "$fixture/.prebuilt"

    if (cd "$fixture" && .ci/prebuilt/stamp-artifacts.sh image > "$output" 2>&1); then
        echo "[!] stamp-artifacts unexpectedly succeeded with a blocked stamp directory" >&2
        exit 1
    fi
}
test_stamp_artifacts_rejects_unknown_flags() {
    local fixture=$tmp/stamp-unknown-flag
    local output=$tmp/stamp-unknown-flag.out

    make_guest_fixture "$fixture"
    cp "$SCRIPT_DIR/stamp-artifacts.sh" "$fixture/.ci/prebuilt/"

    (cd "$fixture" && .ci/prebuilt/stamp-artifacts.sh --help > "$output")
    assert_contains "$output" 'CI helper for recording .prebuilt'
    assert_contains "$output" 'Usage: stamp-artifacts.sh \[--clear\] <image\|rootfs\|test-tools\|all>\.\.\.'

    if (cd "$fixture" && .ci/prebuilt/stamp-artifacts.sh all --no-ext > "$output" 2>&1); then
        echo "[!] stamp-artifacts unexpectedly accepted an unknown flag" >&2
        exit 1
    fi
    assert_contains "$output" 'stamp-artifacts\.sh is a CI helper; unsupported helper option: --no-ext'

    if (cd "$fixture" && .ci/prebuilt/stamp-artifacts.sh > "$output" 2>&1); then
        echo "[!] stamp-artifacts unexpectedly accepted missing target" >&2
        exit 1
    fi
    assert_contains "$output" 'requires an artifact class to stamp'
}
