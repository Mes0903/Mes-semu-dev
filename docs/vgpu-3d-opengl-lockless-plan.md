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
- The guest can run `startx` and `glxgears` with the VirGL renderer.
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

- [ ] **Step 1: Split decode from execute**

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

- [ ] **Step 2: Keep software-only commands synchronous**

Commands delegated to the software backend can stay synchronous while
`ENABLE_VIRGL=1`:

- `GET_DISPLAY_INFO`
- `RESOURCE_CREATE_2D`
- `GET_EDID`
- CPU cursor update/move payload creation

- [ ] **Step 3: Drain renderer requests from the SDL loop**

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

- [ ] **Step 4: Return completions to the emulator thread**

After each request completes, enqueue `struct vgpu_renderer_completion` through
`vgpu_renderer_complete()` and wake the emulator thread using the existing
backend wake pipe.

- [ ] **Step 5: Drain renderer completions from emulator ticks**

In `emu_tick_peripherals()`, before `virtio_gpu_poll(&emu->vgpu)`, drain all
renderer completions and call `virtio_gpu_complete_ctrl_response()` for each
matching token.

- [ ] **Step 6: Cache `num_capsets` outside the MMIO config read path**

During renderer init and reset, the GL owner calls
`virgl_renderer_get_cap_set()` for `VIRTIO_GPU_CAPSET_VIRGL` and
`VIRTIO_GPU_CAPSET_VIRGL2`, then publishes a cached count to the emulator-owned
virtio-gpu state. `virtio_gpu_reg_read()` must answer
`virtio_gpu_config.num_capsets` from that cache and must not call
`virtio_gpu_backend_get_num_capsets()` or any virglrenderer API.

- [ ] **Step 7: Move fence callbacks to the completion queue**

VirGL `write_fence` and `write_context_fence` callbacks must enqueue
`VGPU_RENDERER_DONE_FENCE`. They must not write guest memory or used-ring state
directly.

- [ ] **Step 8: Add renderer poll/wakeup ownership**

The GL owner must drive `virgl_renderer_poll()`.

- If `virgl_renderer_get_poll_fd()` returns a valid fd, integrate a nonblocking
  readiness check into the SDL main-loop drain path and call
  `virgl_renderer_poll()` when the fd is readable.
- Keep a bounded fallback timeout while deferred renderer work is outstanding,
  so the SDL loop cannot sleep forever with a pending fence.
- Every fence or controlq completion enqueued by the GL owner must call
  `g_window.window_wake_backend()` to unblock the emulator thread.

- [ ] **Step 9: Remove backend lock hooks**

Remove these from `struct virtio_gpu_cmd_backend`:

```c
virtio_gpu_backend_lifecycle_func thread_enter;
virtio_gpu_backend_lifecycle_func command_enter;
virtio_gpu_backend_lifecycle_func command_leave;
```

Remove `virtio_gpu_thread_enter()` and its call from `main.c`. The renderer no
longer needs emulator-thread ctx0 handoff because the GL owner owns ctx0.

- [ ] **Step 10: Keep a temporary compatibility commit boundary**

At the end of this phase, the old `vgpu_gl_lock()` may still exist if tests
show a remaining path uses GL from both threads. If so, do not proceed to
Phase 5 until the remaining path is listed in this plan with a concrete owner
transfer.

- [ ] **Step 11: Verify**

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

- [ ] **Step 12: Commit**

```sh
git add main.c window-sw.c vgpu-gl.h virtio-gpu.h virtio-gpu.c \
    virtio-gpu-virgl.c tests/virtio-gpu-virgl-test.c \
    tests/virtio-gpu-fence-test.c tests/vgpu-virgl-backend-build-test.sh
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

- [ ] **Step 1: Add the no-lock source gate**

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

- [ ] **Step 2: Add the Makefile target**

Add:

```make
.PHONY: test-vgpu-no-gl-lock
test-vgpu-no-gl-lock:
	$(Q)bash tests/vgpu-no-gl-lock-test.sh
```

- [ ] **Step 3: Remove lock declarations and definitions**

Delete from `vgpu-gl.h`:

```c
void vgpu_gl_lock(void);
void vgpu_gl_unlock(void);
```

Delete the `pthread_mutex_t vgpu_gl_mutex` block from `window-sw.c`.

- [ ] **Step 4: Remove remaining lock call sites**

No code path may wrap `sdl_scanout_render_gl()`,
`sdl_scanout_apply_gl_frame()`, `sdl_scanout_apply_gl_cursor_frame()`,
`virgl_renderer_poll()`, or renderer reset with a mutex. These functions should
already run on the GL owner after Phase 4.

- [ ] **Step 5: Verify**

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

- [ ] **Step 6: Commit**

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

- [ ] **Step 1: Keep raw input delivery uncoalesced by default**

Keep `virtio-input-event.c` publishing each SDL mouse motion and keep
`virtio-input.c` forwarding each host motion to the guest. The existing
`test-vinput-event-coalesce` gate should continue to reject input coalescing.

- [ ] **Step 2: Batch presentation, not guest input**

Optimize only the SDL/GL presentation side. The guest may receive every mouse
motion, but the host window should render at most once per main-loop drain
cycle.

In `window_drain_display_queue()`, keep the existing `dirty_scanouts[]` batching
and add a local `cursor_dirty_scanouts[]` array so multiple cursor moves in the
same drain batch produce one `SDL_GL_SwapWindow()`.

- [ ] **Step 3: Add a cursor repaint regression test**

Extend `tests/vgpu-display-test.c` with a test that publishes three cursor
moves and one cursor set, then verifies FIFO command order remains intact. This
keeps event ordering visible while allowing the window backend to batch
presentation.

- [ ] **Step 4: Add a manual FPS note to the live smoke**

When `.ci/test-virgl.sh` runs `glxgears`, save the first 40 lines of
`/tmp/glxgears.log` as it does now. Manual testing should compare FPS while
the mouse is idle and while moving quickly.

- [ ] **Step 5: Verify**

Run:

```sh
make test-vgpu-display
make test-vinput-event-coalesce
make ENABLE_VIRGL=1 semu
.ci/test-virgl.sh
```

Expected: input coalescing remains disabled, cursor display ordering tests
pass, `glxgears` still renders, and fast mouse movement no longer crashes.

- [ ] **Step 6: Commit**

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

- [ ] **Step 1: Run automated verification**

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

Expected: all commands exit 0. The final `./semu` should be built with
`ENABLE_VIRGL=1` for manual smoke testing.

- [ ] **Step 2: Run visible OpenGL smoke**

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

- [ ] **Step 3: Run reset/reboot smoke**

Run:

```sh
SEMU_VIRGL_REBOOT_TEST=1 .ci/test-virgl.sh
```

Expected: guest reboot does not leave stale fences, stale scanout textures, or
a stuck host window.

- [ ] **Step 4: Run interactive stress**

Run:

```sh
scripts/run-vgpu-crash-debug.sh
```

Guest commands:

```sh
root
startx
DISPLAY=:0 glxgears
```

Move the mouse quickly for at least five minutes. Expected: no host segfault,
no guest Xorg crash, cursor remains visible without black square artifacts, and
window close exits cleanly.

- [ ] **Step 5: Update final docs**

Record:

- OpenGL/VirGL first version is complete.
- No global GL mutex remains.
- Blob/context_init/Venus/Vulkan remain future work.
- Headless CI does not run visible OpenGL smoke.
- Known performance status for fast mouse movement.

- [ ] **Step 6: Commit**

```sh
git add README.md /home/mes/MesRepo/dev-docs/MesDevLog/semu-vgpu-3D/README.md
git commit -m "Mark OpenGL VirGL path merge-ready"
```

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
