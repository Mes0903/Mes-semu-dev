#!/usr/bin/env bash
#
# Local/Make entry point: create a local-first resolver plan, then execute that
# exact plan.  CI performs the same two phases explicitly so it can restore cache
# between them and expose selected resolver outputs to GitHub Actions.

set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/../.." && pwd)
cd "$REPO_ROOT"

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

plan_file=$tmpdir/prebuilt-plan.env
PREBUILT_LOCAL_FIRST=1 "$SCRIPT_DIR/resolve-artifacts.sh" "$@" > "$plan_file"
PREBUILT_PLAN_FILE="$plan_file" "$SCRIPT_DIR/materialize-artifacts.sh"
