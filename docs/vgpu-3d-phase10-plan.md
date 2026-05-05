# virtio-gpu 3D Phase 10 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the VirGL manual smoke path into a usable live gate and close the
most obvious SDL/OpenGL context-current ownership gap before first real 3D
bring-up.

**Architecture:** Keep VirGL execution in the emulator thread and SDL event /
display draining on the main thread. The phase does not introduce a new GL
actor yet; it makes the existing callback/display split explicit, avoids
leaving SDL GL contexts current across idle periods, and documents the remaining
thread-affinity gate for the first live smoke run.

**Tech Stack:** C, SDL2/OpenGL, virglrenderer callbacks, Bash/Expect smoke
tests, existing fake VirGL unit tests.

---

### Task 1: Strengthen the Manual VirGL Smoke Script

**Files:**
- Modify: `.ci/test-virgl.sh`
- Modify: `tests/vgpu-virgl-image-test.sh`
- Modify: `README.md`

- [x] **Step 1: Add host dependency gates**

Check for a visible host display, `sdl2-config`, and the VirGL pkg-config
modules before booting semu:

```sh
pkg-config --exists virglrenderer epoxy gl egl
sdl2-config --version
```

- [x] **Step 2: Start guest X11 when needed**

After guest login, source `/root/local-env.sh` when it exists, start `Xorg :0`
if `/tmp/.X11-unix/X0` is absent, wait for the socket, and dump
`/tmp/xorg.log` on GLX failure.

- [x] **Step 3: Keep the test manually scoped**

Keep `.ci/test-virgl.sh` out of headless CI. Document that
`SEMU_VIRGL_REBOOT_TEST=1 .ci/test-virgl.sh` can be used as an optional live
reboot/reset check after the basic `glxinfo`/`glxgears` path passes.

### Task 2: Detach SDL GL Contexts Between Active Uses

**Files:**
- Modify: `window-sw.c`

- [x] **Step 1: Detach the display context after initialization**

After the initial clear/swap in `window_init_sw()`, call
`SDL_GL_MakeCurrent(scanout->window, NULL)` so the display context is not left
current on the main thread before the emulator thread starts.

- [x] **Step 2: Detach after display-frame GL work**

In `sdl_scanout_apply_gl_frame()` and `sdl_scanout_render_gl()`, detach the
display context after framebuffer validation/blit work completes. This keeps
the display context current only while the main thread is actively consuming a
display command.

- [x] **Step 3: Detach after VirGL context creation**

In `vgpu_window_virgl_create_context()`, use the display context only to create
a shared VirGL context, then detach before returning. VirGL will still make the
returned context current through the normal `make_current` callback.

### Task 3: Update 3D Status Documentation

**Files:**
- Modify: `/home/mes/MesRepo/dev-docs/MesDevLog/semu-vgpu-3D/README.md`

- [x] **Step 1: Mark completed implementation phases**

Record that Phase 1 through Phase 9 have implementation commits on `vgpu-3D`.

- [x] **Step 2: Promote live smoke and thread-affinity to remaining gates**

Keep blob/Venus/security/headless CI as future work, but make clear that the
first mergeable VirGL/OpenGL PR still needs a visible host smoke result and a
decision on whether the current callback/display threading model is acceptable.

### Task 4: Verification

Run:

```sh
bash -n .ci/test-virgl.sh
bash -n tests/vgpu-virgl-image-test.sh
make test-vgpu-virgl-image
make test-vgpu-virgl
make test-vgpu-virgl-gate
make test-vgpu-virgl-backend-build
make -j4 semu
make ENABLE_VIRGL=1 -j4 semu
git diff --check
```

Expected:

- the manual VirGL script is syntax-valid and self-gates host/guest display
  prerequisites
- fake VirGL tests still pass after GL context detach changes
- default and `ENABLE_VIRGL=1` builds still compile
- live rendering is still explicitly manual and not claimed until a visible host
  run has been executed

Result on 2026-05-05: `bash -n .ci/test-virgl.sh`,
`bash -n tests/vgpu-virgl-image-test.sh`, `make test-vgpu-virgl-image`,
`make test-vgpu-virgl`, `make test-vgpu-virgl-gate`,
`make test-vgpu-virgl-backend-build`, default `make -j4 semu`,
`make ENABLE_VIRGL=1 -j4 semu`, `clang-format-20 --dry-run --Werror
window-sw.c`, and `git diff --check` passed locally. The visible
`.ci/test-virgl.sh` smoke remains manual because it requires a host display and
a visible SDL/OpenGL window.
