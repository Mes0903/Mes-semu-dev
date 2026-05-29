# shellcheck shell=bash
# Sourced by .ci/prebuilt/test-flow.sh.

write_input_list_makefile() {
    local dst=$1

    cat > "$dst/Makefile" <<EOF
Q := @
VECHO = printf '%b'
PRINTF = printf
SHA1SUM = sha1sum
.PHONY: FORCE
FORCE:

include $REPO_ROOT/mk/external.mk
EOF
}

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
    local test_tools_after
    local test_tools_recipe_file

    mkdir -p "$fixture"
    copy_prebuilt_inputs "$fixture"
    cp "$SCRIPT_DIR/artifact-inputs.sh" "$fixture/scripts/prebuilt/"

    (
        cd "$fixture"
        image_before=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_before=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_before=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)
        test_tools_recipe_file=$(grep -rl '^TEST_TOOLS_SIZE_MB=' scripts/prebuilt)
        sed -i 's/^TEST_TOOLS_SIZE_MB=.*/TEST_TOOLS_SIZE_MB=256/' "$test_tools_recipe_file"
        image_after=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key image)
        rootfs_after=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key rootfs)
        test_tools_after=$(. scripts/prebuilt/artifact-inputs.sh; prebuilt_class_recipe_key test-tools)

        [ "$image_before" = "$image_after" ]
        [ "$rootfs_before" = "$rootfs_after" ]
        [ "$test_tools_before" != "$test_tools_after" ]
    )
}
test_input_lists_match_makefile() {
    local fixture=$tmp/input-lists

    mkdir -p "$fixture"
    copy_prebuilt_inputs "$fixture"
    write_input_list_makefile "$fixture"

    prebuilt_class_inputs image > "$tmp/shell-image-inputs"
    prebuilt_class_inputs rootfs > "$tmp/shell-rootfs-inputs"
    prebuilt_class_inputs test-tools > "$tmp/shell-test-tools-inputs"
    (cd "$REPO_ROOT" && prebuilt_class_recipe_key image) > "$tmp/shell-image-sha1"
    (cd "$REPO_ROOT" && prebuilt_class_recipe_key rootfs) > "$tmp/shell-rootfs-sha1"
    (cd "$REPO_ROOT" && prebuilt_class_recipe_key test-tools) > "$tmp/shell-test-tools-sha1"
    make --no-print-directory -C "$fixture" prebuilt-inputs CLASS=image > "$tmp/make-image-inputs"
    make --no-print-directory -C "$fixture" prebuilt-inputs CLASS=rootfs > "$tmp/make-rootfs-inputs"
    make --no-print-directory -C "$fixture" prebuilt-inputs CLASS=test-tools > "$tmp/make-test-tools-inputs"
    make --no-print-directory -C "$fixture" prebuilt-recipe-key CLASS=image > "$tmp/make-image-sha1"
    make --no-print-directory -C "$fixture" prebuilt-recipe-key CLASS=rootfs > "$tmp/make-rootfs-sha1"
    make --no-print-directory -C "$fixture" prebuilt-recipe-key CLASS=test-tools > "$tmp/make-test-tools-sha1"

    diff -u "$tmp/shell-image-inputs" "$tmp/make-image-inputs"
    diff -u "$tmp/shell-rootfs-inputs" "$tmp/make-rootfs-inputs"
    diff -u "$tmp/shell-test-tools-inputs" "$tmp/make-test-tools-inputs"
    diff -u "$tmp/shell-image-sha1" "$tmp/make-image-sha1"
    diff -u "$tmp/shell-rootfs-sha1" "$tmp/make-rootfs-sha1"
    diff -u "$tmp/shell-test-tools-sha1" "$tmp/make-test-tools-sha1"
}
