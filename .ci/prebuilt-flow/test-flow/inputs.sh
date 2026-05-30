# shellcheck shell=bash
# Sourced by .ci/prebuilt-flow/test-flow.sh.


test_class_registry_exposes_plan_metadata() {
    [ "$(prebuilt_class_plan_prefix image)" = image ]
    [ "$(prebuilt_class_plan_prefix rootfs)" = rootfs ]
    [ "$(prebuilt_class_plan_prefix test-tools)" = test_tools ]
    [ "$(prebuilt_artifact_recipe_entry Image)" = Image.recipe-key ]
    [ "$(prebuilt_class_recipe_entries test-tools)" = test-tools.img.recipe-key ]
    [ "$(prebuilt_class_artifacts image)" = Image ]
    [ "$(prebuilt_class_artifacts rootfs)" = rootfs.cpio ]
    [ "$(prebuilt_class_artifacts test-tools)" = test-tools.img ]
    [ "$(prebuilt_plan_class_var_name image action)" = image_action ]
    [ "$(prebuilt_plan_class_var_name rootfs current_recipe_key)" = current_rootfs_recipe_key ]
    [ "$(prebuilt_plan_class_var_name test-tools local_status)" = local_test_tools_status ]
    prebuilt_plan_class_var_matches image_action image action
    prebuilt_plan_key_is_output_class_var local_test_tools_status test-tools
}
test_recipe_keys_only_track_class_recipe_inputs() {
    local fixture=$tmp/recipe-class-isolation
    local image_before
    local image_after
    local rootfs_before
    local rootfs_after
    local test_tools_before
    local image_after_linux
    local rootfs_after_linux
    local test_tools_after_linux
    local test_tools_after
    local test_tools_recipe_file

    mkdir -p "$fixture"
    copy_prebuilt_inputs "$fixture"
    mkdir -p "$fixture/.ci/prebuilt"
    cp "$SCRIPT_DIR/artifact-inputs.sh" "$fixture/.ci/prebuilt/"

    (
        cd "$fixture"
        image_before=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_before=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_before=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)
        test_tools_recipe_file=$(grep -rl '^TEST_TOOLS_SIZE_MB=' scripts/prebuilt)

        sed -i 's/^LINUX_REV=.*/LINUX_REV=1111111111111111111111111111111111111111/' "$test_tools_recipe_file"
        image_after_linux=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_after_linux=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_after_linux=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)

        [ "$image_before" != "$image_after_linux" ]
        [ "$rootfs_before" = "$rootfs_after_linux" ]
        [ "$test_tools_before" = "$test_tools_after_linux" ]

        sed -i 's/^TEST_TOOLS_SIZE_MB=.*/TEST_TOOLS_SIZE_MB=256/' "$test_tools_recipe_file"
        image_after=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_after=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_after=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)

        [ "$image_after_linux" = "$image_after" ]
        [ "$rootfs_before" = "$rootfs_after" ]
        [ "$test_tools_before" != "$test_tools_after" ]
    )
}
test_recipe_keys_ignore_ci_wrapper_and_scope_payload_helper() {
    local fixture=$tmp/recipe-wrapper-payload-isolation
    local image_before
    local rootfs_before
    local test_tools_before
    local image_after_wrapper
    local rootfs_after_wrapper
    local test_tools_after_wrapper
    local image_after_payload
    local rootfs_after_payload
    local test_tools_after_payload

    mkdir -p "$fixture"
    copy_prebuilt_inputs "$fixture"
    mkdir -p "$fixture/.ci/prebuilt"
    cp "$SCRIPT_DIR/artifact-inputs.sh" \
       "$SCRIPT_DIR/build-plan-artifacts.sh" \
       "$fixture/.ci/prebuilt/"

    (
        cd "$fixture"
        image_before=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_before=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_before=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)

        printf '\n# wrapper-only edit\n' >> .ci/prebuilt/build-plan-artifacts.sh
        image_after_wrapper=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_after_wrapper=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_after_wrapper=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)

        [ "$image_before" = "$image_after_wrapper" ]
        [ "$rootfs_before" = "$rootfs_after_wrapper" ]
        [ "$test_tools_before" = "$test_tools_after_wrapper" ]

        printf '\n# payload-helper edit\n' >> scripts/prebuilt/test-tools-payloads.sh
        image_after_payload=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_after_payload=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_after_payload=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)

        [ "$image_after_wrapper" = "$image_after_payload" ]
        [ "$rootfs_after_wrapper" = "$rootfs_after_payload" ]
        [ "$test_tools_after_wrapper" != "$test_tools_after_payload" ]
    )
}
test_test_tools_payload_rejects_legacy_aliases() {
    local fixture=$tmp/test-tools-payload-legacy-aliases
    local output=$tmp/test-tools-payload-legacy-aliases.out
    local payload

    mkdir -p "$fixture/scripts/prebuilt"
    cp "$BUILD_SCRIPT_DIR/test-tools-payloads.sh" "$fixture/scripts/prebuilt/"

    for payload in none directfb2-test; do
        if (
            cd "$fixture"
            PREBUILT_TEST_TOOLS_PAYLOADS=$payload bash -c '. scripts/prebuilt/test-tools-payloads.sh; prebuilt_test_tools_payloads' > "$output" 2>&1
        ); then
            echo "[!] test-tools payload alias unexpectedly succeeded: $payload" >&2
            exit 1
        fi
        assert_contains "$output" "Unknown test-tools payload: $payload"
    done
}
test_test_tools_recipe_key_tracks_payload_selection() {
    local fixture=$tmp/recipe-test-tools-payloads
    local image_before
    local image_after
    local rootfs_before
    local rootfs_after
    local test_tools_canonical
    local test_tools_x11

    mkdir -p "$fixture"
    copy_prebuilt_inputs "$fixture"
    mkdir -p "$fixture/.ci/prebuilt"
    cp "$SCRIPT_DIR/artifact-inputs.sh" "$fixture/.ci/prebuilt/"

    (
        cd "$fixture"
        image_before=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_before=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_canonical=$(. .ci/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)
        image_after=$(. .ci/prebuilt/artifact-inputs.sh; PREBUILT_TEST_TOOLS_PAYLOADS=x11 prebuilt_class_recipe_key image)
        rootfs_after=$(. .ci/prebuilt/artifact-inputs.sh; PREBUILT_TEST_TOOLS_PAYLOADS=x11 prebuilt_class_recipe_key rootfs)
        test_tools_x11=$(. .ci/prebuilt/artifact-inputs.sh; PREBUILT_TEST_TOOLS_PAYLOADS=x11 prebuilt_class_recipe_key test-tools)

        [ "$image_before" = "$image_after" ]
        [ "$rootfs_before" = "$rootfs_after" ]
        [ "$test_tools_canonical" != "$test_tools_x11" ]
    )
}
