#!/usr/bin/env bash

# This recipe library is sourced by scripts/build-image.sh and the internal
# prebuilt plan builder. It intentionally enables fail-fast shell options for
# the caller before any artifact work starts.
set -euo pipefail

BUILD_ARTIFACTS_DIR=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

# Load build recipe pins and payload selection helpers before defining build
# helpers. CI owns recipe-key/stamp metadata separately under .ci/prebuilt; this
# file keeps the build-facing source configuration in one place.
# The linter cannot resolve this runtime script directory.
# shellcheck disable=SC1090,SC1091
{
    . "$BUILD_ARTIFACTS_DIR/artifact-recipe.env"
    . "$BUILD_ARTIFACTS_DIR/test-tools-payloads.sh"
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
    printf ' [ %b OK %b ]\n' "$PASS_COLOR" "$NO_COLOR"
}

if command -v nproc >/dev/null 2>&1; then
    build_jobs=$(nproc)
else
    build_jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')
fi
PARALLEL="-j$build_jobs"

# Drivers may override this before calling do_rootfs, but the recipe library
# owns the default because do_rootfs_unlocked reads it directly under set -u.
NO_EXT4=${NO_EXT4:-0}

# Buildroot keeps mutable checkout, configuration, host toolchain, and rootfs
# output under one shared buildroot/ tree.  If multiple source-build entrypoints
# run concurrently, serialize the outer Buildroot operation while still letting
# Buildroot use its own internal -j parallelism.
BUILDROOT_LOCK_DIR=${BUILDROOT_LOCK_DIR:-.semu-buildroot.lock}
BUILDROOT_LOCK_PID_GRACE_SECONDS=${BUILDROOT_LOCK_PID_GRACE_SECONDS:-1}
BUILDROOT_LOCK_DEPTH=0

buildroot_lock_identity() {
    local pid_file=$BUILDROOT_LOCK_DIR/pid
    local pid
    local inode

    inode=$(ls -id "$BUILDROOT_LOCK_DIR" 2>/dev/null) || inode=missing
    inode=${inode%% *}

    if [ ! -f "$pid_file" ]; then
        printf 'missing-pid:%s\n' "$inode"
        return 0
    fi
    if ! IFS= read -r pid < "$pid_file"; then
        printf 'unreadable-pid:%s\n' "$inode"
        return 0
    fi
    printf 'pid:%s:%s\n' "$inode" "$pid"
}

buildroot_lock_is_stale() {
    local pid_file=$BUILDROOT_LOCK_DIR/pid
    local pid

    # A process that just won mkdir publishes pid immediately after acquiring
    # the lock.  Recheck pid-less locks after a short grace period so waiters do
    # not delete that fresh lock before pid publication. If the pid is still
    # missing after the grace period, treat it as an interrupted stale lock.
    if [ ! -f "$pid_file" ]; then
        sleep "$BUILDROOT_LOCK_PID_GRACE_SECONDS"
        [ -f "$pid_file" ] || return 0
    fi
    IFS= read -r pid < "$pid_file" || return 0
    case "$pid" in
        ''|*[!0-9]*) return 0 ;;
    esac

    ! kill -0 "$pid" 2>/dev/null
}

buildroot_remove_stale_lock() {
    local observed_identity=$1

    # More than one waiter can notice a stale lock. Only the waiter that still
    # sees the same lock owner may remove it; if another process already
    # acquired a fresh lock, leave it alone and keep waiting.
    [ "$(buildroot_lock_identity)" = "$observed_identity" ] || return 1
    rm -rf "$BUILDROOT_LOCK_DIR"
}

buildroot_force_release_lock() {
    if [ "${BUILDROOT_LOCK_DEPTH:-0}" -gt 0 ]; then
        rm -rf "$BUILDROOT_LOCK_DIR"
        BUILDROOT_LOCK_DEPTH=0
    fi
}

buildroot_release_lock() {
    if [ "${BUILDROOT_LOCK_DEPTH:-0}" -gt 1 ]; then
        BUILDROOT_LOCK_DEPTH=$((BUILDROOT_LOCK_DEPTH - 1))
        return
    fi

    buildroot_force_release_lock
}

buildroot_acquire_lock() {
    local waited=false
    local observed_identity

    if [ "${BUILDROOT_LOCK_DEPTH:-0}" -gt 0 ]; then
        BUILDROOT_LOCK_DEPTH=$((BUILDROOT_LOCK_DEPTH + 1))
        return 0
    fi

    while ! mkdir "$BUILDROOT_LOCK_DIR" 2>/dev/null; do
        observed_identity=$(buildroot_lock_identity)
        if buildroot_lock_is_stale; then
            buildroot_remove_stale_lock "$observed_identity" || true
            continue
        fi
        if [ "$waited" = false ]; then
            echo "Waiting for Buildroot source build lock..."
            waited=true
        fi
        sleep 1
    done

    BUILDROOT_LOCK_DEPTH=1
    # The build recipe is sourced by simple drivers that do not install their
    # own EXIT trap. While a lock is held, this library owns the caller EXIT
    # trap so interrupted builds do not leave Buildroot locked forever.
    trap buildroot_force_release_lock EXIT
    if ! printf '%s\n' "$$" > "$BUILDROOT_LOCK_DIR/pid"; then
        buildroot_force_release_lock
        return 1
    fi
}

with_buildroot_lock() {
    local status

    buildroot_acquire_lock
    if "$@"; then
        buildroot_release_lock
        return 0
    fi

    status=$?
    buildroot_release_lock
    exit "$status"
}

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

    ensure_buildroot_checkout
    configure_buildroot "$mode"
    cp -f configs/busybox.config buildroot/busybox.config
    cp -f target/init buildroot/fs/cpio/init

    # Buildroot does not reliably remove files that were installed into
    # output/target by a previous, larger rootfs configuration. For example,
    # switching from a test-tools/X11 rootfs back to the default rootfs may leave
    # old libraries or binaries in output/target unless that tree is cleaned.
    #
    # This recipe intentionally does not guess when such a clean is wanted:
    # local development keeps normal incremental Buildroot behavior, and device
    # test scripts that require a specific rootfs profile should clean/rebuild
    # explicitly before running. Typical manual repairs are:
    #
    #   scripts/build-image.sh rootfs --no-ext4 --clean-build
    #
    # or, for a canonical test-tools disk:
    #
    #   scripts/build-image.sh test-tools --clean-build
    #
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
}

do_rootfs_unlocked() {
    build_buildroot_rootfs default

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

do_rootfs() {
    with_buildroot_lock do_rootfs_unlocked
}

do_linux_unlocked() {
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

configure_test_tools_payload_flags() {
    BUILD_X11=0
    BUILD_DIRECTFB_TEST=0

    if prebuilt_test_tools_payload_is_enabled x11; then
        BUILD_X11=1
    fi
    if prebuilt_test_tools_payload_is_enabled directfb2; then
        BUILD_DIRECTFB_TEST=1
    fi
}

do_linux() {
    with_buildroot_lock do_linux_unlocked
}

do_test_tools_unlocked() {
    local test_tools_rootfs

    configure_test_tools_payload_flags

    if [[ $BUILD_X11 -eq 1 ]]; then
        build_buildroot_rootfs x11
    else
        build_buildroot_rootfs default
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
        stage_cxx_runtime
        ASSERT ./scripts/rootfs_ext4.sh "$test_tools_rootfs" ./test-tools.img \
            "$TEST_TOOLS_SIZE_MB" ./extra_packages
    else
        ASSERT ./scripts/rootfs_ext4.sh "$test_tools_rootfs" ./test-tools.img \
            "$TEST_TOOLS_SIZE_MB"
    fi
}

do_test_tools() {
    with_buildroot_lock do_test_tools_unlocked
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
