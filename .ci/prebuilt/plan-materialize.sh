#!/usr/bin/env bash
#
# Test/helper entry point: create a CI resolver plan, then execute that exact
# plan. The GitHub workflow normally performs these two phases explicitly so it
# can restore cache between them and expose selected resolver outputs.

set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/../.." && pwd)
cd "$REPO_ROOT"

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

plan_file=$tmpdir/prebuilt-plan.env
"$SCRIPT_DIR/resolve-artifacts.sh" "$@" > "$plan_file"
PREBUILT_PLAN_FILE="$plan_file" "$SCRIPT_DIR/materialize-artifacts.sh"
