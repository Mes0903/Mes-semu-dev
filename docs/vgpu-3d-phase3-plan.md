# virtio-gpu 3D Phase 3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans
> to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Complete the VirGL backend command surface for context, resource,
backing, transfer, and submit handling, while keeping guest feature
advertisement disabled until the GL display bridge exists.

**Architecture:** `virtio-gpu-virgl.c` owns VirGL resource/context command
translation and delegates the existing CPU scanout/cursor path to
`g_virtio_gpu_sw_backend`. Command payloads are read through the Phase 1
descriptor helpers, and a small VirGL resource registry decides whether common
commands belong to VirGL or the software backend.

**Tech Stack:** C, GNU Make, virglrenderer API, fake virglrenderer unit fixture,
existing virtio-gpu descriptor helpers.

---

### Task 1: Phase 3 Backend Tests

**Files:**
- Create: `tests/fakes/virglrenderer.h`
- Create: `tests/virtio-gpu-virgl-test.c`
- Modify: `Makefile`

- [x] **Step 1: Add fake virglrenderer header**

Create `tests/fakes/virglrenderer.h` with the VirGL prototypes used by
`virtio-gpu-virgl.c`: capset, reset, context create/destroy, context
resource attach/detach, resource create/unref, backing attach/detach,
transfer read/write, and submit.

- [x] **Step 2: Add direct backend unit test target**

Add `make test-vgpu-virgl`, compiling `tests/virtio-gpu-virgl-test.c` with
`-DSEMU_FEATURE_VIRGL=1 -Itests/fakes`.

- [x] **Step 3: Cover command mapping**

The test must verify:

- `CTX_CREATE` calls `virgl_renderer_context_create()` and rejects
  `context_init`
- `CTX_DESTROY`, `CTX_ATTACH_RESOURCE`, and `CTX_DETACH_RESOURCE` call the
  corresponding virglrenderer APIs
- `RESOURCE_CREATE_3D` calls `virgl_renderer_resource_create()` and registers
  the resource id
- `RESOURCE_ATTACH_BACKING` copies backing entries from readable descriptor
  payload and passes host iovecs to virglrenderer
- `RESOURCE_DETACH_BACKING` and `RESOURCE_UNREF` detach/free backing and unref
  registered VirGL resources
- `TRANSFER_TO_HOST_3D` and `TRANSFER_FROM_HOST_3D` forward all box, offset,
  stride, level, and context fields
- `SUBMIT_3D` copies command bytes from payload offset
  `sizeof(struct virtio_gpu_cmd_submit)` and calls
  `virgl_renderer_submit_cmd(buf, ctx_id, size / 4)`
- common commands targeting non-VirGL resources still delegate to the software
  backend

Run:

```sh
make test-vgpu-virgl
```

Expected before implementation: FAIL because these handlers are still routed to
`VIRTIO_GPU_CMD_UNDEF` or the software delegate.

### Task 2: VirGL Resource Registry

**Files:**
- Modify: `virtio-gpu-virgl.c`

- [x] **Step 1: Add process-local resource registry**

Add a small linked list keyed by `resource_id`. It tracks only resources
created by `RESOURCE_CREATE_3D`, because Phase 4 will later extend scanout and
texture metadata.

- [x] **Step 2: Clear registry on reset**

`reset` must call `virgl_renderer_reset()`, clear the VirGL registry, then run
the software backend reset so existing 2D display cleanup remains intact.

### Task 3: Context and Resource Commands

**Files:**
- Modify: `virtio-gpu-virgl.c`

- [x] **Step 1: Implement context handlers**

Implement `CTX_CREATE`, `CTX_DESTROY`, `CTX_ATTACH_RESOURCE`, and
`CTX_DETACH_RESOURCE`. `VIRTIO_GPU_F_CONTEXT_INIT` is not advertised in this
phase, so non-zero `context_init` returns `VIRTIO_GPU_RESP_ERR_UNSPEC`.

- [x] **Step 2: Implement 3D resource create/unref**

Implement `RESOURCE_CREATE_3D` with duplicate and zero-id checks, call
`virgl_renderer_resource_create()`, then register the resource. Implement
VirGL `RESOURCE_UNREF` by detaching backing iovecs, freeing them, calling
`virgl_renderer_resource_unref()`, and removing the registry entry.

### Task 4: Backing and Transfer Commands

**Files:**
- Modify: `virtio-gpu-virgl.c`

- [x] **Step 1: Implement backing attach/detach**

For registered VirGL resources, `RESOURCE_ATTACH_BACKING` must validate
`nr_entries`, readable payload size, guest address bounds, and allocation
overflow before calling `virgl_renderer_resource_attach_iov()`.
`RESOURCE_DETACH_BACKING` must free any iovec array returned by
`virgl_renderer_resource_detach_iov()`.

- [x] **Step 2: Implement transfers**

Implement `TRANSFER_TO_HOST_2D`, `TRANSFER_TO_HOST_3D`, and
`TRANSFER_FROM_HOST_3D` through virglrenderer transfer APIs. Keep display flush
delegated to the software path until GL scanout exists.

### Task 5: Submit Command

**Files:**
- Modify: `virtio-gpu-virgl.c`

- [x] **Step 1: Implement payload copy and validation**

`SUBMIT_3D` must reject zero size, non-4-byte-aligned size, allocation failure,
short payload, and invalid descriptor copies. It copies the payload into a
host-owned buffer before calling virglrenderer, because guest memory must not be
concurrently mutable while virglrenderer decodes the command stream.

### Task 6: Fake Build Gate Maintenance

**Files:**
- Modify: `tests/vgpu-virgl-backend-build-test.sh`

- [x] **Step 1: Extend fake virglrenderer**

The fake pkg-config build test must provide stubs for every virglrenderer
symbol referenced by `virtio-gpu-virgl.c`, so `make
test-vgpu-virgl-backend-build` keeps proving optional dependency wiring without
requiring host virglrenderer.

### Task 7: Verification and Commit

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

- default and fake-dependency tests pass
- default `semu` build still uses the software backend
- `make ENABLE_VIRGL=1 semu` either links on hosts with real dependencies or
  fails fast with the existing pkg-config dependency error
- no guest-visible VirGL feature bit is advertised until Phase 4
