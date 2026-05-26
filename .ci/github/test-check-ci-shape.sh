#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)

tmpdir=$(mktemp -d)
cleanup() {
    rm -rf "$tmpdir"
}
trap cleanup EXIT

mkdir -p "$tmpdir/.ci/github" "$tmpdir/.ci/prebuilt" "$tmpdir/.github/workflows" "$tmpdir/mk"
cp "$REPO_ROOT/.ci/github/check-ci-shape.sh" "$tmpdir/.ci/github/check-ci-shape.sh"
cp "$REPO_ROOT/.github/workflows/main.yml" "$tmpdir/.github/workflows/main.yml"
cp "$REPO_ROOT/.github/workflows/prebuilt.yml" "$tmpdir/.github/workflows/prebuilt.yml"
cp "$REPO_ROOT/mk/external.mk" "$tmpdir/mk/external.mk"

(
    cd "$tmpdir"
    bash .ci/github/check-ci-shape.sh
)

(
    cd "$tmpdir"
    printf '\nCOMMON_URL := https://example.invalid/prebuilt\n' >> mk/external.mk
    if bash .ci/github/check-ci-shape.sh > common-url.out 2>&1; then
        echo "[!] check-ci-shape accepted forbidden COMMON_URL" >&2
        exit 1
    fi
    grep -F '[!] Unexpected text in mk/external.mk: COMMON_URL' common-url.out >/dev/null
)

(
    cd "$tmpdir"
    sed -i '/COMMON_URL/d' mk/external.mk
    printf '\n# 0000000000000000000000000000000000000000\n' >> .github/workflows/main.yml
    if bash .ci/github/check-ci-shape.sh > zero-sha.out 2>&1; then
        echo "[!] check-ci-shape accepted all-zero SHA pin" >&2
        exit 1
    fi
    grep -F '[!] Unexpected text in .github/workflows/main.yml: 0000000000000000000000000000000000000000' zero-sha.out >/dev/null
)

(
    cd "$tmpdir"
    sed -i '/0000000000000000000000000000000000000000/d' .github/workflows/main.yml
    touch .ci/publish-prebuilt.sh
    if bash .ci/github/check-ci-shape.sh > old-wrapper.out 2>&1; then
        echo "[!] check-ci-shape accepted old publish-prebuilt wrapper" >&2
        exit 1
    fi
    grep -F '[!] Unexpected path exists: .ci/publish-prebuilt.sh' old-wrapper.out >/dev/null
)
