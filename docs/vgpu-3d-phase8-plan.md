# virtio-gpu 3D Phase 8 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Clear visible VirGL scanouts before renderer resources/textures are
destroyed during `RESOURCE_UNREF` and device reset.

**Architecture:** Keep ownership in `virtio-gpu-virgl.c`: VirGL resources stay
tracked by the VirGL registry, while scanout bindings remain in shared
`virtio_gpu_scanout_info`. `RESOURCE_UNREF` clears matching GL primary scanouts
before calling virglrenderer unref; reset delegates the software display reset
before `virgl_renderer_reset()` so queued GL scanout payloads become stale
before renderer-owned texture IDs are invalidated.

**Tech Stack:** C, GNU Make, fake virglrenderer unit tests, existing display
bridge generation clears.

---

### Task 1: Red Tests for Resource Lifetime Ordering

**Files:**
- Modify: `tests/virtio-gpu-virgl-test.c`

- [x] **Step 1: Track display clear ordering in the fake renderer**

Extend `struct virgl_test_calls` with snapshots recorded by
`virgl_renderer_resource_unref()` and `virgl_renderer_reset()`:

```c
int resource_unref_primary_clear_count;
uint32_t resource_unref_primary_resource_id;
int reset_primary_clear_count;
int reset_cursor_clear_count;
uint32_t reset_primary_resource_id;
uint32_t reset_cursor_resource_id;
```

- [x] **Step 2: Add cursor clear and software reset stubs**

The fake software backend reset should clear the shared scanout state and
publish primary/cursor clears, matching the production software backend reset.
`fresh_vgpu()` then zeroes the publish counters after its initial reset so each
test starts from a clean fixture.

- [x] **Step 3: Add failing tests**

Add:

```c
test_resource_unref_clears_bound_gl_scanout_before_unref()
test_reset_clears_scanout_before_renderer_reset()
```

Expected pre-fix behavior:

```sh
make test-vgpu-virgl
```

fails because `resource_unref` does not clear bound GL scanouts, and reset calls
`virgl_renderer_reset()` before the software display reset.

### Task 2: Clear Bound GL Scanouts on Resource Unref

**Files:**
- Modify: `virtio-gpu-virgl.c`

- [x] **Step 1: Add `vgpu_virgl_clear_resource_scanouts()`**

For each enabled scanout whose `primary_resource_id` matches the VirGL resource
being destroyed:

- set `primary_resource_id = 0`
- set `src_x/src_y/src_w/src_h = 0`
- call `vgpu_display_publish_primary_clear(scanout_id)`

- [x] **Step 2: Call it before renderer resource destruction**

In `vgpu_virgl_cmd_resource_unref_handler()`, call the helper after validating
the response descriptor and before `virgl_renderer_resource_detach_iov()` /
`virgl_renderer_resource_unref()`.

### Task 3: Reset Display State Before Renderer Reset

**Files:**
- Modify: `virtio-gpu-virgl.c`

- [x] **Step 1: Reorder `vgpu_virgl_delegate_reset()`**

Run the delegated software reset before `virgl_renderer_reset()`. This clears
primary/cursor display generations and destroys CPU 2D resources before the
VirGL renderer invalidates GL texture IDs.

- [x] **Step 2: Keep VirGL registry cleanup after renderer reset**

After `virgl_renderer_reset()`, free the VirGL registry because the renderer has
destroyed those resources.

### Task 4: Verification and Commit

Run:

```sh
make test-vgpu-virgl
make test-vgpu-fence
make test-vgpu-chain
make test-vgpu-desc
make test-vgpu-virgl-gate
make test-vgpu-virgl-backend-build
make -j4 semu
make ENABLE_VIRGL=1 -j4 semu
clang-format-20 --dry-run --Werror virtio-gpu-virgl.c tests/virtio-gpu-virgl-test.c
git diff --check
```

Expected:

- unref of a visible VirGL resource publishes primary clear before renderer
  unref
- reset publishes primary/cursor clears before `virgl_renderer_reset()`
- existing VirGL, fence, descriptor, default, and `ENABLE_VIRGL=1` builds still
  pass

Result on 2026-05-05: the verification commands above passed locally. The
additional `.ci/test-gpu.sh` headless virtio-gpu + DirectFB2 smoke test also
passed.
