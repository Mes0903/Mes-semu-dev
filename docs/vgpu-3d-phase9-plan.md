# virtio-gpu 3D Phase 9 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the VirGL guest image and manual 3D smoke path reproducible.

**Architecture:** Keep default CI on the headless 2D/DirectFB smoke path, but
add a separate manual VirGL smoke script for hosts with SDL/OpenGL available.
Track `configs/virgl.config` as a prebuilt input so published `test-tools.img`
and local drift detection account for the Mesa VirGL guest package fragment.

**Tech Stack:** Bash, GNU Make, Buildroot config fragments, GitHub Actions
workflow metadata, existing expect-based guest smoke style.

---

### Task 1: Red Test for VirGL Image Plumbing

**Files:**
- Create: `tests/vgpu-virgl-image-test.sh`
- Modify: `Makefile`

- [x] **Step 1: Add a static image plumbing test**

The test checks:

- `configs/virgl.config` enables `BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_VIRGL=y`
- `configs/x11.config` keeps the VirGL driver disabled by default
- `scripts/build-image.sh --help` documents `--x11` and `--virgl`
- `README.md` documents `--x11`, `--virgl`, and the manual VirGL smoke path
- `.ci/publish-prebuilt.sh`, `mk/external.mk`, and GitHub workflows include
  `configs/virgl.config` in their prebuilt input lists / cache keys
- `.github/workflows/prebuilt.yml` builds prebuilt test tools with `--virgl`
- `.ci/test-virgl.sh` exists and contains the expected guest smoke markers

- [x] **Step 2: Add `make test-vgpu-virgl-image`**

Compile nothing; the target runs the shell test:

```sh
make test-vgpu-virgl-image
```

Expected before implementation: FAIL because `.ci/test-virgl.sh` does not
exist and prebuilt input lists do not include `configs/virgl.config`.

### Task 2: Manual VirGL Smoke Script

**Files:**
- Create: `.ci/test-virgl.sh`

- [x] **Step 1: Add explicit host/display gates**

The script must fail early with a clear message when neither `DISPLAY` nor
`WAYLAND_DISPLAY` is set, because this smoke uses the SDL/OpenGL window path and
must not run headless.

- [x] **Step 2: Build and boot the VirGL backend**

Run:

```sh
make ENABLE_VIRGL=1 semu minimal.dtb Image test-tools.img
./semu -k Image -c 1 -b minimal.dtb -d test-tools.img
```

The command intentionally omits `-H`.

- [x] **Step 3: Guest-side checks**

After logging in as `root`, verify:

- `/dev/dri/card0` and `/dev/dri/renderD128` exist
- `glxinfo -B` is available and reports a renderer string containing `virgl`
- `glxgears` can start under `DISPLAY=:0`

The script should print `PASS: visible VirGL virtio-gpu smoke checks` only when
those markers succeed.

### Task 3: Prebuilt and Documentation Sync

**Files:**
- Modify: `README.md`
- Modify: `scripts/build-image.sh`
- Modify: `.ci/publish-prebuilt.sh`
- Modify: `mk/external.mk`
- Modify: `.github/workflows/prebuilt.yml`
- Modify: `.github/workflows/main.yml`

- [x] **Step 1: Document script usage**

README and `scripts/build-image.sh --help` must list `--x11` and `--virgl`.

- [x] **Step 2: Track `configs/virgl.config`**

Add `configs/virgl.config` anywhere the prebuilt input set is enumerated:

- `.ci/publish-prebuilt.sh`
- `mk/external.mk`
- `.github/workflows/prebuilt.yml`
- `.github/workflows/main.yml`

- [x] **Step 3: Build prebuilt test tools with VirGL**

Change prebuilt workflow command from:

```sh
./scripts/build-image.sh --all --x11 --directfb2-test
```

to:

```sh
./scripts/build-image.sh --all --virgl --directfb2-test
```

This preserves X11 because `--virgl` implies `--x11`.

### Task 4: Verification and Commit

Run:

```sh
bash -n .ci/test-virgl.sh
bash -n tests/vgpu-virgl-image-test.sh
make test-vgpu-virgl-image
make test-vgpu-virgl-gate
make test-vgpu-virgl-backend-build
make test-vgpu-virgl
make -j4 semu
make ENABLE_VIRGL=1 -j4 semu
git diff --check
```

Expected:

- static image test proves VirGL config is part of guest/prebuilt plumbing
- default build still succeeds
- `ENABLE_VIRGL=1` build still links with host VirGL deps
- manual `.ci/test-virgl.sh` is syntax-valid but not run automatically in
  headless CI

Result on 2026-05-05: shell syntax checks, `make test-vgpu-virgl-image`,
`make test-vgpu-virgl-gate`, `make test-vgpu-virgl-backend-build`,
`make test-vgpu-virgl`, default `make -j4 semu`, real
`make ENABLE_VIRGL=1 -j4 semu`, and `git diff --check` passed locally.
