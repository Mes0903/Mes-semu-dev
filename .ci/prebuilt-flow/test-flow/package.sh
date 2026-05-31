# shellcheck shell=bash
# Sourced by .ci/prebuilt-flow/test-flow.sh.

test_package_and_verify() {
    local fixture=$tmp/package
    local output=$tmp/package.out

    make_guest_fixture "$fixture"

    (
        cd "$fixture"
        .ci/prebuilt/package.sh > "$output" 2> "$tmp/package.err"
        .ci/prebuilt/verify-package.sh > "$tmp/verify-package.out"
        test -s Image.bz2
        test -s rootfs.cpio.bz2
        test -s test-tools.img.bz2
        test -s prebuilt.sha1
    )

    assert_not_contains "$fixture/prebuilt.sha1" '^[0-9a-f]{40}  Image\.bz2$'
    assert_not_contains "$fixture/prebuilt.sha1" '^[0-9a-f]{40}  rootfs\.cpio\.bz2$'
    assert_not_contains "$fixture/prebuilt.sha1" '^[0-9a-f]{40}  test-tools\.img\.bz2$'
    assert_contains "$fixture/prebuilt.sha1" '^[0-9a-f]{40}  Image\.recipe-key$'
    assert_contains "$fixture/prebuilt.sha1" '^[0-9a-f]{40}  rootfs\.cpio\.recipe-key$'
    assert_contains "$fixture/prebuilt.sha1" '^[0-9a-f]{40}  test-tools\.img\.recipe-key$'
    assert_not_contains "$output" '^kernel_sha1=[0-9a-f]{40}$'
    assert_not_contains "$output" '^initrd_sha1=[0-9a-f]{40}$'
    assert_not_contains "$output" '^test_tools_sha1=[0-9a-f]{40}$'
    test ! -s "$output"
    test ! -s "$tmp/verify-package.out"

    (
        cd "$fixture"
        sed '/rootfs.cpio.recipe-key/d' prebuilt.sha1 > prebuilt.sha1.missing-input
        mv prebuilt.sha1.missing-input prebuilt.sha1
        if .ci/prebuilt/verify-package.sh > "$tmp/verify-package-missing.out" 2> "$tmp/verify-package-missing.err"; then
            echo "[!] verify-package unexpectedly accepted a manifest without rootfs.cpio.recipe-key" >&2
            exit 1
        fi
    )
    assert_contains "$tmp/verify-package-missing.err" 'prebuilt.sha1 missing rootfs\.cpio\.recipe-key'
}
test_package_removes_partial_archive_on_failure() {
    local fixture=$tmp/package-part-failure
    local output=$tmp/package-part-failure.out

    make_guest_fixture "$fixture"
    mkdir -p "$fixture/fakebin"
    cat > "$fixture/fakebin/bzip2" <<'EOF'
#!/usr/bin/env bash
if [ "$1" = -c ]; then
    printf 'partial archive'
else
    artifact=${@: -1}
    printf 'partial archive' > "$artifact.bz2"
fi
exit 1
EOF
    chmod +x "$fixture/fakebin/bzip2"

    if (cd "$fixture" && PATH="$PWD/fakebin:$PATH" .ci/prebuilt/package.sh > "$output" 2>&1); then
        echo "[!] package unexpectedly accepted a failing compressor" >&2
        exit 1
    fi

    test ! -f "$fixture/Image.bz2"
    test ! -f "$fixture/Image.bz2.part"
}
