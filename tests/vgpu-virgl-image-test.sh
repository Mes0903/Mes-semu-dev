#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${REPO_ROOT}"

fail()
{
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

require_file()
{
    local path="$1"
    [ -f "${path}" ] || fail "missing ${path}"
}

require_literal()
{
    local needle="$1"
    local path="$2"

    grep -Fq -- "${needle}" "${path}" ||
        fail "${path} does not contain: ${needle}"
}

reject_literal()
{
    local needle="$1"
    local path="$2"

    if grep -Fq -- "${needle}" "${path}"; then
        fail "${path} unexpectedly contains: ${needle}"
    fi
}

require_help_literal()
{
    local needle="$1"
    local output="$2"

    grep -Fq -- "${needle}" <<<"${output}" ||
        fail "build-image --help does not contain: ${needle}"
}

require_file configs/x11.config
require_file configs/virgl.config
require_literal 'BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_VIRGL=n' configs/x11.config
require_literal 'BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_VIRGL=y' configs/virgl.config
require_literal 'BR2_PACKAGE_LIBEPOXY=y' configs/virgl.config

set +e
help_output="$(scripts/build-image.sh --help 2>&1)"
help_status=$?
set -e
if [ "${help_status}" -ne 1 ]; then
    fail "scripts/build-image.sh --help exited ${help_status}, expected 1"
fi
require_help_literal '--x11' "${help_output}"
require_help_literal '--virgl' "${help_output}"
require_help_literal 'Build test-tools.img from an X11 rootfs with Mesa VirGL' \
    "${help_output}"

require_literal 'scripts/build-image.sh --virgl' README.md
require_literal '.ci/test-virgl.sh' README.md
require_literal 'virtio_gpu_dri.so' README.md
require_literal 'glamoregl' README.md
require_literal 'glxinfo -B' README.md
require_literal 'glxgears' README.md
require_literal 'profile changes' README.md
require_literal 'buildroot/output/target' README.md
reject_literal 'buildroot-output/' README.md

require_file .ci/test-virgl.sh
require_literal 'ENABLE_VIRGL=1' .ci/test-virgl.sh
require_literal 'pkg-config --exists virglrenderer epoxy gl egl' \
    .ci/test-virgl.sh
require_literal './semu -k Image -c 1 -b minimal.dtb -d test-tools.img' \
    .ci/test-virgl.sh
require_literal '/etc/semu-test-tools-virgl' .ci/test-virgl.sh
require_literal '/usr/lib/dri/virtio_gpu_dri.so' .ci/test-virgl.sh
require_literal 'MESA_LOADER_DRIVER_OVERRIDE=virtio_gpu' .ci/test-virgl.sh
require_literal 'virtio-gpu-features' .ci/test-virgl.sh
require_literal 'kernel virtio-gpu diagnostic' .ci/test-virgl.sh
require_literal 'xorg dri diagnostic' .ci/test-virgl.sh
require_literal 'Xorg :0 -noreset -nolisten tcp' .ci/test-virgl.sh
require_literal '__VIRGL_XORG_READY__' .ci/test-virgl.sh
require_literal 'glxinfo -B' .ci/test-virgl.sh
require_literal 'glxgears' .ci/test-virgl.sh
require_literal 'SEMU_VIRGL_REBOOT_TEST' .ci/test-virgl.sh
require_literal '__VIRGL_RENDERER_OK__' .ci/test-virgl.sh
require_literal 'PASS: visible VirGL virtio-gpu smoke checks' .ci/test-virgl.sh

require_literal 'configs/virgl.config' .ci/publish-prebuilt.sh
require_literal 'scripts/build-image.sh --all --virgl --directfb2-test' \
    .ci/publish-prebuilt.sh
require_literal 'mesa3d-dirclean' scripts/build-image.sh
require_literal 'xserver_xorg-server-dirclean' scripts/build-image.sh
require_literal 'libglamoregl.so' scripts/build-image.sh
require_literal 'stage_virgl_smoke_marker' scripts/build-image.sh
require_literal 'semu-test-tools-virgl' scripts/build-image.sh
require_literal 'detect_buildroot_profile_from_config' scripts/build-image.sh
require_literal 'clean_buildroot_target_on_profile_switch "$mode"' \
    scripts/build-image.sh
require_literal 'rm -rf buildroot/output/target buildroot/output/images' \
    scripts/build-image.sh
require_literal "find buildroot/output/build -name '.stamp_target_installed' -delete" \
    scripts/build-image.sh
require_literal 'record_buildroot_profile "$mode"' scripts/build-image.sh
reject_literal 'BUILDROOT_OUTPUT_BASE' scripts/build-image.sh
reject_literal 'buildroot-output' scripts/build-image.sh
require_literal 'cp -f buildroot/output/images/rootfs.cpio ./rootfs.cpio' \
    scripts/build-image.sh
require_literal 'test_tools_rootfs=./buildroot/output/images/rootfs.cpio' \
    scripts/build-image.sh
require_literal 'export PATH="$PWD/buildroot/output/host/bin:$PATH"' \
    scripts/build-image.sh
require_literal 'stage_cxx_runtime' scripts/build-image.sh
require_literal 'do_extra_packages' scripts/build-image.sh
reject_literal 'buildroot-output/' .gitignore
require_literal 'configs/virgl.config' mk/external.mk
require_literal 'configs/virgl.config' .github/workflows/prebuilt.yml
require_literal './scripts/build-image.sh --all --virgl --directfb2-test' \
    .github/workflows/prebuilt.yml
require_literal 'configs/virgl.config' .github/workflows/main.yml
