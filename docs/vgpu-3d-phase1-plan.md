# semu virtio-gpu 3D Phase 1 plan

> Date: 2026-05-05
> Branch: `vgpu-3D`
> Scope: wire/API cleanup required before wiring virglrenderer.

## Goal

Phase 1 prepares the current 2D virtio-gpu frontend for the 3D path without
advertising VirGL yet. The important outcome is that command payload parsing is
no longer hard-coded to `vq_desc[1]`, and the guest-visible wire structs and
constants match the local Linux UAPI before a virgl backend starts consuming
them.

## Non-goals

- Do not advertise `VIRTIO_GPU_F_VIRGL`.
- Do not add virglrenderer, OpenGL, or `ENABLE_VIRGL`.
- Do not change the current SDL/software display path.
- Do not implement generic unbounded virtqueue descriptor chains yet.

## Implementation Steps

1. Add a small unit test for virtio-gpu readable descriptor payload handling.
   The test must cover request-plus-data descriptors, data packed after the
   fixed request struct, short payloads, writable response descriptors ending
   the readable stream, and high-address rejection.

2. Move guest-RAM descriptor payload reading into a small helper owned by the
   virtio-gpu frontend. The helper treats the device-readable descriptors before
   the first device-writable descriptor as one byte stream.

3. Keep the existing fixed `VIRTIO_GPU_MAX_DESC == 3` parser for now, but make
   handlers consume payload through the helper. This preserves today's 2D
   behavior while giving `SUBMIT_3D` and `RESOURCE_ATTACH_BACKING` the same
   parsing model QEMU/Linux expect.

4. Align wire/API definitions:
   - expose virtio-gpu feature bit masks in `virtio-gpu.h`
   - add capset IDs from the local Linux UAPI
   - change `struct virtio_gpu_cmd_submit` from `num_in_fences` to `padding`

5. Convert the software backend's `RESOURCE_ATTACH_BACKING` handler to read
   each `virtio_gpu_mem_entry` through the new descriptor payload helper.

6. Verify:
   - `make test-vgpu-desc`
   - `make -j4 semu`
   - existing 2D smoke test if local guest artifacts are available

## Open Issues After Phase 1

- Longer descriptor chains are still rejected by the existing fixed parser.
  Phase 1 only removes direct handler assumptions about `vq_desc[1]`.
- `GET_CAPSET_INFO` and `GET_CAPSET` still return unsupported data until the
  virgl backend owns capset discovery.
- `VIRTIO_GPU_F_CONTEXT_INIT`, blob resources, Venus, and DRM capsets remain
  gated until their backend paths exist.
