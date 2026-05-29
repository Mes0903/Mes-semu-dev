#!/usr/bin/env bash
#
# Provider-neutral planner for prebuilt guest artifacts.  This script never
# mutates raw artifacts; it compares three states and prints a KEY=VALUE plan:
#   current recipe keys  - what this checkout would build
#   release recipe keys  - what the rolling prebuilt release advertises
#   local recipe stamps  - what already exists in the working tree/cache
# GitHub cache restore, workflow artifacts, and publish policy live above this
# layer in the workflow or composite actions.

set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/../.." && pwd)
cd "$REPO_ROOT"

# Load the class registry and recipe-key helpers.  The resolver uses this as
# its only knowledge of artifact classes, so adding a class does not require a
# separate resolver-local state table.
# The linter cannot resolve this runtime script directory.
# shellcheck disable=SC1090,SC1091
. "$SCRIPT_DIR/artifact-inputs.sh"

# Local development defaults to non-strict: stale or unmanaged raw artifacts
# block with a clear message so the user can choose rebuild, replace, or ignore.
# CI uses strict mode so the plan may replace stale cache entries by download or
# rebuild without requiring interactive repair.
case "${PREBUILT_STRICT:-0}" in
    1|true)
        strict=true
        ;;
    0|false|'')
        strict=false
        ;;
    *)
        echo "[!] PREBUILT_STRICT must be 0/1 or true/false, got: ${PREBUILT_STRICT:-}" >&2
        exit 1
        ;;
esac

# Local-first is a performance/UX policy for materialization, not publish.
# When selected local artifacts already match current recipe keys, the resolver
# can skip fetching the release manifest entirely.  Publish gates must use a
# normal non-local-first pass so release_needs_update is based on release state.
case "${PREBUILT_LOCAL_FIRST:-0}" in
    1|true)
        local_first=true
        ;;
    0|false|'')
        local_first=false
        ;;
    *)
        echo "[!] PREBUILT_LOCAL_FIRST must be 0/1 or true/false, got: ${PREBUILT_LOCAL_FIRST:-}" >&2
        exit 1
        ;;
esac

case "${PREBUILT_IGNORE_SHA:-0}" in
    1|true)
        ignore_sha=true
        ;;
    0|false|'')
        ignore_sha=false
        ;;
    *)
        echo "[!] PREBUILT_IGNORE_SHA must be 0/1 or true/false, got: ${PREBUILT_IGNORE_SHA:-}" >&2
        exit 1
        ;;
esac

# This is only for fork/mirror first-run bootstrap when their rolling
# prebuilt release has never existed. Other manifest fetch failures stay fatal.
case "${PREBUILT_BOOTSTRAP_ON_404:-0}" in
    1|true)
        bootstrap_on_404=true
        ;;
    0|false|'')
        bootstrap_on_404=false
        ;;
    *)
        echo "[!] PREBUILT_BOOTSTRAP_ON_404 must be 0/1 or true/false, got: ${PREBUILT_BOOTSTRAP_ON_404:-}" >&2
        exit 1
        ;;
esac

prebuilt_plan_for_each_class_set selected false
if [ "$#" -eq 0 ]; then
    prebuilt_plan_select_all_classes
else
    for requested_class in "$@"; do
        prebuilt_plan_select_class "$requested_class" || exit 1
    done
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

manifest=$tmpdir/prebuilt.sha1
manifest_url=
release_manifest_available=false
release_manifest_sha1=
release_manifest_http_code=

while IFS= read -r class; do
    prebuilt_plan_set_class_var "$class" release_recipe_key ''
done < <(prebuilt_classes)

# prebuilt.sha1 stores recipe keys as virtual sha1sum entries such as
# Image.recipe-key.  Those entries describe source/recipe state, not archive
# content, and are the only release state the resolver uses for decisions.
manifest_class_recipe_key() {
    local class=$1
    local artifact
    local entry
    local next
    local recipe_key=
    local have_key=false

    while IFS= read -r artifact; do
        entry=$(prebuilt_artifact_recipe_entry "$artifact")
        next=$(awk -v f="$entry" '$2 == f {print $1; found = 1; exit}' "$manifest")
        if [ -z "$next" ]; then
            printf '\n'
            return
        fi
        if [ "$have_key" = false ]; then
            recipe_key=$next
            have_key=true
        elif [ "$recipe_key" != "$next" ]; then
            printf '\n'
            return
        fi
    done < <(prebuilt_class_artifacts "$class")

    printf '%s\n' "$recipe_key"
}

fetch_release_manifest() {
    local http_code
    local curl_err=$tmpdir/curl.err
    local class

    : "${PREBUILT_URL:?Need PREBUILT_URL pointing at the prebuilt artifact directory}"
    manifest_url="${PREBUILT_URL}/prebuilt.sha1"
    if http_code=$(curl --fail --silent --show-error --retry 3 --retry-delay 1 \
        --write-out '%{http_code}' -L -o "$manifest" "$manifest_url" 2>"$curl_err"); then
        release_manifest_available=true
        release_manifest_http_code=$http_code
        release_manifest_sha1=$(prebuilt_sha1sum "$manifest" | awk '{print $1}')
        while IFS= read -r class; do
            prebuilt_plan_set_class_var "$class" release_recipe_key "$(manifest_class_recipe_key "$class")"
        done < <(prebuilt_classes)
    else
        release_manifest_http_code=$http_code
        if [ "$bootstrap_on_404" = false ] || [ "$release_manifest_http_code" != 404 ]; then
            cat "$curl_err" >&2
        fi
        echo "prebuilt manifest is unavailable at $manifest_url" >&2
        return 1
    fi
}

class_any_artifact_exists() {
    local artifact

    while IFS= read -r artifact; do
        if [ -f "$artifact" ]; then
            return 0
        fi
    done < <(prebuilt_class_artifacts "$1")

    return 1
}

class_all_artifacts_exist() {
    local artifact

    while IFS= read -r artifact; do
        if [ ! -f "$artifact" ]; then
            return 1
        fi
    done < <(prebuilt_class_artifacts "$1")

    return 0
}

class_local_recipe_key() {
    local class=$1
    local artifact
    local recipe_stamp
    local recipe_key=
    local next=
    local have_key=false

    while IFS= read -r artifact; do
        recipe_stamp=$(prebuilt_artifact_recipe_stamp "$artifact")
        if [ ! -f "$artifact" ]; then
            return 1
        fi
        if [ ! -f "$recipe_stamp" ]; then
            return 2
        fi
        IFS= read -r next < "$recipe_stamp" || return 2
        if [ -z "$next" ]; then
            return 2
        fi
        if [ "$have_key" = false ]; then
            recipe_key=$next
            have_key=true
        elif [ "$recipe_key" != "$next" ]; then
            return 2
        fi
    done < <(prebuilt_class_artifacts "$class")

    if [ "$have_key" = false ]; then
        return 1
    fi

    printf '%s\n' "$recipe_key"
}

# Local validity is stamp-based by design.  We trust a matching recipe-key
# stamp as the local/cache contract and do not re-hash raw artifact contents
# here; external cache or filesystem corruption is intentionally left as a
# human-debuggable failure outside the planner's responsibility.
resolve_local_class() {
    local class=$1
    local recipe_key=
    local status=missing

    if class_any_artifact_exists "$class"; then
        if [ "$ignore_sha" = true ]; then
            if class_all_artifacts_exist "$class"; then
                status=ignored
            else
                status=unmanaged
            fi
        elif recipe_key=$(class_local_recipe_key "$class" 2>/dev/null); then
            status=valid
        else
            status=unmanaged
        fi
    fi

    prebuilt_plan_set_class_var "$class" local_recipe_key "$recipe_key"
    prebuilt_plan_set_class_var "$class" local_status "$status"
}

# The action is the only authority consumed by materialize-artifacts.sh:
# use-local, download-release, build, skip, or blocked-*.  Summary booleans such
# as requires_build are derived from these actions for workflow convenience.
decide_action() {
    local current_recipe_key=$1
    local release_recipe_key=$2
    local local_recipe_key=$3
    local local_status=$4

    if [ "$local_status" = ignored ]; then
        printf '%s\n' use-local
        return
    fi

    if [ "$local_status" = valid ] && [ "$local_recipe_key" = "$current_recipe_key" ]; then
        printf '%s\n' use-local
        return
    fi

    if [ "$strict" = false ]; then
        case "$local_status" in
            unmanaged) printf '%s\n' blocked-unmanaged; return ;;
            valid) printf '%s\n' blocked-stale; return ;;
        esac
    fi

    if [ -n "$release_recipe_key" ] && [ "$release_recipe_key" = "$current_recipe_key" ]; then
        printf '%s\n' download-release
    else
        printf '%s\n' build
    fi
}

class_needs_release_manifest() {
    local current_recipe_key=$1
    local local_recipe_key=$2
    local local_status=$3

    if [ "$local_status" = ignored ]; then
        return 1
    fi

    if [ "$local_status" = valid ] && [ "$local_recipe_key" = "$current_recipe_key" ]; then
        return 1
    fi

    if [ "$strict" = false ]; then
        case "$local_status" in
            unmanaged|valid) return 1 ;;
        esac
    fi

    return 0
}

current_recipe_classes_sha1() {
    local class

    while IFS= read -r class; do
        prebuilt_plan_get_class_var "$class" current_recipe_key
    done < <(prebuilt_classes) | prebuilt_sha1sum | awk '{print $1}'
}

while IFS= read -r class; do
    prebuilt_plan_set_class_var "$class" current_recipe_key "$(prebuilt_class_recipe_key "$class")"
    prebuilt_plan_set_class_var "$class" local_recipe_key ''
    prebuilt_plan_set_class_var "$class" local_status missing
    resolve_local_class "$class"
done < <(prebuilt_classes)
current_recipe_classes_sha1=$(current_recipe_classes_sha1)

release_manifest_needed=true
if [ "$local_first" = true ]; then
    release_manifest_needed=false
    while IFS= read -r class; do
        if prebuilt_plan_class_is_selected "$class" && \
           class_needs_release_manifest \
               "$(prebuilt_plan_get_class_var "$class" current_recipe_key)" \
               "$(prebuilt_plan_get_class_var "$class" local_recipe_key)" \
               "$(prebuilt_plan_get_class_var "$class" local_status)"; then
            release_manifest_needed=true
            break
        fi
    done < <(prebuilt_classes)
fi

if [ "$release_manifest_needed" = true ]; then
    if ! fetch_release_manifest; then
        if [ "$bootstrap_on_404" = true ] && [ "$release_manifest_http_code" = 404 ]; then
            echo "prebuilt manifest is missing; bootstrapping from source" >&2
        else
            exit 1
        fi
    fi
fi

while IFS= read -r class; do
    if prebuilt_plan_class_is_selected "$class"; then
        action=$(decide_action \
            "$(prebuilt_plan_get_class_var "$class" current_recipe_key)" \
            "$(prebuilt_plan_get_class_var "$class" release_recipe_key)" \
            "$(prebuilt_plan_get_class_var "$class" local_recipe_key)" \
            "$(prebuilt_plan_get_class_var "$class" local_status)")
    else
        action=skip
    fi
    prebuilt_plan_set_class_var "$class" action "$action"
done < <(prebuilt_classes)

requires_build=false
blocked=false
while IFS= read -r class; do
    case "$(prebuilt_plan_get_class_var "$class" action)" in
        build) requires_build=true ;;
        blocked-*) blocked=true ;;
    esac
done < <(prebuilt_classes)

# A local-first plan may skip the release manifest when selected local artifacts
# are already usable. In that case release_needs_update stays false for plan
# compatibility; publish gates must use a non-local-first resolver pass.
release_needs_update=false
if [ "$release_manifest_needed" = true ]; then
    while IFS= read -r class; do
        if prebuilt_plan_class_is_selected "$class" && \
           [ "$(prebuilt_plan_get_class_var "$class" release_recipe_key)" != "$(prebuilt_plan_get_class_var "$class" current_recipe_key)" ]; then
            release_needs_update=true
            break
        fi
    done < <(prebuilt_classes)
fi

printf 'plan_version=1\n'
prebuilt_plan_print_class_values action
prebuilt_plan_print_class_values current_recipe_key
printf 'current_recipe_classes_sha1=%s\n' "$current_recipe_classes_sha1"
prebuilt_plan_print_class_values release_recipe_key
prebuilt_plan_print_class_values local_recipe_key
prebuilt_plan_print_class_values local_status
printf 'release_manifest_available=%s\n' "$release_manifest_available"
printf 'release_manifest_sha1=%s\n' "$release_manifest_sha1"
printf 'requires_build=%s\n' "$requires_build"
printf 'release_needs_update=%s\n' "$release_needs_update"
printf 'blocked=%s\n' "$blocked"
printf 'strict=%s\n' "$strict"
printf 'ignore_sha=%s\n' "$ignore_sha"
