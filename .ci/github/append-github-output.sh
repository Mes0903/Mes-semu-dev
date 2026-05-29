#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "Usage: append-github-output.sh <file> [allowed-key ...]" >&2
    exit 1
fi

file=$1
shift

: "${GITHUB_OUTPUT:?Need GITHUB_OUTPUT from GitHub Actions}"

awk -v keys="$*" '
    BEGIN {
        has_filter = (keys != "")
        count = split(keys, key_list, " ")
        for (i = 1; i <= count; i++) {
            if (key_list[i] != "") allowed[key_list[i]] = 1
        }
    }
    /^[A-Za-z_][A-Za-z0-9_]*=/ {
        key = $0
        sub(/=.*/, "", key)
        if (!has_filter || key in allowed) print
        next
    }
    {
        print "::error::Unexpected non-output line: " $0 > "/dev/stderr"
        bad = 1
    }
    END { exit bad }
' "$file" >> "$GITHUB_OUTPUT"
