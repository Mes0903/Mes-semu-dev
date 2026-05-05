# virtio-gpu 3D Phase 7 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Support multi-SG virtio-gpu control queue descriptor chains so VirGL
commands are no longer limited to request + one payload descriptor + response.

**Architecture:** Keep the existing backend command ABI (`struct virtq_desc *`)
but enlarge the frontend-collected descriptor view to the virtqueue maximum.
All existing payload helpers already consume a descriptor array plus a
`max_desc` count, so `SUBMIT_3D` and `RESOURCE_ATTACH_BACKING` can read payloads
split across many device-readable descriptors once the frontend parser stops
rejecting chains longer than three entries.

**Tech Stack:** C, GNU Make, virtio MMIO queue parser, existing virtio-gpu
descriptor copy helpers, focused queue-level unit tests.

---

### Task 1: Red Test for Multi-SG Queue Parsing

**Files:**
- Create: `tests/virtio-gpu-chain-test.c`
- Modify: `Makefile`

- [x] **Step 1: Add a queue-level multi-SG test**

Create a standalone test that includes `virtio-gpu.c` with a fake `SUBMIT_3D`
backend. The guest descriptor table must contain four descriptors:

- descriptor 0: `struct virtio_gpu_cmd_submit`
- descriptor 1: first half of the command stream
- descriptor 2: second half of the command stream
- descriptor 3: writable `struct virtio_gpu_ctrl_hdr` response

The fake backend copies `request->size` bytes from readable descriptors at
offset `sizeof(struct virtio_gpu_cmd_submit)` and records the words it saw.

- [x] **Step 2: Add `make test-vgpu-chain`**

Compile `tests/virtio-gpu-chain-test.c` directly, the same way the existing
frontend fence test compiles `virtio-gpu.c` into its harness.

- [x] **Step 3: Verify red**

Run:

```sh
make test-vgpu-chain
```

Expected before implementation: FAIL because the frontend rejects the
4-descriptor chain before the fake `SUBMIT_3D` backend can copy the payload.

### Task 2: Expand Descriptor Chain Capacity

**Files:**
- Modify: `virtio-gpu.h`
- Modify: `virtio-gpu.c`

- [x] **Step 1: Move queue maximum to the public header**

Define `VIRTIO_GPU_QUEUE_NUM_MAX` in `virtio-gpu.h` and make
`VIRTIO_GPU_MAX_DESC` equal to it. A valid descriptor chain cannot contain more
descriptors than the queue itself, so this is the natural frontend parser cap.

- [x] **Step 2: Update parser comments and rejection text**

Replace the old fixed-3 descriptor comment with the new contract:

- request is descriptor 0
- any number of device-readable payload descriptors may follow
- the first writable descriptor is the response
- chains that still have `NEXT` set after `VIRTIO_GPU_MAX_DESC` entries are
  malformed/cyclic and put the device into reset or return `ERR_UNSPEC` if a
  response descriptor was already visible

### Task 3: Update Direct Backend Tests for the Larger View

**Files:**
- Modify: `tests/virtio-gpu-virgl-test.c`

- [x] **Step 1: Resize local descriptor arrays**

Change direct backend tests from `struct virtq_desc desc[3]` to
`struct virtq_desc desc[VIRTIO_GPU_MAX_DESC]`. The direct handler tests pass
`VIRTIO_GPU_MAX_DESC` to helper functions, so their local arrays must be large
enough once the production max grows.

- [x] **Step 2: Keep helper initialization zeroing the whole array**

Update `init_desc_no_payload()` and `init_desc_with_payload()` to `memset()` the
full `VIRTIO_GPU_MAX_DESC` array.

### Task 4: Verification and Commit

Run:

```sh
bash -n tests/vgpu-virgl-backend-build-test.sh
make test-vgpu-chain
make test-vgpu-fence
make test-vgpu-desc
make test-vgpu-virgl
make test-vgpu-virgl-gate
make test-vgpu-virgl-backend-build
make -j4 semu
make ENABLE_VIRGL=1 -j4 semu
clang-format-20 --dry-run --Werror virtio-gpu.c virtio-gpu.h tests/virtio-gpu-chain-test.c tests/virtio-gpu-virgl-test.c
git diff --check
```

Expected:

- a 4-descriptor `SUBMIT_3D` chain reaches the backend and copies split command
  payload bytes correctly
- existing fence, descriptor, VirGL backend, default, and `ENABLE_VIRGL=1`
  builds still pass
- malformed/cyclic chains still cannot run unbounded because the parser cap is
  the virtqueue maximum
