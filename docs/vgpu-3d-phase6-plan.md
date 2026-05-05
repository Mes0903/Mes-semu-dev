# virtio-gpu 3D Phase 6 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans
> to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Delay successful fenced virtio-gpu control queue completion until the
matching VirGL renderer fence retires.

**Architecture:** `virtio-gpu.c` owns generic pending used-ring completion
state because only the frontend knows queue indexes and descriptor ids. The
VirGL backend still owns renderer fence creation, but successful fenced
responses are registered with the frontend instead of being written to the
guest immediately. VirGL fence callbacks call back into the frontend to append
the used-ring entry and raise the virtio-gpu interrupt.

**Tech Stack:** C, GNU Make, existing virtio-gpu queue parser, virglrenderer
fence callbacks, focused unit tests with a fake backend.

---

### Task 1: Failing Frontend Fence Completion Tests

**Files:**
- Create: `tests/virtio-gpu-fence-test.c`
- Modify: `Makefile`

- [x] **Step 1: Add frontend queue test target**

Add `make test-vgpu-fence`, compiling a standalone test that includes
`virtio-gpu.c` with a fake backend.

- [x] **Step 2: Test delayed used-ring completion**

The test constructs a controlq descriptor chain with a fenced request and a
response descriptor, calls `virtio_gpu_queue_notify_handler()`, and verifies:

- the avail entry is consumed
- the used ring index is unchanged
- the response buffer is still untouched
- after `virtio_gpu_complete_fence(... fence_id ...)`, the response is written,
  used ring entry is appended, and `VIRTIO_INT__USED_RING` is set

- [x] **Step 3: Test context fence matching**

The test registers a fenced request with `VIRTIO_GPU_FLAG_INFO_RING_IDX`, proves
that a wrong `(ctx_id, ring_idx)` completion does not release it, then proves
that the matching completion does.

Run:

```sh
make test-vgpu-fence
```

Expected before implementation: FAIL because there is no frontend pending fence
API and fenced commands complete synchronously.

### Task 2: Frontend Pending Completion Queue

**Files:**
- Modify: `virtio-gpu.h`
- Modify: `virtio-gpu.c`

- [x] **Step 1: Add pending fence records**

Extend `virtio_gpu_data_t` with a small fixed array of pending fenced
completions. Each record stores queue index, descriptor id, response descriptor,
response type, command header fields, and active state.

- [x] **Step 2: Track dispatch context**

During `virtio_gpu_desc_handler()`, record the queue index and descriptor id in
`virtio_gpu_data_t` so a command handler can ask the frontend to defer the
response without knowing queue internals.

- [x] **Step 3: Add defer API**

Expose:

```c
bool virtio_gpu_defer_ctrl_response(virtio_gpu_state_t *vgpu,
                                    const struct virtio_gpu_ctrl_hdr *request,
                                    const struct virtq_desc *response_desc,
                                    uint32_t response_type);
```

It returns false when no queue dispatch is active or the pending array is full.

- [x] **Step 4: Add completion API**

Expose:

```c
void virtio_gpu_complete_fence(virtio_gpu_state_t *vgpu,
                               bool context_fence,
                               uint32_t ctx_id,
                               uint32_t ring_idx,
                               uint64_t fence_id);
```

It writes the response, appends a used-ring entry for each matching pending
record, clears it, and sets `VIRTIO_INT__USED_RING` unless the queue asked for
no interrupt.

### Task 3: VirGL Uses Delayed Completion

**Files:**
- Modify: `virtio-gpu-virgl.c`
- Modify: `tests/virtio-gpu-virgl-test.c`

- [x] **Step 1: Defer successful fenced VirGL responses**

After successful renderer fence creation, call
`virtio_gpu_defer_ctrl_response()`. If it succeeds, return the frontend
deferred sentinel and do not write the response immediately.

- [x] **Step 2: Complete from callbacks**

`write_fence` calls `virtio_gpu_complete_fence(vgpu, false, 0, 0, fence)`.
`write_context_fence` calls `virtio_gpu_complete_fence(vgpu, true, ctx_id,
ring_idx, fence_id)`.

- [x] **Step 3: Keep direct backend tests synchronous**

Existing direct VirGL backend tests provide a fake
`virtio_gpu_defer_ctrl_response()` returning false, so handler-level tests still
verify renderer fence creation without requiring a frontend queue.

### Task 4: Reset Cleanup

**Files:**
- Modify: `virtio-gpu.c`
- Modify: `virtio-gpu-virgl.c`
- Test: `tests/virtio-gpu-fence-test.c`

- [x] **Step 1: Clear pending completions on reset**

Generic virtio-gpu reset clears pending fence records before preserving scanout
configuration.

- [x] **Step 2: Keep late callbacks harmless**

After reset, `virtio_gpu_complete_fence()` finds no active pending record and
does not write stale response buffers.

### Task 5: Verification and Commit

Run:

```sh
bash -n tests/vgpu-virgl-backend-build-test.sh
make test-vgpu-fence
make test-vgpu-virgl
make test-vgpu-virgl-backend-build
make test-vgpu-virgl-gate
make test-vgpu-desc
make -j4 semu
make ENABLE_VIRGL=1 semu
git diff --check
```

Expected:

- fenced controlq entries stay out of the used ring until the matching fence
  completes
- unfenced and error responses still complete synchronously
- reset drops pending fenced completions without writing stale guest buffers
- existing descriptor, VirGL, fake dependency, default, and real VirGL builds
  still pass
