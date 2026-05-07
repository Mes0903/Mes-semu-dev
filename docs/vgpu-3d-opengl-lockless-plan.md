# semu virtio-gpu OpenGL-first 3D Lockless Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish a mergeable OpenGL/VirGL-only virtio-gpu 3D path while
restoring the lockless ownership model that `vgpu-pr` established for display
and input.

**Architecture:** Keep the guest-visible virtio-gpu device owned by the
emulator thread, keep SDL/window state owned by the main thread, and move all
OpenGL/virglrenderer calls to one GL owner thread. Cross-thread communication
must use bounded SPSC queues plus wakeups, not shared mutable GL state protected
by a mutex.

**Tech Stack:** C, SDL2/OpenGL through epoxy, virglrenderer, virtio-gpu MMIO,
existing SPSC display/input queues, Bash/Expect smoke tests, fake virglrenderer
unit tests.

---

## Current Assessment

The current `vgpu-3D` branch has a working first OpenGL/VirGL spike:

- `ENABLE_VIRGL=1` advertises `VIRTIO_GPU_F_VIRGL`.
- VirGL/VirGL2 capsets, context/resource/backing/transfer/submit/fence handlers
  exist.
- The guest can use the boot-started `Xorg :0` server and run `glxgears` with
  the VirGL renderer.
- The cursor format issue and the host Mesa/GLX crash under fast mouse movement
  have been fixed for the current threaded model.

The current branch only partially follows the `vgpu-pr` lockless architecture:

- It still follows the `vgpu-pr` model for input and CPU display payloads:
  SDL/main-thread producers and emulator-thread consumers communicate through
  SPSC queues, and guest-visible virtio state is not touched from SDL code.
- It does not fully follow that model for 3D: virglrenderer runs on the
  emulator thread while SDL/OpenGL presentation runs on the main thread, and
  both touch a shared GL context group.
- The global `vgpu_gl_lock()` mutex is a correct crash fix for the current
  split, but it is a temporary safety net. The final OpenGL architecture should
  not need it.

OpenGL-only scope for this plan:

- Keep: VirGL/OpenGL, VirGL and VirGL2 capsets, Xorg glamor, GLX smoke.
- Keep disabled: `VIRTIO_GPU_F_RESOURCE_BLOB`,
  `VIRTIO_GPU_F_CONTEXT_INIT`, Venus, DRM native context, Vulkan, host-visible
  SHM, vhost-user-gpu.
- Keep manual: visible host smoke tests requiring an SDL/OpenGL window.

## Phase 0: Code Review Findings and Fix Strategy

**Goal:** Lock in the concrete review findings from the current `vgpu-3D`
branch before implementation work starts, so later phases remove the temporary
mutex without losing the crash fixes it currently provides.

**Files:**

- Modify: `docs/vgpu-3d-opengl-lockless-plan.md`

- [x] **Finding 1: The global GL mutex is a temporary crash stopgap, not the
  target architecture**

Current evidence:

- `window-sw.c:31-42` defines `pthread_mutex_t vgpu_gl_mutex`.
- `virtio-gpu-virgl.c:95-143` wraps `virgl_renderer_init()`, ctx0 handoff,
  command dispatch, and `virgl_renderer_poll()` in `vgpu_gl_lock()`.
- `window-sw.c:692-782` takes the same lock while the SDL main thread uploads
  cursor textures, binds VirGL scanout textures, and swaps the window.
- `virtio-gpu-virgl.c:1208-1213` wires backend `thread_enter`,
  `command_enter`, and `command_leave` hooks solely to preserve this lock.

Risk: the mutex serializes two owners of one GL context group instead of
establishing one owner. It prevents the observed crash only by blocking the SDL
main thread and emulator thread around shared GL state, so it can still hide
thread-affinity bugs and presentation stalls.

Fix strategy:

- Phase 2 introduces explicit renderer request/completion queues.
- Phase 4 moves virglrenderer execution to the GL owner.
- Phase 5 deletes `vgpu_gl_lock()`, `vgpu_gl_unlock()`, the mutex, and backend
  command/thread lock hooks.

- [x] **Finding 2: VirGL callbacks currently call SDL/OpenGL from the emulator
  thread**

Current evidence:

- `virtio-gpu-virgl.c:62-83` forwards `create_gl_context`,
  `destroy_gl_context`, and `make_current` callbacks to `window-sw.c`.
- `window-sw.c:1054-1100` implements those callbacks with
  `SDL_GL_MakeCurrent()`, `SDL_GL_SetAttribute()`, `SDL_GL_CreateContext()`,
  `SDL_GL_DeleteContext()`, and the SDL window pointer.
- `main.c:1988-1999` runs the emulator in a background thread while the main
  thread owns the SDL event loop.

Risk: a mutex does not make SDL video APIs main-thread-owned. The current
callback path can create, delete, or bind SDL GL contexts from whichever thread
virglrenderer is executing on.

Fix strategy:

- Phase 4 makes the SDL main thread the GL owner and drains renderer requests
  there.
- VirGL callbacks must remain local to the GL owner. They must not be callable
  from the emulator thread after the ownership move.
- `vgpu-gl.h` should become a GL-owner API only, then shrink or disappear after
  Phase 5.

- [x] **Finding 3: `num_capsets` config reads bypass the current GL lock**

Current evidence:

- `virtio-gpu.c:1033-1035` answers `virtio_gpu_config.num_capsets` by calling
  `virtio_gpu_backend_get_num_capsets()` directly from the MMIO config read
  path.
- `virtio-gpu-virgl.c:1079-1098` implements that function by calling
  `virgl_renderer_get_cap_set()` twice without `vgpu_gl_lock()`.

Risk: even the current mutex model does not serialize all virglrenderer entry
points. A guest config read can race renderer reset, capset commands, or future
GL-owner work.

Fix strategy:

- Phase 4 must move capset discovery to the GL owner during renderer init and
  reset.
- `virtio-gpu.c` should read a cached `num_capsets` value owned by the
  guest-visible device state, not call virglrenderer from the MMIO read path.
- `GET_CAPSET_INFO` and `GET_CAPSET` remain renderer requests because they
  require capset data, but the fixed-size config field must be synchronous and
  cache-backed.

- [x] **Finding 4: Fence callbacks currently write guest-visible virtqueue
  state directly**

Current evidence:

- `virtio-gpu-virgl.c:43-60` calls `virtio_gpu_complete_fence()` from
  virglrenderer fence callbacks.
- `virtio-gpu.c:233-270` writes the response descriptor, pushes the used-ring
  entry, and sets interrupt state from `virtio_gpu_complete_fence()`.

Risk: this is only safe while virglrenderer callbacks execute on the emulator
thread. Once virglrenderer moves to the GL owner, callback-driven completion
would mutate guest-visible virtio state from the wrong thread.

Fix strategy:

- Phase 3 generalizes pending controlq records and adds reset generations.
- Phase 4 changes fence callbacks to enqueue `VGPU_RENDERER_DONE_FENCE`.
- Only the emulator thread drains renderer completions and writes used rings or
  `InterruptStatus`.

- [x] **Finding 5: Fenced response registration and renderer wakeups need a
  stricter contract**

Current evidence:

- `virtio-gpu-virgl.c:254-279` creates a renderer fence before recording the
  deferred controlq response.
- `main.c:257` calls `virtio_gpu_poll()` from the periodic peripheral tick.
- The local virglrenderer header documents `virgl_renderer_poll()` as the fence
  forcing path and `virgl_renderer_get_poll_fd()` as the companion to
  `VIRGL_RENDERER_THREAD_SYNC`.

Risk: if a fence callback is delivered synchronously or before the pending
record is visible, the completion can be lost. Separately, relying only on the
peripheral tick can leave fence progress tied to guest CPU activity instead of
renderer readiness.

Fix strategy:

- Phase 3 records a pending token before renderer execution is allowed to
  complete it; if renderer fence creation fails, the pending record is
  completed with an error or explicitly cancelled.
- Phase 4 gives the GL owner a renderer poll source: use
  `virgl_renderer_get_poll_fd()` when available, and keep a bounded fallback
  timeout so the SDL loop cannot sleep indefinitely while renderer fences are
  pending.
- Every renderer completion must wake the emulator thread through the existing
  backend wake pipe.

## File Structure

New files planned:

- `vgpu-renderer.h`: lockless renderer-owner queue API and request/completion
  data types.
- `vgpu-renderer.c`: SPSC request queue from emulator thread to GL owner,
  SPSC completion queue from GL owner to emulator thread, and wake hooks.
- `tests/vgpu-renderer-queue-test.c`: unit tests for renderer request and
  completion ordering, queue-full behavior, and reset generation filtering.
- `tests/vgpu-opengl-scope-test.sh`: source-level gate proving that only the
  OpenGL/VirGL feature set is advertised.
- `tests/vgpu-no-gl-lock-test.sh`: source-level gate proving that the final
  OpenGL path no longer depends on `pthread_mutex` or `vgpu_gl_lock()`.

Existing files to modify:

- `Makefile`: add test targets for the new renderer queue, OpenGL scope gate,
  and no-GL-lock gate.
- `main.c`: keep SDL main-thread ownership; add a main-thread wake path when
  the emulator enqueues renderer work.
- `window.h`: expose a frontend wake hook so the emulator can wake the SDL loop
  without touching SDL state directly.
- `window-sw.c`: make the SDL main thread the GL owner, drain renderer work
  from the main loop, and remove the global GL mutex after ownership is fixed.
- `vgpu-gl.h`: keep only GL context callback declarations that are called by
  the GL owner; remove lock declarations.
- `virtio-gpu.c`: generalize pending controlq completion so any async renderer
  command can complete asynchronously on the emulator thread.
- `virtio-gpu.h`: add async completion APIs and remove backend lock hooks.
- `virtio-gpu-virgl.c`: split emulator-thread command decode from GL-owner
  virglrenderer execution.
- `tests/virtio-gpu-virgl-test.c`: adapt fake backend tests to async renderer
  dispatch and completion.
- `tests/virtio-gpu-fence-test.c`: extend fence tests to cover async renderer
  completion tokens and reset generation invalidation.
- `tests/vgpu-virgl-backend-build-test.sh`: add stubs for new queue/owner
  functions and keep optional dependency coverage.
- `.ci/test-virgl.sh`: keep the visible smoke as the live gate; add optional
  crash-log path instructions only if a run fails.
- `README.md`: document OpenGL-only support and the lockless owner model.
- `/home/mes/MesRepo/dev-docs/MesDevLog/semu-vgpu-3D/README.md`: replace the
  old phase status with this OpenGL-first completion plan and remaining gates.

## Phase 1: Freeze OpenGL-only Scope and Guardrails

**Goal:** Make the supported and unsupported feature set explicit before moving
thread ownership.

**Files:**

- Create: `tests/vgpu-opengl-scope-test.sh`
- Modify: `Makefile`
- Modify: `README.md`
- Modify:
  `/home/mes/MesRepo/dev-docs/MesDevLog/semu-vgpu-3D/README.md`

- [x] **Step 1: Add the source gate script**

Create `tests/vgpu-opengl-scope-test.sh`:

```sh
#!/usr/bin/env bash

set -euo pipefail

fail()
{
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

grep -Fq 'VIRTIO_GPU_F_EDID | (SEMU_HAS(VIRGL) ? VIRTIO_GPU_F_VIRGL : 0)' \
    virtio-gpu.c ||
    fail "device features must advertise only EDID plus optional VIRGL"

grep -Fq '.resource_create_blob = VIRTIO_GPU_CMD_UNDEF' virtio-gpu-virgl.c ||
    fail "OpenGL-only VirGL backend must not implement blob resource creation"
grep -Fq '.resource_map_blob = VIRTIO_GPU_CMD_UNDEF' virtio-gpu-virgl.c ||
    fail "OpenGL-only VirGL backend must not implement blob mapping"
grep -Fq '.resource_unmap_blob = VIRTIO_GPU_CMD_UNDEF' virtio-gpu-virgl.c ||
    fail "OpenGL-only VirGL backend must not implement blob unmapping"

grep -Fq 'if (request->context_init)' virtio-gpu-virgl.c ||
    fail "CTX_CREATE must reject context_init while CONTEXT_INIT is not advertised"

if grep -Eq 'VIRTIO_GPU_F_RESOURCE_BLOB|VIRTIO_GPU_F_CONTEXT_INIT' \
    <(sed -n '970,990p' virtio-gpu.c); then
    fail "DeviceFeatures must not advertise blob or context_init"
fi

if rg -n 'CAPSET_(VENUS|DRM)|VIRTIO_GPU_CAPSET_VENUS|VIRTIO_GPU_CAPSET_DRM' \
    virtio-gpu.c virtio-gpu-virgl.c >/dev/null; then
    fail "OpenGL-only plan must not expose Venus or native DRM capsets"
fi
```

- [x] **Step 2: Add the Makefile target**

Add:

```make
.PHONY: test-vgpu-opengl-scope
test-vgpu-opengl-scope:
	$(Q)bash tests/vgpu-opengl-scope-test.sh
```

- [x] **Step 3: Run the gate**

Run:

```sh
bash -n tests/vgpu-opengl-scope-test.sh
make test-vgpu-opengl-scope
```

Expected: both commands exit 0 on the current branch.

- [x] **Step 4: Update docs with the frozen scope**

Document that the first mergeable 3D PR is OpenGL/VirGL only:

```text
Supported now: VIRTIO_GPU_F_VIRGL, VirGL/VirGL2 capsets, Xorg glamor, GLX.
Not supported now: RESOURCE_BLOB, CONTEXT_INIT, Venus, Vulkan, native DRM
contexts, host-visible SHM, vhost-user-gpu.
```

- [x] **Step 5: Commit**

```sh
git add Makefile README.md \
    /home/mes/MesRepo/dev-docs/MesDevLog/semu-vgpu-3D/README.md \
    tests/vgpu-opengl-scope-test.sh
git commit -m "Document OpenGL-only VirGL scope"
```

## Phase 2: Specify the Lockless Renderer Owner Boundary

**Goal:** Introduce an explicit renderer-owner queue abstraction without moving
virglrenderer execution yet.

**Files:**

- Create: `vgpu-renderer.h`
- Create: `vgpu-renderer.c`
- Create: `tests/vgpu-renderer-queue-test.c`
- Modify: `Makefile`
- Modify: `window.h`
- Modify: `window-sw.c`

- [x] **Step 1: Add renderer queue types**

Create `vgpu-renderer.h` with these ownership rules:

```c
#pragma once

#if !SEMU_HAS(VIRGL)
#error Only valid when VirGL is enabled.
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "virtio-gpu.h"

enum vgpu_renderer_request_type {
    VGPU_RENDERER_REQ_INIT = 0,
    VGPU_RENDERER_REQ_RESET,
    VGPU_RENDERER_REQ_POLL,
    VGPU_RENDERER_REQ_CTRL,
    VGPU_RENDERER_REQ_SHUTDOWN,
};

enum vgpu_renderer_completion_type {
    VGPU_RENDERER_DONE_CTRL = 0,
    VGPU_RENDERER_DONE_FENCE,
    VGPU_RENDERER_DONE_FATAL,
};

struct vgpu_renderer_token {
    uint32_t id;
    uint32_t generation;
};

struct vgpu_renderer_request {
    enum vgpu_renderer_request_type type;
    struct vgpu_renderer_token token;
    uint32_t command_type;
    void *payload;
    size_t payload_size;
};

struct vgpu_renderer_completion {
    enum vgpu_renderer_completion_type type;
    struct vgpu_renderer_token token;
    uint32_t response_type;
    bool context_fence;
    uint32_t ctx_id;
    uint32_t ring_idx;
    uint64_t fence_id;
};

void vgpu_renderer_set_wake_frontend(void (*wake_frontend)(void));
bool vgpu_renderer_submit(const struct vgpu_renderer_request *request);
bool vgpu_renderer_pop_request(struct vgpu_renderer_request *request);
bool vgpu_renderer_complete(const struct vgpu_renderer_completion *completion);
bool vgpu_renderer_pop_completion(struct vgpu_renderer_completion *completion);
void vgpu_renderer_reset_queues(uint32_t generation);
```

- [x] **Step 2: Implement bounded SPSC queues**

Create `vgpu-renderer.c` with two fixed-size queues:

```c
#include "vgpu-renderer.h"

#define VGPU_RENDERER_QUEUE_SIZE 256U
#define VGPU_RENDERER_QUEUE_MASK (VGPU_RENDERER_QUEUE_SIZE - 1U)

static struct vgpu_renderer_request request_queue[VGPU_RENDERER_QUEUE_SIZE];
static uint32_t request_head;
static uint32_t request_tail;

static struct vgpu_renderer_completion completion_queue[VGPU_RENDERER_QUEUE_SIZE];
static uint32_t completion_head;
static uint32_t completion_tail;

static void (*wake_frontend_cb)(void);
static uint32_t active_generation;
```

Use the same acquire/release pattern as `vgpu-display.c` and
`virtio-input-event.c`: producer writes an entry, then stores `head` with
`__ATOMIC_RELEASE`; consumer loads `head` with `__ATOMIC_ACQUIRE`, copies the
entry, then stores `tail` with `__ATOMIC_RELEASE`.

- [x] **Step 3: Add queue unit tests**

Create `tests/vgpu-renderer-queue-test.c` with tests for:

- request FIFO ordering
- completion FIFO ordering
- full queue rejects the newest request without corrupting older entries
- `vgpu_renderer_reset_queues(new_generation)` drops stale completions whose
  token generation does not match

- [x] **Step 4: Add Makefile target**

Add:

```make
VGPU_RENDERER_TEST := tests/vgpu-renderer-queue-test

.PHONY: test-vgpu-renderer
test-vgpu-renderer: $(VGPU_RENDERER_TEST)
	$(Q)./$<

$(VGPU_RENDERER_TEST): tests/vgpu-renderer-queue-test.c vgpu-renderer.c vgpu-renderer.h virtio-gpu.h
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -O2 -g -Wall -Wextra -include common.h \
	    -DSEMU_FEATURE_VIRGL=1 -Itests/fakes -o $@ $< vgpu-renderer.c
```

- [x] **Step 5: Add main-thread wake hook**

Extend `struct window_backend` in `window.h`:

```c
void (*window_wake_frontend)(void);
```

Implement `window_wake_frontend_sw()` in `window-sw.c` by pushing an SDL user
event:

```c
static void window_wake_frontend_sw(void)
{
    if (!sdl_initialized || headless_mode)
        return;

    SDL_Event event = {0};
    event.type = SDL_USEREVENT;
    SDL_PushEvent(&event);
}
```

- [x] **Step 6: Wire the wake hook**

During window initialization, call:

```c
vgpu_renderer_set_wake_frontend(window_wake_frontend_sw);
```

The emulator thread will use this to wake the SDL loop after submitting GL
owner work.

- [x] **Step 7: Verify**

Run:

```sh
make test-vgpu-renderer
make test-vgpu-display
make test-vinput-event-coalesce
make ENABLE_VIRGL=1 semu
git diff --check
```

Expected: queue tests pass, existing display/input queue tests still pass, and
the VirGL build still links.

- [x] **Step 8: Commit**

```sh
git add Makefile window.h window-sw.c vgpu-renderer.h vgpu-renderer.c \
    tests/vgpu-renderer-queue-test.c
git commit -m "Add lockless renderer owner queues"
```

## Phase 3: Generalize Async Virtio-gpu Completion

**Goal:** Let the emulator thread defer any renderer-backed ctrlq command,
not only fenced commands, while preserving guest-visible virtqueue ownership on
the emulator thread.

**Files:**

- Modify: `virtio-gpu.h`
- Modify: `virtio-gpu.c`
- Modify: `tests/virtio-gpu-fence-test.c`

- [x] **Step 1: Rename pending fence records to pending ctrl records**

Replace `struct virtio_gpu_pending_fence` with:

```c
struct virtio_gpu_pending_ctrl {
    bool active;
    uint32_t generation;
    uint8_t queue_index;
    uint16_t buffer_idx;
    struct virtq_desc response_desc;
    uint32_t response_type;
    uint32_t request_type;
    uint32_t request_flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t ring_idx;
};
```

- [x] **Step 2: Add generic defer API**

Expose:

```c
bool virtio_gpu_defer_ctrl_response(virtio_gpu_state_t *vgpu,
                                    const struct virtio_gpu_ctrl_hdr *request,
                                    const struct virtq_desc *response_desc,
                                    uint32_t response_type,
                                    uint32_t generation);
```

The API records queue index and descriptor id from the active dispatch context
and returns false if there is no active dispatch or the pending table is full.

- [x] **Step 3: Add generic completion API**

Expose:

```c
void virtio_gpu_complete_ctrl_response(virtio_gpu_state_t *vgpu,
                                       uint32_t generation,
                                       uint64_t fence_id,
                                       bool context_fence,
                                       uint32_t ctx_id,
                                       uint32_t ring_idx);
```

For unfenced commands, callers pass `fence_id = 0` and
`context_fence = false`. For fenced commands, the function matches the fence
fields before writing the response and used-ring entry.

- [x] **Step 4: Register pending work before renderer execution can complete**

When dispatching a fenced renderer request, allocate and publish the pending
record before the GL owner calls `virgl_renderer_create_fence()` or
`virgl_renderer_context_create_fence()`. If fence creation fails, complete the
pending record with `VIRTIO_GPU_RESP_ERR_UNSPEC` or cancel it before returning
the error response. This prevents a synchronous or early fence callback from
being lost.

- [x] **Step 5: Keep all guest writes on the emulator thread**

Assert in comments and tests that `virtio_gpu_complete_ctrl_response()` is only
called while draining renderer completions on the emulator thread. The GL owner
must only enqueue `struct vgpu_renderer_completion`.

- [x] **Step 6: Extend tests**

Add cases to `tests/virtio-gpu-fence-test.c`:

- an unfenced deferred command completes through the generic completion API
- a fenced deferred command does not complete on the wrong fence
- reset increments generation and ignores stale completions
- a fence completion produced before pending registration would have failed in
  the old order and is now handled by the token-first order

- [x] **Step 7: Verify**

Run:

```sh
make test-vgpu-fence
make test-vgpu-chain
make test-vgpu-desc
make test-vgpu-virgl
git diff --check
```

Expected: frontend async completion tests pass and existing descriptor-chain
tests still pass.

- [x] **Step 8: Commit**

```sh
git add virtio-gpu.h virtio-gpu.c tests/virtio-gpu-fence-test.c
git commit -m "Generalize virtio-gpu async ctrl completion"
```

## Phase 4: Move VirGL Execution to the GL Owner

**Goal:** Stop calling virglrenderer and SDL_GL APIs from different threads.
After this phase, the SDL main thread owns all OpenGL and virglrenderer calls.

**Files:**

- Modify: `virtio-gpu-virgl.c`
- Modify: `virtio-gpu.c`
- Modify: `virtio-gpu.h`
- Modify: `window-sw.c`
- Modify: `vgpu-gl.h`
- Modify: `tests/virtio-gpu-virgl-test.c`
- Modify: `tests/vgpu-virgl-backend-build-test.sh`

### Phase 4A: Route Fence Completions Through the Emulator Thread

**Goal:** Establish the completion side of the GL-owner boundary before moving
full command execution. VirGL fence callbacks must be able to run on the GL
owner without touching guest-visible virtqueue state.

**Files:**

- Create: `tests/vgpu-renderer-owner-test.sh`
- Modify: `Makefile`
- Modify: `vgpu-renderer.h`
- Modify: `vgpu-renderer.c`
- Modify: `virtio-gpu.c`
- Modify: `virtio-gpu-virgl.c`
- Modify: `window-sw.c`
- Modify: `tests/vgpu-renderer-queue-test.c`
- Modify: `tests/virtio-gpu-fence-test.c`
- Modify: `tests/virtio-gpu-virgl-test.c`

- [x] **Step 1: Add a source gate for fence callback ownership**

`tests/vgpu-renderer-owner-test.sh` asserts that `write_fence` and
`write_context_fence` callbacks enqueue `VGPU_RENDERER_DONE_FENCE` through
`vgpu_renderer_complete()` and do not call `virtio_gpu_complete_fence()`
directly.

- [x] **Step 2: Wake the emulator thread for renderer completions**

`vgpu_renderer_set_wake_backend()` wires renderer completions to the existing
backend wake pipe. Stale-generation completions are rejected without waking the
backend.

- [x] **Step 3: Drain renderer completions from the emulator side**

`virtio_gpu_poll()` drains `vgpu_renderer_pop_completion()` before calling the
backend `poll` hook, then completes matching pending ctrlq responses through
`virtio_gpu_complete_ctrl_response()`.

- [x] **Step 4: Move VirGL fence callbacks to the completion queue**

`vgpu_virgl_write_fence()` and `vgpu_virgl_write_context_fence()` now record
debug fence state and enqueue renderer fence completions. The emulator thread
remains the only writer of response descriptors, used rings, and interrupt
state.

- [x] **Step 5: Verify**

Run:

```sh
bash -n tests/vgpu-renderer-owner-test.sh
make test-vgpu-renderer-owner
make test-vgpu-renderer
make test-vgpu-fence
make test-vgpu-virgl
```

Expected: callbacks publish completion queue records, renderer completions wake
the backend, and fence completions only write the used ring after
`virtio_gpu_poll()` drains the completion queue.

**Remaining Phase 4 work:** command decode/execute split, SDL-loop request
drain, capset cache ownership, renderer poll-fd integration, reset ownership,
and removal of backend command/thread lock hooks are still tracked by the
unchecked Phase 4 steps below.

### Phase 4B: Cache `num_capsets` Outside MMIO Config Reads

**Goal:** Remove the last direct virglrenderer call from the MMIO config read
path. The emulator thread should answer `virtio_gpu_config.num_capsets` from
guest-visible device state, not by calling backend or renderer APIs while the
guest reads config space.

**Files:**

- Modify: `virtio-gpu.h`
- Modify: `virtio-gpu.c`
- Modify: `virtio-gpu-virgl.c`
- Modify: `tests/vgpu-renderer-owner-test.sh`
- Modify: `tests/virtio-gpu-fence-test.c`
- Modify: `tests/virtio-gpu-virgl-test.c`

- [x] **Step 1: Add cache state and setter**

`virtio_gpu_data_t` now stores `num_capsets`, and
`virtio_gpu_set_num_capsets()` is the backend-facing publication API.

- [x] **Step 2: Read config from cache**

The `virtio_gpu_config.num_capsets` MMIO read case returns
`PRIV(vgpu)->num_capsets`; it no longer calls
`virtio_gpu_backend_get_num_capsets()` or any virglrenderer API.

- [x] **Step 3: Publish VirGL capset count during renderer init/reset**

The VirGL backend counts VirGL/VirGL2 capsets after renderer init and reset,
then publishes the result into the emulator-owned virtio-gpu state.

- [x] **Step 4: Verify**

Run:

```sh
make test-vgpu-renderer-owner
make test-vgpu-fence
make test-vgpu-virgl
```

Expected: source gate rejects backend calls from the config read path, MMIO
config reads return the cached value, and VirGL init/reset refresh the cache.

**Remaining Phase 4 work:** the cache now exists, but renderer init/reset still
run under the temporary GL mutex until the full GL-owner request path is moved.

### Phase 4C: Move Renderer Poll to the GL Owner Queue

**Goal:** Establish the request-drain side of the GL-owner boundary for the
smallest renderer operation. Renderer polling should be requested by the
emulator thread but executed by the SDL/main-thread GL owner.

**Files:**

- Modify: `vgpu-gl.h`
- Modify: `virtio-gpu-virgl.c`
- Modify: `window-sw.c`
- Modify: `tests/vgpu-renderer-owner-test.sh`
- Modify: `tests/virtio-gpu-virgl-test.c`

- [x] **Step 1: Add the GL-owner execute hook**

`vgpu_virgl_execute_renderer_request()` is the GL-owner entry point for
renderer requests. This checkpoint supports `VGPU_RENDERER_REQ_POLL`; later
Phase 4 work will add controlq command execution.

- [x] **Step 2: Drain renderer requests before display work**

`window_main_loop_sw()` now calls `window_drain_renderer_queue()` before
`window_drain_display_queue()`, so renderer work reaches virglrenderer before
the same SDL tick renders dependent scanout state.

- [x] **Step 3: Enqueue poll requests instead of polling from the emulator**

Fenced responses mark a pending renderer fence and enqueue
`VGPU_RENDERER_REQ_POLL`; the backend `poll` hook only requests poll work while
a renderer fence remains pending. `virgl_renderer_poll()` is called by the
GL-owner execute hook, not by the emulator thread.

- [x] **Step 4: Verify**

Run:

```sh
make test-vgpu-renderer-owner
make test-vgpu-virgl
make test-vgpu-renderer
make test-vgpu-fence
make test-vgpu-display
make test-vgpu-virgl-backend-build
make ENABLE_VIRGL=1 semu
```

Expected: source gate confirms renderer queue drain order, fake VirGL tests
confirm fenced submits enqueue poll requests, and both fake/real VirGL builds
link.

**Remaining Phase 4 work:** `CTX_*`, resource, transfer, submit, capset,
scanout/flush, reset, and init execution are still synchronous backend calls
under the temporary GL mutex until the full command decode/execute split lands.

- [x] **Step 1: Split decode from execute**

In `virtio-gpu-virgl.c`, keep guest descriptor parsing on the emulator thread.
For each renderer-backed command, copy guest-provided payload into a
host-owned request object before submitting it to `vgpu_renderer_submit()`.

Commands that must go through the GL owner:

- `CTX_CREATE`
- `CTX_DESTROY`
- `CTX_ATTACH_RESOURCE`
- `CTX_DETACH_RESOURCE`
- `RESOURCE_CREATE_3D`
- `RESOURCE_UNREF` for VirGL resources
- `RESOURCE_ATTACH_BACKING`
- `RESOURCE_DETACH_BACKING`
- `TRANSFER_TO_HOST_2D`
- `TRANSFER_TO_HOST_3D`
- `TRANSFER_FROM_HOST_3D`
- `SUBMIT_3D`
- `SET_SCANOUT` for VirGL resources
- `RESOURCE_FLUSH` for VirGL resources
- `GET_CAPSET_INFO`
- `GET_CAPSET`
- renderer capset discovery used to cache `virtio_gpu_config.num_capsets`
- renderer `POLL`
- renderer `RESET`

- [x] **Step 2: Keep software-only commands synchronous**

Commands delegated to the software backend can stay synchronous while
`ENABLE_VIRGL=1`:

- `GET_DISPLAY_INFO`
- `RESOURCE_CREATE_2D`
- `GET_EDID`
- CPU cursor update/move payload creation

- [x] **Step 3: Drain renderer requests from the SDL loop**

In `window_main_loop_sw()`, drain renderer requests before display rendering:

```c
#if SEMU_HAS(VIRGL)
        window_drain_renderer_queue();
#endif
#if SEMU_HAS(VIRTIOGPU)
        window_drain_display_queue();
#endif
```

`window_drain_renderer_queue()` must run on the SDL main thread and call the
VirGL execute functions directly.

- [x] **Step 4: Return completions to the emulator thread**

After each request completes, enqueue `struct vgpu_renderer_completion` through
`vgpu_renderer_complete()` and wake the emulator thread using the existing
backend wake pipe.

- [x] **Step 5: Drain renderer completions from emulator ticks**

In `emu_tick_peripherals()`, before `virtio_gpu_poll(&emu->vgpu)`, drain all
renderer completions and call `virtio_gpu_complete_ctrl_response()` for each
matching token.

- [x] **Step 6: Cache `num_capsets` outside the MMIO config read path**

During renderer init and reset, the GL owner calls
`virgl_renderer_get_cap_set()` for `VIRTIO_GPU_CAPSET_VIRGL` and
`VIRTIO_GPU_CAPSET_VIRGL2`, then publishes a cached count to the emulator-owned
virtio-gpu state. `virtio_gpu_reg_read()` must answer
`virtio_gpu_config.num_capsets` from that cache and must not call
`virtio_gpu_backend_get_num_capsets()` or any virglrenderer API.

- [x] **Step 7: Move fence callbacks to the completion queue**

VirGL `write_fence` and `write_context_fence` callbacks must enqueue
`VGPU_RENDERER_DONE_FENCE`. They must not write guest memory or used-ring state
directly.

- [x] **Step 8: Add renderer poll/wakeup ownership**

The GL owner must drive `virgl_renderer_poll()`.

- If `virgl_renderer_get_poll_fd()` returns a valid fd, integrate a nonblocking
  readiness check into the SDL main-loop drain path and call
  `virgl_renderer_poll()` when the fd is readable.
- Keep a bounded fallback timeout while deferred renderer work is outstanding,
  so the SDL loop cannot sleep forever with a pending fence.
- Every fence or controlq completion enqueued by the GL owner must call
  `g_window.window_wake_backend()` to unblock the emulator thread.

- [x] **Step 9: Remove backend lock hooks**

Remove these from `struct virtio_gpu_cmd_backend`:

```c
virtio_gpu_backend_lifecycle_func thread_enter;
virtio_gpu_backend_lifecycle_func command_enter;
virtio_gpu_backend_lifecycle_func command_leave;
```

Remove `virtio_gpu_thread_enter()` and its call from `main.c`. The renderer no
longer needs emulator-thread ctx0 handoff because the GL owner owns ctx0.

- [x] **Step 10: Keep a temporary compatibility commit boundary**

At the end of this phase, the old `vgpu_gl_lock()` may still exist if tests
show a remaining path uses GL from both threads. If so, do not proceed to
Phase 5 until the remaining path is listed in this plan with a concrete owner
transfer.

Phase 4 leaves `vgpu_gl_lock()` only around SDL/window presentation helpers in
`window-sw.c` and the matching declarations/stubs in `vgpu-gl.h` and VirGL unit
tests. VirGL command execution, renderer reset, and renderer poll no longer use
backend command/thread lock hooks. Phase 5 owns deletion of the remaining
window-presentation mutex boundary.

Additional review fixes completed in this phase:

- Token-addressed async controlq completions and token-specific cancel prevent
  unfenced renderer work from completing or cancelling the wrong descriptor.
- Fence pending accounting is registered before `virgl_renderer_create_fence()`
  so synchronous fence callbacks cannot leave a stale poll request pending.
- Renderer queue reset releases queued request payloads and queued completion
  responses so dropped async work does not leak host-owned memory.

- [x] **Step 11: Verify**

Run:

```sh
make test-vgpu-renderer
make test-vgpu-fence
make test-vgpu-virgl
make test-vgpu-virgl-backend-build
make ENABLE_VIRGL=1 semu
git diff --check
```

Expected: all renderer-backed VirGL commands complete through the async
request/completion queues, and no guest-visible virtqueue write happens from
the SDL main thread.

Verified during implementation:

```sh
make test-vgpu-renderer
make test-vgpu-fence
make test-vgpu-virgl
make test-vgpu-renderer-owner
make test-vgpu-opengl-scope
make test-vgpu-display
make test-vinput-event-coalesce
make test-vgpu-chain
make test-vgpu-desc
make test-vgpu-virgl-backend-build
make ENABLE_VIRGL=1 semu
git diff --check
```

- [x] **Step 12: Commit**

```sh
git add main.c virtio-gpu.h virtio-gpu.c virtio-gpu-sw.c \
    virtio-gpu-virgl.c vgpu-renderer.h vgpu-renderer.c \
    tests/vgpu-renderer-queue-test.c tests/vgpu-renderer-owner-test.sh \
    tests/virtio-gpu-virgl-test.c tests/virtio-gpu-fence-test.c \
    docs/vgpu-3d-opengl-lockless-plan.md
git commit -m "Move VirGL execution to the GL owner"
```

## Phase 5: Remove the Global GL Mutex

**Goal:** Enforce the final lockless architecture with tests.

**Files:**

- Create: `tests/vgpu-no-gl-lock-test.sh`
- Modify: `Makefile`
- Modify: `window-sw.c`
- Modify: `vgpu-gl.h`
- Modify: `virtio-gpu-virgl.c`
- Modify: `tests/virtio-gpu-virgl-test.c`

- [x] **Step 1: Add the no-lock source gate**

Create `tests/vgpu-no-gl-lock-test.sh`:

```sh
#!/usr/bin/env bash

set -euo pipefail

fail()
{
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

if rg -n 'pthread_mutex|vgpu_gl_lock|vgpu_gl_unlock' \
    window-sw.c vgpu-gl.h virtio-gpu-virgl.c virtio-gpu.c virtio-gpu.h; then
    fail "final OpenGL path must not use a global GL mutex"
fi

if rg -n 'command_enter|command_leave|thread_enter|virtio_gpu_thread_enter' \
    virtio-gpu.c virtio-gpu.h virtio-gpu-virgl.c main.c; then
    fail "VirGL ownership must not rely on backend command/thread lock hooks"
fi
```

- [x] **Step 2: Add the Makefile target**

Add:

```make
.PHONY: test-vgpu-no-gl-lock
test-vgpu-no-gl-lock:
	$(Q)bash tests/vgpu-no-gl-lock-test.sh
```

- [x] **Step 3: Remove lock declarations and definitions**

Delete from `vgpu-gl.h`:

```c
void vgpu_gl_lock(void);
void vgpu_gl_unlock(void);
```

Delete the `pthread_mutex_t vgpu_gl_mutex` block from `window-sw.c`.

- [x] **Step 4: Remove remaining lock call sites**

No code path may wrap `sdl_scanout_render_gl()`,
`sdl_scanout_apply_gl_frame()`, `sdl_scanout_apply_gl_cursor_frame()`,
`virgl_renderer_poll()`, or renderer reset with a mutex. These functions should
already run on the GL owner after Phase 4.

- [x] **Step 5: Verify**

Run:

```sh
bash -n tests/vgpu-no-gl-lock-test.sh
make test-vgpu-no-gl-lock
make test-vgpu-renderer
make test-vgpu-display
make test-vgpu-virgl
make test-vgpu-fence
make ENABLE_VIRGL=1 semu
git diff --check
```

Expected: no global GL lock references remain and all VirGL tests still pass.

Verified during implementation:

```sh
bash -n tests/vgpu-no-gl-lock-test.sh
make test-vgpu-no-gl-lock
make test-vgpu-renderer
make test-vgpu-display
make test-vgpu-virgl
make test-vgpu-fence
make ENABLE_VIRGL=1 semu
git diff --check
```

- [x] **Step 6: Commit**

```sh
git add Makefile window-sw.c vgpu-gl.h virtio-gpu-virgl.c virtio-gpu.c \
    virtio-gpu.h main.c tests/vgpu-no-gl-lock-test.sh \
    tests/virtio-gpu-virgl-test.c
git commit -m "Remove temporary GL mutex"
```

## Phase 6: Cursor and Render-loop Performance

**Goal:** Reduce mouse-motion FPS collapse without hiding correctness bugs
during stress testing.

**Files:**

- Modify: `window-sw.c`
- Modify: `vgpu-display.c`
- Modify: `tests/vgpu-display-test.c`
- Modify: `tests/vinput-event-coalesce-test.sh`
- Modify: `.ci/test-virgl.sh`

- [x] **Step 1: Keep raw input delivery uncoalesced by default**

Keep `virtio-input-event.c` publishing each SDL mouse motion and keep
`virtio-input.c` forwarding each host motion to the guest. The existing
`test-vinput-event-coalesce` gate should continue to reject input coalescing.

- [x] **Step 2: Batch presentation, not guest input**

Optimize only the SDL/GL presentation side. The guest may receive every mouse
motion, but the host window should render at most once per main-loop drain
cycle.

In `window_drain_display_queue()`, keep the existing `dirty_scanouts[]` batching
and add a local `cursor_dirty_scanouts[]` array so multiple cursor moves in the
same drain batch produce one `SDL_GL_SwapWindow()`.

- [x] **Step 3: Add a cursor repaint regression test**

Extend `tests/vgpu-display-test.c` with a test that publishes three cursor
moves and one cursor set, then verifies FIFO command order remains intact. This
keeps event ordering visible while allowing the window backend to batch
presentation.

- [x] **Step 4: Add a manual FPS note to the live smoke**

When `.ci/test-virgl.sh` runs `glxgears`, save the first 40 lines of
`/tmp/glxgears.log` as it does now. Manual testing should compare FPS while
the mouse is idle and while moving quickly.

- [x] **Step 5: Verify**

Run:

```sh
make test-vgpu-display
make test-vinput-event-coalesce
make ENABLE_VIRGL=1 semu
.ci/test-virgl.sh
```

Expected: input coalescing remains disabled, cursor display ordering tests
pass, `glxgears` still renders, and fast mouse movement no longer crashes.

Verified during implementation:

```sh
bash -n .ci/test-virgl.sh
make test-vgpu-display
make test-vinput-event-coalesce
make ENABLE_VIRGL=1 semu
xvfb-run -a .ci/test-virgl.sh
git diff --check
```

Note: direct `.ci/test-virgl.sh` needs a host `DISPLAY` or `WAYLAND_DISPLAY`.
This environment did not provide one, so the smoke was run through
`xvfb-run -a`. Manual FPS comparison while moving the mouse quickly still needs
a real visible display.

- [x] **Step 6: Commit**

```sh
git add window-sw.c vgpu-display.c tests/vgpu-display-test.c \
    tests/vinput-event-coalesce-test.sh .ci/test-virgl.sh
git commit -m "Batch VirGL cursor presentation without input coalescing"
```

## Phase 7: Final Merge Gates

**Goal:** Prove the OpenGL-only path is mergeable without claiming support for
future 3D features.

**Files:**

- Modify: `README.md`
- Modify:
  `/home/mes/MesRepo/dev-docs/MesDevLog/semu-vgpu-3D/README.md`

- [x] **Step 1: Run automated verification**

Run:

```sh
bash -n .ci/test-virgl.sh
bash -n scripts/run-vgpu-crash-debug.sh
bash -n tests/vgpu-opengl-scope-test.sh
bash -n tests/vgpu-no-gl-lock-test.sh
make test-vgpu-opengl-scope
make test-vgpu-no-gl-lock
make test-vgpu-renderer
make test-vgpu-display
make test-vinput-event-coalesce
make test-vgpu-desc
make test-vgpu-chain
make test-vgpu-fence
make test-vgpu-virgl
make test-vgpu-virgl-gate
make test-vgpu-virgl-image
make test-vgpu-virgl-init-order
make test-vgpu-virgl-backend-build
make clean
make semu
make clean
make ENABLE_VIRGL=1 semu
git diff --check
```

Final automated gate after Phase 12 also passed on 2026-05-07:

```sh
bash -n .ci/test-virgl.sh
bash -n scripts/run-vgpu-crash-debug.sh
bash -n tests/vgpu-opengl-scope-test.sh
bash -n tests/vgpu-no-gl-lock-test.sh
make test-vgpu-opengl-scope test-vgpu-no-gl-lock test-vgpu-renderer \
    test-vgpu-display test-vinput-event-coalesce test-vgpu-desc \
    test-vgpu-chain test-vgpu-fence test-vgpu-virgl test-vgpu-virgl-gate \
    test-vgpu-virgl-image test-vgpu-virgl-init-order \
    test-vgpu-virgl-backend-build
make clean
make semu
make clean
make ENABLE_VIRGL=1 semu
git diff --check
```

Expected: all commands exit 0. The final `./semu` should be built with
`ENABLE_VIRGL=1` for manual smoke testing.

Verified during implementation:

```sh
bash -n .ci/test-virgl.sh
bash -n scripts/run-vgpu-crash-debug.sh
bash -n tests/vgpu-opengl-scope-test.sh
bash -n tests/vgpu-no-gl-lock-test.sh
make test-vgpu-opengl-scope test-vgpu-no-gl-lock test-vgpu-renderer \
    test-vgpu-display test-vinput-event-coalesce test-vgpu-desc \
    test-vgpu-chain test-vgpu-fence test-vgpu-virgl test-vgpu-virgl-gate \
    test-vgpu-virgl-image test-vgpu-virgl-init-order \
    test-vgpu-virgl-backend-build
make clean
make semu
make clean
make ENABLE_VIRGL=1 semu
git diff --check
```

- [x] **Step 2: Run visible OpenGL smoke**

Run:

```sh
scripts/build-image.sh --virgl
.ci/test-virgl.sh
```

Expected:

- guest has `/usr/lib/dri/virtio_gpu_dri.so`
- guest has `/dev/dri/card0` and `/dev/dri/renderD128`
- `glxinfo -B` reports a renderer containing `virgl`
- `glxgears` starts and renders in the SDL/OpenGL window

Verified during implementation:

```sh
scripts/build-image.sh --virgl
xvfb-run -a .ci/test-virgl.sh
```

This environment did not provide a real host `DISPLAY` or `WAYLAND_DISPLAY`, so
the smoke was run with Xvfb. That proves the automated SDL/OpenGL/VirGL
plumbing, but it is not a substitute for visible mouse/FPS stress on a real
host display.

- [x] **Step 3: Run reset/reboot smoke**

Run:

```sh
SEMU_VIRGL_REBOOT_TEST=1 .ci/test-virgl.sh
```

Expected: guest reboot does not leave stale fences, stale scanout textures, or
a stuck host window.

Verified during implementation:

```sh
xvfb-run -a env SEMU_VIRGL_REBOOT_TEST=1 .ci/test-virgl.sh
```

The run reached the VirGL renderer and `glxgears`, then the guest printed
`reboot: Restarting system` and semu reported `system reset: type=1, reason=0`
before exiting cleanly. Current semu SBI reset handling stops the host process
instead of warm-rebooting the guest in-place; the smoke script now accepts that
clean reset exit and also accepts a future return to the login prompt.

- [x] **Step 4: Run interactive stress**

Run:

```sh
scripts/run-vgpu-crash-debug.sh
```

Guest commands:

```sh
root
. /root/local-env.sh
DISPLAY=:0 twm >/tmp/twm.log 2>&1 &
DISPLAY=:0 xterm >/tmp/xterm.log 2>&1 &
DISPLAY=:0 glxgears
```

Move the mouse quickly for at least five minutes. Expected: no host segfault,
no guest Xorg crash, cursor remains visible without black square artifacts, and
window close exits cleanly.

Note: the VirGL test-tools image starts `Xorg :0` during boot. Do not run
`startx` for this manual gate because that starts a second X server on `:1`;
that is a separate multi-server stress path and was the source of the
`Xorg.1.log` / `COPY_TRANSFER3D` errors seen during the first manual attempt.

Verified after the follow-up race fixes on a real visible host display:

- single-Xorg `DISPLAY=:0` path with `twm`, `xterm`, and `glxgears` remained
  responsive;
- `startx` / twm-menu xterm path no longer reproduced the known
  `COPY_TRANSFER3D` / `Illegal command buffer` errors;
- two `glmark2` instances with aggressive mouse movement stayed live for about
  ten minutes after the renderer MPSC queue fix.

The earlier headless/Xvfb runs still cover automated VirGL plumbing, but the
visible stress result above is the coverage for mouse capture, compositor, and
real host display behavior.

- [x] **Step 5: Update final docs**

Record:

- OpenGL/VirGL first version is complete.
- No global GL mutex remains.
- Blob/context_init/Venus/Vulkan remain future work.
- Headless CI does not run visible OpenGL smoke.
- Known performance status for fast mouse movement.

- [x] **Step 6: Commit**

```sh
git add README.md docs/vgpu-3d-opengl-lockless-plan.md .ci/test-virgl.sh
git commit -m "Document VirGL lockless merge gate status"
```

Note: `/home/mes/MesRepo/dev-docs/MesDevLog/semu-vgpu-3D/README.md` is outside
the Mes-semu git repository in this workspace. It was updated as a local
documentation file but cannot be included in the Mes-semu commit.

## Phase 8: ctx0 Fence Current-context Regression

Status: implemented, committed, and manually retested. The single-Xorg visible
SDL retest passed with the manual `DISPLAY=:0 ...` commands. The separate
`startx` / multi-Xorg path is tracked in Phase 9.

### Trigger

During 2026-05-07 manual visible VirGL testing, the stable trigger was:

```sh
DISPLAY=:0 twm >/tmp/twm.log 2>&1 &
DISPLAY=:0 xterm >/tmp/xterm.log 2>&1 &
DISPLAY=:0 glxgears
```

Then open additional `xterm` windows while `glxgears` is running. The failure
showed:

```text
Failed to create fence sync object
vrend_check_no_error: context error reported 1 "Xorg" Unknown 1282
context 1 failed to dispatch COPY_TRANSFER3D: 22
vrend_decode_ctx_submit_cmd: context error reported 1 "Xorg" Illegal command buffer 917549
```

`917549` is `0xe002d`: a VirGL command buffer entry with size `0xe` and command
`0x2d`, i.e. `VIRGL_CCMD_COPY_TRANSFER3D`.

### Root Cause

`virgl_renderer_create_fence()` creates a legacy ctx0 fence, but virglrenderer
does not make ctx0 current before calling `glFenceSync()`. After the lockless GL
owner work, SDL presentation can detach the GL context from the calling thread,
so a fenced ctx0 submit can try to create a GL fence with no current context.

That explains the leading `Failed to create fence sync object` message. The
following `GL_INVALID_OPERATION` / `COPY_TRANSFER3D` report is consistent with a
stale or downstream GL error observed during the next Xorg transfer command.

### Fix

Before calling `virgl_renderer_create_fence()` for a non-ring-index fence,
explicitly force ctx0 current:

```c
virgl_renderer_force_ctx_0();
ret = virgl_renderer_create_fence((int) (uint32_t) work->hdr.fence_id, 0);
```

Context/ring-index fences continue to use `virgl_renderer_context_create_fence()`
without changing that path.

### Regression Coverage

Added `test_ctx0_fence_forces_ctx0_current_before_create()` to
`tests/virtio-gpu-virgl-test.c`. The test submits a fenced legacy ctx0 command
and requires:

- `submit_cmd()` is called once.
- `virgl_renderer_force_ctx_0()` is called before the legacy fence.
- `virgl_renderer_create_fence()` is used.
- `virgl_renderer_context_create_fence()` is not used.

Fresh verification after the fix:

```sh
make test-vgpu-virgl test-vgpu-fence test-vgpu-no-gl-lock test-vgpu-opengl-scope
make ENABLE_VIRGL=1 semu
git diff --check
xvfb-run -a .ci/test-virgl.sh
```

Additional headless stress also passed by running guest Xorg with `glxgears` and
opening three `xterm` windows under Xvfb. This does not replace the real visible
SDL test because Xvfb does not cover actual mouse capture, host window handling,
or the user's compositor/driver path.

### Manual Retest Gate

Rebuild has already produced a VirGL-enabled `semu`. Retest on the visible host
display with:

```sh
scripts/run-vgpu-crash-debug.sh
```

Guest:

```sh
root
DISPLAY=:0 twm >/tmp/twm.log 2>&1 &
DISPLAY=:0 xterm >/tmp/xterm1.log 2>&1 &
DISPLAY=:0 glxgears >/tmp/glxgears.log 2>&1 &
```

Then open more xterms from the same guest shell while `glxgears` is still
running:

```sh
DISPLAY=:0 xterm >/tmp/xterm2.log 2>&1 &
DISPLAY=:0 xterm >/tmp/xterm3.log 2>&1 &
```

Expected: no `Failed to create fence sync object`, no `COPY_TRANSFER3D`, no
`Illegal command buffer`, and the visible SDL window keeps repainting.

## Phase 9: startx Multi-Xorg Burst Queue Regression

Status: implemented, committed, and manually retested. This fixed the
pending-slot/resource ordering failure that appeared in automated `startx`
testing. The later visible twm-menu xterm failure is tracked separately in
Phase 10.

### Trigger

The test-tools image starts `Xorg :0` during guest boot. Running `startx` from
the shell starts a second server, `Xorg :1`, and generates a much larger burst
of VirGL control commands than the single-Xorg smoke path.

The failing `startx` path showed:

```text
vrend_check_no_error: context error reported 2 "X" Unknown 1282
context 2 failed to dispatch COPY_TRANSFER3D: 22
vrend_decode_ctx_submit_cmd: context error reported 2 "X" Illegal command buffer 917549
```

The automated reproduction exposed the earlier host-side failures that led to
that renderer error:

```text
[SEMU VGPU] virtio_gpu_defer_ctrl_response_token(): no free pending ctrl slot
[SEMU VGPU] vgpu_sw_cmd_resource_attach_backing_handler(): invalid resource id
vrend_renderer_copy_transfer3d: context error reported 2 "X" Illegal resource 117
```

### Root Cause

The guest virtio-gpu queue supports up to `VIRTIO_GPU_QUEUE_NUM_MAX` descriptors
(`1024`), but the lockless VirGL async path only had:

- `VIRTIO_GPU_PENDING_CTRLS_MAX = 256`
- `VGPU_RENDERER_QUEUE_SIZE = 256`, with one slot reserved by the ring full/empty
  convention, so only `255` usable request slots

When `startx` queued a burst from the second X server, semu ran out of pending
response slots before the GL owner could drain enough renderer work. Some
`RESOURCE_CREATE_3D` requests failed and removed their resource tracking entries;
later `RESOURCE_ATTACH_BACKING` / VirGL submit commands then referenced resource
ids that no longer existed, ending in VirGL `COPY_TRANSFER3D` illegal-resource
errors.

### Fix

- Set `VIRTIO_GPU_PENDING_CTRLS_MAX` to `VIRTIO_GPU_QUEUE_NUM_MAX`, so there is
  enough pending response state for one full guest control-queue burst.
- Set `VGPU_RENDERER_QUEUE_SIZE` to `VIRTIO_GPU_QUEUE_NUM_MAX * 2`. The internal
  ring intentionally keeps one slot empty, so it must be larger than the guest
  queue depth to accept all `1024` descriptors.

### Regression Coverage

Added/updated:

- `tests/vgpu-renderer-queue-test.c`: renderer request queue must accept a full
  `VIRTIO_GPU_QUEUE_NUM_MAX` burst before draining.
- `tests/virtio-gpu-fence-test.c`: pending control capacity must cover the full
  virtio-gpu queue depth.

The new tests failed before the fix:

```text
tests/vgpu-renderer-queue-test.c:142: check failed: accepted >= VIRTIO_GPU_QUEUE_NUM_MAX
tests/virtio-gpu-fence-test.c:485: check failed: VIRTIO_GPU_PENDING_CTRLS_MAX >= VIRTIO_GPU_QUEUE_NUM_MAX
```

They pass after the fix, and the `startx` automation no longer reproduces the
bad host log strings during the wait window.

### Manual Retest Gate

After rebuilding `semu`, run:

```sh
scripts/run-vgpu-crash-debug.sh
```

Guest:

```sh
root
startx
```

Expected: no `no free pending ctrl slot`, no `invalid resource id`, no
`Illegal resource`, no `COPY_TRANSFER3D`, and no `Illegal command buffer`.

## Phase 10: VirGL Cached Current-context Regression

Status: implemented, committed, and manually retested. The visible `startx`
twm-menu xterm path no longer reproduced the known `COPY_TRANSFER3D` /
`Illegal command buffer` failure after this fix.

### Trigger

Run `startx`, then use the twm root menu in the visible SDL window to open
additional `xterm` windows. The remaining failure did not show the Phase 8
`Failed to create fence sync object` message and did not show the Phase 9
pending-slot/resource-ordering messages. It reported:

```text
vrend_check_no_error: context error reported 2 "X" Unknown 1282
context 2 failed to dispatch COPY_TRANSFER3D: 22
vrend_decode_ctx_submit_cmd: context error reported 2 "X" Illegal command buffer 917549
```

### Root Cause

`virgl_renderer_submit_cmd()` eventually calls `vrend_hw_switch_context()`, but
virglrenderer skips the actual `make_current()` call when its cached
`current_ctx/current_hw_ctx` already match the submitted context.

That cache can become stale in semu because SDL presentation intentionally
detaches the real GL context after rendering. The next VirGL submit for the same
ctx id can therefore run with virglrenderer believing ctx2 is current while SDL
has no real current context bound on the thread.

QEMU avoids this by calling `virgl_renderer_force_ctx_0()` before dispatching
each VirGL command. That resets virglrenderer's cached current context and
forces the next submit to perform a real `make_current()`.

### Fix

At the GL-owner boundary, call `virgl_renderer_force_ctx_0()` before executing
each renderer-owned VirGL control command. Keep the separate legacy ctx0 fence
force as well, because a successful submit switches into the guest context
before fence creation.

The earlier scanout publish-specific force calls are now redundant and were
removed; publishing only hands a texture payload to the display queue and does
not itself issue GL commands.

### Regression Coverage

Updated `test_submit_3d_copies_payload_to_renderer()` so
`virgl_renderer_submit_cmd()` records the `force_ctx_0` count at submit time and
requires it to be `1`.

The test failed before the fix:

```text
tests/virtio-gpu-virgl-test.c:992: check failed: g_calls.submit_force_ctx_0_count == 1
```

After the fix, `make test-vgpu-virgl` passes, and automated `startx` with
multiple xterms does not emit the known bad strings.

### Manual Retest Gate

Rebuild has already produced a VirGL-enabled `semu`. Retest the visible path:

```sh
scripts/run-vgpu-crash-debug.sh
```

Guest:

```sh
root
startx
```

Then in the SDL window, use the twm root menu to open at least two additional
`xterm` windows. Expected: no `COPY_TRANSFER3D`, no `Illegal command buffer`,
and the X session remains responsive.

## Phase 11: Input PageUp/Wheel Diagnostic Logging

Status: implemented, committed, and manually retested. The diagnostic
instrumentation identified actual PageUp key forwarding after host Alt+Tab, and
the focus-loss keyboard suppression fix stopped that visible `^[[5~`
reproduction.

### Trigger

After a long `glmark2` run, moving the mouse in the SDL window can make the
focused `xterm` continuously print:

```text
^[[5~
```

In xterm this is the PageUp escape sequence. In the current input model that can
come from either an actual guest PageUp key event or from scroll/wheel events
being interpreted by xterm.

The current manual reproduction is:

1. Enter the SDL mouse grab state.
2. Press Alt+Tab while still grabbed and switch to a host window.
3. Switch back to semu.
4. Move the mouse over the focused guest `xterm`.

### Diagnostic Gap

The existing VGPU progress log could identify renderer/display/fence stalls, but
it could not say which input boundary produced the bad event:

- SDL frontend event pump.
- Host-to-emulator virtio-input command queue.
- Guest-facing virtio-input event queue.

Without those counters, the symptom looked like "mouse motion becomes keyboard
input", but there was no evidence showing whether semu sent a PageUp key,
`REL_WHEEL`, or only normal `REL_Y` motion.

### Root Cause

The `20260507-171321` progress log showed:

- `input-host sdl_wheel=0/0`
- `input-guest scroll_batches=0`
- `input-guest rel_x/y/hwheel/wheel=.../0/0`
- `input-guest pageup` increased, with `last_key=104`

`104` is `SEMU_KEY_PAGEUP`, so this was not mouse wheel input. SDL delivered
real PageUp keyboard events around the host Alt+Tab / focus transition, and
semu forwarded them to the guest after the window was re-entered.

### Fix

On `SDL_WINDOWEVENT_FOCUS_LOST`, release any guest-visible pressed keyboard
keys, reset SDL's keyboard state, disable keyboard forwarding, and leave
forwarding disabled until the user explicitly clicks the semu window to re-enter
mouse-grabbed guest input mode.

The implementation keeps a small host-side pressed-key bitmap so the focus-loss
path can synthesize releases for keys already visible to the guest. It also
clears that bitmap on virtio-input keyboard reset.

### Instrumentation

Extend the progress monitor with two input lines:

- `input-host`: queue depths, pushed/dropped/popped host commands, SDL key/button
  counts, SDL mouse motion counts, SDL wheel counts, and last motion/wheel deltas.
- `input-guest`: guest key count, PageUp key count, mouse motion batch count,
  scroll batch count, REL_X/REL_Y/REL_HWHEEL/REL_WHEEL counts, eventq writes and
  drops, and last keyboard/mouse relative event.

The important fields for this bug are:

- `sdl_wheel`: SDL is delivering wheel events.
- `scroll_batches` and `rel_x/y/hwheel/wheel`: semu is sending guest scroll
  events.
- `pageup`: semu is sending an actual guest PageUp key.
- `sdl_motion`, `motion_batches`, and `rel_x/y/hwheel/wheel`: semu is sending
  normal mouse motion only.

### Manual Retest Gate

Run the normal crash-debug wrapper:

```sh
scripts/run-vgpu-crash-debug.sh
```

Guest:

```sh
root
startx
glmark2
```

Before the fix, when `^[[5~` started appearing, the saved
`vgpu-progress.log` classified the issue as actual PageUp key forwarding rather
than wheel or relative motion. After the fix, repeat the host Alt+Tab while
grabbed, switch back to semu, click to re-enter guest input, and move the mouse
over the focused `xterm`.

Expected after the fix:

- no new `^[[5~` stream appears in the focused `xterm`;
- keyboard forwarding resumes only after an explicit click/regrab;
- mouse motion continues to appear as relative motion, not PageUp keys.

Diagnostic interpretation if this regresses:

- If `input-host sdl_wheel` rises together with `input-guest scroll_batches` or
  `rel_wheel`, the host frontend is receiving wheel input and forwarding it as
  guest scroll.
- If `input-guest pageup` rises, semu is incorrectly forwarding PageUp key
  events.
- If only `sdl_motion`, `motion_batches`, and `rel_y` rise, the guest/X stack is
  interpreting normal mouse motion in an unexpected way and the next
  investigation should move into the guest X input path.

## Phase 12: Renderer MPSC Queue Lost Poll Fix

Status: implemented, committed, and manually retested. A visible run with two
`glmark2` instances and aggressive mouse movement stayed responsive for about
ten minutes, which covers the original freeze trigger.

### Trigger

With X still alive, running two `glmark2` instances and moving the mouse for a
few minutes could freeze visible rendering. Pressing Ctrl-C in the guest serial
did not stop host-side `gdb`; sending SIGINT to the host semu PID produced the
capture in:

```text
vgpu-crash-logs/20260507-172416/
```

### Evidence

The progress log showed the frontend loop was alive while VirGL progress was
stuck:

- `stage=handle-events`, with `sdl_events` and `renderer_drains` still rising.
- `renderer_reqs`, `display_cmds`, and `renders` stopped changing.
- `virgl pending_fences=57106 poll_pending=1 poll=312994/312993`.
- `renderer submitted=424649 popped=424648 req_q=0`.
- `virtio pending_ctrls=343`.

The gdb backtrace matched that state: the main thread was in SDL event polling,
and the emulator thread was still in `vm_step_many()`. There was no host crash
or blocking GL call on the main thread.

### Root Cause

`vgpu-renderer.c` implemented the request and completion queues as if each ring
had one producer. That was false:

- the emulator thread submits renderer control work;
- the SDL/GL owner submits poll work after creating a successful renderer
  fence;
- fence callbacks can submit completions.

Two concurrent producers could both read the same ring `head`, write the same
slot, and store the same next `head`. One submit appeared successful in debug
counters but became unreachable to the consumer. If the lost request was
`VGPU_RENDERER_REQ_POLL`, `g_vgpu_virgl_poll_request_pending` stayed true
forever. Future fence creation then refused to enqueue another poll request,
so fence retirement and display updates stopped even though both semu threads
remained alive.

### Fix

Change the renderer request and completion rings to multi-producer,
single-consumer lockless queues:

- use monotonic `uint64_t` head/tail counters to avoid modulo ABA;
- producers reserve a slot with CAS;
- producers publish the slot with a per-slot ready byte after copying payload;
- the single consumer only pops FIFO slots whose ready byte is visible.

This keeps the intended lockless GL-owner boundary while preventing producer
overwrite/lost-submit races.

### Regression Coverage

Added concurrent producer tests to `tests/vgpu-renderer-queue-test.c`:

- request producers submit 384 total requests from 16 threads, then verify every
  unique request is popped once;
- completion producers do the same through `vgpu_renderer_complete()`.

The request test failed before the fix:

```text
tests/vgpu-renderer-queue-test.c:258: check failed: popped == CONCURRENT_REQUESTS_TOTAL
```

### Verification

```sh
make test-vgpu-renderer
make test-vgpu-fence test-vgpu-virgl
make ENABLE_VIRGL=1 semu
```

### Manual Retest Gate

Run:

```sh
scripts/run-vgpu-crash-debug.sh
```

Guest:

```sh
root
startx
```

Then start two `glmark2` instances, move the mouse aggressively for at least
three minutes, and stop the run if the picture freezes. Expected after this fix:

- `poll_pending` should not stay at `1` with `poll=submitted/executed+1`;
- `renderer submitted` and `popped` should not remain mismatched while
  `req_q=0`;
- `pending_fences` should continue to move instead of permanently backing up.

## Remaining Future Work Outside This Plan

- Venus/Vulkan.
- `VIRTIO_GPU_F_RESOURCE_BLOB`.
- `VIRTIO_GPU_F_CONTEXT_INIT`.
- Host-visible SHM.
- vhost-user-gpu or renderer sandboxing.
- Dedicated headless GL CI runner.
- Full multi-process renderer isolation.
- RV64/SMP-wide audit of all guest physical address assumptions.

## Self-review Notes

- Spec coverage: OpenGL-only scope is covered by Phase 1, lockless architecture
  by Phases 2-5, mouse/render performance by Phase 6, and merge validation by
  Phase 7.
- The plan deliberately treats the current global GL mutex as temporary. It is
  acceptable as a crash fix in the current branch, but Phase 5 makes its
  removal a hard gate.
- The plan keeps the `vgpu-pr` ownership invariant: SDL/main thread owns SDL
  and GL, emulator thread owns guest-visible virtio state, and the only
  cross-thread state transfer is through bounded queues and wakeups.
