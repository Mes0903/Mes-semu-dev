#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)

# Verification follows the class registry rather than a release-service-specific
# file list.  That keeps package validation about artifact shape only; publish
# mechanics stay in the CI adapter.
# The linter cannot resolve this runtime script directory.
# shellcheck disable=SC1090,SC1091
. "$SCRIPT_DIR/artifact-inputs.sh"

if [ ! -f prebuilt.sha1 ]; then
    echo "[!] Missing prebuilt.sha1" >&2
    exit 1
fi

while IFS= read -r class; do
    while IFS= read -r artifact; do
        archive=${artifact}.bz2
        if [ ! -f "$archive" ]; then
            echo "[!] Missing $archive" >&2
            exit 1
        fi
    done < <(prebuilt_class_artifacts "$class")

    while IFS= read -r entry; do
        if ! awk -v f="$entry" '$2 == f {found = 1} END {exit !found}' prebuilt.sha1; then
            echo "[!] prebuilt.sha1 missing $entry" >&2
            exit 1
        fi
    done < <(prebuilt_class_recipe_entries "$class")
done < <(prebuilt_classes)
