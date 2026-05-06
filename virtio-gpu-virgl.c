#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

#include <virglrenderer.h>

#include "device.h"
#include "vgpu-display.h"
#include "vgpu-gl.h"
#include "vgpu-renderer.h"
#include "virtio-gpu.h"
#include "virtio.h"

#define PRIV(x) ((virtio_gpu_data_t *) x->priv)

__attribute__((used)) const char g_vgpu_virgl_backend_link_marker[] =
    "SEMU_VIRGL_BACKEND_LINKED";

struct vgpu_virgl_resource {
    uint32_t resource_id;
    bool backing_attached;
    struct vgpu_virgl_resource *next;
};

struct vgpu_virgl_box {
    uint32_t x, y, z;
    uint32_t w, h, d;
};

struct vgpu_virgl_fence_state {
    uint64_t last_ctx0_fence;
    uint32_t last_context_ctx_id;
    uint32_t last_context_ring_idx;
    uint64_t last_context_fence;
};

static struct vgpu_virgl_resource *g_vgpu_virgl_resources;
static struct vgpu_virgl_fence_state g_vgpu_virgl_fences;

static uint32_t vgpu_virgl_count_capsets(void)
{
    uint32_t count = 0;
    uint32_t max_version = 0;
    uint32_t max_size = 0;

    virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VIRGL, &max_version,
                               &max_size);
    if (max_version && max_size)
        count++;

    max_version = 0;
    max_size = 0;
    virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VIRGL2, &max_version,
                               &max_size);
    if (max_version && max_size)
        count++;

    return count;
}

static void vgpu_virgl_publish_capsets(virtio_gpu_state_t *vgpu)
{
    virtio_gpu_set_num_capsets(vgpu, vgpu_virgl_count_capsets());
}

static void vgpu_virgl_write_fence(void *cookie, uint32_t fence)
{
    g_vgpu_virgl_fences.last_ctx0_fence = fence;
    if (!cookie)
        return;

    struct vgpu_renderer_completion completion = {
        .type = VGPU_RENDERER_DONE_FENCE,
        .token = {.generation = virtio_gpu_ctrl_generation(cookie)},
        .context_fence = false,
        .fence_id = fence,
    };
    if (!vgpu_renderer_complete(&completion))
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): dropped renderer fence completion %" PRIu32 "\n",
                __func__, fence);
}

static void vgpu_virgl_write_context_fence(void *cookie,
                                           uint32_t ctx_id,
                                           uint32_t ring_idx,
                                           uint64_t fence_id)
{
    g_vgpu_virgl_fences.last_context_ctx_id = ctx_id;
    g_vgpu_virgl_fences.last_context_ring_idx = ring_idx;
    g_vgpu_virgl_fences.last_context_fence = fence_id;
    if (!cookie)
        return;

    struct vgpu_renderer_completion completion = {
        .type = VGPU_RENDERER_DONE_FENCE,
        .token = {.generation = virtio_gpu_ctrl_generation(cookie)},
        .context_fence = true,
        .ctx_id = ctx_id,
        .ring_idx = ring_idx,
        .fence_id = fence_id,
    };
    if (!vgpu_renderer_complete(&completion))
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): dropped renderer context fence completion %" PRIu64
                "\n",
                __func__, fence_id);
}

static virgl_renderer_gl_context vgpu_virgl_create_context(
    void *cookie,
    int scanout_idx,
    struct virgl_renderer_gl_ctx_param *param)
{
    (void) cookie;
    return vgpu_window_virgl_create_context(scanout_idx, param);
}

static void vgpu_virgl_destroy_context(void *cookie,
                                       virgl_renderer_gl_context ctx)
{
    (void) cookie;
    vgpu_window_virgl_destroy_context(ctx);
}

static int vgpu_virgl_make_current(void *cookie,
                                   int scanout_idx,
                                   virgl_renderer_gl_context ctx)
{
    (void) cookie;
    return vgpu_window_virgl_make_current(scanout_idx, ctx);
}

static struct virgl_renderer_callbacks g_vgpu_virgl_callbacks = {
    .version = VIRGL_RENDERER_CALLBACKS_VERSION,
    .write_fence = vgpu_virgl_write_fence,
    .create_gl_context = vgpu_virgl_create_context,
    .destroy_gl_context = vgpu_virgl_destroy_context,
    .make_current = vgpu_virgl_make_current,
    .write_context_fence = vgpu_virgl_write_context_fence,
};

static void vgpu_virgl_init(virtio_gpu_state_t *vgpu)
{
    vgpu_gl_lock();
    int ret = virgl_renderer_init(vgpu, VIRGL_RENDERER_THREAD_SYNC,
                                  &g_vgpu_virgl_callbacks);
    if (ret) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): failed to initialize virglrenderer (%d)\n",
                __func__, ret);
        vgpu_gl_unlock();
        exit(EXIT_FAILURE);
    }

    vgpu_virgl_publish_capsets(vgpu);

    /* virglrenderer initializes ctx0 on the caller's thread, but semu runs
     * guest GPU commands on the emulator thread once the SDL main loop starts.
     * Drop the init-thread current context; thread_enter will rebind ctx0 on
     * the thread that actually executes virglrenderer commands.
     */
    vgpu_window_virgl_make_current(0, NULL);
    vgpu_gl_unlock();
}

static void vgpu_virgl_thread_enter(virtio_gpu_state_t *vgpu)
{
    (void) vgpu;
    vgpu_gl_lock();
    virgl_renderer_force_ctx_0();
    vgpu_gl_unlock();
}

static void vgpu_virgl_command_enter(virtio_gpu_state_t *vgpu)
{
    (void) vgpu;
    vgpu_gl_lock();
}

static void vgpu_virgl_command_leave(virtio_gpu_state_t *vgpu)
{
    (void) vgpu;
    vgpu_gl_unlock();
}

static void vgpu_virgl_poll(virtio_gpu_state_t *vgpu)
{
    (void) vgpu;
    vgpu_gl_lock();
    virgl_renderer_poll();
    vgpu_gl_unlock();
}

static void vgpu_virgl_delegate_reset(virtio_gpu_state_t *vgpu)
{
    vgpu_gl_lock();
    memset(&g_vgpu_virgl_fences, 0, sizeof(g_vgpu_virgl_fences));
    /* Clear display generations before virglrenderer destroys GL resources.
     * This makes any queued GL scanout payload stale while the texture IDs it
     * names are still valid.
     */
    if (g_virtio_gpu_sw_backend.reset)
        g_virtio_gpu_sw_backend.reset(vgpu);
    virgl_renderer_reset();
    vgpu_virgl_publish_capsets(vgpu);
    /* The renderer destroys all contexts/resources on reset. Mirror that in
     * semu's ownership registry after the renderer reset completes.
     */
    while (g_vgpu_virgl_resources) {
        struct vgpu_virgl_resource *res = g_vgpu_virgl_resources;
        g_vgpu_virgl_resources = res->next;
        free(res);
    }
    vgpu_gl_unlock();
}

#define VGPU_VIRGL_DELEGATE_CMD(name)                                         \
    static void vgpu_virgl_delegate_##name(                                   \
        virtio_gpu_state_t *vgpu, struct virtq_desc *vq_desc, uint32_t *plen) \
    {                                                                         \
        g_virtio_gpu_sw_backend.name(vgpu, vq_desc, plen);                    \
    }

VGPU_VIRGL_DELEGATE_CMD(get_display_info)
VGPU_VIRGL_DELEGATE_CMD(resource_create_2d)
VGPU_VIRGL_DELEGATE_CMD(get_edid)
VGPU_VIRGL_DELEGATE_CMD(update_cursor)
VGPU_VIRGL_DELEGATE_CMD(move_cursor)

#define VGPU_VIRGL_BACKING_ENTRY_PAGE_SIZE 4096U
#define VGPU_VIRGL_MAX_BACKING_ENTRIES \
    (RAM_SIZE / VGPU_VIRGL_BACKING_ENTRY_PAGE_SIZE + 1U)

static struct vgpu_virgl_resource *vgpu_virgl_find_resource(
    uint32_t resource_id)
{
    for (struct vgpu_virgl_resource *res = g_vgpu_virgl_resources; res;
         res = res->next) {
        if (res->resource_id == resource_id)
            return res;
    }

    return NULL;
}

static void vgpu_virgl_insert_resource(struct vgpu_virgl_resource *res)
{
    res->next = g_vgpu_virgl_resources;
    g_vgpu_virgl_resources = res;
}

static void vgpu_virgl_remove_resource(uint32_t resource_id)
{
    struct vgpu_virgl_resource **cursor = &g_vgpu_virgl_resources;
    while (*cursor) {
        struct vgpu_virgl_resource *res = *cursor;
        if (res->resource_id == resource_id) {
            *cursor = res->next;
            free(res);
            return;
        }
        cursor = &res->next;
    }
}

static void vgpu_virgl_clear_resource_scanouts(virtio_gpu_state_t *vgpu,
                                               uint32_t resource_id)
{
    for (uint32_t i = 0; i < PRIV(vgpu)->num_scanouts; i++) {
        struct virtio_gpu_scanout_info *scanout = &PRIV(vgpu)->scanouts[i];
        if (!scanout->enabled || scanout->primary_resource_id != resource_id)
            continue;

        scanout->primary_resource_id = 0;
        scanout->src_x = 0;
        scanout->src_y = 0;
        scanout->src_w = 0;
        scanout->src_h = 0;
        vgpu_display_publish_primary_clear(i);
    }
}

static const struct virtq_desc *vgpu_virgl_get_response_desc(
    struct virtq_desc *vq_desc,
    size_t response_size,
    uint32_t *plen)
{
    int resp_idx = virtio_gpu_get_response_desc(vq_desc, VIRTIO_GPU_MAX_DESC,
                                                response_size);
    if (resp_idx >= 0)
        return &vq_desc[resp_idx];

    *plen = 0;
    return NULL;
}

static void vgpu_virgl_write_response(virtio_gpu_state_t *vgpu,
                                      const struct virtio_gpu_ctrl_hdr *request,
                                      const struct virtq_desc *response_desc,
                                      uint32_t type,
                                      uint32_t *plen)
{
    if (type == VIRTIO_GPU_RESP_OK_NODATA &&
        (request->flags & VIRTIO_GPU_FLAG_FENCE)) {
        uint32_t generation = virtio_gpu_ctrl_generation(vgpu);
        bool deferred = virtio_gpu_defer_ctrl_response(
            vgpu, request, response_desc, type, generation);

        int ret;
        if (request->flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) {
            ret = virgl_renderer_context_create_fence(
                request->ctx_id, VIRGL_RENDERER_FENCE_FLAG_MERGEABLE,
                request->ring_idx, request->fence_id);
        } else {
            ret = virgl_renderer_create_fence(
                (int) (uint32_t) request->fence_id, 0);
        }

        if (ret) {
            if (deferred)
                virtio_gpu_cancel_ctrl_response(vgpu, generation, request);
            fprintf(stderr,
                    VIRTIO_GPU_LOG_PREFIX
                    "%s(): failed to create renderer fence %" PRIu64
                    " for cmd 0x%x (%d)\n",
                    __func__, request->fence_id, request->type, ret);
            *plen = virtio_gpu_write_ctrl_response(vgpu, request, response_desc,
                                                   VIRTIO_GPU_RESP_ERR_UNSPEC);
            return;
        }

        if (deferred) {
            *plen = VIRTIO_GPU_RESPONSE_DEFERRED;
            return;
        }

        *plen =
            virtio_gpu_write_ctrl_response(vgpu, request, response_desc, type);
        if (*plen) {
            virgl_renderer_poll();
        }
        return;
    }

    *plen = virtio_gpu_write_ctrl_response(vgpu, request, response_desc, type);
}

static struct vgpu_virgl_box vgpu_virgl_box_from_virtio(
    const struct virtio_gpu_box *box)
{
    return (struct vgpu_virgl_box) {
        .x = box->x,
        .y = box->y,
        .z = box->z,
        .w = box->w,
        .h = box->h,
        .d = box->d,
    };
}

static struct vgpu_virgl_box vgpu_virgl_box_from_rect(
    const struct virtio_gpu_rect *rect)
{
    return (struct vgpu_virgl_box) {
        .x = rect->x,
        .y = rect->y,
        .z = 0,
        .w = rect->width,
        .h = rect->height,
        .d = 1,
    };
}

static int vgpu_virgl_detach_iov(uint32_t resource_id)
{
    struct iovec *iov = NULL;
    int num_iovs = 0;
    virgl_renderer_resource_detach_iov(resource_id, &iov, &num_iovs);
    free(iov);
    return num_iovs;
}

static void vgpu_virgl_cmd_ctx_create_handler(virtio_gpu_state_t *vgpu,
                                              struct virtq_desc *vq_desc,
                                              uint32_t *plen)
{
    const struct virtq_desc *response_desc = vgpu_virgl_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    const struct virtio_gpu_ctx_create *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_ctx_create));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (request->nlen > sizeof(request->debug_name)) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER, plen);
        return;
    }

    if (request->context_init) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_UNSPEC, plen);
        return;
    }
    int ret = virgl_renderer_context_create(request->hdr.ctx_id, request->nlen,
                                            request->debug_name);
    vgpu_virgl_write_response(
        vgpu, &request->hdr, response_desc,
        ret ? VIRTIO_GPU_RESP_ERR_UNSPEC : VIRTIO_GPU_RESP_OK_NODATA, plen);
}

static void vgpu_virgl_cmd_ctx_destroy_handler(virtio_gpu_state_t *vgpu,
                                               struct virtq_desc *vq_desc,
                                               uint32_t *plen)
{
    const struct virtq_desc *response_desc = vgpu_virgl_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    const struct virtio_gpu_ctx_destroy *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_ctx_destroy));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }
    virgl_renderer_context_destroy(request->hdr.ctx_id);
    vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                              VIRTIO_GPU_RESP_OK_NODATA, plen);
}

static void vgpu_virgl_cmd_ctx_attach_resource_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    const struct virtq_desc *response_desc = vgpu_virgl_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    const struct virtio_gpu_ctx_resource *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_ctx_resource));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }
    virgl_renderer_ctx_attach_resource(request->hdr.ctx_id,
                                       request->resource_id);
    vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                              VIRTIO_GPU_RESP_OK_NODATA, plen);
}

static void vgpu_virgl_cmd_ctx_detach_resource_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    const struct virtq_desc *response_desc = vgpu_virgl_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    const struct virtio_gpu_ctx_resource *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_ctx_resource));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }
    virgl_renderer_ctx_detach_resource(request->hdr.ctx_id,
                                       request->resource_id);
    vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                              VIRTIO_GPU_RESP_OK_NODATA, plen);
}

static void vgpu_virgl_cmd_resource_create_3d_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    const struct virtq_desc *response_desc = vgpu_virgl_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    const struct virtio_gpu_resource_create_3d *request =
        virtio_gpu_get_request(vgpu, vq_desc,
                               sizeof(struct virtio_gpu_resource_create_3d));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (request->resource_id == 0 ||
        vgpu_virgl_find_resource(request->resource_id)) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID,
                                  plen);
        return;
    }

    struct vgpu_virgl_resource *res = calloc(1, sizeof(*res));
    if (!res) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY, plen);
        return;
    }

    struct virgl_renderer_resource_create_args args = {
        .handle = request->resource_id,
        .target = request->target,
        .format = request->format,
        .bind = request->bind,
        .width = request->width,
        .height = request->height,
        .depth = request->depth,
        .array_size = request->array_size,
        .last_level = request->last_level,
        .nr_samples = request->nr_samples,
        .flags = request->flags,
    };
    int ret = virgl_renderer_resource_create(&args, NULL, 0);
    if (ret) {
        free(res);
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_UNSPEC, plen);
        return;
    }

    res->resource_id = request->resource_id;
    vgpu_virgl_insert_resource(res);
    vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                              VIRTIO_GPU_RESP_OK_NODATA, plen);
}

static void vgpu_virgl_cmd_resource_unref_handler(virtio_gpu_state_t *vgpu,
                                                  struct virtq_desc *vq_desc,
                                                  uint32_t *plen)
{
    const struct virtio_gpu_res_unref *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_res_unref));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct vgpu_virgl_resource *res =
        vgpu_virgl_find_resource(request->resource_id);
    if (!res) {
        g_virtio_gpu_sw_backend.resource_unref(vgpu, vq_desc, plen);
        return;
    }

    const struct virtq_desc *response_desc = vgpu_virgl_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    vgpu_virgl_clear_resource_scanouts(vgpu, request->resource_id);
    if (res->backing_attached)
        vgpu_virgl_detach_iov(request->resource_id);
    virgl_renderer_resource_unref(request->resource_id);
    vgpu_virgl_remove_resource(request->resource_id);
    vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                              VIRTIO_GPU_RESP_OK_NODATA, plen);
}

static bool vgpu_virgl_validate_backing_payload(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    const struct virtio_gpu_res_attach_backing *request,
    const struct virtq_desc *response_desc,
    uint32_t *plen)
{
    if (request->nr_entries == 0 ||
        request->nr_entries > VGPU_VIRGL_MAX_BACKING_ENTRIES ||
        request->nr_entries > INT32_MAX) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER, plen);
        return false;
    }

    size_t nr_entries = request->nr_entries;
    if (nr_entries > SIZE_MAX / sizeof(struct virtio_gpu_mem_entry)) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER, plen);
        return false;
    }

    size_t entries_size = sizeof(struct virtio_gpu_mem_entry) * nr_entries;
    size_t readable_size = 0;
    if (!virtio_gpu_desc_readable_size(vq_desc, VIRTIO_GPU_MAX_DESC,
                                       &readable_size)) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return false;
    }

    if (readable_size < sizeof(*request) ||
        readable_size - sizeof(*request) < entries_size) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER, plen);
        return false;
    }

    return true;
}

static void vgpu_virgl_cmd_resource_attach_backing_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    const struct virtio_gpu_res_attach_backing *request =
        virtio_gpu_get_request(vgpu, vq_desc,
                               sizeof(struct virtio_gpu_res_attach_backing));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct vgpu_virgl_resource *res =
        vgpu_virgl_find_resource(request->resource_id);
    if (!res) {
        g_virtio_gpu_sw_backend.resource_attach_backing(vgpu, vq_desc, plen);
        return;
    }

    const struct virtq_desc *response_desc = vgpu_virgl_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    if (res->backing_attached) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_UNSPEC, plen);
        return;
    }

    if (!vgpu_virgl_validate_backing_payload(vgpu, vq_desc, request,
                                             response_desc, plen))
        return;

    size_t nr_entries = request->nr_entries;
    struct iovec *iov = calloc(nr_entries, sizeof(*iov));
    if (!iov) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY, plen);
        return;
    }

    for (size_t i = 0; i < nr_entries; i++) {
        struct virtio_gpu_mem_entry entry = {0};
        size_t entry_offset =
            sizeof(*request) + i * sizeof(struct virtio_gpu_mem_entry);
        enum virtio_gpu_desc_copy_result copy_result =
            virtio_gpu_desc_copy_from_readable(
                vgpu, vq_desc, VIRTIO_GPU_MAX_DESC, entry_offset, &entry,
                sizeof(entry));

        if (copy_result != VIRTIO_GPU_DESC_COPY_OK) {
            free(iov);
            if (copy_result == VIRTIO_GPU_DESC_COPY_INVALID) {
                virtio_gpu_set_fail(vgpu);
                *plen = 0;
            } else {
                vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                          VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER,
                                          plen);
            }
            return;
        }

        if (entry.addr > UINT32_MAX || entry.length == 0) {
            free(iov);
            vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                      VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER,
                                      plen);
            return;
        }

        iov[i].iov_base = virtio_gpu_mem_guest_to_host(
            vgpu, (uint32_t) entry.addr, entry.length);
        iov[i].iov_len = entry.length;
        if (!iov[i].iov_base) {
            free(iov);
            vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                      VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER,
                                      plen);
            return;
        }
    }
    int ret = virgl_renderer_resource_attach_iov(request->resource_id, iov,
                                                 (int) nr_entries);
    if (ret) {
        free(iov);
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_UNSPEC, plen);
        return;
    }

    res->backing_attached = true;
    vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                              VIRTIO_GPU_RESP_OK_NODATA, plen);
}

static void vgpu_virgl_cmd_resource_detach_backing_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    const struct virtio_gpu_res_detach_backing *request =
        virtio_gpu_get_request(vgpu, vq_desc,
                               sizeof(struct virtio_gpu_res_detach_backing));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct vgpu_virgl_resource *res =
        vgpu_virgl_find_resource(request->resource_id);
    if (!res) {
        g_virtio_gpu_sw_backend.resource_detach_backing(vgpu, vq_desc, plen);
        return;
    }

    const struct virtq_desc *response_desc = vgpu_virgl_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    if (!res->backing_attached) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_UNSPEC, plen);
        return;
    }

    vgpu_virgl_detach_iov(request->resource_id);
    res->backing_attached = false;
    vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                              VIRTIO_GPU_RESP_OK_NODATA, plen);
}

static void vgpu_virgl_cmd_transfer_to_host_2d_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    const struct virtio_gpu_trans_to_host_2d *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_trans_to_host_2d));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (!vgpu_virgl_find_resource(request->resource_id)) {
        g_virtio_gpu_sw_backend.transfer_to_host_2d(vgpu, vq_desc, plen);
        return;
    }

    const struct virtq_desc *response_desc = vgpu_virgl_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    struct vgpu_virgl_box box = vgpu_virgl_box_from_rect(&request->r);
    int ret = virgl_renderer_transfer_write_iov(request->resource_id, 0, 0, 0,
                                                0, (struct virgl_box *) &box,
                                                request->offset, NULL, 0);
    vgpu_virgl_write_response(
        vgpu, &request->hdr, response_desc,
        ret ? VIRTIO_GPU_RESP_ERR_UNSPEC : VIRTIO_GPU_RESP_OK_NODATA, plen);
}

static void vgpu_virgl_cmd_transfer_to_host_3d_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    const struct virtio_gpu_transfer_host_3d *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_transfer_host_3d));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    const struct virtq_desc *response_desc = vgpu_virgl_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    if (!vgpu_virgl_find_resource(request->resource_id)) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID,
                                  plen);
        return;
    }

    if (request->level > INT32_MAX) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER, plen);
        return;
    }

    struct vgpu_virgl_box box = vgpu_virgl_box_from_virtio(&request->box);
    int ret = virgl_renderer_transfer_write_iov(
        request->resource_id, request->hdr.ctx_id, (int) request->level,
        request->stride, request->layer_stride, (struct virgl_box *) &box,
        request->offset, NULL, 0);
    vgpu_virgl_write_response(
        vgpu, &request->hdr, response_desc,
        ret ? VIRTIO_GPU_RESP_ERR_UNSPEC : VIRTIO_GPU_RESP_OK_NODATA, plen);
}

static void vgpu_virgl_cmd_transfer_from_host_3d_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    const struct virtio_gpu_transfer_host_3d *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_transfer_host_3d));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    const struct virtq_desc *response_desc = vgpu_virgl_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    if (!vgpu_virgl_find_resource(request->resource_id)) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID,
                                  plen);
        return;
    }

    struct vgpu_virgl_box box = vgpu_virgl_box_from_virtio(&request->box);
    int ret = virgl_renderer_transfer_read_iov(
        request->resource_id, request->hdr.ctx_id, request->level,
        request->stride, request->layer_stride, (struct virgl_box *) &box,
        request->offset, NULL, 0);
    vgpu_virgl_write_response(
        vgpu, &request->hdr, response_desc,
        ret ? VIRTIO_GPU_RESP_ERR_UNSPEC : VIRTIO_GPU_RESP_OK_NODATA, plen);
}

static struct virtio_gpu_scanout_info *vgpu_virgl_get_scanout(
    virtio_gpu_state_t *vgpu,
    uint32_t scanout_id)
{
    if (scanout_id >= PRIV(vgpu)->num_scanouts)
        return NULL;

    struct virtio_gpu_scanout_info *scanout = &PRIV(vgpu)->scanouts[scanout_id];
    return scanout->enabled ? scanout : NULL;
}

static bool vgpu_virgl_rect_fits(uint32_t width,
                                 uint32_t height,
                                 const struct virtio_gpu_rect *rect)
{
    if (rect->width == 0 || rect->height == 0)
        return false;
    if (rect->x >= width || rect->y >= height)
        return false;

    return rect->width <= width - rect->x && rect->height <= height - rect->y;
}

static struct vgpu_display_payload *vgpu_virgl_create_gl_payload(
    uint32_t resource_id,
    const struct virtio_gpu_scanout_info *scanout,
    const struct virgl_renderer_resource_info *info)
{
    struct vgpu_display_payload *payload = calloc(1, sizeof(*payload));
    if (!payload)
        return NULL;

    payload->kind = VGPU_DISPLAY_PAYLOAD_GL_SCANOUT;
    payload->gl.texture_id = info->tex_id;
    payload->gl.width = info->width;
    payload->gl.height = info->height;
    payload->gl.src_x = scanout->src_x;
    payload->gl.src_y = scanout->src_y;
    payload->gl.src_w = scanout->src_w;
    payload->gl.src_h = scanout->src_h;
    payload->gl.y_0_top = (info->flags & VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP) != 0;

    (void) resource_id;
    return payload;
}

static bool vgpu_virgl_publish_gl_scanout(
    uint32_t scanout_id,
    uint32_t resource_id,
    const struct virtio_gpu_scanout_info *scanout)
{
    if (!vgpu_display_can_publish())
        return true;

    struct virgl_renderer_resource_info info = {0};
    int ret = virgl_renderer_resource_get_info((int) resource_id, &info);
    if (ret)
        return false;

    struct vgpu_display_payload *payload =
        vgpu_virgl_create_gl_payload(resource_id, scanout, &info);
    if (!payload)
        return false;
    virgl_renderer_force_ctx_0();
    vgpu_display_publish_primary_set(scanout_id, payload);
    return true;
}

static void vgpu_virgl_cmd_set_scanout_handler(virtio_gpu_state_t *vgpu,
                                               struct virtq_desc *vq_desc,
                                               uint32_t *plen)
{
    const struct virtio_gpu_set_scanout *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_set_scanout));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (request->resource_id != 0 &&
        !vgpu_virgl_find_resource(request->resource_id)) {
        g_virtio_gpu_sw_backend.set_scanout(vgpu, vq_desc, plen);
        return;
    }

    const struct virtq_desc *response_desc = vgpu_virgl_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    struct virtio_gpu_scanout_info *scanout =
        vgpu_virgl_get_scanout(vgpu, request->scanout_id);
    if (!scanout) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID, plen);
        return;
    }

    if (request->resource_id == 0) {
        scanout->primary_resource_id = 0;
        scanout->src_x = scanout->src_y = 0;
        scanout->src_w = scanout->src_h = 0;
        vgpu_display_publish_primary_clear(request->scanout_id);
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_OK_NODATA, plen);
        return;
    }

    struct virgl_renderer_resource_info info = {0};
    int ret =
        virgl_renderer_resource_get_info((int) request->resource_id, &info);
    if (ret) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID,
                                  plen);
        return;
    }

    if (!vgpu_virgl_rect_fits(info.width, info.height, &request->r) ||
        request->r.width > scanout->width ||
        request->r.height > scanout->height) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER, plen);
        return;
    }

    scanout->primary_resource_id = request->resource_id;
    scanout->src_x = request->r.x;
    scanout->src_y = request->r.y;
    scanout->src_w = request->r.width;
    scanout->src_h = request->r.height;

    if (vgpu_display_can_publish()) {
        struct vgpu_display_payload *payload =
            vgpu_virgl_create_gl_payload(request->resource_id, scanout, &info);
        if (!payload) {
            vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                      VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY, plen);
            return;
        }
        virgl_renderer_force_ctx_0();
        vgpu_display_publish_primary_set(request->scanout_id, payload);
    }

    vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                              VIRTIO_GPU_RESP_OK_NODATA, plen);
}

static void vgpu_virgl_cmd_resource_flush_handler(virtio_gpu_state_t *vgpu,
                                                  struct virtq_desc *vq_desc,
                                                  uint32_t *plen)
{
    const struct virtio_gpu_res_flush *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_res_flush));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (!vgpu_virgl_find_resource(request->resource_id)) {
        g_virtio_gpu_sw_backend.resource_flush(vgpu, vq_desc, plen);
        return;
    }

    const struct virtq_desc *response_desc = vgpu_virgl_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    for (uint32_t i = 0; i < PRIV(vgpu)->num_scanouts; i++) {
        struct virtio_gpu_scanout_info *scanout = &PRIV(vgpu)->scanouts[i];
        if (!scanout->enabled ||
            scanout->primary_resource_id != request->resource_id)
            continue;

        if (!vgpu_virgl_publish_gl_scanout(i, request->resource_id, scanout)) {
            vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                      VIRTIO_GPU_RESP_ERR_UNSPEC, plen);
            return;
        }
    }

    vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                              VIRTIO_GPU_RESP_OK_NODATA, plen);
}

static void vgpu_virgl_cmd_submit_3d_handler(virtio_gpu_state_t *vgpu,
                                             struct virtq_desc *vq_desc,
                                             uint32_t *plen)
{
    const struct virtq_desc *response_desc = vgpu_virgl_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr), plen);
    if (!response_desc)
        return;

    const struct virtio_gpu_cmd_submit *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_cmd_submit));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (request->size == 0 || (request->size % sizeof(uint32_t)) != 0 ||
        request->size / sizeof(uint32_t) > INT32_MAX) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER, plen);
        return;
    }

    void *buffer = malloc(request->size);
    if (!buffer) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY, plen);
        return;
    }

    enum virtio_gpu_desc_copy_result copy_result =
        virtio_gpu_desc_copy_from_readable(vgpu, vq_desc, VIRTIO_GPU_MAX_DESC,
                                           sizeof(struct virtio_gpu_cmd_submit),
                                           buffer, request->size);
    if (copy_result != VIRTIO_GPU_DESC_COPY_OK) {
        free(buffer);
        if (copy_result == VIRTIO_GPU_DESC_COPY_INVALID) {
            virtio_gpu_set_fail(vgpu);
            *plen = 0;
        } else {
            vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                      VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER,
                                      plen);
        }
        return;
    }
    int ret = virgl_renderer_submit_cmd(
        buffer, request->hdr.ctx_id, (int) (request->size / sizeof(uint32_t)));
    free(buffer);

    vgpu_virgl_write_response(
        vgpu, &request->hdr, response_desc,
        ret ? VIRTIO_GPU_RESP_ERR_UNSPEC : VIRTIO_GPU_RESP_OK_NODATA, plen);
}

static uint32_t vgpu_virgl_capset_id_for_index(uint32_t capset_index)
{
    uint32_t max_version = 0;
    uint32_t max_size = 0;
    uint32_t index = 0;

    virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VIRGL, &max_version,
                               &max_size);
    if (max_version && max_size) {
        if (capset_index == index)
            return VIRTIO_GPU_CAPSET_VIRGL;
        index++;
    }

    max_version = 0;
    max_size = 0;
    virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VIRGL2, &max_version,
                               &max_size);
    if (max_version && max_size && capset_index == index)
        return VIRTIO_GPU_CAPSET_VIRGL2;

    return 0;
}

uint32_t virtio_gpu_backend_get_num_capsets(void)
{
    return vgpu_virgl_count_capsets();
}

static void vgpu_virgl_cmd_get_capset_info_handler(virtio_gpu_state_t *vgpu,
                                                   struct virtq_desc *vq_desc,
                                                   uint32_t *plen)
{
    const struct virtio_gpu_get_capset_info *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_get_capset_info));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    int resp_idx = virtio_gpu_get_response_desc(
        vq_desc, VIRTIO_GPU_MAX_DESC,
        sizeof(struct virtio_gpu_resp_capset_info));
    if (resp_idx < 0) {
        *plen = 0;
        return;
    }

    struct virtio_gpu_resp_capset_info *response = virtio_gpu_mem_guest_to_host(
        vgpu, vq_desc[resp_idx].addr,
        sizeof(struct virtio_gpu_resp_capset_info));
    if (!response) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    memset(response, 0, sizeof(*response));
    response->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET_INFO;
    if (request->hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
        response->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
        response->hdr.fence_id = request->hdr.fence_id;
    }

    response->capset_id = vgpu_virgl_capset_id_for_index(request->capset_index);
    if (response->capset_id) {
        uint32_t max_version = 0;
        uint32_t max_size = 0;

        virgl_renderer_get_cap_set(response->capset_id, &max_version,
                                   &max_size);
        response->capset_max_version = max_version;
        response->capset_max_size = max_size;
    }

    *plen = sizeof(*response);
}

static void vgpu_virgl_cmd_get_capset_handler(virtio_gpu_state_t *vgpu,
                                              struct virtq_desc *vq_desc,
                                              uint32_t *plen)
{
    const struct virtio_gpu_get_capset *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_get_capset));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    int resp_idx = virtio_gpu_get_response_desc(
        vq_desc, VIRTIO_GPU_MAX_DESC, sizeof(struct virtio_gpu_resp_capset));
    if (resp_idx < 0) {
        *plen = 0;
        return;
    }

    uint32_t max_version = 0;
    uint32_t max_size = 0;
    virgl_renderer_get_cap_set(request->capset_id, &max_version, &max_size);
    if (!max_version || !max_size || request->capset_version > max_version) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, &vq_desc[resp_idx],
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        return;
    }

    size_t response_size = sizeof(struct virtio_gpu_resp_capset) + max_size;
    if (response_size < max_size || response_size > UINT32_MAX ||
        vq_desc[resp_idx].len < response_size) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, &vq_desc[resp_idx],
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        return;
    }

    struct virtio_gpu_resp_capset *response = virtio_gpu_mem_guest_to_host(
        vgpu, vq_desc[resp_idx].addr, (uint32_t) response_size);
    if (!response) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    memset(response, 0, sizeof(*response));
    response->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET;
    if (request->hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
        response->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
        response->hdr.fence_id = request->hdr.fence_id;
    }
    virgl_renderer_fill_caps(request->capset_id, request->capset_version,
                             response->capset_data);

    *plen = (uint32_t) response_size;
}

const struct virtio_gpu_cmd_backend g_virtio_gpu_backend = {
    .init = vgpu_virgl_init,
    .thread_enter = vgpu_virgl_thread_enter,
    .command_enter = vgpu_virgl_command_enter,
    .command_leave = vgpu_virgl_command_leave,
    .poll = vgpu_virgl_poll,
    .reset = vgpu_virgl_delegate_reset,
    .get_display_info = vgpu_virgl_delegate_get_display_info,
    .resource_create_2d = vgpu_virgl_delegate_resource_create_2d,
    .resource_unref = vgpu_virgl_cmd_resource_unref_handler,
    .set_scanout = vgpu_virgl_cmd_set_scanout_handler,
    .resource_flush = vgpu_virgl_cmd_resource_flush_handler,
    .transfer_to_host_2d = vgpu_virgl_cmd_transfer_to_host_2d_handler,
    .resource_attach_backing = vgpu_virgl_cmd_resource_attach_backing_handler,
    .resource_detach_backing = vgpu_virgl_cmd_resource_detach_backing_handler,
    .get_capset_info = vgpu_virgl_cmd_get_capset_info_handler,
    .get_capset = vgpu_virgl_cmd_get_capset_handler,
    .get_edid = vgpu_virgl_delegate_get_edid,
    .resource_assign_uuid = VIRTIO_GPU_CMD_UNDEF,
    .resource_create_blob = VIRTIO_GPU_CMD_UNDEF,
    .set_scanout_blob = VIRTIO_GPU_CMD_UNDEF,
    .ctx_create = vgpu_virgl_cmd_ctx_create_handler,
    .ctx_destroy = vgpu_virgl_cmd_ctx_destroy_handler,
    .ctx_attach_resource = vgpu_virgl_cmd_ctx_attach_resource_handler,
    .ctx_detach_resource = vgpu_virgl_cmd_ctx_detach_resource_handler,
    .resource_create_3d = vgpu_virgl_cmd_resource_create_3d_handler,
    .transfer_to_host_3d = vgpu_virgl_cmd_transfer_to_host_3d_handler,
    .transfer_from_host_3d = vgpu_virgl_cmd_transfer_from_host_3d_handler,
    .submit_3d = vgpu_virgl_cmd_submit_3d_handler,
    .resource_map_blob = VIRTIO_GPU_CMD_UNDEF,
    .resource_unmap_blob = VIRTIO_GPU_CMD_UNDEF,
    .update_cursor = vgpu_virgl_delegate_update_cursor,
    .move_cursor = vgpu_virgl_delegate_move_cursor,
};
