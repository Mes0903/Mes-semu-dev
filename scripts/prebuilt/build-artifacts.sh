#!/usr/bin/env bash

# This recipe library is sourced by scripts/build-image.sh and the internal
# prebuilt plan builder. It intentionally enables fail-fast shell options for
# the caller before any artifact work starts.
set -euo pipefail

BUILD_ARTIFACTS_DIR=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

# Load recipe pins and class metadata before defining build helpers.  The same
# files are also part of recipe-key calculation, so this keeps "what we build"
# and "what invalidates the prebuilt cache/release" tied to one source of truth.
# The linter cannot resolve this runtime script directory.
# shellcheck disable=SC1090,SC1091
{
    . "$BUILD_ARTIFACTS_DIR/artifact-recipe-buildroot.env"
    . "$BUILD_ARTIFACTS_DIR/artifact-recipe-linux.env"
    . "$BUILD_ARTIFACTS_DIR/artifact-recipe-test-tools.env"
    . "$BUILD_ARTIFACTS_DIR/artifact-inputs.sh"
}

ASSERT() {
    "$@"
    local res=$?
    if [ $res -ne 0 ]; then
        echo 'Assert failed: "' "$@" '"'
        exit $res
    fi
}

PASS_COLOR='\e[32;01m'
NO_COLOR='\e[0m'
OK() {
    printf ' [ %s OK %s ]\n' "$PASS_COLOR" "$NO_COLOR"
}

if command -v nproc >/dev/null 2>&1; then
    build_jobs=$(nproc)
else
    build_jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')
fi
PARALLEL="-j$build_jobs"

checkout_repo_rev() {
    local dir=$1
    local repo=$2
    local rev=$3

    if [ ! -d "$dir/.git" ]; then
        echo "Cloning $dir..."
        rm -rf "$dir"
        ASSERT git init "$dir"
        pushd "$dir"
        ASSERT git remote add origin "$repo"
        if ! git fetch --depth=1 origin "$rev"; then
            ASSERT git fetch origin "$rev"
        fi
        ASSERT git checkout --detach FETCH_HEAD
        popd
        return
    fi

    echo "$dir already exists, verifying checkout..."
    pushd "$dir"
    if ! git cat-file -e "$rev^{commit}" 2>/dev/null; then
        if ! git fetch --depth=1 origin "$rev"; then
            ASSERT git fetch origin "$rev"
        fi
    fi
    ASSERT git checkout --detach "$rev"
    popd
}


ensure_buildroot_checkout() {
    checkout_repo_rev buildroot "$BUILDROOT_REPO" "$BUILDROOT_REV"
}


meson_setup_or_reconfigure() {
    local build_dir=$1
    shift

    if [ -f "$build_dir/build.ninja" ]; then
        if ! meson setup --reconfigure "$@" "$build_dir"; then
            echo "Recreating stale Meson build directory: $build_dir"
            rm -rf "$build_dir"
            ASSERT meson setup "$@" "$build_dir"
        fi
    else
        ASSERT meson setup "$@" "$build_dir"
    fi
}

configure_buildroot() {
    local mode=${1:-default}
    local buildroot_config=configs/buildroot.config
    local x11_config=configs/x11.config
    local merge_tool=buildroot/support/kconfig/merge_config.sh

    if [[ "$mode" == "x11" ]]; then
        echo "Preparing Buildroot config with X11 fragment..."
        ASSERT "$merge_tool" -m -r -O buildroot "$buildroot_config" "$x11_config"
    else
        echo "Preparing default Buildroot config..."
        cp -f "$buildroot_config" buildroot/.config
    fi
}

buildroot_rootfs_state_file() {
    printf '%s\n' buildroot/output/.semu-rootfs-state
}

buildroot_rootfs_state() {
    local mode=$1
    local class=$2
    local recipe_key

    recipe_key=$(prebuilt_class_recipe_key "$class")
    printf 'mode=%s\n' "$mode"
    printf 'class=%s\n' "$class"
    printf 'recipe_key=%s\n' "$recipe_key"
}

clean_buildroot_rootfs_output() {
    local reason=$1
    local state_file

    state_file=$(buildroot_rootfs_state_file)
    echo "Cleaning Buildroot rootfs output ($reason)..."
    rm -rf buildroot/output/target \
           buildroot/output/images \
           buildroot/output/build/buildroot-fs
    if [ -d buildroot/output/build ]; then
        find buildroot/output/build \
            \( -name '.stamp_target_installed' -o \
               -name '.stamp_images_installed' \) \
            -exec rm -f {} +
    fi
    rm -f "$state_file"
}

prepare_buildroot_rootfs_output() {
    local mode=$1
    local class=${2:-rootfs}
    local state_file
    local expected_state
    local current_state=

    state_file=$(buildroot_rootfs_state_file)
    expected_state=$(buildroot_rootfs_state "$mode" "$class")

    if [ -f "$state_file" ]; then
        current_state=$(cat "$state_file")
    fi

    if [ "$current_state" = "$expected_state" ]; then
        return 0
    fi

    if [ -n "$current_state" ]; then
        clean_buildroot_rootfs_output "rootfs recipe changed"
    elif [ -d buildroot/output/target ] || \
         [ -d buildroot/output/images ] || \
         [ -d buildroot/output/build/buildroot-fs ]; then
        clean_buildroot_rootfs_output "rootfs state is unknown"
    else
        rm -f "$state_file"
    fi
}

record_buildroot_rootfs_output_state() {
    local mode=$1
    local class=${2:-rootfs}
    local state_file
    local tmp_state

    state_file=$(buildroot_rootfs_state_file)
    mkdir -p "$(dirname "$state_file")"
    tmp_state=$state_file.tmp
    buildroot_rootfs_state "$mode" "$class" > "$tmp_state"
    mv -f "$tmp_state" "$state_file"
}

build_buildroot_toolchain() {
    ensure_buildroot_checkout
    configure_buildroot default

    unset LD_LIBRARY_PATH
    pushd buildroot
    ASSERT make olddefconfig
    ASSERT make toolchain "$PARALLEL"
    popd
}

build_buildroot_rootfs() {
    local mode=${1:-default}
    local class=${2:-rootfs}

    ensure_buildroot_checkout
    configure_buildroot "$mode"
    cp -f configs/busybox.config buildroot/busybox.config
    cp -f target/init buildroot/fs/cpio/init
    prepare_buildroot_rootfs_output "$mode" "$class"

    # Otherwise, the error below raises:
    #   You seem to have the current working directory in your
    #   LD_LIBRARY_PATH environment variable. This doesn't work.
    unset LD_LIBRARY_PATH
    pushd buildroot
    ASSERT make olddefconfig
    if [[ "$mode" == "x11" && \
          ! -x output/host/bin/riscv32-buildroot-linux-gnu-g++ ]]; then
        echo "Rebuilding Buildroot final GCC with C++ support..."
        ASSERT make host-gcc-final-dirclean
    fi
    ASSERT make "$PARALLEL"
    popd
    record_buildroot_rootfs_output_state "$mode" "$class"
}

do_rootfs() {
    build_buildroot_rootfs default rootfs

    # rootfs.cpio is the canonical Buildroot output and serves both as the
    # source for ext4.img and as the legacy initramfs payload.
    echo "Publishing rootfs.cpio"
    cp -f buildroot/output/images/rootfs.cpio ./rootfs.cpio

    if [[ $NO_EXT4 -eq 1 ]]; then
        echo "Skipping ext4.img build (--no-ext4)"
    else
        ASSERT ./scripts/rootfs_ext4.sh ./rootfs.cpio ./ext4.img
    fi
}

do_linux() {
    build_buildroot_toolchain

    checkout_repo_rev linux "$LINUX_REPO" "$LINUX_REV"

    cp -f configs/linux.config linux/.config

    export PATH="$PWD/buildroot/output/host/bin:$PATH"
    export CROSS_COMPILE=riscv32-buildroot-linux-gnu-
    export ARCH=riscv
    pushd linux
    ASSERT make olddefconfig
    ASSERT make "$PARALLEL"
    cp -f arch/riscv/boot/Image ../Image
    popd
}

do_directfb() {
    export PATH="$PWD/buildroot/output/host/bin:$PATH"
    export BUILDROOT_OUT=$PWD/buildroot/output/
    export DIRECTFB_STAGE=$PWD/directfb
    mkdir -p directfb

    checkout_repo_rev DirectFB2 "$DIRECTFB2_REPO" "$DIRECTFB2_REV"
    pushd DirectFB2
    cp ../configs/riscv-cross-file .
    meson_setup_or_reconfigure build/riscv -Ddrmkms=true --cross-file \
        riscv-cross-file
    ASSERT meson compile -C build/riscv
    ASSERT env DESTDIR="$BUILDROOT_OUT/host/riscv32-buildroot-linux-gnu/sysroot" meson install -C build/riscv
    ASSERT env DESTDIR="$DIRECTFB_STAGE" meson install -C build/riscv
    popd

    checkout_repo_rev DirectFB-examples "$DIRECTFB_EXAMPLES_REPO" \
        "$DIRECTFB_EXAMPLES_REV"
    pushd DirectFB-examples/
    cp ../configs/riscv-cross-file .
    meson_setup_or_reconfigure build/riscv --cross-file riscv-cross-file
    ASSERT meson compile -C build/riscv
    ASSERT env DESTDIR="$DIRECTFB_STAGE" meson install -C build/riscv
    popd
}

do_extra_packages() {
    export PATH="$PWD/buildroot/output/host/bin:$PATH"
    export CROSS_COMPILE=riscv32-buildroot-linux-gnu-

    rm -rf directfb extra_packages
    mkdir -p directfb
    mkdir -p extra_packages
    mkdir -p extra_packages/root

    do_directfb && OK

    if ! find directfb -mindepth 1 -print -quit | grep -q .; then
        echo "Error: DirectFB staging tree is empty."
        exit 1
    fi

    ASSERT cp -r directfb/. extra_packages/
    ASSERT cp target/local-env.sh extra_packages/root/
}

stage_cxx_runtime() {
    local toolchain_lib=buildroot/output/host/riscv32-buildroot-linux-gnu/lib
    local libstdcpp=$toolchain_lib/libstdc++.so.6
    local libstdcpp_real

    if [ ! -e "$libstdcpp" ]; then
        echo "Error: libstdc++.so.6 not found in $toolchain_lib"
        exit 1
    fi

    libstdcpp_real=$(readlink "$libstdcpp" || basename "$libstdcpp")
    if [[ "$libstdcpp_real" != /* ]]; then
        libstdcpp_real=$toolchain_lib/$libstdcpp_real
    fi
    mkdir -p extra_packages/lib
    ASSERT cp -a "$toolchain_lib/libstdc++.so" "$libstdcpp" \
        "$libstdcpp_real" extra_packages/lib/
}

do_test_tools() {
    local test_tools_rootfs

    BUILD_X11=1
    BUILD_DIRECTFB_TEST=1

    if [[ $BUILD_X11 -eq 1 ]]; then
        build_buildroot_rootfs x11 test-tools
    else
        build_buildroot_rootfs default test-tools
    fi
    test_tools_rootfs=./buildroot/output/images/rootfs.cpio

    if [[ $BUILD_DIRECTFB_TEST -eq 1 ]]; then
        do_extra_packages
        if [[ $BUILD_X11 -eq 1 ]]; then
            stage_cxx_runtime
        fi
        ASSERT ./scripts/rootfs_ext4.sh "$test_tools_rootfs" ./test-tools.img \
            "$TEST_TOOLS_SIZE_MB" ./extra_packages
    elif [[ $BUILD_X11 -eq 1 ]]; then
        rm -rf extra_packages
        mkdir -p extra_packages
        stage_cxx_runtime
        ASSERT ./scripts/rootfs_ext4.sh "$test_tools_rootfs" ./test-tools.img \
            "$TEST_TOOLS_SIZE_MB" ./extra_packages
    else
        ASSERT ./scripts/rootfs_ext4.sh "$test_tools_rootfs" ./test-tools.img \
            "$TEST_TOOLS_SIZE_MB"
    fi
}

build_prebuilt_artifact_classes() {
    local build_image=0
    local build_rootfs=0
    local build_test_tools=0
    local class

    if [ "$#" -eq 0 ]; then
        echo "[!] build_prebuilt_artifact_classes requires at least one class" >&2
        return 1
    fi

    for class in "$@"; do
        case "$class" in
            image)
                build_image=1
                ;;
            rootfs)
                build_rootfs=1
                ;;
            test-tools)
                build_test_tools=1
                ;;
            all)
                build_image=1
                build_rootfs=1
                build_test_tools=1
                ;;
            *)
                echo "[!] Unknown prebuilt artifact class: $class" >&2
                return 1
                ;;
        esac
    done

    if [[ $build_image -eq 1 ]]; then
        do_linux && OK
    fi

    if [[ $build_rootfs -eq 1 ]]; then
        NO_EXT4=1
        do_rootfs && OK
    fi

    if [[ $build_test_tools -eq 1 ]]; then
        do_test_tools && OK
    fi
}
