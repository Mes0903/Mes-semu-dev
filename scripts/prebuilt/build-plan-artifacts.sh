#!/usr/bin/env bash
#
# Internal prebuilt builder. It builds raw artifacts requested by a resolver plan
# and deliberately does not write .prebuilt stamps; the materializer commits
# stamps after the plan action succeeds.  Keeping this separate from the
# user-facing build-image.sh avoids feeding build-image CLI side effects back
# into the provider-neutral materialization layer.

set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/../.." && pwd)
cd "$REPO_ROOT"

# Reuse the build recipe library, but keep stamping outside this wrapper.  The
# materializer owns the commit point because only it knows which resolver action
# completed successfully.
# The linter cannot resolve this runtime script directory.
# shellcheck disable=SC1090,SC1091
. "$SCRIPT_DIR/build-artifacts.sh"

build_prebuilt_artifact_classes "$@"
