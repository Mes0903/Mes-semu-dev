#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/../.." && pwd)
export REPO_ROOT
TEST_FLOW_DIR=$SCRIPT_DIR/github

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

assert_contains() {
    local file=$1
    local pattern=$2

    if ! grep -Eq "$pattern" "$file"; then
        echo "[!] Expected $file to contain: $pattern" >&2
        echo "--- $file ---" >&2
        cat "$file" >&2
        exit 1
    fi
}

assert_not_contains() {
    local file=$1
    local pattern=$2

    if grep -Eq "$pattern" "$file"; then
        echo "[!] Expected $file not to contain: $pattern" >&2
        echo "--- $file ---" >&2
        cat "$file" >&2
        exit 1
    fi
}

# Source the GitHub workflow-shape assertions into this small driver so they can reuse
# the same assert helpers and temporary directory.
# The linter cannot resolve this runtime test module directory.
# shellcheck disable=SC1090,SC1091
. "$TEST_FLOW_DIR/workflow-shape.sh"

test_github_output_helper_filters_keys
test_workflow_filters_github_outputs
test_workflow_does_not_thread_package_recipe_keys
test_workflow_filters_initial_resolve_outputs
test_workflow_filters_post_cache_resolve_outputs
test_workflow_keeps_post_cache_plan_separate
test_workflow_post_cache_plan_is_ci_only
test_workflow_enables_missing_release_bootstrap
test_workflow_restores_raw_cache_on_master_push
test_workflow_raw_cache_uses_platform_action_cache_tag
test_release_download_path_does_not_use_actions_cache
test_workflow_raw_artifact_bundle_is_raw_only
test_release_guest_artifact_action_forbids_source_builds
test_prebuilt_flow_has_its_own_ci_job

echo "GitHub workflow shape tests passed"
