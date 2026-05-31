#!/usr/bin/env bash
#
# CI planner for prebuilt guest artifacts. It never mutates raw artifacts; it
# compares the current recipe keys, the rolling-release recipe keys, and any
# restored workflow-cache stamps, then prints a KEY=VALUE plan for the CI
# materializer. Local Make builds do not use this script.

set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/../.." && pwd)
cd "$REPO_ROOT"

# Load the CI class registry and recipe-key helpers. The build recipe itself
# lives under scripts/prebuilt; this file only describes artifact identity and
# cache/release keys for CI decisions.
# shellcheck disable=SC1090,SC1091
. "$SCRIPT_DIR/artifact-inputs.sh"

# This is only for fork/mirror first-run bootstrap when their rolling prebuilt
# release has never existed. Other manifest fetch failures stay fatal.
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
release_manifest_http_code=

while IFS= read -r class; do
    prebuilt_plan_set_class_var "$class" release_recipe_key ''
done < <(prebuilt_classes)

# prebuilt.sha1 stores recipe keys as virtual sha1sum entries such as
# Image.recipe-key. Those entries describe source/recipe state, not archive
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
        next=$(awk -v f="$entry" '$2 == f {print $1; exit}' "$manifest")
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

class_local_recipe_key() {
    local class=$1
    local artifact
    local recipe_stamp
    local recipe_key=
    local next=
    local have_key=false

    while IFS= read -r artifact; do
        recipe_stamp=$(prebuilt_artifact_recipe_stamp "$artifact")
        if [ ! -f "$artifact" ] || [ ! -f "$recipe_stamp" ]; then
            return 1
        fi
        IFS= read -r next < "$recipe_stamp" || return 1
        if [ -z "$next" ]; then
            return 1
        fi
        if [ "$have_key" = false ]; then
            recipe_key=$next
            have_key=true
        elif [ "$recipe_key" != "$next" ]; then
            return 1
        fi
    done < <(prebuilt_class_artifacts "$class")

    if [ "$have_key" = false ]; then
        return 1
    fi

    printf '%s\n' "$recipe_key"
}

# Local state here means a workflow cache restore, not a developer workspace.
# CI trusts a matching recipe-key stamp as the cache contract; raw artifact
# content corruption is intentionally left as a normal CI failure to inspect.
resolve_local_class() {
    local class=$1
    local recipe_key=
    local status=missing

    if class_any_artifact_exists "$class"; then
        if recipe_key=$(class_local_recipe_key "$class" 2>/dev/null); then
            status=valid
        else
            status=unmanaged
        fi
    fi

    prebuilt_plan_set_class_var "$class" local_recipe_key "$recipe_key"
    prebuilt_plan_set_class_var "$class" local_status "$status"
}

# The action is the only authority consumed by materialize-artifacts.sh:
# use-local, download-release, build, or skip.
decide_action() {
    local current_recipe_key=$1
    local release_recipe_key=$2
    local local_recipe_key=$3
    local local_status=$4

    if [ "$local_status" = valid ] && [ "$local_recipe_key" = "$current_recipe_key" ]; then
        printf '%s\n' use-local
        return
    fi

    if [ -n "$release_recipe_key" ] && [ "$release_recipe_key" = "$current_recipe_key" ]; then
        printf '%s\n' download-release
    else
        printf '%s\n' build
    fi
}

platform_action_cache_tag() {
    local class

    while IFS= read -r class; do
        prebuilt_plan_get_class_var "$class" current_recipe_key
    done < <(prebuilt_classes) | prebuilt_sha1sum | awk '{print $1}'
}

while IFS= read -r class; do
    prebuilt_plan_set_class_var "$class" current_recipe_key "$(prebuilt_class_recipe_key "$class")"
    resolve_local_class "$class"
done < <(prebuilt_classes)
platform_action_cache_tag=$(platform_action_cache_tag)

if ! fetch_release_manifest; then
    if [ "$bootstrap_on_404" = true ] && [ "$release_manifest_http_code" = 404 ]; then
        echo "prebuilt manifest is missing; bootstrapping from source" >&2
    else
        exit 1
    fi
fi

while IFS= read -r class; do
    if prebuilt_plan_class_is_selected "$class"; then
        action=$(decide_action             "$(prebuilt_plan_get_class_var "$class" current_recipe_key)"             "$(prebuilt_plan_get_class_var "$class" release_recipe_key)"             "$(prebuilt_plan_get_class_var "$class" local_recipe_key)"             "$(prebuilt_plan_get_class_var "$class" local_status)")
    else
        action=skip
    fi
    prebuilt_plan_set_class_var "$class" action "$action"
done < <(prebuilt_classes)

requires_build=false
while IFS= read -r class; do
    case "$(prebuilt_plan_get_class_var "$class" action)" in
        build) requires_build=true ;;
    esac
done < <(prebuilt_classes)

release_needs_update=false
while IFS= read -r class; do
    if prebuilt_plan_class_is_selected "$class" && [ "$(prebuilt_plan_get_class_var "$class" release_recipe_key)" != "$(prebuilt_plan_get_class_var "$class" current_recipe_key)" ]; then
        release_needs_update=true
        break
    fi
done < <(prebuilt_classes)

printf 'plan_version=1\n'
prebuilt_plan_print_class_values action
prebuilt_plan_print_class_values current_recipe_key
printf 'platform_action_cache_tag=%s\n' "$platform_action_cache_tag"
prebuilt_plan_print_class_values release_recipe_key
prebuilt_plan_print_class_values local_recipe_key
prebuilt_plan_print_class_values local_status
printf 'release_manifest_available=%s\n' "$release_manifest_available"
printf 'requires_build=%s\n' "$requires_build"
printf 'release_needs_update=%s\n' "$release_needs_update"
