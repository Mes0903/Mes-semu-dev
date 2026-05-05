# virtio-gpu 3D Phase 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans
> to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Add the optional host and guest build gates needed before the
virglrenderer backend is introduced.

**Architecture:** `ENABLE_VIRGL` remains disabled by default and does not affect
the existing software virtio-gpu path. When enabled, Makefile parsing fails
early if the required `pkg-config` packages are unavailable, and the guest image
builder gets a separate `--virgl` path that layers a Mesa VirGL config fragment
on top of the existing X11 rootfs.

**Tech Stack:** GNU Make, `pkg-config`, Buildroot config fragments, shell tests.

---

### Task 1: Host `ENABLE_VIRGL` Build Gate

**Files:**
- Modify: `Makefile`
- Modify: `feature.h`
- Test: `tests/vgpu-virgl-gate-test.sh`

- [ ] **Step 1: Add the failing shell test**

Create `tests/vgpu-virgl-gate-test.sh` with checks for disabled mode, missing
dependencies, and fake dependency success:

```sh
bash tests/vgpu-virgl-gate-test.sh
```

Expected before implementation: FAIL because `print-vgpu-virgl-config` does not
exist.

- [ ] **Step 2: Implement the Makefile gate**

Add:

```make
PKG_CONFIG ?= pkg-config
ENABLE_VIRGL ?= 0
VIRGL_PKGS := virglrenderer epoxy gl egl
$(call set-feature, VIRGL)
```

When `ENABLE_VIRGL=1`, require `ENABLE_VIRTIOGPU=1`, check
`$(PKG_CONFIG) --exists $(VIRGL_PKGS)`, and append the corresponding cflags and
libs.

- [ ] **Step 3: Verify the gate**

Run:

```sh
make test-vgpu-virgl-gate
make test-vgpu-desc
make -j4 semu
```

Expected: all commands exit 0. On this host, a direct
`make ENABLE_VIRGL=1 semu` is expected to fail until `virglrenderer` and `epoxy`
development packages are installed.

### Task 2: Guest VirGL Buildroot Fragment

**Files:**
- Create: `configs/virgl.config`
- Modify: `scripts/build-image.sh`
- Modify: `README.md`

- [ ] **Step 1: Add the VirGL config fragment**

Create `configs/virgl.config`:

```text
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_VIRGL=y
```

- [ ] **Step 2: Add `--virgl` build-image mode**

Extend `scripts/build-image.sh` so:

```sh
scripts/build-image.sh --virgl
```

sets `BUILD_X11=1`, merges `configs/buildroot.config`,
`configs/x11.config`, and `configs/virgl.config`, and emits
`test-tools.img`.

- [ ] **Step 3: Document the new mode**

Update `README.md` so the image build examples mention:

```sh
scripts/build-image.sh --virgl
```

as the rootfs path for future Mesa VirGL smoke tests.

- [ ] **Step 4: Verify syntax and default behavior**

Run:

```sh
bash -n scripts/build-image.sh
make -j4 semu
```

Expected: shell syntax is valid and default `semu` build does not require
VirGL host dependencies.
