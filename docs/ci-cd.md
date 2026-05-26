# CI/CD Pipeline in semu

This document describes how semu builds, tests, caches, and publishes guest
artifacts in CI. It also defines the boundary between provider-neutral project
logic and provider-specific CI adapter logic.

## Architecture

The pipeline is split into two layers:

- The provider-neutral layer defines what semu needs: guest artifact input
  fingerprinting, prebuilt drift detection, package creation, package
  verification, local download behavior, and test commands.
- The provider adapter layer defines how a CI service runs those steps:
  triggers, job dependencies, cache restore/save, cross-job artifacts, release
  upload, and pull request review comments.

The GitHub Actions workflows are the current adapter. The `.ci/prebuilt/`
scripts and `mk/external.mk` are the stable contract that a GitLab CI, Gitea
Actions, or external storage adapter should reuse.

## Prebuilt Artifact Contract

Consumers read guest artifacts from `PREBUILT_URL` with fixed file names:

- `$PREBUILT_URL/Image.bz2`
- `$PREBUILT_URL/rootfs.cpio.bz2`
- `$PREBUILT_URL/test-tools.img.bz2`
- `$PREBUILT_URL/prebuilt.sha1`

`prebuilt.sha1` has four lines:

```text
<sha1>  Image.bz2
<sha1>  rootfs.cpio.bz2
<sha1>  test-tools.img.bz2
<sha1>  inputs
```

The first three lines verify the downloadable archives. The `inputs` line is a
virtual entry recording the SHA-1 fingerprint of semu-side files that define the
guest artifacts.

The input list currently lives in `.ci/prebuilt/inputs.sh`:

- `configs/linux.config`
- `configs/busybox.config`
- `configs/buildroot.config`
- `configs/x11.config`
- `configs/riscv-cross-file`
- `scripts/build-image.sh`
- `scripts/rootfs_ext4.sh`
- `target/init`
- `target/local-env.sh`

`mk/external.mk` keeps a Makefile copy of the same list because recipes need to
expand it directly. `.ci/prebuilt/inputs.sh` is the canonical source.

## Main CI Flow

`.github/workflows/main.yml` runs on `push` and `pull_request`.

Every run starts with `guest-artifact-build`:

1. Check out the source tree.
2. Run `.ci/prebuilt/detect-drift.sh`.
3. Compare the local input fingerprint with `$PREBUILT_URL/prebuilt.sha1`.
4. Set `should_build=true` when the manifest is unavailable, the manifest has no
   `inputs` line, or the local fingerprint differs from the published one.

When `should_build=false`, the expensive guest build is skipped. Test jobs use
their external artifact cache if present; otherwise `make` downloads from
`PREBUILT_URL` through `mk/external.mk`.

When `should_build=true`, `guest-artifact-build` must provide fresh guest
artifacts to all test jobs. On `master` pushes it builds from source. On pull
requests or non-master branch pushes, it first tries to restore a raw artifact
cache keyed by the exact input fingerprint:

```text
prebuilt-raw-v1-${runner.os}-${live_inputs_sha1}
```

That cache contains only raw artifacts:

- `Image`
- `rootfs.cpio`
- `test-tools.img`

On cache miss, CI runs:

```shell
./scripts/build-image.sh --all --x11 --directfb2-test
```

On cache hit, CI skips the Buildroot/kernel/rootfs build. In both cases it then
runs `.ci/prebuilt/package.sh` to create:

- `Image.bz2`
- `rootfs.cpio.bz2`
- `test-tools.img.bz2`
- `prebuilt.sha1`

The job uploads both raw and packaged files as the `guest-artifacts` workflow
artifact. Downstream Linux, legacy initramfs, and macOS test jobs download that
workflow artifact whenever `should_build=true`, so all test jobs exercise the
same artifacts from the same workflow run.

## Publish Flow

`publish-prebuilt` is part of `main.yml`, but it only runs when all of these are
true:

- The event is `push`.
- The ref is `refs/heads/master`.
- `guest-artifact-build` reported `should_build=true`.
- All required test and style jobs passed.

The publish job does not build or repackage guest artifacts. It checks out the
repository only to get `.ci/prebuilt/verify-package.sh`, downloads the tested
`guest-artifacts` workflow artifact, verifies the package, and uploads the
release files to the rolling `prebuilt` release.

This keeps the normal master path as:

- Detect drift.
- Build and package once.
- Test the same artifacts.
- Publish the same artifacts.

## Manual Fallback Flow

`.github/workflows/prebuilt.yml` is a manual fallback, triggered only through
`workflow_dispatch`.

It builds guest artifacts, packages them with `.ci/prebuilt/package.sh`,
verifies them with `.ci/prebuilt/verify-package.sh`, and uploads them to the
rolling `prebuilt` release. This path is a maintainer override, not the normal
tested publish path.

## Responsibility Boundaries

### Provider-neutral project logic

These files must not depend on GitHub Actions, GitLab CI, Gitea Actions, or
provider-specific variables such as `GITHUB_OUTPUT`:

| Path | Responsibility |
|------|----------------|
| `.ci/prebuilt/inputs.sh` | Defines guest artifact input files and SHA-1 helper functions. |
| `.ci/prebuilt/detect-drift.sh` | Compares local input fingerprint with `$PREBUILT_URL/prebuilt.sha1`. The caller must set `PREBUILT_URL`. |
| `.ci/prebuilt/package.sh` | Compresses raw artifacts and writes `prebuilt.sha1`. |
| `.ci/prebuilt/verify-package.sh` | Verifies packaged artifacts against `prebuilt.sha1`. |
| `.ci/common.sh` | Shared test script helpers such as cleanup, assertions, and timeouts. |
| `.ci/autorun.sh`, `.ci/device-smoke/test-*.sh` | Smoke test commands that CI adapters can call directly. |
| `.ci/check-format.sh` | Formatting enforcement command. |
| `mk/external.mk` | Downloads and verifies prebuilt artifacts for `make`, using `PREBUILT_URL`. |
| `scripts/build-image.sh` | Builds the Linux kernel, rootfs, and optional test tools disk. |

`mk/external.mk` keeps the upstream GitHub release as the local `make` default
because upstream GitHub is the current public artifact host. CI adapters should
set `PREBUILT_URL` explicitly for their own artifact backend.

### GitHub adapter logic

These files are intentionally GitHub-specific:

| Path | Responsibility |
|------|----------------|
| `.github/workflows/main.yml` | GitHub CI trigger, job graph, caches, workflow artifacts, release publish gate, and `$GITHUB_OUTPUT` plumbing. |
| `.github/workflows/prebuilt.yml` | GitHub manual fallback for rebuilding and publishing the rolling prebuilt release. |
| `.github/actions/setup-semu/action.yml` | GitHub composite action that installs Linux/macOS test dependencies. |
| `.ci/github/check-ci-shape.sh` | Validates the expected GitHub workflow adapter shape. |
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

On non-master branch pushes and pull requests, drifted guest inputs still produce
fresh guest artifacts for testing, but CI does not publish them to the rolling
release.

The raw artifact cache avoids rebuilding Buildroot repeatedly for the same
guest input fingerprint. This is intentionally keyed by the input hash rather
than by the pull request SHA, so normal code-only commits can reuse the same
guest artifacts.

No restore prefix is used for this cache. A different input fingerprint must
miss the cache and rebuild from source.

## Local Consumer Behavior

For local builds and no-drift CI runs, `mk/external.mk` downloads release
artifacts from `PREBUILT_URL`, verifies the archives listed in `prebuilt.sha1`,
then decompresses them for use by `make`.

The default is:

```make
PREBUILT_URL ?= https://github.com/sysprog21/semu/releases/download/prebuilt
```

Users and CI adapters can override it:

```shell
make PREBUILT_URL=https://example.org/semu/prebuilt
```

The Makefile uses non-interactive `mv -f` updates and treats the manifest as an
order-only prerequisite for raw artifact downloads. A refreshed manifest should
not make already-provided workflow artifacts look stale.
