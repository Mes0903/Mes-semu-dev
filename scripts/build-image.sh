#!/usr/bin/env bash

function ASSERT
{
    $*
    RES=$?
    if [ $RES -ne 0 ]; then
        echo 'Assert failed: "' $* '"'
        exit $RES
    fi
}

PASS_COLOR='\e[32;01m'
NO_COLOR='\e[0m'
function OK
{
    printf " [ ${PASS_COLOR} OK ${NO_COLOR} ]\n"
}

PARALLEL="-j$(nproc)"

function safe_copy {
    local src="$1"
    local dst="$2"

    if [ ! -f "$dst" ]; then
        echo "Copying $src -> $dst"
        cp -f "$src" "$dst"
    else
        echo "$dst already exists, skipping copy"
    fi
}

function has_extra_packages
{
    [ -d extra_packages ] && [ -n "$(ls -A extra_packages 2>/dev/null)" ]
}

function stage_rootfs_tree
{
    local input_cpio="$1"
    local output_dir="$2"
    local input_cpio_path="$PWD/$input_cpio"
    local output_dir_path="$PWD/$output_dir"
    local extra_packages_dir="$PWD/extra_packages"

    echo "Preparing staged rootfs tree at $output_dir..."
    ASSERT env ROOTFS_STAGE_DIR="$output_dir_path" \
        ROOTFS_INPUT_CPIO="$input_cpio_path" \
        EXTRA_PACKAGES_DIR="$extra_packages_dir" \
        /bin/bash -lc '
            set -euo pipefail
            rm -rf "$ROOTFS_STAGE_DIR"
            mkdir -p "$ROOTFS_STAGE_DIR"
            cd "$ROOTFS_STAGE_DIR"
            cpio -idmv < "$ROOTFS_INPUT_CPIO"
            if [ -d "$EXTRA_PACKAGES_DIR" ] &&
               [ -n "$(ls -A "$EXTRA_PACKAGES_DIR" 2>/dev/null)" ]; then
                echo "[*] Merging extra packages into staged rootfs..."
                cp -r "$EXTRA_PACKAGES_DIR"/. "$ROOTFS_STAGE_DIR"/
            else
                echo "[*] No extra packages to merge into staged rootfs."
            fi
        '
}

function repack_rootfs_cpio
{
    local source_dir="$1"
    local output_cpio="$2"
    local source_dir_path="$PWD/$source_dir"
    local output_cpio_path="$PWD/$output_cpio"

    echo "Packing staged rootfs into $output_cpio..."
    ASSERT env ROOTFS_STAGE_DIR="$source_dir_path" \
        ROOTFS_OUTPUT_CPIO="$output_cpio_path" \
        /bin/bash -lc '
            set -euo pipefail
            cd "$ROOTFS_STAGE_DIR"
            find . -print0 | cpio --null -o -H newc > "$ROOTFS_OUTPUT_CPIO"
        '
}

function copy_buildroot_config
{
    local buildroot_config="configs/buildroot.config"
    local x11_config="configs/x11.config"
    local output_config="buildroot/.config"
    local merge_tool="buildroot/support/kconfig/merge_config.sh"

    echo "Preparing Buildroot config..."

    # Check X11 option
    if [[ $BUILD_X11 -eq 1 ]]; then
        # Compile Buildroot with X11
        "$merge_tool" -m -r -O buildroot "$buildroot_config" "$x11_config"
    else
        # Compile Buildroot without X11
        cp -f "$buildroot_config" "$output_config"
    fi
}

function do_buildroot
{
    local buildroot_cpio="buildroot/output/images/rootfs.cpio"
    local staged_rootfs_dir="rootfs.staging"

    if [ ! -d buildroot ]; then
        echo "Cloning Buildroot..."
        ASSERT git clone https://github.com/buildroot/buildroot -b 2025.02.x --depth=1
    else
        echo "buildroot/ already exists, skipping clone"
    fi

    copy_buildroot_config
    safe_copy configs/busybox.config buildroot/busybox.config
    cp -f target/init buildroot/fs/cpio/init

    # Otherwise, the error below raises:
    #   You seem to have the current working directory in your
    #   LD_LIBRARY_PATH environment variable. This doesn't work.
    unset LD_LIBRARY_PATH
    pushd buildroot
    ASSERT make olddefconfig
    ASSERT make $PARALLEL
    popd

    if [[ $BUILD_DIRECTFB_TEST -eq 1 ]]; then
        do_directfb_test_payload
    fi

    if [[ $MERGE_EXTRA_PACKAGES -eq 1 ]] && ! has_extra_packages; then
        echo "Error: --merge-extra-packages requested, but extra_packages/ is empty."
        echo "       Run with --directfb-test first or stage other packages into extra_packages/."
        exit 1
    fi

    if [[ $EXTERNAL_ROOT -eq 1 ]]; then
        if [[ $MERGE_EXTRA_PACKAGES -eq 1 ]]; then
            stage_rootfs_tree "$buildroot_cpio" "$staged_rootfs_dir"
            repack_rootfs_cpio "$staged_rootfs_dir" rootfs_full.cpio
        else
            echo "Copying rootfs.cpio to rootfs_full.cpio (external root mode)"
            cp -f "$buildroot_cpio" ./rootfs_full.cpio
        fi

        ASSERT ./scripts/rootfs_ext4.sh
    else
        if [[ $MERGE_EXTRA_PACKAGES -eq 1 ]]; then
            stage_rootfs_tree "$buildroot_cpio" "$staged_rootfs_dir"
            repack_rootfs_cpio "$staged_rootfs_dir" rootfs.cpio
        else
            echo "Copying rootfs.cpio to rootfs.cpio (initramfs mode)"
            cp -f "$buildroot_cpio" ./rootfs.cpio
        fi
    fi
}

function do_linux
{
    if [ ! -d linux ]; then
        echo "Cloning Linux kernel..."
        ASSERT git clone https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git -b linux-6.12.y --depth=1
    else
        echo "linux/ already exists, skipping clone"
    fi

    safe_copy configs/linux.config linux/.config

    export PATH="$PWD/buildroot/output/host/bin:$PATH"
    export CROSS_COMPILE=riscv32-buildroot-linux-gnu-
    export ARCH=riscv
    pushd linux
    ASSERT make olddefconfig
    ASSERT make $PARALLEL
    cp -f arch/riscv/boot/Image ../Image
    popd
}

function show_help {
    cat << EOF
Usage: $0 [--buildroot] [--x11] [--linux] [--directfb-test] [--merge-extra-packages] [--all] [--external-root] [--clean-build] [--help]

Options:
  --buildroot         Build Buildroot rootfs
  --x11               Build Buildroot with X11
  --directfb-test     Build and stage the DirectFB test payload under extra_packages/
  --merge-extra-packages
                      Merge staged extra packages into the final rootfs
  --linux             Build Linux kernel
  --all               Build both Buildroot and Linux kernel
  --external-root     Use external rootfs instead of initramfs
  --clean-build       Remove entire buildroot/ and/or linux/ directories before build
  --help              Show this message
EOF
    exit 1
}

BUILD_BUILDROOT=0
BUILD_X11=0
BUILD_DIRECTFB_TEST=0
MERGE_EXTRA_PACKAGES=0
BUILD_LINUX=0
EXTERNAL_ROOT=0
CLEAN_BUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --buildroot)
            BUILD_BUILDROOT=1
            ;;
        --x11)
            BUILD_X11=1
            ;;
        --directfb-test)
            BUILD_DIRECTFB_TEST=1
            ;;
        --merge-extra-packages)
            MERGE_EXTRA_PACKAGES=1
            ;;
        --linux)
            BUILD_LINUX=1
            ;;
        --all)
            BUILD_BUILDROOT=1
            BUILD_LINUX=1
            ;;
        --external-root)
            EXTERNAL_ROOT=1
            ;;
        --clean-build)
            CLEAN_BUILD=1
            ;;
        --help|-h)
            show_help
            ;;
        *)
            echo "Unknown option: $1"
            show_help
            ;;
    esac
    shift
done

function do_directfb
{
    export PATH=$PATH:$PWD/buildroot/output/host/bin
    export BUILDROOT_OUT=$PWD/buildroot/output/
    export DIRECTFB_STAGE=$PWD/directfb
    mkdir -p directfb

    # Build DirectFB2
    if [ ! -d DirectFB2 ]; then
        echo "Cloning DirectFB2..."
        ASSERT git clone https://github.com/directfb2/DirectFB2.git
    else
        echo "DirectFB2 already exists, skipping clone..."
    fi
    pushd DirectFB2
    cp ../configs/riscv-cross-file .
    ASSERT meson -Ddrmkms=true --cross-file riscv-cross-file build/riscv
    ASSERT meson compile -C build/riscv
    ASSERT env DESTDIR=$BUILDROOT_OUT/host/riscv32-buildroot-linux-gnu/sysroot meson install -C build/riscv
    ASSERT env DESTDIR=$DIRECTFB_STAGE meson install -C build/riscv
    popd

    # Build DirectFB2 examples
    if [ ! -d DirectFB-examples ]; then
        echo "Cloning DirectFB-examples..."
        ASSERT git clone https://github.com/directfb2/DirectFB-examples.git
    else
        echo "DirectFB-examples already exists, skipping clone..."
    fi
    pushd DirectFB-examples/
    cp ../configs/riscv-cross-file .
    ASSERT meson --cross-file riscv-cross-file build/riscv
    ASSERT meson compile -C build/riscv
    ASSERT env DESTDIR=$DIRECTFB_STAGE meson install -C build/riscv
    popd
}

function do_directfb_test_payload
{
    export PATH="$PWD/buildroot/output/host/bin:$PATH"
    export CROSS_COMPILE=riscv32-buildroot-linux-gnu-

    rm -rf extra_packages
    mkdir -p extra_packages
    mkdir -p extra_packages/root

    do_directfb && OK

    if ! find directfb -mindepth 1 -print -quit | grep -q .; then
        echo "Error: DirectFB staging tree is empty."
        exit 1
    fi

    ASSERT cp -r directfb/. extra_packages/
    ASSERT cp target/run.sh extra_packages/root/
}

if [[ $BUILD_BUILDROOT -eq 0 && $BUILD_LINUX -eq 0 ]]; then
    echo "Error: No build target specified. Use --buildroot, --linux, or --all."
    show_help
fi

if [[ $BUILD_DIRECTFB_TEST -eq 1 && $BUILD_BUILDROOT -eq 0 ]]; then
    echo "Error: --directfb-test requires --buildroot to be specified."
    show_help
fi

if [[ $MERGE_EXTRA_PACKAGES -eq 1 && $BUILD_BUILDROOT -eq 0 ]]; then
    echo "Error: --merge-extra-packages requires --buildroot to be specified."
    show_help
fi

if [[ $BUILD_X11 -eq 1 && $BUILD_BUILDROOT -eq 0 ]]; then
    echo "Error: --x11 requires --buildroot to be specified."
    show_help
fi

if [[ $CLEAN_BUILD -eq 1 && $BUILD_BUILDROOT -eq 1 && -d buildroot ]]; then
    echo "Removing buildroot/ for clean build..."
    rm -rf buildroot
fi

if [[ $CLEAN_BUILD -eq 1 && $BUILD_LINUX -eq 1 && -d linux ]]; then
    echo "Removing linux/ for clean build..."
    rm -rf linux
fi

if [[ $BUILD_BUILDROOT -eq 1 ]]; then
    do_buildroot && OK
fi

if [[ $BUILD_LINUX -eq 1 ]]; then
    do_linux && OK
fi
