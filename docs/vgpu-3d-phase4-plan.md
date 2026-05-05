# virtio-gpu 3D Phase 4 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans
> to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Enable the first visible VirGL path by initializing virglrenderer,
advertising VirGL/capsets, publishing GL scanout payloads, and rendering those
payloads through an SDL/OpenGL window.

**Architecture:** `virtio-gpu.c` exposes `VIRTIO_GPU_F_VIRGL` only when
`ENABLE_VIRGL=1`. `virtio-gpu-virgl.c` owns virglrenderer init, scanout binding,
and GL payload publication. `vgpu-display` carries either CPU snapshots or GL
scanout references. `window-sw.c` remains the single SDL backend, but switches
to an OpenGL window/context under `ENABLE_VIRGL=1`.

**Tech Stack:** C, GNU Make, SDL2, OpenGL via epoxy, virglrenderer, fake
virglrenderer unit tests.

---

### Task 1: Host Dependency Documentation

**Files:**
- Modify: `/home/mes/MesRepo/dev-docs/MesDevLog/semu-vgpu-3D/README.md`

- [x] **Step 1: Document Ubuntu dependencies**

Document `libvirglrenderer-dev` and `libepoxy-dev`, plus the pkg-config check:

```sh
sudo apt update
sudo apt install libvirglrenderer-dev libepoxy-dev
pkg-config --exists virglrenderer epoxy gl egl
```

### Task 2: Real VirGL Build Baseline

**Files:**
- Modify: `virtio-gpu-virgl.c`

- [x] **Step 1: Fix system header compatibility**

Avoid requiring a complete `struct virgl_box` definition from
`virglrenderer.h`; Ubuntu 24.04 exposes it as an opaque pointer. Use a local
same-layout box and cast only at the virglrenderer API boundary.

Run:

```sh
make ENABLE_VIRGL=1 semu
```

Expected: build/link succeeds with installed host dependencies.

### Task 3: Feature and Capset Advertisement

**Files:**
- Modify: `virtio-gpu.c`
- Modify: `virtio-gpu.h`
- Modify: `virtio-gpu-sw.c`
- Modify: `virtio-gpu-virgl.c`
- Test: `tests/virtio-gpu-virgl-test.c`

- [x] **Step 1: Add backend capset-count hook**

Expose `virtio_gpu_backend_get_num_capsets()` from the active backend. The
software backend returns `0`; the VirGL backend asks virglrenderer which of
`VIRTIO_GPU_CAPSET_VIRGL` and `VIRTIO_GPU_CAPSET_VIRGL2` are available.

- [x] **Step 2: Advertise VirGL**

When `SEMU_HAS(VIRGL)`, `DeviceFeaturesSel == 0` returns
`VIRTIO_GPU_F_EDID | VIRTIO_GPU_F_VIRGL`, and config `num_capsets` returns the
backend capset count. Do not advertise `RESOURCE_BLOB` or `CONTEXT_INIT`.

### Task 4: VirGL Renderer Lifecycle

**Files:**
- Create: `vgpu-gl.h`
- Modify: `virtio-gpu.h`
- Modify: `virtio-gpu.c`
- Modify: `virtio-gpu-sw.c`
- Modify: `virtio-gpu-virgl.c`
- Modify: `window-sw.c`
- Test: `tests/virtio-gpu-virgl-test.c`

- [x] **Step 1: Add backend init hook**

Add `init` to `struct virtio_gpu_cmd_backend`, call it from
`virtio_gpu_init()`, and keep the software backend no-op.

- [x] **Step 2: Initialize virglrenderer**

The VirGL backend calls `virgl_renderer_init(vgpu, VIRGL_RENDERER_THREAD_SYNC,
callbacks)`. The callbacks delegate GL context create/destroy/make-current to
window-owned functions declared in `vgpu-gl.h`. Fence callback remains no-op
for this synchronous response phase.

### Task 5: Display Payload Variant

**Files:**
- Modify: `vgpu-display.h`
- Modify: `vgpu-display.c`
- Modify: `virtio-gpu-sw.c`
- Test: `tests/virtio-gpu-virgl-test.c`

- [x] **Step 1: Add payload kind**

Add `VGPU_DISPLAY_PAYLOAD_CPU` and `VGPU_DISPLAY_PAYLOAD_GL_SCANOUT`. Existing
software snapshots set kind `CPU`; VirGL scanout payloads set kind
`GL_SCANOUT`.

- [x] **Step 2: Add GL payload fields**

Carry texture id, resource size, source rect, and `y_0_top`. The display bridge
still owns only the heap payload object; the OpenGL texture remains owned by
virglrenderer.

### Task 6: VirGL Scanout and Flush

**Files:**
- Modify: `virtio-gpu-virgl.c`
- Test: `tests/virtio-gpu-virgl-test.c`

- [x] **Step 1: Implement VirGL `SET_SCANOUT`**

For registered VirGL resources, call `virgl_renderer_resource_get_info()`, bind
scanout metadata in `vgpu->priv`, and publish a GL payload. Resource id `0`
clears the primary plane. Non-VirGL resources still delegate to the software
backend.

- [x] **Step 2: Implement VirGL `RESOURCE_FLUSH`**

For scanouts currently bound to the flushed VirGL resource, republish a GL
payload so the window can blit the latest texture. Non-VirGL resources still
delegate to the software backend.

### Task 7: SDL/OpenGL Window Path

**Files:**
- Modify: `window-sw.c`
- Modify: `Makefile`

- [x] **Step 1: Create OpenGL window mode**

Under `ENABLE_VIRGL=1`, create the SDL window with `SDL_WINDOW_OPENGL`, create
a display GL context, and do not create an SDL renderer for the primary plane.

- [x] **Step 2: Render GL scanouts**

When a `VGPU_DISPLAY_PAYLOAD_GL_SCANOUT` primary payload arrives, make the
display context current, attach the virgl texture to a framebuffer, blit it to
the default framebuffer, and swap the window.

### Task 8: Verification and Commit

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

- default `ENABLE_VIRGL=0` build and tests continue to pass
- `ENABLE_VIRGL=1` links against real host virglrenderer/epoxy/GL/EGL
- fake dependency build still proves optional linking without host packages
- guest-visible VirGL feature/capset advertisement is gated by
  `ENABLE_VIRGL=1`

Result on 2026-05-05: all commands above passed on the local host.
