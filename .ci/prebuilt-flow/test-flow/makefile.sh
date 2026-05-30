# shellcheck shell=bash
# Sourced by .ci/prebuilt-flow/test-flow.sh.

write_external_makefile() {
    local dst=$1

    cat > "$dst/Makefile" <<EOF
Q := @
KERNEL_DATA = Image
INITRD_DATA = rootfs.cpio
TEST_TOOLS_DATA = test-tools.img
BIN = semu
.PHONY: FORCE
FORCE:

include $REPO_ROOT/mk/external.mk

scripts/rootfs_ext4.sh:
	@:
minimal.dtb:
	@:
\$(BIN):
	@:
ext4.img: \$(INITRD_DATA) scripts/rootfs_ext4.sh
	\$(Q)scripts/rootfs_ext4.sh \$(INITRD_DATA) \$@

RUN_HEADLESS ?= 1
RUN_DISK ?= ext4.img
RUN_HEADLESS_OPT := \$(if \$(filter 0 false no,\$(RUN_HEADLESS)),,-H)
RUN_DISK_OPT := \$(if \$(RUN_DISK),-d \$(RUN_DISK))
RUN_DISK_DEP := \$(RUN_DISK)
RUN_COMMON_ARGS = -k \$(KERNEL_DATA) -b minimal.dtb \$(RUN_HEADLESS_OPT) \$(RUN_DISK_OPT)

check: \$(BIN) minimal.dtb \$(KERNEL_DATA) \$(INITRD_DATA) \$(RUN_DISK_DEP)
	\$(Q)./\$(BIN) \$(RUN_COMMON_ARGS)

BUILD_IMAGE_ARGS ?= all
build-image:
	scripts/build-image.sh \$(BUILD_IMAGE_ARGS)

distclean:
	\$(RM) Image rootfs.cpio prebuilt.sha1
	\$(RM) Image.bz2 rootfs.cpio.bz2 test-tools.img.bz2
	\$(RM) ext4.img test-tools.img
	\$(RM) -r .prebuilt
EOF
}

test_make_local_targets_download_release_by_default() {
    local fixture=$tmp/make-local-release-default
    local output=$tmp/make-local-release-default.out
    local tools_output=$tmp/make-local-release-test-tools.out

    mkdir -p "$fixture"
    write_external_makefile "$fixture"

    make -C "$fixture" --no-print-directory -n check > "$output"
    assert_contains "$output" 'Image\.bz2'
    assert_contains "$output" 'rootfs\.cpio\.bz2'
    assert_contains "$output" '^scripts/rootfs_ext4\.sh rootfs\.cpio ext4\.img$'
    assert_not_contains "$output" '^scripts/build-image\.sh (image|rootfs)'
    assert_not_contains "$output" 'resolve-artifacts\.sh|materialize-artifacts\.sh|plan-materialize\.sh|PREBUILT_'

    make -C "$fixture" --no-print-directory -n test-tools.img > "$tools_output"
    assert_contains "$tools_output" 'test-tools\.img\.bz2'
    assert_not_contains "$tools_output" '^scripts/build-image\.sh test-tools$'
    assert_not_contains "$tools_output" 'resolve-artifacts\.sh|materialize-artifacts\.sh|plan-materialize\.sh|PREBUILT_'
}

test_make_build_image_uses_build_system() {
    local output=$tmp/make-build-image.out

    (
        cd "$REPO_ROOT"
        make --no-print-directory -n build-image > "$output"
    )

    assert_contains "$output" '^scripts/build-image\.sh all$'
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
}
