# virtio-gpu 3D Phase 3a Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans
> to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Add a virglrenderer backend skeleton that owns capset command handling
without advertising VirGL to the guest yet.

**Architecture:** The default software backend remains unchanged when
`ENABLE_VIRGL=0`. When `ENABLE_VIRGL=1`, semu links a virgl backend wrapper that
delegates existing 2D/cursor commands to the software backend and handles
`GET_CAPSET_INFO` / `GET_CAPSET` through virglrenderer. Runtime feature
advertisement stays off until the resource/context/submit path exists.

**Tech Stack:** C, GNU Make, `pkg-config`, virglrenderer C API, shell build
fixtures.

---

### Task 1: VirGL Backend Build Fixture

**Files:**
- Modify: `Makefile`
- Create: `tests/vgpu-virgl-backend-build-test.sh`

- [ ] **Step 1: Add the failing backend-link test**

Create a shell test that builds `semu` with `ENABLE_VIRGL=1` using a temporary
fake `pkg-config`, fake `virglrenderer.h`, and fake static virglrenderer
library. The test then checks that `semu` contains the marker string:

```text
SEMU_VIRGL_BACKEND_LINKED
```

Run:

```sh
make test-vgpu-virgl-backend-build
```

Expected before backend implementation: FAIL because the marker is absent.

- [ ] **Step 2: Add the Makefile target**

Add:

```make
.PHONY: test-vgpu-virgl-backend-build
test-vgpu-virgl-backend-build:
	$(Q)bash tests/vgpu-virgl-backend-build-test.sh
```

### Task 2: Backend Symbol Split

**Files:**
- Modify: `Makefile`
- Modify: `virtio-gpu.h`
- Modify: `virtio-gpu-sw.c`

- [ ] **Step 1: Build both backend objects when VirGL is enabled**

When `ENABLE_VIRGL=1`, compile `virtio-gpu-sw.o` and `virtio-gpu-virgl.o`.
When it is off, continue compiling only the software backend object.

- [ ] **Step 2: Rename the software backend export under VirGL**

In `virtio-gpu-sw.c`, export the software backend as
`g_virtio_gpu_sw_backend` when `SEMU_HAS(VIRGL)` is true. Otherwise, keep the
existing guest-visible backend symbol name `g_virtio_gpu_backend`.

### Task 3: VirGL Capset Skeleton

**Files:**
- Create: `virtio-gpu-virgl.c`

- [ ] **Step 1: Add the backend marker and delegation wrappers**

Create `virtio-gpu-virgl.c` with the marker string and wrapper functions that
delegate the existing 2D/cursor command handlers to `g_virtio_gpu_sw_backend`.

- [ ] **Step 2: Add capset handlers**

Implement:

```c
GET_CAPSET_INFO
GET_CAPSET
```

using:

```c
virgl_renderer_get_cap_set()
virgl_renderer_fill_caps()
```

Responses must use semu's existing descriptor helpers and must reject invalid
capset IDs, invalid versions, short response buffers, and invalid guest memory
without leaving stale output lengths.

- [ ] **Step 3: Keep 3D resource/context/submit commands undefined**

Do not implement `CTX_CREATE`, `RESOURCE_CREATE_3D`, `SUBMIT_3D`, or transfer
3D commands in this task. They remain explicit unsupported commands until the
next phase.

### Task 4: Verification

Run:

```sh
bash -n tests/vgpu-virgl-backend-build-test.sh
make test-vgpu-virgl-backend-build
make test-vgpu-virgl-gate
make test-vgpu-desc
make -j4 semu
git diff --check
```

Expected:

- fake `ENABLE_VIRGL=1` build links the virgl backend marker
- existing Phase 1/2 tests pass
- default software backend build still succeeds
- `ENABLE_VIRGL=1` without real host packages still fails at the Phase 2
  dependency gate on this host
