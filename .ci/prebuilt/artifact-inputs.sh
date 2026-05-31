#!/usr/bin/env bash

# CI prebuilt metadata shared by resolver, materializer, packager,
# stamp helper, and tests.  The registry intentionally contains
# only stable class identity: user-facing class name, KEY=VALUE plan prefix,
# and raw artifact filenames.  File recipe inputs stay in prebuilt_class_inputs()
# because they are easier to review as explicit per-class lists; selected
# variables from scripts/prebuilt/artifact-recipe.env are added as virtual
# manifest entries so CI invalidation can stay per-class without shaping the
# local build recipe into multiple env files.
#
# Adding a new class should start here, then add its recipe inputs and build
# implementation.  GitHub workflow artifact path lists may still need explicit
# updates because Actions YAML cannot consume this shell registry dynamically.
prebuilt_class_registry() {
    printf '%s\t%s\t%s\n' image image Image
    printf '%s\t%s\t%s\n' rootfs rootfs rootfs.cpio
    printf '%s\t%s\t%s\n' test-tools test_tools test-tools.img
}

prebuilt_classes() {
    local class
    local _prefix
    local _artifacts

    while IFS=$'\t' read -r class _prefix _artifacts; do
        printf '%s\n' "$class"
    done < <(prebuilt_class_registry)
}

# test-tools payload selection is build behavior first and CI key metadata
# second. Source the build-owned helper so local builds and CI keys cannot drift.
# shellcheck disable=SC1090,SC1091
. "$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../scripts/prebuilt" && pwd)/test-tools-payloads.sh"

prebuilt_class_inputs() {
    case "$1" in
        image)
            printf '%s\n' \
                configs/buildroot.config \
                configs/linux.config \
                scripts/prebuilt/build-artifacts.sh
            ;;
        rootfs)
            printf '%s\n' \
                configs/buildroot.config \
                configs/busybox.config \
                scripts/prebuilt/build-artifacts.sh \
                target/init
            ;;
        test-tools)
            prebuilt_test_tools_payload_key >/dev/null || return 1
            printf '%s\n' \
                configs/buildroot.config \
                configs/busybox.config \
                scripts/prebuilt/build-artifacts.sh \
                scripts/prebuilt/test-tools-payloads.sh \
                scripts/rootfs_ext4.sh \
                target/init
            if prebuilt_test_tools_payload_is_enabled x11; then
                printf '%s\n' configs/x11.config
            fi
            if prebuilt_test_tools_payload_is_enabled directfb2; then
                printf '%s\n' \
                    configs/riscv-cross-file \
                    target/local-env.sh
            fi
            ;;
        *)
            echo "[!] Unknown prebuilt input class: $1" >&2
            return 1
            ;;
    esac
}

prebuilt_inputs() {
    local class

    while IFS= read -r class; do
        prebuilt_class_inputs "$class"
    done < <(prebuilt_classes) | awk '!seen[$0]++'
}

prebuilt_class_artifacts() {
    local want=$1
    local class
    local _prefix
    local artifacts

    while IFS=$'\t' read -r class _prefix artifacts; do
        if [ "$class" = "$want" ]; then
            printf '%s\n' "$artifacts" | tr ' ' '\n'
            return 0
        fi
    done < <(prebuilt_class_registry)

    echo "[!] Unknown prebuilt artifact class: $want" >&2
    return 1
}

prebuilt_class_plan_prefix() {
    local want=$1
    local class
    local prefix
    local _artifacts

    while IFS=$'\t' read -r class prefix _artifacts; do
        if [ "$class" = "$want" ]; then
            printf '%s\n' "$prefix"
            return 0
        fi
    done < <(prebuilt_class_registry)

    echo "[!] Unknown prebuilt artifact class: $want" >&2
    return 1
}

# Resolver plans are shell-friendly KEY=VALUE files.  Class-scoped keys are
# generated from the registry prefix so resolver and materializer do not each
# hard-code image/rootfs/test-tools names.  The per-class action key is the
# authoritative materialization decision; the remaining class keys are metadata
# for diagnostics, publish gates, and stamping after a successful action.
prebuilt_plan_class_var_name() {
    local class=$1
    local kind=$2
    local prefix

    prefix=$(prebuilt_class_plan_prefix "$class") || return 1
    case "$kind" in
        selected) printf 'selected_%s\n' "$prefix" ;;
        action) printf '%s_action\n' "$prefix" ;;
        current_recipe_key) printf 'current_%s_recipe_key\n' "$prefix" ;;
        release_recipe_key) printf 'release_%s_recipe_key\n' "$prefix" ;;
        local_recipe_key) printf 'local_%s_recipe_key\n' "$prefix" ;;
        local_status) printf 'local_%s_status\n' "$prefix" ;;
        *) echo "[!] Unknown prebuilt plan variable kind: $kind" >&2; return 1 ;;
    esac
}

prebuilt_plan_set_class_var() {
    local class=$1
    local kind=$2
    local value=$3
    local var

    var=$(prebuilt_plan_class_var_name "$class" "$kind") || return 1
    printf -v "$var" '%s' "$value"
}

prebuilt_plan_get_class_var() {
    local class=$1
    local kind=$2
    local var

    var=$(prebuilt_plan_class_var_name "$class" "$kind") || return 1
    printf '%s\n' "${!var-}"
}

prebuilt_plan_for_each_class_set() {
    local kind=$1
    local value=$2
    local class

    while IFS= read -r class; do
        prebuilt_plan_set_class_var "$class" "$kind" "$value"
    done < <(prebuilt_classes)
}

prebuilt_plan_class_is_selected() {
    [ "$(prebuilt_plan_get_class_var "$1" selected)" = true ]
}

prebuilt_plan_select_all_classes() {
    prebuilt_plan_for_each_class_set selected true
}

prebuilt_plan_select_class() {
    if [ "$1" = all ]; then
        prebuilt_plan_select_all_classes
        return
    fi

    if prebuilt_class_plan_prefix "$1" >/dev/null 2>&1; then
        prebuilt_plan_set_class_var "$1" selected true
        return
    fi

    echo "[!] Unknown prebuilt artifact class: $1" >&2
    echo "[!] Expected one of: $(prebuilt_classes | tr '\n' ' ')or all" >&2
    return 1
}

prebuilt_plan_print_class_values() {
    local kind=$1
    local class

    while IFS= read -r class; do
        printf '%s=%s\n' \
            "$(prebuilt_plan_class_var_name "$class" "$kind")" \
            "$(prebuilt_plan_get_class_var "$class" "$kind")"
    done < <(prebuilt_classes)
}

prebuilt_plan_class_var_matches() {
    local plan_key=$1
    local class=$2
    local kind
    shift 2

    for kind in "$@"; do
        if [ "$plan_key" = "$(prebuilt_plan_class_var_name "$class" "$kind")" ]; then
            return 0
        fi
    done

    return 1
}

prebuilt_plan_key_is_output_class_var() {
    local plan_key=$1
    local class=$2

    prebuilt_plan_class_var_matches "$plan_key" "$class" \
        action \
        current_recipe_key \
        release_recipe_key \
        local_recipe_key \
        local_status
}

prebuilt_artifact_recipe_entry() {
    printf '%s.recipe-key\n' "$1"
}

prebuilt_class_recipe_entries() {
    local artifact

    while IFS= read -r artifact; do
        prebuilt_artifact_recipe_entry "$artifact"
    done < <(prebuilt_class_artifacts "$1")
}

prebuilt_artifact_recipe_stamp() {
    printf '.prebuilt/%s.recipe-key\n' "$1"
}


# Recipe keys are produced on Linux and consumed again on Linux/macOS test
# jobs. Keep this helper limited to sha1sum-compatible output over explicit
# relative paths so GNU sha1sum and macOS shasum generate identical manifests.
prebuilt_sha1sum() {
    if command -v sha1sum >/dev/null 2>&1; then
        sha1sum "$@"
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 1 "$@"
    else
        echo "[!] Need sha1sum or shasum on PATH" >&2
        return 1
    fi
}

prebuilt_recipe_env_file() {
    printf '%s\n' scripts/prebuilt/artifact-recipe.env
}

prebuilt_recipe_env_line() {
    local name=$1
    local env_file
    local line

    env_file=$(prebuilt_recipe_env_file)
    if [ ! -f "$env_file" ]; then
        echo "[!] Missing prebuilt recipe env $env_file" >&2
        return 1
    fi
    line=$(grep -E "^${name}=" "$env_file" || true)
    if [ -z "$line" ]; then
        echo "[!] Missing prebuilt recipe variable $name in $env_file" >&2
        return 1
    fi
    if [ "$(printf '%s\n' "$line" | wc -l | tr -d ' ')" != 1 ]; then
        echo "[!] Duplicate prebuilt recipe variable $name in $env_file" >&2
        return 1
    fi
    printf '%s\n' "$line"
}

prebuilt_class_recipe_env_vars() {
    local class=$1

    case "$class" in
        image)
            printf '%s\n' \
                BUILDROOT_REPO \
                BUILDROOT_REV \
                LINUX_REPO \
                LINUX_REV
            ;;
        rootfs)
            printf '%s\n' \
                BUILDROOT_REPO \
                BUILDROOT_REV
            ;;
        test-tools)
            printf '%s\n' \
                BUILDROOT_REPO \
                BUILDROOT_REV \
                TEST_TOOLS_SIZE_MB
            if prebuilt_test_tools_payload_is_enabled directfb2; then
                printf '%s\n' \
                    DIRECTFB2_REPO \
                    DIRECTFB2_REV \
                    DIRECTFB_EXAMPLES_REPO \
                    DIRECTFB_EXAMPLES_REV
            fi
            ;;
        *)
            echo "[!] Unknown prebuilt input class: $class" >&2
            return 1
            ;;
    esac
}

prebuilt_recipe_env_virtual_entry() {
    local name=$1
    local line

    line=$(prebuilt_recipe_env_line "$name") || return 1
    printf '%s\n' "$line" | prebuilt_sha1sum | \
        awk -v name="$name" '{print $1 "  artifact-recipe.env:" name}'
}

prebuilt_class_recipe_virtual_entries() {
    local class=$1
    local value
    local recipe_var

    while IFS= read -r recipe_var; do
        prebuilt_recipe_env_virtual_entry "$recipe_var"
    done < <(prebuilt_class_recipe_env_vars "$class")

    case "$class" in
        test-tools)
            value=$(prebuilt_test_tools_payload_key) || return 1
            printf '%s' "$value" | prebuilt_sha1sum | \
                awk '{print $1 "  test-tools.payloads"}'
            ;;
    esac
}

prebuilt_class_recipe_manifest() {
    local class=$1
    local -a inputs
    local input

    while IFS= read -r input; do
        inputs+=("$input")
    done < <(prebuilt_class_inputs "$class")

    if [ "${#inputs[@]}" -eq 0 ]; then
        echo "[!] Prebuilt input class $class has no inputs" >&2
        return 1
    fi

    for input in "${inputs[@]}"; do
        if [ ! -f "$input" ]; then
            echo "[!] Missing prebuilt input $input" >&2
            return 1
        fi
    done

    prebuilt_sha1sum "${inputs[@]}"
    prebuilt_class_recipe_virtual_entries "$class"
}

prebuilt_class_recipe_key() {
    local manifest

    manifest=$(prebuilt_class_recipe_manifest "$1") || return 1
    printf '%s\n' "$manifest" | prebuilt_sha1sum | awk '{print $1}'
}

prebuilt_usage() {
    cat <<'USAGE'
Usage: artifact-inputs.sh <classes|inputs|artifacts|recipe-key> [class]

Commands:
  classes           List prebuilt artifact classes.
  inputs <class>    List file inputs for one class recipe key.
  artifacts <class> List raw artifact files produced by one class.
  recipe-key <class> Print the current recipe key for one class.
                     Recipe keys also include selected artifact-recipe.env vars.

Environment:
  PREBUILT_TEST_TOOLS_PAYLOADS selects test-tools payloads. Unset means the
  canonical x11,directfb2 payload; use x11, directfb2, or minimal for local
  variants.
USAGE
}

prebuilt_main() {
    local cmd=${1:-}
    local class=${2:-}

    case "$cmd" in
        classes)
            prebuilt_classes
            ;;
        inputs)
            if [ -z "$class" ]; then
                prebuilt_usage >&2
                return 1
            fi
            prebuilt_class_inputs "$class"
            ;;
        artifacts)
            if [ -z "$class" ]; then
                prebuilt_usage >&2
                return 1
            fi
            prebuilt_class_artifacts "$class"
            ;;
        recipe-key)
            if [ -z "$class" ]; then
                prebuilt_usage >&2
                return 1
            fi
            prebuilt_class_recipe_key "$class"
            ;;
        --help|-h|help)
            prebuilt_usage
            ;;
        *)
            prebuilt_usage >&2
            return 1
            ;;
    esac
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    prebuilt_main "$@"
fi
