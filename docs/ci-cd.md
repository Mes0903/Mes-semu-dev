# CI/CD Pipeline in semu

This document describes how semu builds, tests, caches, and publishes guest
artifacts in CI. It also defines the boundary between provider-neutral project
logic and provider-specific CI adapter logic.

## Architecture

The pipeline is split into two layers:

- The provider-neutral layer defines what semu needs: guest artifact input
  fingerprinting, prebuilt artifact resolution, package creation, package
  verification, local materialization behavior, and test commands.
- The provider adapter layer defines how a CI service runs those steps:
  triggers, job dependencies, cache restore/save, cross-job artifacts, release
  upload, and pull request review comments.

The GitHub Actions workflows are the current adapter. The `scripts/prebuilt/`
scripts and `mk/external.mk` are the stable contract that a GitLab CI, Gitea
Actions, or external storage adapter should reuse.

## Prebuilt Artifact Contract

Consumers read guest artifacts from `PREBUILT_URL` with fixed file names:

- `$PREBUILT_URL/Image.bz2`
- `$PREBUILT_URL/rootfs.cpio.bz2`
- `$PREBUILT_URL/test-tools.img.bz2`
- `$PREBUILT_URL/prebuilt.sha1`

`prebuilt.sha1` records the three recipe keys that describe the published guest
artifact state:

```text
<sha1>  Image.recipe-key
<sha1>  rootfs.cpio.recipe-key
<sha1>  test-tools.img.recipe-key
```

The `*.recipe-key` lines are virtual entries recording the SHA-1 recipe fingerprint
of semu-side files that define
the Image, rootfs, and test-tools classes. Each class fingerprint is the SHA-1 of
a per-file manifest in `sha1sum` format, so file contents, file names, order,
and boundaries are all part of the fingerprint.

The input graph has three published artifact classes:

- `Image.recipe-key` fingerprints the inputs that define the Linux Image artifact.
- `rootfs.cpio.recipe-key` fingerprints the inputs that define `rootfs.cpio`.
- `test-tools.img.recipe-key` fingerprints the inputs that define `test-tools.img`.

`scripts/prebuilt/artifact-inputs.sh` is the single source for those lists.
It also exposes a small command-line API used by `mk/external.mk`:

```shell
scripts/prebuilt/artifact-inputs.sh classes
scripts/prebuilt/artifact-inputs.sh inputs image
scripts/prebuilt/artifact-inputs.sh recipe-key rootfs
```

`mk/external.mk` exposes those same operations as local build-system targets
without expanding the input lists during Make parse time:

```shell
make prebuilt-classes
make prebuilt-inputs CLASS=image
make prebuilt-recipe-key CLASS=rootfs
make prebuilt-plan
```

## Recipe Key Model

Artifact decisions are made per class with three keys:

- The current recipe key is computed from the checked-out source/config files and
  artifact recipe files listed by `scripts/prebuilt/artifact-inputs.sh`.
- The release recipe key is read from `$PREBUILT_URL/prebuilt.sha1` via the
  `Image.recipe-key`, `rootfs.cpio.recipe-key`, and `test-tools.img.recipe-key` entries.
- The local recipe key is read from `.prebuilt/*.recipe-key` stamps beside local raw artifacts.

The provider-neutral resolver follows this order for each class:

```text
if local artifact exists and local recipe key == current recipe key:
    use-local
elif release recipe key == current recipe key:
    download-release
else:
    build
```

Local `make` defaults to non-strict mode. If it sees an unmanaged raw artifact
without a `.prebuilt/` stamp, or with a stale stamp that does not match current
inputs, it stops instead of overwriting the file. This protects old worktrees and
manual local builds. A user can opt into deterministic replacement with:

```shell
make PREBUILT_STRICT=1
```

CI adapters should run strict mode after restoring caches. In strict mode, stale or
unmanaged local artifacts are not trusted; the materializer downloads the
matching release artifact or rebuilds from source.

`.prebuilt/` contains local metadata for raw artifacts:

- `.prebuilt/Image.recipe-key`
- `.prebuilt/rootfs.cpio.recipe-key`
- `.prebuilt/test-tools.img.recipe-key`

The `*.recipe-key` files drive local/current recipe key decisions.

## Main CI Flow

`.github/workflows/main.yml` runs on `push` and `pull_request`.

The GitHub adapter reads prebuilt artifacts from the current repository's
`prebuilt` release by default. Forks or mirrors without that release enable the
`PREBUILT_BOOTSTRAP_ON_404` bootstrap mode in `guest-artifact-build`: an HTTP
404 for `prebuilt.sha1` is treated as "no release has been published yet", so
CI rebuilds from source until that repository publishes its own prebuilt release
or changes `PREBUILT_URL` to another artifact backend. The canonical
`sysprog21/semu` repository does not enable this exception; an HTTP 404 there is
an external release-state problem and fails fast for maintainer inspection.
Other manifest fetch failures still fail fast.

Every run starts with `guest-artifact-build`:

1. Check out the source tree.
2. Run `scripts/prebuilt/resolve-artifacts.sh`.
3. Compare current recipe keys with `$PREBUILT_URL/prebuilt.sha1` release recipe keys.
4. Set `release_needs_update=true` when the release manifest is a bootstrap
   HTTP 404, is missing recipe-key entries, or records a different Image, rootfs,
   or test-tools recipe key.

When `release_needs_update=false`, `guest-artifact-build` does not materialize,
package, or upload guest artifacts. Test jobs use
`.github/actions/provide-guest-artifacts`, which restores a release-downloaded
raw artifact cache keyed by the release manifest hash and the same prebuilt
pipeline surface used by the workflow-local raw artifact cache. On cache miss,
`make` calls `mk/external.mk`, which invokes
`scripts/prebuilt/plan-materialize.sh` to resolve a local-first plan and
then download the matching release archives through the materializer.

When `release_needs_update=true`, `guest-artifact-build` must provide a complete
workflow-local raw artifact set to all test jobs. On pull requests or non-master
branch pushes, it first tries to restore a raw artifact cache keyed by the exact
current recipe fingerprint and prebuilt pipeline hash:

```text
prebuilt-raw-${current_recipe_classes_sha1}-${prebuilt_pipeline_hash}
```

`prebuilt_pipeline_hash` covers the provider-neutral prebuilt scripts, the
artifact recipe metadata, the build recipe library, `scripts/rootfs_ext4.sh`,
and `mk/external.mk`. It intentionally does not include the `scripts/build-image.sh`
CLI wrapper; artifact-affecting build behavior belongs in
`scripts/prebuilt/build-artifacts.sh` or the relevant `scripts/prebuilt/artifact-recipe-*.env` file.

That cache contains raw artifacts, packaged archives, the workflow-local manifest,
and local metadata:

- `Image`
- `rootfs.cpio`
- `test-tools.img`
- `Image.bz2`
- `rootfs.cpio.bz2`
- `test-tools.img.bz2`
- `prebuilt.sha1`
- `.prebuilt/`

After cache restore, CI runs `scripts/prebuilt/resolve-artifacts.sh` again in strict
mode. This second resolver pass validates restored cache recipe-key metadata
instead of trusting the cache hit. Build dependencies are installed only when
that strict post-cache decision still requires a source build.

CI then runs `scripts/prebuilt/materialize-artifacts.sh` with `PREBUILT_STRICT=1`
and `PREBUILT_PLAN_FILE` pointing at the strict post-cache resolver output.
The materializer only executes that supplied provider-neutral plan: reuse
valid local raw artifacts, download release artifacts for matching classes,
or invoke `scripts/prebuilt/build-plan-artifacts.sh` for classes that must
be rebuilt from source. Local `make` reaches the same executor through
`scripts/prebuilt/plan-materialize.sh`, which first creates a local-first
resolver plan. `scripts/build-image.sh` is only the user-facing wrapper; the
artifact-affecting recipe metadata lives in `scripts/prebuilt/artifact-recipe-*.env`,
and build behavior lives in `scripts/prebuilt/build-artifacts.sh` plus the internal
plan builder. Those files are part of the recipe key.

After materialization, CI packages the raw artifacts with
`scripts/prebuilt/package.sh` and uploads two workflow artifacts:

- `guest-artifacts-raw`: raw files, `prebuilt.sha1`, and `.prebuilt/` for test
  jobs.
- `guest-artifacts-packaged`: compressed release files and `prebuilt.sha1` for
  the publish job.

Downstream Linux, legacy initramfs, and macOS test jobs use
`.github/actions/provide-guest-artifacts`. When `release_needs_update=true`, the
action downloads `guest-artifacts-raw`, sets `PREBUILT_URL=file://$PWD`, sets
`PREBUILT_STRICT=1`, and lets `make` validate the recipe-key stamps for
workflow-provided raw files through the materializer. When
`release_needs_update=false`, the action restores the release-download cache and
still runs `make` in strict mode, so stale or unmanaged cached raw artifacts
cannot silently pass as valid.

## Decision Graph

```text
main.yml
  |
  v
guest-artifact-build
  |
  +-- scripts/prebuilt/resolve-artifacts.sh
  |     |
  |     +-- scripts/prebuilt/artifact-inputs.sh
  |     |     - prebuilt_class_recipe_key image
  |     |     - prebuilt_class_recipe_key rootfs
  |     |     - prebuilt_class_recipe_key test-tools
  |     |
  |     +-- compare current recipe keys with $PREBUILT_URL/prebuilt.sha1
  |           |
  |           +-- release recipe keys match current recipe keys
  |           |     -> release_needs_update=false
  |           |     -> no package/upload in guest-artifact-build
  |           |
  |           +-- release missing / old manifest / key mismatch
  |                 -> release_needs_update=true
  |
  +-- release_needs_update=true?
        |
        +-- no
        |     |
        |     v
        |   test jobs
        |     |
        |     +-- .github/actions/provide-guest-artifacts
        |     |     - restore release-downloaded raw artifact cache
        |     |     - set PREBUILT_STRICT=1
        |     |
        |     +-- make / mk/external.mk
        |           - calls scripts/prebuilt/plan-materialize.sh
        |           - resolves a local-first plan for requested classes
        |           - materializer validates local stamps or downloads release archives
        |
        +-- yes
              |
              +-- PR or non-master branch?
              |     |
              |     +-- yes: restore raw artifact cache
              |     |          key = prebuilt-raw-${current_recipe_classes_sha1}-${prebuilt_pipeline_hash}
              |     |
              |     +-- no: master push skips raw cache
              |
              +-- scripts/prebuilt/resolve-artifacts.sh with PREBUILT_STRICT=1
              |     - validates restored cache state
              |     - decides whether source build is still required
              |
              +-- install build dependencies only if source build is required
              |
              +-- scripts/prebuilt/materialize-artifacts.sh with PREBUILT_STRICT=1
              |     - reads PREBUILT_PLAN_FILE
              |     - use-local for valid cached/workspace artifacts
              |     - download-release for matching release classes
              |     - build for classes that do not match release/local recipe keys
              |
              +-- scripts/prebuilt/package.sh
              |     - writes Image.bz2/rootfs.cpio.bz2/test-tools.img.bz2
              |     - writes prebuilt.sha1 with *.recipe-key entries
              |
              +-- upload workflow artifacts
                    |
                    +-- guest-artifacts-raw
                    |     - Image/rootfs.cpio/test-tools.img
                    |     - prebuilt.sha1
                    |     - .prebuilt/
                    |     -> test jobs validate through make/materializer
                    |
                    +-- guest-artifacts-packaged
                          -> publish-prebuilt after all tests pass on master
                             - scripts/prebuilt/verify-package.sh
                             - upload rolling prebuilt release
```

## Publish Flow

`publish-prebuilt` is part of `main.yml`, but it only runs when all of these are
true:

- The event is `push`.
- The ref is `refs/heads/master`.
- `guest-artifact-build` reported `release_needs_update=true`.
- All required test and style jobs passed.

Only the first non-local-first resolver pass is a publish gate. A local-first
materialization plan may skip the release manifest when selected local artifacts
are already current; in that case `release_needs_update=false` only means that
this plan did not check the release, not that the rolling release is current.

The publish job does not build or repackage guest artifacts. It checks out the
repository only to get `scripts/prebuilt/verify-package.sh`, downloads the tested
`guest-artifacts-packaged` workflow artifact, verifies the package, and uploads
the release files to the rolling `prebuilt` release. Master push runs are not
canceled while in progress, and the publish job uses the `prebuilt-release`
concurrency group so a later publish job cannot interrupt an active release
upload.

This keeps the normal master path as:

- Resolve whether the release needs an update.
- Materialize and package once.
- Test the same artifacts.
- Publish the same artifacts.

This flow intentionally does not publish when only CI packaging, release
plumbing, release notes, or manifest-writing code changes while the guest
artifact input classes still match the current rolling release. In that case
there is no new `Image`, `rootfs.cpio`, or `test-tools.img` content to publish.
If a future change needs to republish the same guest artifact content under a
new package or manifest format, add an explicit force-publish path instead of
silently widening the normal master publish gate.

The rolling release is assumed to be internally consistent once published. The
normal CI path does not try to self-repair missing or corrupted release assets
when the release recipe keys already match the current source tree.

## Responsibility Boundaries

### Provider-neutral project logic

These files must not depend on GitHub Actions, GitLab CI, Gitea Actions, or
provider-specific variables such as `GITHUB_OUTPUT`:

| Path | Responsibility |
|------|----------------|
| `scripts/prebuilt/artifact-inputs.sh` | Defines guest artifact input files, artifact classes, local CLI queries, and SHA-1 helper functions. |
| `scripts/prebuilt/artifact-recipe-*.env` | Records immutable artifact-affecting repository revisions and sizing constants used by the build recipe, split by recipe domain. |
| `scripts/prebuilt/build-artifacts.sh` | Implements the artifact build recipe shared by local builds and CI materialization. |
| `scripts/prebuilt/build-plan-artifacts.sh` | Internal builder used by the materializer to execute build actions from a resolver plan without writing stamps itself. |
| `scripts/prebuilt/resolve-artifacts.sh` | Compares current recipe keys, release manifest keys, and local `.prebuilt/*.recipe-key` stamps to decide whether each artifact class should use local files, download the release, or rebuild. |
| `scripts/prebuilt/stamp-artifacts.sh` | Writes `.prebuilt/` metadata that ties raw guest artifacts to recipe keys. |
| `scripts/prebuilt/plan-materialize.sh` | Local/build-system wrapper that resolves a local-first plan and passes it to the materializer. |
| `scripts/prebuilt/materialize-artifacts.sh` | Executes a supplied resolver plan by reusing local artifacts, downloading release artifacts, or invoking the internal source build path. |
| `scripts/prebuilt/package.sh` | Compresses raw artifacts and writes `prebuilt.sha1`. |
| `scripts/prebuilt/verify-package.sh` | Verifies packaged artifact presence and required `prebuilt.sha1` recipe-key entries. |
| `.ci/prebuilt/test-flow.sh` | Tests packaging, resolver/materializer behavior, and Makefile consumption of prebuilt artifacts. |
| `.ci/common.sh` | Shared test script helpers such as cleanup, assertions, and timeouts. |
| `.ci/autorun.sh`, `.ci/device-smoke/test-*.sh` | Smoke test commands that CI adapters can call directly. |
| `.ci/check-format.sh` | Formatting enforcement command. |
| `mk/external.mk` | Defines Make external artifact targets, local prebuilt query targets, and delegates local-first resolution/materialization to `scripts/prebuilt/plan-materialize.sh`. |
| `scripts/build-image.sh` | User-facing wrapper for the artifact build recipe. It parses targets/options and calls `scripts/prebuilt/build-artifacts.sh`. |

`mk/external.mk` keeps the upstream GitHub release as the local `make` default
because upstream GitHub is the current public artifact host. CI adapters should
set `PREBUILT_URL` explicitly for their own artifact backend.

### GitHub adapter logic

These files are intentionally GitHub-specific:

| Path | Responsibility |
|------|----------------|
| `.github/workflows/main.yml` | GitHub CI trigger, job graph, caches, workflow artifacts, release publish gate, and `$GITHUB_OUTPUT` plumbing. |
| `.github/actions/setup-semu/action.yml` | GitHub composite action that installs Linux/macOS test dependencies. |
| `.github/actions/provide-guest-artifacts/action.yml` | GitHub composite action that provides test-job guest artifacts: workflow-built raw artifact download for release-update runs, release-download cache for no-update runs, strict materialization, and local `PREBUILT_URL` wiring. |
| `.ci/github/suggest-format.sh` | Uses reviewdog's GitHub reporter to post pull request format suggestions. |

If semu gains a GitLab or Gitea adapter, it should call the provider-neutral
scripts above and replace only the adapter mechanisms:

| Current GitHub mechanism | Portable role |
|--------------------------|---------------|
| `actions/cache` | Restore/save CI cache entries. |
| `actions/upload-artifact` and `actions/download-artifact` | Move workflow-built guest artifacts between jobs. |
| `softprops/action-gh-release` | Upload release/package assets to the selected prebuilt backend. |
| `${{ github.* }}` and `$GITHUB_OUTPUT` | Provider expression and job-output plumbing. |
| reviewdog GitHub reporter | Pull request review suggestion adapter. |

## Non-master and Pull Request Behavior

On non-master branch pushes and pull requests, guest input changes that differ
from the rolling release still produce fresh guest artifacts for testing, but CI
does not publish them to the rolling release.

The raw artifact cache avoids rebuilding Buildroot repeatedly for the same
current recipe fingerprint. This is intentionally keyed by recipe hashes
rather than by the pull request SHA, so normal code-only commits can reuse the
same guest artifacts.

No restore prefix is used for this cache. A different recipe fingerprint
must miss the cache and rebuild or reassemble from source plus release assets.

## Local Consumer Behavior

For local builds, `mk/external.mk` routes each requested raw artifact target
through `scripts/prebuilt/plan-materialize.sh` with its artifact class. The
default `make check` path requests `Image` and the normal rootfs path
(`rootfs.cpio` directly for initramfs boot, or through `ext4.img` for external
rootfs boot). `test-tools.img` is materialized only when a target explicitly
needs that replacement disk, such as `make test-tools.img` or
`RUN_DISK=test-tools.img`.

The wrapper asks the resolver for a local-first plan. The resolver first
compares the current recipe keys with local `.prebuilt/` stamps. When all
requested local artifacts are already current, the plan reuses them without
fetching `$PREBUILT_URL/prebuilt.sha1`; otherwise it reads the release manifest
before deciding whether to download release archives or rebuild from source.
The materializer then executes only that plan.

Local inspection targets expose the same provider-neutral model used by CI:

```shell
make prebuilt-classes
make prebuilt-inputs CLASS=image
make prebuilt-recipe-key CLASS=test-tools
make prebuilt-plan
```

The default is:

```make
PREBUILT_URL ?= https://github.com/sysprog21/semu/releases/download/prebuilt
```

Users and CI adapters can override it:

```shell
make PREBUILT_URL=https://example.org/semu/prebuilt
```

The local default is non-strict: unmanaged or stale raw artifacts are not
silently overwritten. Use `PREBUILT_STRICT=1` when the caller wants deterministic
replacement of stale or unmanaged local artifacts, which is the mode CI adapters
should use after restoring their raw artifact cache.

Use `PREBUILT_IGNORE_SHA=1` when the caller intentionally wants existing local
raw artifacts to be trusted without matching `.prebuilt/*.recipe-key` stamps.
This is a local escape hatch for manual artifacts; CI should not use it.

`make` suppresses materializer machine output by default. Use
`make prebuilt-plan` for a standalone decision dump, or set
`PREBUILT_VERBOSE=1` when debugging a `make` command that materializes guest
artifacts.

If no valid local artifact exists and the release manifest is stale, the
resolver chooses the source build path. If the release manifest is unavailable,
resolution fails fast instead of treating the external outage as a reason to
rebuild or publish. On machines that are not prepared to build guest artifacts,
run `make prebuilt-plan` first to see whether local `make` will download or
build.

`make clean` keeps `.prebuilt/` so downloaded or locally materialized artifacts
remain tied to their recipe keys across emulator rebuilds. `make distclean`
removes `.prebuilt/` together with the raw guest artifacts.
