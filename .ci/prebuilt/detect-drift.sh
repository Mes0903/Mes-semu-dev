#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$SCRIPT_DIR/inputs.sh"

: "${PREBUILT_URL:?Need PREBUILT_URL pointing at the prebuilt artifact directory}"
manifest=prebuilt.sha1
manifest_url="${PREBUILT_URL}/${manifest}"

live=$(prebuilt_inputs_sha1)
remote=

if ! curl --fail --silent --show-error --retry 3 --retry-delay 1 \
    -L -o "$manifest" "$manifest_url"; then
    echo "prebuilt manifest is unavailable at $manifest_url; will rebuild from source" >&2
    echo "should_build=true"
    echo "live_inputs_sha1=$live"
    echo "remote_inputs_sha1="
    exit 0
fi

remote=$(awk '$2 == "inputs" {print $1}' "$manifest")
if [ -z "$remote" ]; then
    echo "prebuilt manifest has no inputs hash; will rebuild from source" >&2
    echo "should_build=true"
    echo "live_inputs_sha1=$live"
    echo "remote_inputs_sha1="
    exit 0
fi

if [ "$live" = "$remote" ]; then
    echo "guest artifact inputs match the prebuilt ($live); skipping rebuild" >&2
    echo "should_build=false"
else
    echo "guest artifact inputs drifted ($live != $remote); will rebuild from source" >&2
    echo "should_build=true"
fi

echo "live_inputs_sha1=$live"
echo "remote_inputs_sha1=$remote"
