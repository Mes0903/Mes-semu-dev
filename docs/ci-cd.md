# CI/CD Pipeline in semu

This document describes how semu builds, tests, caches, and publishes guest
artifacts in CI. The important split is deliberate:

- Local development keeps the normal Make file-target model. Missing raw guest
  artifacts are downloaded from the rolling `prebuilt` release by default, while
  source builds stay explicit through `make build-image`.
- CI owns prebuilt artifact decisions. The CI prebuilt layer decides whether a
  workflow should reuse restored raw artifacts, download the rolling release, or
  rebuild guest artifacts from source.
- The GitHub Actions adapter adds provider-specific transport: cache restore,
  workflow artifacts, release uploads, job outputs, and publish gating.

This keeps local builds understandable while still avoiding full guest rebuilds
on every CI push.

## Artifact Contract

The rolling `prebuilt` release exposes fixed names under `PREBUILT_URL`:

- `$PREBUILT_URL/Image.bz2`
- `$PREBUILT_URL/rootfs.cpio.bz2`
- `$PREBUILT_URL/test-tools.img.bz2`
- `$PREBUILT_URL/prebuilt.sha1`

`prebuilt.sha1` records recipe-key entries only:

```text
<sha1>  Image.recipe-key
<sha1>  rootfs.cpio.recipe-key
<sha1>  test-tools.img.recipe-key
```

A recipe key is the SHA-1 fingerprint of the semu-side files and selected
recipe variables that define one artifact class. It is not a checksum of the
published `.bz2` archive bytes. CI intentionally treats a successful archive
fetch/decompression plus a matching recipe-key manifest as sufficient for normal
release consumption. If release assets become stale, corrupt, or inconsistent
with `prebuilt.sha1`, that is an external release-state failure for maintainers
to inspect rather than a self-healing condition in the resolver/materializer.

The CI registry for those classes lives in `.ci/prebuilt/artifact-inputs.sh`.
The build recipe itself lives under `scripts/prebuilt/`, so local and CI builds
share the same source build logic. `scripts/prebuilt/artifact-recipe.env`
remains a single local build recipe file; CI reads only the variables relevant
to each class into virtual manifest entries.

The published classes are:

- `image`: produces `Image`.
- `rootfs`: produces `rootfs.cpio`.
- `test-tools`: produces `test-tools.img`.

`test-tools` has selectable payloads through `PREBUILT_TEST_TOOLS_PAYLOADS`.
The canonical CI payload is `x11,directfb2`; smaller local variants such as
`minimal` or `x11` get different recipe keys.

## Local Build Path

Local Make does not call the CI resolver or materializer. The local file targets
are intentionally simple:

```text
Image          -> download $PREBUILT_URL/Image.bz2
rootfs.cpio    -> download $PREBUILT_URL/rootfs.cpio.bz2
test-tools.img -> download $PREBUILT_URL/test-tools.img.bz2
ext4.img       -> scripts/rootfs_ext4.sh rootfs.cpio ext4.img
```

That means:

- Fresh local `make check` consumes the rolling release instead of running the
  source builder. Override `PREBUILT_URL` only when you intentionally want a
  different release directory.
- If a raw artifact already exists, Make can use it as an ordinary file target.
- After changing guest configs, run `make build-image BUILD_IMAGE_ARGS=...`
  explicitly when you want the build system to refresh existing artifacts from
  source. Local Make does not compare configs against CI recipe keys and will
  not warn that the rolling release differs from your working tree.
- `.prebuilt/*.recipe-key` metadata is CI cache metadata, not a local Make
  validation contract.

The public local source-build entrypoint is:

```text
make build-image BUILD_IMAGE_ARGS="<image|rootfs|test-tools|all> ..."
```


For example:

```shell
make build-image BUILD_IMAGE_ARGS=image
make build-image BUILD_IMAGE_ARGS="rootfs --no-ext4"
make build-image BUILD_IMAGE_ARGS="test-tools --test-tools-payloads=x11"
make build-image BUILD_IMAGE_ARGS="all --clean-build"
```

## CI Decision Model

CI makes decisions per artifact class using three states:

- `current_recipe_key`: computed from the checked-out source/config/recipe files.
- `release_recipe_key`: read from `$PREBUILT_URL/prebuilt.sha1`.
- `local_recipe_key`: read from `.prebuilt/*.recipe-key` restored from the
  workflow-local raw artifact cache.

The resolver emits a `KEY=VALUE` plan. For each selected class, the action is:

```text
if restored raw artifact has local_recipe_key == current_recipe_key:
    use-local
elif release_recipe_key == current_recipe_key:
    download-release
else:
    build
```

The resolver does not verify raw artifact bytes. A matching `.prebuilt` stamp is
the workflow cache contract. Corrupt external cache, release, or download state
is treated as an operational problem and fails the job rather than triggering
self-repair logic.

## Main GitHub Flow

`.github/workflows/main.yml` runs on `push` and `pull_request`. The main artifact
job is `guest-artifact-build`:

1. Check out the source tree.
2. Run `.ci/prebuilt/resolve-artifacts.sh` against the rolling release manifest.
3. Publish `release_needs_update` and `platform_action_cache_tag` as step outputs.
   `platform_action_cache_tag` is a cache namespace derived from the three current
   per-class recipe keys; it is not used for release/local correctness checks.
4. If the rolling release already matches current recipe keys, skip materialize,
   package, and upload in this job.
5. If the release is stale, restore the workflow-local raw artifact cache keyed
   by `platform_action_cache_tag` and the prebuilt pipeline hash.
6. Re-run the resolver after cache restore.
7. Install Buildroot/Linux build dependencies only when the post-cache plan still
   contains a `build` action.
8. Run `.ci/prebuilt/materialize-artifacts.sh` with the post-cache plan.
9. Run `.ci/prebuilt/package.sh`.
10. Upload raw artifacts for test jobs and packaged artifacts for publish.

The workflow-local raw artifact cache contains:

- `Image`
- `rootfs.cpio`
- `test-tools.img`
- `.prebuilt/`

Master pushes also restore this cache. The publish decision is separate: master
can publish only after the same materialized artifacts pass all required tests.

Forks and mirrors may not have their own rolling `prebuilt` release yet. The
GitHub workflow enables `PREBUILT_BOOTSTRAP_ON_404` only for non-canonical
repositories, so an HTTP 404 for `prebuilt.sha1` becomes a first-run source
build. The canonical `sysprog21/semu` repository does not enable that exception;
404 there is an external release-state problem and should fail for maintainer
inspection.

## Test Jobs

Linux, legacy initramfs, and macOS jobs use
`.github/actions/provide-guest-artifacts` before running `make`:

- If `release_needs_update=true`, the action downloads the raw artifact bundle
  produced by this workflow run.
- If `release_needs_update=false`, the action resolves and materializes directly
  from the rolling release with `PREBUILT_FORBID_BUILD=1`, so test jobs fail fast
  if the plan would require a source build instead of consuming release
  artifacts. Release downloads are intentionally not cached in Actions cache; the
  rolling release is the source of truth for this path.

After that, test jobs run normal local commands such as `make`, `make check`, and
`.ci/autorun.sh`. They do not make publish decisions.

## Publish Flow

`publish-prebuilt` runs only when all of these are true:

- The event is `push`.
- The ref is `refs/heads/master`.
- `guest-artifact-build` reported `release_needs_update=true`.
- Linux, legacy initramfs, macOS, style, and prebuilt-flow jobs passed.

The publish job does not rebuild or repackage. It downloads
`guest-artifacts-packaged`, runs `.ci/prebuilt/verify-package.sh`, and uploads
`Image.bz2`, `rootfs.cpio.bz2`, `test-tools.img.bz2`, and `prebuilt.sha1` to the
rolling prerelease.

## Responsibility Boundaries

### Local Build Recipe

| Path | Responsibility |
|------|----------------|
| `scripts/build-image.sh` | User-facing local guest artifact build CLI. |
| `scripts/prebuilt/build-artifacts.sh` | Shared source build recipe for Linux, Buildroot rootfs, and test-tools. |
| `scripts/prebuilt/artifact-recipe.env` | Immutable external revisions and sizing constants used by the build recipe; CI hashes selected variables per artifact class. |
| `scripts/prebuilt/test-tools-payloads.sh` | Shared payload-selection helper for local builds and CI recipe keys. |
| `mk/external.mk` | Local Make file targets for guest artifacts. |

### CI Prebuilt Layer

| Path | Responsibility |
|------|----------------|
| `.ci/prebuilt/artifact-inputs.sh` | CI class registry, recipe-key helpers, and plan-key helpers. |
| `.ci/prebuilt/resolve-artifacts.sh` | Create a materialization plan from current, release, and restored-cache recipe keys. |
| `.ci/prebuilt/materialize-artifacts.sh` | Execute a supplied plan: use local, download release, or build. |
| `.ci/prebuilt/build-plan-artifacts.sh` | Internal CI build wrapper used by the materializer. |
| `.ci/prebuilt/stamp-artifacts.sh` | Commit `.prebuilt/*.recipe-key` stamps after CI materialization actions succeed. |
| `.ci/prebuilt/package.sh` | Compress raw artifacts and write the release manifest. |
| `.ci/prebuilt/verify-package.sh` | Verify packaged artifact presence and manifest shape before publish. |
| `.ci/prebuilt-flow/test-flow.sh` | Integration tests for the CI prebuilt layer and local Make boundary. |

### GitHub Adapter

| Path | Responsibility |
|------|----------------|
| `.github/workflows/main.yml` | GitHub job graph, caches, workflow artifacts, publish gate, and output plumbing. |
| `.github/actions/provide-guest-artifacts/action.yml` | Test-job artifact provisioning from workflow artifacts or the rolling release. |
| `.github/actions/setup-semu/action.yml` | GitHub test dependency installation. |
| `.ci/github/append-github-output.sh` | Validated `$GITHUB_OUTPUT` writer. |
| `.ci/prebuilt-flow/test-workflow-shape.sh` | Shape tests for GitHub-specific workflow contracts. |
| `.ci/github/suggest-format.sh` | reviewdog/GitHub PR formatting suggestions. |

Provider-specific files may use GitHub Actions concepts. The local build recipe
and CI prebuilt scripts should stay free of GitHub expression syntax and
`$GITHUB_OUTPUT` plumbing.

## Decision Graph

```text
local developer
  |
  +-- make / make check
        |
        +-- mk/external.mk
              |
              +-- missing Image          -> download Image.bz2
              +-- missing rootfs.cpio    -> download rootfs.cpio.bz2
              +-- missing test-tools.img -> download test-tools.img.bz2
              +-- explicit source build  -> make build-image

GitHub Actions
  |
  +-- guest-artifact-build
        |
        +-- .ci/prebuilt/resolve-artifacts.sh
        |     -> release_needs_update?
        |
        +-- if release is current: skip build/package/upload
        |
        +-- if release is stale:
              |
              +-- restore workflow-local raw cache
              +-- resolve again after cache restore
              +-- materialize supplied plan
              |     +-- use-local restored raw artifacts
              |     +-- download matching release classes
              |     +-- build changed classes from source
              +-- package
              +-- upload raw artifact bundle for tests
              +-- upload packaged bundle for publish

test jobs
  |
  +-- provide-guest-artifacts
        |
        +-- release stale: download workflow raw bundle
        +-- release current: download release artifacts
  |
  +-- make / make check

publish-prebuilt
  |
  +-- only master push + release_needs_update + all tests passed
  +-- verify packaged bundle
  +-- update rolling prebuilt release
```

## Useful Commands

Local build examples:

```shell
make build-image BUILD_IMAGE_ARGS=image
make build-image BUILD_IMAGE_ARGS="rootfs --no-ext4"
make build-image BUILD_IMAGE_ARGS="test-tools --test-tools-payloads=x11"
make check RUN_HEADLESS=0
make check RUN_DISK=test-tools.img
```

CI-layer diagnostics:

```shell
PREBUILT_URL=https://github.com/sysprog21/semu/releases/download/prebuilt \
  .ci/prebuilt/resolve-artifacts.sh

PREBUILT_PLAN_FILE=/path/to/prebuilt-plan.env \
  .ci/prebuilt/materialize-artifacts.sh

bash .ci/prebuilt-flow/test-flow.sh
bash .ci/prebuilt-flow/test-workflow-shape.sh
```

`make clean` preserves guest artifacts. `make distclean` removes raw artifacts,
packaged artifacts, `prebuilt.sha1`, and `.prebuilt/` CI metadata.
