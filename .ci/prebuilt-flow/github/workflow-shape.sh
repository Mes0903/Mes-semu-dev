# shellcheck shell=bash
# GitHub Actions workflow shape tests sourced by .ci/prebuilt-flow/test-workflow-shape.sh.

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

    cat > "$input" <<'EOF'
keep=value
EOF
    : > "$output"
    if GITHUB_OUTPUT="$output" "$REPO_ROOT/.ci/github/append-github-output.sh" "$input" keep missing > /dev/null 2> "$err"; then
        echo "[!] append-github-output unexpectedly accepted a missing requested key" >&2
        exit 1
    fi
    assert_contains "$err" 'Missing requested output key: missing'
}

test_workflow_filters_github_outputs() {
    local workflow=$REPO_ROOT/.github/workflows/main.yml

    assert_contains "$workflow" '\.ci/github/append-github-output\.sh'
    assert_not_contains "$workflow" 'append_github_output\(\)'
    # shellcheck disable=SC2016
    assert_not_contains "$workflow" '>> "\$GITHUB_OUTPUT"'
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
    assert_contains "$workflow" 'platform_action_cache_tag'
    assert_not_contains "$workflow" 'release_manifest_sha1'
    assert_not_contains "$workflow" 'current_recipe_classes_sha1'
}
test_workflow_filters_post_cache_resolve_outputs() {
    local workflow=$REPO_ROOT/.github/workflows/main.yml
    local post_cache_output=$tmp/workflow-post-cache-resolve-output.yml

    awk '/append-github-output\.sh "\$RUNNER_TEMP\/prebuilt-plan-post-cache\.env"/ { print; getline; print; exit }' \
        "$workflow" > "$post_cache_output"

    # shellcheck disable=SC2016  # Pattern intentionally matches the literal GitHub env expression.
    assert_contains "$post_cache_output" '\.ci/github/append-github-output\.sh "\$RUNNER_TEMP/prebuilt-plan-post-cache\.env"'
    assert_contains "$post_cache_output" 'requires_build'
    assert_not_contains "$post_cache_output" 'release_needs_update'
}
test_workflow_keeps_post_cache_plan_separate() {
    local workflow=$REPO_ROOT/.github/workflows/main.yml

    assert_contains "$workflow" 'id: post_cache_resolve'
    assert_contains "$workflow" 'prebuilt-plan-post-cache\.env'
    assert_contains "$workflow" 'PREBUILT_PLAN_FILE: \$\{\{ runner\.temp \}\}/prebuilt-plan-post-cache\.env'
    assert_not_contains "$workflow" 'id: materialize_plan'
}
test_workflow_post_cache_plan_is_ci_only() {
    local workflow=$REPO_ROOT/.github/workflows/main.yml

    assert_contains "$workflow" 'id: post_cache_resolve'
    # shellcheck disable=SC2016  # Pattern intentionally matches the literal GitHub env expression.
    assert_contains "$workflow" '\.ci/prebuilt/resolve-artifacts\.sh > "\$RUNNER_TEMP/prebuilt-plan-post-cache\.env"'
}
test_workflow_enables_missing_release_bootstrap() {
    local workflow=$REPO_ROOT/.github/workflows/main.yml

    assert_contains "$workflow" "PREBUILT_BOOTSTRAP_ON_404:.*github.repository != 'sysprog21/semu'"
    assert_contains "$workflow" 'canonical repo should fail fast'
}
test_workflow_restores_raw_cache_on_master_push() {
    local workflow=$REPO_ROOT/.github/workflows/main.yml
    local cache_block=$tmp/workflow-cache-block.yml

    awk '/name: cache workflow-local raw guest artifacts/ { in_block = 1 }
         in_block { print }
         in_block && /id: guest_artifact_cache/ { exit }' "$workflow" > "$cache_block"

    assert_not_contains "$cache_block" "github\.ref != 'refs/heads/master'"
}
test_workflow_raw_cache_uses_platform_action_cache_tag() {
    local workflow=$REPO_ROOT/.github/workflows/main.yml

    assert_contains "$workflow" 'prebuilt-raw-\$\{\{ steps\.resolve\.outputs\.platform_action_cache_tag \}\}-'
    assert_not_contains "$workflow" 'steps\.resolve\.outputs\.current_recipe_classes_sha1'
}

test_release_download_path_does_not_use_actions_cache() {
    local workflow=$REPO_ROOT/.github/workflows/main.yml
    local action=$REPO_ROOT/.github/actions/provide-guest-artifacts/action.yml

    assert_not_contains "$action" 'manifest-sha1'
    assert_not_contains "$action" 'release-downloaded'
    assert_not_contains "$action" 'external-\$\{\{'
    assert_not_contains "$action" 'actions/cache@v5'
    assert_not_contains "$workflow" 'manifest-sha1:'
    assert_not_contains "$workflow" 'release_manifest_sha1'
}
test_workflow_raw_artifact_bundle_is_raw_only() {
    local workflow=$REPO_ROOT/.github/workflows/main.yml
    local raw_block=$tmp/workflow-raw-artifact-block.yml

    awk '/name: cache workflow-local raw guest artifacts/ { in_block = 1 }
         /name: materialization plan after cache restore/ { if (in_block) exit }
         in_block { print }' "$workflow" > "$raw_block"
    awk '/name: upload raw guest artifacts/ { in_block = 1 }
         /name: upload packaged guest artifacts/ { if (in_block) exit }
         in_block { print }' "$workflow" >> "$raw_block"

    assert_contains "$raw_block" '^          Image$'
    assert_contains "$raw_block" '^          rootfs\.cpio$'
    assert_contains "$raw_block" '^          test-tools\.img$'
    assert_contains "$raw_block" '^          \.prebuilt$'
    assert_not_contains "$raw_block" 'Image\.bz2|rootfs\.cpio\.bz2|test-tools\.img\.bz2|prebuilt\.sha1'
}
test_release_guest_artifact_action_forbids_source_builds() {
    local action=$REPO_ROOT/.github/actions/provide-guest-artifacts/action.yml

    assert_contains "$action" 'PREBUILT_FORBID_BUILD=1'
}
test_prebuilt_flow_has_its_own_ci_job() {
    local workflow=$REPO_ROOT/.github/workflows/main.yml

    assert_contains "$workflow" '^  prebuilt-flow:$'
    assert_contains "$workflow" '^      - prebuilt-flow$'
}
