# shellcheck shell=bash
# GitHub Actions workflow shape tests sourced by .ci/github/test-workflow-shape.sh.

test_github_output_helper_filters_keys() {
    local input=$tmp/github-output-helper.env
    local output=$tmp/github-output-helper.out
    local err=$tmp/github-output-helper.err

    cat > "$input" <<'EOF'
keep=value
drop=value
EOF
    GITHUB_OUTPUT="$output" "$REPO_ROOT/.ci/github/append-github-output.sh" "$input" keep
    assert_contains "$output" '^keep=value$'
    assert_not_contains "$output" '^drop=value$'

    printf 'not-output\n' > "$input"
    if GITHUB_OUTPUT="$output" "$REPO_ROOT/.ci/github/append-github-output.sh" "$input" > /dev/null 2> "$err"; then
        echo "[!] append-github-output unexpectedly accepted a non-output line" >&2
        exit 1
    fi
    assert_contains "$err" 'Unexpected non-output line: not-output'
}

test_workflow_filters_github_outputs() {
    local workflow=$REPO_ROOT/.github/workflows/main.yml

    assert_contains "$workflow" '\.ci/github/append-github-output\.sh'
    assert_not_contains "$workflow" 'append_github_output\(\)'
    # shellcheck disable=SC2016
    assert_not_contains "$workflow" 'scripts/prebuilt/materialize-artifacts\.sh >> "\$GITHUB_OUTPUT"'
    # shellcheck disable=SC2016
    assert_not_contains "$workflow" 'scripts/prebuilt/package\.sh >> "\$GITHUB_OUTPUT"'
}
test_workflow_does_not_thread_package_recipe_keys() {
    local workflow=$REPO_ROOT/.github/workflows/main.yml

    assert_not_contains "$workflow" 'image_recipe_key:'
    assert_not_contains "$workflow" 'rootfs_recipe_key:'
    assert_not_contains "$workflow" 'test_tools_recipe_key:'
    assert_not_contains "$workflow" 'needs\.guest-artifact-build\.outputs\.(image|rootfs|test_tools)_recipe_key'
    assert_contains "$workflow" 'prebuilt\.sha1` manifest'
}

test_workflow_filters_initial_resolve_outputs() {
    local workflow=$REPO_ROOT/.github/workflows/main.yml

    # shellcheck disable=SC2016
    assert_contains "$workflow" '\.ci/github/append-github-output\.sh "\$RUNNER_TEMP/prebuilt-plan\.env"'
    assert_contains "$workflow" 'release_needs_update'
    assert_contains "$workflow" 'release_manifest_sha1'
    assert_contains "$workflow" 'current_recipe_classes_sha1'
}
test_workflow_keeps_strict_plan_separate() {
    local workflow=$REPO_ROOT/.github/workflows/main.yml

    assert_contains "$workflow" 'id: strict_resolve'
    assert_contains "$workflow" 'prebuilt-plan-strict\.env'
    assert_contains "$workflow" 'PREBUILT_PLAN_FILE: \$\{\{ runner\.temp \}\}/prebuilt-plan-strict\.env'
    assert_not_contains "$workflow" 'id: materialize_plan'
}
test_workflow_strict_materialization_plan_is_local_first() {
    local workflow=$REPO_ROOT/.github/workflows/main.yml

    assert_contains "$workflow" 'PREBUILT_LOCAL_FIRST: 1'
}
test_workflow_enables_missing_release_bootstrap() {
    local workflow=$REPO_ROOT/.github/workflows/main.yml

    assert_contains "$workflow" "PREBUILT_BOOTSTRAP_ON_404:.*github.repository != 'sysprog21/semu'"
    assert_contains "$workflow" 'canonical repo should fail fast'
    assert_not_contains "$workflow" 'PREBUILT_ALLOW_MISSING_RELEASE'
}
test_workflow_restores_raw_cache_on_master_push() {
    local workflow=$REPO_ROOT/.github/workflows/main.yml
    local cache_block=$tmp/workflow-cache-block.yml

    awk '/name: cache workflow-local raw guest artifacts/ { in_block = 1 }
         in_block { print }
         in_block && /id: guest_artifact_cache/ { exit }' "$workflow" > "$cache_block"

    assert_not_contains "$cache_block" "github\.ref != 'refs/heads/master'"
}
test_workflow_artifact_fallback_includes_archives() {
    local workflow=$REPO_ROOT/.github/workflows/main.yml

    assert_contains "$workflow" 'Image\.bz2'
    assert_contains "$workflow" 'rootfs\.cpio\.bz2'
    assert_contains "$workflow" 'test-tools\.img\.bz2'
}
test_prebuilt_flow_has_its_own_ci_job() {
    local workflow=$REPO_ROOT/.github/workflows/main.yml

    assert_contains "$workflow" '^  prebuilt-flow:$'
    assert_contains "$workflow" '^      - prebuilt-flow$'
}
