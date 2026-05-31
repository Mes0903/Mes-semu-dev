# shellcheck shell=bash
# Sourced by .ci/prebuilt-flow/test-flow.sh.

copy_prebuilt_inputs() {
    local dst=$1
    local input

    (
        cd "$REPO_ROOT"
        while IFS= read -r input; do
            mkdir -p "$dst/$(dirname "$input")"
            cp "$input" "$dst/$input"
        done < <(prebuilt_inputs)
        input=$(prebuilt_recipe_env_file)
        mkdir -p "$dst/$(dirname "$input")"
        cp "$input" "$dst/$input"
    )
}
make_guest_fixture() {
    local dst=$1

    mkdir -p "$dst"
    mkdir -p "$dst/.ci/prebuilt" "$dst/scripts/prebuilt"
    cp "$SCRIPT_DIR/artifact-inputs.sh" \
       "$SCRIPT_DIR/package.sh" \
       "$SCRIPT_DIR/verify-package.sh" \
       "$dst/.ci/prebuilt/"
    copy_prebuilt_inputs "$dst"
    printf 'kernel\n' > "$dst/Image"
    printf 'rootfs\n' > "$dst/rootfs.cpio"
    printf 'tools\n' > "$dst/test-tools.img"
}

assert_contains() {
    local file=$1
    local pattern=$2

    if ! grep -Eq "$pattern" "$file"; then
        echo "[!] Expected $file to contain: $pattern" >&2
        echo "--- $file ---" >&2
        cat "$file" >&2
        exit 1
    fi
}
assert_not_contains() {
    local file=$1
    local pattern=$2

    if grep -Eq "$pattern" "$file"; then
        echo "[!] Expected $file not to contain: $pattern" >&2
        echo "--- $file ---" >&2
        cat "$file" >&2
        exit 1
    fi
}
make_resolver_fixture() {
    local dst=$1

    make_guest_fixture "$dst"
    cp "$SCRIPT_DIR/resolve-artifacts.sh" "$dst/.ci/prebuilt/"
}
make_materializer_fixture() {
    local dst=$1

    make_resolver_fixture "$dst"
    cp "$SCRIPT_DIR/stamp-artifacts.sh" \
       "$SCRIPT_DIR/materialize-artifacts.sh" \
       "$SCRIPT_DIR/build-plan-artifacts.sh" \
       "$SCRIPT_DIR/plan-materialize.sh" \
       "$dst/.ci/prebuilt/"
}
write_release_manifest() {
    local release=$1
    local image_key=$2
    local rootfs_key=$3
    local test_tools_key=$4

    mkdir -p "$release"
    {
        printf '%s  Image.recipe-key\n' "$image_key"
        printf '%s  rootfs.cpio.recipe-key\n' "$rootfs_key"
        printf '%s  test-tools.img.recipe-key\n' "$test_tools_key"
    } > "$release/prebuilt.sha1"
}
write_release_artifact() {
    local release=$1
    local raw=$2
    local content=$3

    printf '%s\n' "$content" > "$release/$raw"
    bzip2 -k -f "$release/$raw"
    rm -f "$release/$raw"
}
write_release_with_archives() {
    local release=$1
    local image_key=$2
    local rootfs_key=$3
    local test_tools_key=$4

    mkdir -p "$release"
    write_release_artifact "$release" Image "release kernel"
    write_release_artifact "$release" rootfs.cpio "release rootfs"
    write_release_artifact "$release" test-tools.img "release tools"
    {
        printf '%s  Image.recipe-key\n' "$image_key"
        printf '%s  rootfs.cpio.recipe-key\n' "$rootfs_key"
        printf '%s  test-tools.img.recipe-key\n' "$test_tools_key"
    } > "$release/prebuilt.sha1"
}
stamp_artifact() {
    local fixture=$1
    local artifact=$2
    local recipe_key=$3

    printf '%s\n' "$recipe_key" > "$fixture/.prebuilt/$artifact.recipe-key"
}
write_local_stamps() {
    local fixture=$1
    local image_key=$2
    local rootfs_key=$3
    local test_tools_key=$4

    mkdir -p "$fixture/.prebuilt"
    stamp_artifact "$fixture" Image "$image_key"
    stamp_artifact "$fixture" rootfs.cpio "$rootfs_key"
    stamp_artifact "$fixture" test-tools.img "$test_tools_key"
}
