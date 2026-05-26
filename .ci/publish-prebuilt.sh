#!/usr/bin/env bash
#
# Compatibility wrapper for the old script name. The provider-neutral
# packaging implementation lives in .ci/prebuilt/package.sh.

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$SCRIPT_DIR/prebuilt/package.sh" "$@"
