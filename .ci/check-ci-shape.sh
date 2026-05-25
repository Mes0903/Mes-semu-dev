#!/usr/bin/env bash

set -euo pipefail

rg -q '^PREBUILT_URL \?=' mk/external.mk
! rg -q 'COMMON_URL' mk/external.mk
rg -q '\.ci/detect-prebuilt-drift\.sh >> "\$GITHUB_OUTPUT"' .github/workflows/main.yml
rg -q '\.ci/package-prebuilt\.sh >> "\$GITHUB_OUTPUT"' .github/workflows/main.yml
rg -q '\.ci/verify-prebuilt-package\.sh' .github/workflows/main.yml
rg -q 'id: guest_artifact_cache' .github/workflows/main.yml
rg -q 'prebuilt-raw-v1-\$\{\{ runner\.os \}\}-\$\{\{ steps\.detect\.outputs\.live_inputs_sha1 \}\}' .github/workflows/main.yml
rg -q "steps\.guest_artifact_cache\.outputs\.cache-hit != 'true'" .github/workflows/main.yml
! rg -q '0000000000000000000000000000000000000000' .github/workflows/main.yml
