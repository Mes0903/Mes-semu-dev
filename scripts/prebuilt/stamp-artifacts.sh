#!/usr/bin/env bash
#
# Stamp helper shared by the user-facing build-image path and the materializer.
# A stamp records which recipe key produced an existing raw artifact; it is not
# an archive checksum and is not a publication manifest.

set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

# Stamping needs only the class registry and recipe-key helpers.  Keeping this
# helper thin lets both build-image and the materializer share the same stamp
# semantics without making stamp-artifacts another artifact-building entrypoint.
# The linter cannot resolve this runtime script directory.
# shellcheck disable=SC1090,SC1091
. "$SCRIPT_DIR/artifact-inputs.sh"

prebuilt_stamp_dir=.prebuilt

prebuilt_stamp_usage() {
    cat <<'USAGE'
Usage: stamp-artifacts.sh [--clear] <image|rootfs|test-tools|all>...

Build-system helper for make build-image. It records .prebuilt/*.recipe-key
stamps for raw artifacts that were already produced by scripts/build-image.sh.
With --clear, it removes the selected stamps before raw artifacts are rebuilt.
This helper accepts artifact classes, not build-image option flags.
USAGE
}

prebuilt_stamp_artifacts_for_class() {
    local class=$1
    local recipe_key=$2
    local artifact

    mkdir -p "$prebuilt_stamp_dir" || return 1
    while IFS= read -r artifact; do
        if [ ! -f "$artifact" ]; then
            echo "[!] Cannot stamp missing artifact: $artifact" >&2
            return 1
        fi
        printf '%s\n' "$recipe_key" > "$(prebuilt_artifact_recipe_stamp "$artifact")"
    done < <(prebuilt_class_artifacts "$class")
}

prebuilt_clear_artifacts_for_class() {
    local class=$1
    local artifact

    while IFS= read -r artifact; do
        rm -f "$(prebuilt_artifact_recipe_stamp "$artifact")"
    done < <(prebuilt_class_artifacts "$class")
    rmdir "$prebuilt_stamp_dir" 2>/dev/null || true
}

prebuilt_stamp_selected_targets() {
    local target_seen=false
    local clear=false
    local arg
    local class
    prebuilt_plan_for_each_class_set selected false

    for arg in "$@"; do
        case "$arg" in
            all)
                prebuilt_plan_select_all_classes
                target_seen=true
                ;;
            --clear)
                clear=true
                ;;
            --help|-h|help)
                prebuilt_stamp_usage
                return 0
                ;;
            --*)
                echo "[!] stamp-artifacts.sh is a build-system helper; unsupported helper option: $arg" >&2
                prebuilt_stamp_usage >&2
                return 1
                ;;
            *)
                if prebuilt_class_plan_prefix "$arg" >/dev/null 2>&1; then
                    prebuilt_plan_select_class "$arg" || return 1
                    target_seen=true
                else
                    echo "[!] stamp-artifacts.sh is a build-system helper; unsupported artifact class: $arg" >&2
                    prebuilt_stamp_usage >&2
                    return 1
                fi
                ;;
        esac
    done

    if [ "$target_seen" = false ]; then
        echo "[!] stamp-artifacts.sh requires an artifact class to stamp" >&2
        prebuilt_stamp_usage >&2
        return 1
    fi

    while IFS= read -r class; do
        if ! prebuilt_plan_class_is_selected "$class"; then
            continue
        fi
        if [ "$clear" = true ]; then
            prebuilt_clear_artifacts_for_class "$class"
        else
            prebuilt_stamp_artifacts_for_class "$class" "$(prebuilt_class_recipe_key "$class")"
        fi
    done < <(prebuilt_classes)
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    prebuilt_stamp_selected_targets "$@"
fi
