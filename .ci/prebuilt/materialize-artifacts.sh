#!/usr/bin/env bash
#
# CI executor for a resolver plan. It deliberately does not run the resolver or
# accept class arguments: callers choose classes while creating the plan, then
# pass PREBUILT_PLAN_FILE here. Local Make builds bypass this layer entirely.

set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/../.." && pwd)
cd "$REPO_ROOT"

# Load the class/plan registry and stamp writer as the materializer's shared
# contract with the resolver.  The materializer consumes plan keys from the
# registry and commits stamps only after download/build actions succeed.
# The linter cannot resolve this runtime script directory.
# shellcheck disable=SC1090,SC1091
{
    . "$SCRIPT_DIR/artifact-inputs.sh"
    . "$SCRIPT_DIR/stamp-artifacts.sh"
}

if [ "$#" -ne 0 ]; then
    echo "[!] materialize-artifacts.sh does not accept class arguments; pass them when creating the plan." >&2
    exit 1
fi

# Release archive downloads are transport, not recipe verification. The
# resolver has already decided that the release recipe key matches current
# source state; this function only fetches and decompresses the archive. We do
# not maintain archive checksums in prebuilt.sha1, so stale/corrupt release
# bytes are external release-state failures for maintainers to inspect.
download_release_artifact() {
    local artifact=$1
    local archive=${artifact}.bz2

    : "${PREBUILT_URL:?Need PREBUILT_URL pointing at the prebuilt artifact directory}"
    if ! curl --fail --silent --show-error --retry 3 --retry-delay 1 \
        -L -o "$archive.part" "${PREBUILT_URL}/${archive}"; then
        rm -f "$archive.part"
        echo "[!] Failed to download $archive" >&2
        return 1
    fi

    mv -f "$archive.part" "$archive"
    if ! bunzip2 -c "$archive" > "$artifact.tmp"; then
        rm -f "$artifact.tmp"
        echo "[!] Failed to decompress $archive" >&2
        return 1
    fi
    mv -f "$artifact.tmp" "$artifact"
    rm -f "$archive"
}

# Downloading consumes an already-decided release action. After every raw
# artifact in the class lands, the materializer commits the recipe-key stamp so
# later CI cache restores can be trusted as use-local actions.
download_release_class() {
    local class=$1
    local recipe_key=$2
    local artifact

    while IFS= read -r artifact; do
        download_release_artifact "$artifact" || {
            echo "[!] Failed to materialize $class from release" >&2
            exit 1
        }
    done < <(prebuilt_class_artifacts "$class")
    prebuilt_stamp_artifacts_for_class "$class" "$recipe_key"
}

# Build actions go through the internal CI builder, not scripts/build-image.sh.
# build-image.sh is the user-facing local CLI; the materializer stamps only
# after the plan-selected CI build action succeeds.
build_selected_classes() {
    local -a args=()
    local class

    while IFS= read -r class; do
        if [ "$(prebuilt_plan_get_class_var "$class" action)" = build ]; then
            args+=("$class")
        fi
    done < <(prebuilt_classes)

    if [ "${#args[@]}" -eq 0 ]; then
        return 0
    fi

    .ci/prebuilt/build-plan-artifacts.sh "${args[@]}" >&2
}

# Materialization consumes only plan_version, *_action, and current_*_recipe_key.
# Resolver-only diagnostics are explicitly tolerated, while unknown future keys
# fail fast so the plan schema cannot silently drift.
parse_resolver_output() {
    local line
    local plan_key
    local value
    local class
    local matched
    local known

    while IFS= read -r line; do
        plan_key=${line%%=*}
        value=${line#*=}
        case "$plan_key" in
            plan_version) plan_version=$value; continue ;;
            requires_build)
                continue
                ;;
            platform_action_cache_tag|\
            release_manifest_available|\
            release_needs_update)
                continue
                ;;
        esac

        matched=false
        known=false
        while IFS= read -r class; do
            if prebuilt_plan_class_var_matches "$plan_key" "$class" action current_recipe_key; then
                printf -v "$plan_key" '%s' "$value"
                matched=true
                known=true
                break
            fi
            if prebuilt_plan_key_is_output_class_var "$plan_key" "$class"; then
                known=true
            fi
        done < <(prebuilt_classes)

        if [ "$matched" = true ] || [ "$known" = true ]; then
            continue
        fi

        echo "[!] Unknown prebuilt plan key: $plan_key" >&2
        return 1
    done
}

if [ -z "${PREBUILT_PLAN_FILE:-}" ]; then
    echo "[!] PREBUILT_PLAN_FILE is required; create a CI resolver plan before materializing." >&2
    exit 1
fi
if [ ! -f "$PREBUILT_PLAN_FILE" ]; then
    echo "[!] PREBUILT_PLAN_FILE does not exist: $PREBUILT_PLAN_FILE" >&2
    exit 1
fi
resolver_output=$(cat "$PREBUILT_PLAN_FILE")

case "${PREBUILT_VERBOSE:-0}" in
    1|true)
        printf '%s\n' "$resolver_output" >&2
        ;;
    0|false|'')
        ;;
    *)
        echo "[!] PREBUILT_VERBOSE must be 0/1 or true/false, got: ${PREBUILT_VERBOSE:-}" >&2
        exit 1
        ;;
esac

case "${PREBUILT_FORBID_BUILD:-0}" in
    1|true)
        forbid_build=true
        ;;
    0|false|'')
        forbid_build=false
        ;;
    *)
        echo "[!] PREBUILT_FORBID_BUILD must be 0/1 or true/false, got: ${PREBUILT_FORBID_BUILD:-}" >&2
        exit 1
        ;;
esac

plan_version=
while IFS= read -r class; do
    prebuilt_plan_set_class_var "$class" action ''
    prebuilt_plan_set_class_var "$class" current_recipe_key ''
done < <(prebuilt_classes)

parse_resolver_output <<< "$resolver_output"

if [ "$plan_version" != 1 ]; then
    echo "[!] Unsupported prebuilt plan version: ${plan_version:-missing}" >&2
    exit 1
fi

while IFS= read -r class; do
    action=$(prebuilt_plan_get_class_var "$class" action)
    case "$action" in
        build|use-local|download-release|skip)
            ;;
        '')
            echo "[!] Missing action for prebuilt class: $class" >&2
            exit 1
            ;;
        *)
            echo "[!] Unsupported action for prebuilt class $class: $action" >&2
            exit 1
            ;;
    esac

    case "$action" in
        build|download-release)
            if [ -z "$(prebuilt_plan_get_class_var "$class" current_recipe_key)" ]; then
                echo "[!] Missing current recipe key for prebuilt class $class with action $action" >&2
                exit 1
            fi
            ;;
    esac
done < <(prebuilt_classes)

requires_build=false
while IFS= read -r class; do
    case "$(prebuilt_plan_get_class_var "$class" action)" in
        build) requires_build=true ;;
    esac
done < <(prebuilt_classes)

if [ "$requires_build" = true ] && [ "$forbid_build" = true ]; then
    echo "[!] PREBUILT_FORBID_BUILD forbids source builds, but the plan contains:" >&2
    while IFS= read -r class; do
        if [ "$(prebuilt_plan_get_class_var "$class" action)" = build ]; then
            printf '[!]   %s=build\n' "$(prebuilt_plan_class_var_name "$class" action)" >&2
        fi
    done < <(prebuilt_classes)
    exit 1
fi

downloaded_any=false
built_any=false

if [ "$requires_build" = true ]; then
    build_selected_classes
    built_any=true
    while IFS= read -r class; do
        if [ "$(prebuilt_plan_get_class_var "$class" action)" = build ]; then
            prebuilt_stamp_artifacts_for_class "$class" "$(prebuilt_plan_get_class_var "$class" current_recipe_key)"
        fi
    done < <(prebuilt_classes)
fi

while IFS= read -r class; do
    if [ "$(prebuilt_plan_get_class_var "$class" action)" = download-release ]; then
        download_release_class "$class" "$(prebuilt_plan_get_class_var "$class" current_recipe_key)"
        downloaded_any=true
    fi
done < <(prebuilt_classes)

printf 'downloaded_any=%s\n' "$downloaded_any"
printf 'built_any=%s\n' "$built_any"
