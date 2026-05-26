#!/usr/bin/env bash

set -euo pipefail

require_text() {
    local file=$1
    local text=$2

    if ! grep -Fq "$text" "$file"; then
        echo "[!] Missing expected text in $file: $text" >&2
        exit 1
    fi
}

reject_text() {
    local file=$1
    local text=$2

    if grep -Fq "$text" "$file"; then
        echo "[!] Unexpected text in $file: $text" >&2
        exit 1
    fi
}

grep -Fq 'PREBUILT_URL ?=' mk/external.mk
! grep -Fq 'COMMON_URL' mk/external.mk

require_text .github/workflows/main.yml '.ci/prebuilt/detect-drift.sh >> "$GITHUB_OUTPUT"'
require_text .github/workflows/main.yml '.ci/prebuilt/package.sh >> "$GITHUB_OUTPUT"'
require_text .github/workflows/main.yml '.ci/prebuilt/verify-package.sh'
require_text .github/workflows/main.yml '.ci/github/suggest-format.sh'
require_text .github/workflows/main.yml '.ci/github/check-ci-shape.sh'
require_text .github/workflows/main.yml '.ci/device-smoke/test-netdev.sh'
require_text .github/workflows/main.yml '.ci/device-smoke/test-sound.sh'
require_text .github/workflows/main.yml '.ci/device-smoke/test-vinput.sh'
require_text .github/workflows/main.yml '.ci/device-smoke/test-gpu.sh'
require_text .github/workflows/main.yml 'id: guest_artifact_cache'
require_text .github/workflows/main.yml 'prebuilt-raw-v1-${{ runner.os }}-${{ steps.detect.outputs.live_inputs_sha1 }}'
require_text .github/workflows/main.yml "steps.guest_artifact_cache.outputs.cache-hit != 'true'"
reject_text .github/workflows/main.yml '.ci/detect-prebuilt-drift.sh'
reject_text .github/workflows/main.yml '.ci/package-prebuilt.sh'
reject_text .github/workflows/main.yml '.ci/verify-prebuilt-package.sh'
reject_text .github/workflows/main.yml '.ci/suggest-format.sh'
reject_text .github/workflows/main.yml '.ci/check-ci-shape.sh'
reject_text .github/workflows/main.yml '.ci/test-netdev.sh'
reject_text .github/workflows/main.yml '.ci/test-sound.sh'
reject_text .github/workflows/main.yml '.ci/test-vinput.sh'
reject_text .github/workflows/main.yml '.ci/test-gpu.sh'
! grep -Fq '0000000000000000000000000000000000000000' .github/workflows/main.yml

if grep -RInE 'github\.com|GITHUB_OUTPUT|GITHUB_TOKEN|github\.event|github\.ref|actions/|softprops/action|reviewdog' .ci/prebuilt; then
    echo "[!] GitHub adapter references must not live in prebuilt core scripts" >&2
    exit 1
fi
