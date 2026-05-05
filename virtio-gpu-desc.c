#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "device.h"
#include "virtio-gpu.h"
#include "virtio.h"

void *virtio_gpu_mem_guest_to_host(virtio_gpu_state_t *vgpu,
                                   uint32_t addr,
                                   uint32_t size)
{
    if (addr >= RAM_SIZE || size > RAM_SIZE ||
        (uint64_t) addr + size > RAM_SIZE) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): guest address 0x%x size 0x%x out of bounds\n",
                __func__, addr, size);
        return NULL;
    }
    return (void *) ((uintptr_t) vgpu->ram + addr);
}

bool virtio_gpu_desc_readable_size(const struct virtq_desc *vq_desc,
                                   int max_desc,
                                   size_t *size)
{
    if (!vq_desc || !size || max_desc < 0)
        return false;

    *size = 0;
    for (int i = 0; i < max_desc; i++) {
        if (vq_desc[i].flags & VIRTIO_DESC_F_WRITE)
            break;
        if (SIZE_MAX - *size < vq_desc[i].len)
            return false;
        *size += vq_desc[i].len;
    }

    return true;
}

enum virtio_gpu_desc_copy_result virtio_gpu_desc_copy_from_readable(
    virtio_gpu_state_t *vgpu,
    const struct virtq_desc *vq_desc,
    int max_desc,
    size_t offset,
    void *buf,
    size_t bytes)
{
    size_t done = 0;

    if (!vgpu || !vq_desc || (!buf && bytes) || max_desc < 0)
        return VIRTIO_GPU_DESC_COPY_INVALID;
    if (bytes == 0)
        return VIRTIO_GPU_DESC_COPY_OK;

    for (int i = 0; i < max_desc; i++) {
        if (vq_desc[i].flags & VIRTIO_DESC_F_WRITE)
            break;

        if (offset >= vq_desc[i].len) {
            offset -= vq_desc[i].len;
            continue;
        }

        uint64_t chunk_addr = vq_desc[i].addr + offset;
        if (vq_desc[i].addr > UINT32_MAX || chunk_addr > UINT32_MAX)
            return VIRTIO_GPU_DESC_COPY_INVALID;

        size_t chunk_avail = vq_desc[i].len - offset;
        size_t remaining = bytes - done;
        size_t chunk_len = (remaining < chunk_avail) ? remaining : chunk_avail;

        if (chunk_len > UINT32_MAX)
            return VIRTIO_GPU_DESC_COPY_INVALID;

        void *src = virtio_gpu_mem_guest_to_host(vgpu, (uint32_t) chunk_addr,
                                                 (uint32_t) chunk_len);
        if (!src)
            return VIRTIO_GPU_DESC_COPY_INVALID;

        memcpy((void *) ((uintptr_t) buf + done), src, chunk_len);
        done += chunk_len;
        offset = 0;

        if (done == bytes)
            return VIRTIO_GPU_DESC_COPY_OK;
    }

    return VIRTIO_GPU_DESC_COPY_SHORT;
}
