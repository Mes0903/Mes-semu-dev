#!/usr/bin/env bash

prebuilt_inputs() {
    printf '%s\n' \
        configs/linux.config \
        configs/busybox.config \
        configs/buildroot.config \
        configs/x11.config \
        configs/riscv-cross-file \
        scripts/build-image.sh \
        scripts/rootfs_ext4.sh \
        target/init \
        target/local-env.sh
}

prebuilt_sha1_tool() {
    if command -v sha1sum >/dev/null 2>&1; then
        printf '%s\n' sha1sum
    elif command -v shasum >/dev/null 2>&1; then
        printf '%s\n' 'shasum -a 1'
    else
        echo "[!] Need sha1sum or shasum on PATH" >&2
        return 1
    fi
}

prebuilt_inputs_sha1() {
    local -a inputs sha1

    read -r -a sha1 <<< "$(prebuilt_sha1_tool)"
    mapfile -t inputs < <(prebuilt_inputs)
    cat "${inputs[@]}" | "${sha1[@]}" | awk '{print $1}'
}
