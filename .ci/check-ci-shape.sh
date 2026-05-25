#!/usr/bin/env bash

set -euo pipefail

grep -Fq 'PREBUILT_URL ?=' mk/external.mk
! grep -Fq 'COMMON_URL' mk/external.mk
grep -Fq '.ci/detect-prebuilt-drift.sh >> "$GITHUB_OUTPUT"' .github/workflows/main.yml
grep -Fq '.ci/package-prebuilt.sh >> "$GITHUB_OUTPUT"' .github/workflows/main.yml
grep -Fq '.ci/verify-prebuilt-package.sh' .github/workflows/main.yml
grep -Fq 'id: guest_artifact_cache' .github/workflows/main.yml
grep -Fq 'prebuilt-raw-v1-${{ runner.os }}-${{ steps.detect.outputs.live_inputs_sha1 }}' .github/workflows/main.yml
grep -Fq "steps.guest_artifact_cache.outputs.cache-hit != 'true'" .github/workflows/main.yml
! grep -Fq '0000000000000000000000000000000000000000' .github/workflows/main.yml
