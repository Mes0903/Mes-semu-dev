# virtio-gpu 3D Phase 5 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans
> to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Wire VirGL fence creation, callbacks, and renderer polling into the
current synchronous virtio-gpu command path without regressing the 2D backend.

**Architecture:** semu still returns ctrlq buffers synchronously in this phase.
The VirGL backend now creates host renderer fences for successful fenced
commands, polls virglrenderer from both command completion and the emulator
peripheral tick, and records fence callbacks only on the renderer owner path.
Delayed used-ring completion remains a later phase because it requires a pending
virtqueue completion queue rather than local descriptor-stack ownership.

**Tech Stack:** C, GNU Make, virglrenderer fence APIs, fake virglrenderer unit
fixture, SDL/OpenGL VirGL backend.

---

### Task 1: Failing Fence Tests

**Files:**
- Modify: `tests/fakes/virglrenderer.h`
- Modify: `tests/virtio-gpu-virgl-test.c`

- [x] **Step 1: Extend fake fence API**

Add fake declarations for:

```c
void virgl_renderer_poll(void);
int virgl_renderer_create_fence(int client_fence_id, uint32_t ctx_id);
int virgl_renderer_context_create_fence(uint32_t ctx_id,
                                        uint32_t flags,
                                        uint32_t ring_idx,
                                        uint64_t fence_id);
```

- [x] **Step 2: Add failing tests**

Add tests proving:

- successful fenced `SUBMIT_3D` creates a ctx0 renderer fence, echoes the
  request fence in the response, and polls virglrenderer
- descriptor `VIRTIO_DESC_F_WRITE` does not create a renderer fence when
  `request->hdr.flags` does not contain `VIRTIO_GPU_FLAG_FENCE`
- `VIRTIO_GPU_FLAG_INFO_RING_IDX` uses
  `virgl_renderer_context_create_fence()`
- backend `poll` calls `virgl_renderer_poll()`

Run:

```sh
make test-vgpu-virgl
```

Expected before implementation: FAIL because the backend does not have a poll
hook and does not create renderer fences.

### Task 2: Backend Poll Hook

**Files:**
- Modify: `virtio-gpu.h`
- Modify: `virtio-gpu.c`
- Modify: `virtio-gpu-sw.c`
- Modify: `virtio-gpu-virgl.c`
- Modify: `main.c`

- [x] **Step 1: Add lifecycle poll hook**

Add `poll` to `struct virtio_gpu_cmd_backend` and expose
`virtio_gpu_poll(virtio_gpu_state_t *vgpu)`.

- [x] **Step 2: Keep software backend no-op**

The software backend sets `.poll = NULL`, and `virtio_gpu_poll()` tolerates a
missing hook.

- [x] **Step 3: Poll from emulator peripheral tick**

Call `virtio_gpu_poll(&emu->vgpu)` from `emu_tick_peripherals()` before updating
the virtio-gpu interrupt line.

### Task 3: VirGL Fence Creation

**Files:**
- Modify: `virtio-gpu.h`
- Modify: `virtio-gpu-virgl.c`
- Modify: `tests/vgpu-virgl-backend-build-test.sh`

- [x] **Step 1: Add `VIRTIO_GPU_FLAG_INFO_RING_IDX`**

Define the flag as `(1 << 1)` next to `VIRTIO_GPU_FLAG_FENCE`.

- [x] **Step 2: Create host fences only from request flags**

After a successful `VIRTIO_GPU_RESP_OK_NODATA` response, if
`request->flags & VIRTIO_GPU_FLAG_FENCE` is set:

- use `virgl_renderer_context_create_fence()` when
  `VIRTIO_GPU_FLAG_INFO_RING_IDX` is set
- otherwise use `virgl_renderer_create_fence()`
- do not inspect descriptor flags for fence decisions

- [x] **Step 3: Poll after fence submission**

Call `virgl_renderer_poll()` after successful fence creation so the callback
path can retire immediately on software renderers.

- [x] **Step 4: Keep fake dependency build current**

Add the new virglrenderer stubs to
`tests/vgpu-virgl-backend-build-test.sh`.

### Task 4: Fence Callback State and Reset Hygiene

**Files:**
- Modify: `virtio-gpu-virgl.c`
- Modify: `tests/virtio-gpu-virgl-test.c`

- [x] **Step 1: Implement callbacks**

Implement both `write_fence` and `write_context_fence`. In this phase they
record the last retired fence for diagnostics and future async completion, but
they do not touch guest descriptors or used rings.

- [x] **Step 2: Clear fence records on reset**

Reset clears the recorded fence state before calling `virgl_renderer_reset()`
and clearing resource registries.

### Task 5: Verification and Commit

- [x] **Step 1: Run verification**

Run:

```sh
bash -n tests/vgpu-virgl-backend-build-test.sh
make test-vgpu-virgl
make test-vgpu-virgl-backend-build
make test-vgpu-virgl-gate
make test-vgpu-desc
make -j4 semu
make ENABLE_VIRGL=1 semu
git diff --check
```

Expected:

- default software build remains unaffected
- fake dependency build still proves optional VirGL linking
- real host VirGL build links with installed dependencies
- VirGL tests prove request-header fence handling, renderer fence creation,
  poll hook behavior, and callback/reset hygiene

Result on 2026-05-05: all commands above passed on the local host.
