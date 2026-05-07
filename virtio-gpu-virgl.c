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

struct vgpu_virgl_pending_fence {
    /* Published last by renderer work and claimed atomically by callbacks so
     * stale reset callbacks cannot observe or reuse a partially written slot.
     */
    uint64_t active_token;
    uint32_t generation;
    bool context_fence;
    uint32_t ctx_id;
    uint32_t ring_idx;
    uint64_t renderer_fence_id;
    uint64_t guest_fence_id;
};

struct vgpu_virgl_pending_fence_snapshot {
    uint64_t active_token;
    uint32_t generation;
    bool context_fence;
    uint32_t ctx_id;
    uint32_t ring_idx;
    uint64_t renderer_fence_id;
    uint64_t guest_fence_id;
};

struct vgpu_virgl_ctrl_work {
    virtio_gpu_state_t *vgpu;
    struct virtio_gpu_ctrl_hdr hdr;
    union {
        struct virtio_gpu_ctx_create ctx_create;
        struct virtio_gpu_ctx_destroy ctx_destroy;
        struct virtio_gpu_ctx_resource ctx_resource;
        struct virtio_gpu_resource_create_3d resource_create_3d;
        struct virtio_gpu_res_unref resource_unref;
        struct virtio_gpu_res_attach_backing attach_backing;
        struct virtio_gpu_res_detach_backing detach_backing;
        struct virtio_gpu_trans_to_host_2d transfer_to_host_2d;
        struct virtio_gpu_transfer_host_3d transfer_3d;
        struct virtio_gpu_set_scanout set_scanout;
        struct virtio_gpu_res_flush resource_flush;
        struct virtio_gpu_cmd_submit submit_3d;
        struct virtio_gpu_get_capset_info get_capset_info;
        struct virtio_gpu_get_capset get_capset;
    } cmd;
    struct iovec *iov;
    int iov_count;
    void *data;
    size_t data_size;
    size_t response_capacity;
};

static struct vgpu_virgl_resource *g_vgpu_virgl_resources;
static struct vgpu_virgl_fence_state g_vgpu_virgl_fences;
static struct vgpu_virgl_pending_fence
    g_vgpu_virgl_pending_fence_records[VIRTIO_GPU_PENDING_CTRLS_MAX];
static uint32_t g_vgpu_virgl_pending_fences;
static bool g_vgpu_virgl_poll_request_pending;
static uint32_t g_vgpu_virgl_next_token;
static uint32_t g_vgpu_virgl_next_ctx0_renderer_fence;
static uint64_t g_vgpu_virgl_next_context_renderer_fence;
static uint64_t g_vgpu_virgl_next_pending_fence_token;
static uint64_t g_vgpu_virgl_debug_poll_requests_submitted;
static uint64_t g_vgpu_virgl_debug_poll_requests_dropped;
static uint64_t g_vgpu_virgl_debug_poll_requests_executed;
static uint64_t g_vgpu_virgl_debug_fences_created;
static uint64_t g_vgpu_virgl_debug_fences_completed;
static uint64_t g_vgpu_virgl_debug_ctrl_requests_started;
static uint64_t g_vgpu_virgl_debug_ctrl_requests_completed;
static uint64_t g_vgpu_virgl_debug_scanouts_published;
static uint64_t g_vgpu_virgl_debug_scanouts_dropped;

#define VGPU_VIRGL_PENDING_FENCE_CLAIMED (UINT64_C(1) << 63)

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

static void vgpu_virgl_reset_poll_state(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_vgpu_virgl_pending_fence_records);
         i++) {
        struct vgpu_virgl_pending_fence *pending =
            &g_vgpu_virgl_pending_fence_records[i];
        for (;;) {
            uint64_t token =
                __atomic_load_n(&pending->active_token, __ATOMIC_ACQUIRE);
            if (!token || (token & VGPU_VIRGL_PENDING_FENCE_CLAIMED))
                break;
            if (__atomic_compare_exchange_n(&pending->active_token, &token, 0,
                                            false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE))
                break;
        }
    }
    __atomic_store_n(&g_vgpu_virgl_pending_fences, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_vgpu_virgl_poll_request_pending, false,
                     __ATOMIC_RELEASE);
}

static void vgpu_virgl_reset_fence_state(void)
{
    __atomic_store_n(&g_vgpu_virgl_fences.last_ctx0_fence, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_vgpu_virgl_fences.last_context_ctx_id, 0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_vgpu_virgl_fences.last_context_ring_idx, 0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_vgpu_virgl_fences.last_context_fence, 0,
                     __ATOMIC_RELEASE);
}

static bool vgpu_virgl_pending_fence_stream_matches(
    const struct vgpu_virgl_pending_fence_snapshot *pending,
    bool context_fence,
    uint32_t ctx_id,
    uint32_t ring_idx)
{
    if (pending->context_fence != context_fence)
        return false;
    if (!context_fence)
        return true;

    return pending->ctx_id == ctx_id && pending->ring_idx == ring_idx;
}

static uint64_t vgpu_virgl_next_pending_fence_token(void)
{
    for (;;) {
        uint64_t token = __atomic_add_fetch(
            &g_vgpu_virgl_next_pending_fence_token, 1, __ATOMIC_RELAXED);
        token &= ~VGPU_VIRGL_PENDING_FENCE_CLAIMED;
        if (token)
            return token;
    }
}

static bool vgpu_virgl_load_pending_fence(
    const struct vgpu_virgl_pending_fence *pending,
    struct vgpu_virgl_pending_fence_snapshot *snapshot)
{
    uint64_t token = __atomic_load_n(&pending->active_token, __ATOMIC_ACQUIRE);
    if (!token || (token & VGPU_VIRGL_PENDING_FENCE_CLAIMED))
        return false;

    *snapshot = (struct vgpu_virgl_pending_fence_snapshot) {
        .active_token = token,
        .generation = __atomic_load_n(&pending->generation, __ATOMIC_RELAXED),
        .context_fence =
            __atomic_load_n(&pending->context_fence, __ATOMIC_RELAXED),
        .ctx_id = __atomic_load_n(&pending->ctx_id, __ATOMIC_RELAXED),
        .ring_idx = __atomic_load_n(&pending->ring_idx, __ATOMIC_RELAXED),
        .renderer_fence_id =
            __atomic_load_n(&pending->renderer_fence_id, __ATOMIC_RELAXED),
        .guest_fence_id =
            __atomic_load_n(&pending->guest_fence_id, __ATOMIC_RELAXED),
    };
    return __atomic_load_n(&pending->active_token, __ATOMIC_ACQUIRE) == token;
}

static bool vgpu_virgl_claim_pending_fence(
    struct vgpu_virgl_pending_fence *pending,
    uint64_t token)
{
    uint64_t expected = token;
    return __atomic_compare_exchange_n(&pending->active_token, &expected,
                                       token | VGPU_VIRGL_PENDING_FENCE_CLAIMED,
                                       false, __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
}

static void vgpu_virgl_release_claimed_fence(
    struct vgpu_virgl_pending_fence *pending)
{
    __atomic_store_n(&pending->active_token, 0, __ATOMIC_RELEASE);
}

static void vgpu_virgl_decrement_pending_fences(void)
{
    uint32_t pending =
        __atomic_load_n(&g_vgpu_virgl_pending_fences, __ATOMIC_ACQUIRE);
    while (pending) {
        if (__atomic_compare_exchange_n(&g_vgpu_virgl_pending_fences, &pending,
                                        pending - 1, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return;
    }
}

static bool vgpu_virgl_record_pending_fence(uint32_t generation,
                                            bool context_fence,
                                            uint32_t ctx_id,
                                            uint32_t ring_idx,
                                            uint64_t renderer_fence_id,
                                            uint64_t guest_fence_id)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_vgpu_virgl_pending_fence_records);
         i++) {
        struct vgpu_virgl_pending_fence *pending =
            &g_vgpu_virgl_pending_fence_records[i];
        if (__atomic_load_n(&pending->active_token, __ATOMIC_ACQUIRE))
            continue;

        __atomic_store_n(&pending->generation, generation, __ATOMIC_RELAXED);
        __atomic_store_n(&pending->context_fence, context_fence,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&pending->ctx_id, ctx_id, __ATOMIC_RELAXED);
        __atomic_store_n(&pending->ring_idx, ring_idx, __ATOMIC_RELAXED);
        __atomic_store_n(&pending->renderer_fence_id, renderer_fence_id,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&pending->guest_fence_id, guest_fence_id,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&pending->active_token,
                         vgpu_virgl_next_pending_fence_token(),
                         __ATOMIC_RELEASE);
        __atomic_add_fetch(&g_vgpu_virgl_pending_fences, 1, __ATOMIC_RELEASE);
        __atomic_add_fetch(&g_vgpu_virgl_debug_fences_created, 1,
                           __ATOMIC_RELAXED);
        return true;
    }

    return false;
}

static bool vgpu_virgl_cancel_pending_fence(bool context_fence,
                                            uint32_t ctx_id,
                                            uint32_t ring_idx,
                                            uint64_t renderer_fence_id)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_vgpu_virgl_pending_fence_records);
         i++) {
        struct vgpu_virgl_pending_fence *pending =
            &g_vgpu_virgl_pending_fence_records[i];
        struct vgpu_virgl_pending_fence_snapshot snapshot;
        if (!vgpu_virgl_load_pending_fence(pending, &snapshot) ||
            !vgpu_virgl_pending_fence_stream_matches(&snapshot, context_fence,
                                                     ctx_id, ring_idx) ||
            snapshot.renderer_fence_id != renderer_fence_id)
            continue;
        if (!vgpu_virgl_claim_pending_fence(pending, snapshot.active_token))
            continue;

        vgpu_virgl_release_claimed_fence(pending);
        vgpu_virgl_decrement_pending_fences();
        return true;
    }

    return false;
}

static bool vgpu_virgl_take_completed_fence(bool context_fence,
                                            uint32_t ctx_id,
                                            uint32_t ring_idx,
                                            uint64_t renderer_fence_id,
                                            uint32_t *generation,
                                            uint64_t *guest_fence_id)
{
    for (;;) {
        uint64_t best_renderer_fence = 0;
        uint32_t best_generation = 0;
        uint64_t best_guest_fence = 0;
        uint64_t best_token = 0;
        size_t best_index = 0;
        bool found = false;

        for (size_t i = 0; i < ARRAY_SIZE(g_vgpu_virgl_pending_fence_records);
             i++) {
            struct vgpu_virgl_pending_fence_snapshot pending;
            if (!vgpu_virgl_load_pending_fence(
                    &g_vgpu_virgl_pending_fence_records[i], &pending) ||
                !vgpu_virgl_pending_fence_stream_matches(
                    &pending, context_fence, ctx_id, ring_idx) ||
                pending.renderer_fence_id > renderer_fence_id)
                continue;
            if (found && pending.renderer_fence_id < best_renderer_fence)
                continue;

            found = true;
            best_renderer_fence = pending.renderer_fence_id;
            best_generation = pending.generation;
            best_guest_fence = pending.guest_fence_id;
            best_token = pending.active_token;
            best_index = i;
        }

        if (!found)
            return false;

        struct vgpu_virgl_pending_fence *best_pending =
            &g_vgpu_virgl_pending_fence_records[best_index];
        if (!vgpu_virgl_claim_pending_fence(best_pending, best_token))
            continue;

        vgpu_virgl_release_claimed_fence(best_pending);
        vgpu_virgl_decrement_pending_fences();
        __atomic_add_fetch(&g_vgpu_virgl_debug_fences_completed, 1,
                           __ATOMIC_RELAXED);

        for (size_t i = 0; i < ARRAY_SIZE(g_vgpu_virgl_pending_fence_records);
             i++) {
            if (i == best_index)
                continue;

            struct vgpu_virgl_pending_fence *pending =
                &g_vgpu_virgl_pending_fence_records[i];
            struct vgpu_virgl_pending_fence_snapshot snapshot;
            if (!vgpu_virgl_load_pending_fence(pending, &snapshot) ||
                !vgpu_virgl_pending_fence_stream_matches(
                    &snapshot, context_fence, ctx_id, ring_idx) ||
                snapshot.generation != best_generation ||
                snapshot.renderer_fence_id > renderer_fence_id)
                continue;
            if (!vgpu_virgl_claim_pending_fence(pending, snapshot.active_token))
                continue;

            vgpu_virgl_release_claimed_fence(pending);
            vgpu_virgl_decrement_pending_fences();
            __atomic_add_fetch(&g_vgpu_virgl_debug_fences_completed, 1,
                               __ATOMIC_RELAXED);
        }

        *generation = best_generation;
        *guest_fence_id = best_guest_fence;
        return true;
    }
}

static void vgpu_virgl_request_poll(void)
{
    if (!__atomic_load_n(&g_vgpu_virgl_pending_fences, __ATOMIC_ACQUIRE))
        return;

    if (__atomic_exchange_n(&g_vgpu_virgl_poll_request_pending, true,
                            __ATOMIC_ACQ_REL))
        return;

    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_POLL,
    };
    if (!vgpu_renderer_submit(&request)) {
        __atomic_add_fetch(&g_vgpu_virgl_debug_poll_requests_dropped, 1,
                           __ATOMIC_RELAXED);
        __atomic_store_n(&g_vgpu_virgl_poll_request_pending, false,
                         __ATOMIC_RELEASE);
        return;
    }

    __atomic_add_fetch(&g_vgpu_virgl_debug_poll_requests_submitted, 1,
                       __ATOMIC_RELAXED);
}

static uint32_t vgpu_virgl_next_token(void)
{
    uint32_t token =
        __atomic_add_fetch(&g_vgpu_virgl_next_token, 1, __ATOMIC_RELAXED);
    return token ? token
                 : __atomic_add_fetch(&g_vgpu_virgl_next_token, 1,
                                      __ATOMIC_RELAXED);
}

static uint32_t vgpu_virgl_next_ctx0_renderer_fence(void)
{
    uint32_t fence = __atomic_add_fetch(&g_vgpu_virgl_next_ctx0_renderer_fence,
                                        1, __ATOMIC_RELAXED);
    return fence ? fence
                 : __atomic_add_fetch(&g_vgpu_virgl_next_ctx0_renderer_fence, 1,
                                      __ATOMIC_RELAXED);
}

static uint64_t vgpu_virgl_next_context_renderer_fence(void)
{
    uint64_t fence = __atomic_add_fetch(
        &g_vgpu_virgl_next_context_renderer_fence, 1, __ATOMIC_RELAXED);
    return fence ? fence
                 : __atomic_add_fetch(&g_vgpu_virgl_next_context_renderer_fence,
                                      1, __ATOMIC_RELAXED);
}

static void vgpu_virgl_free_ctrl_work(struct vgpu_virgl_ctrl_work *work)
{
    if (!work)
        return;

    free(work->iov);
    free(work->data);
    free(work);
}

static void vgpu_virgl_release_ctrl_payload(void *payload)
{
    vgpu_virgl_free_ctrl_work(payload);
}

static void vgpu_virgl_write_fence(void *cookie, uint32_t fence)
{
    __atomic_store_n(&g_vgpu_virgl_fences.last_ctx0_fence, fence,
                     __ATOMIC_RELEASE);
    if (!cookie)
        return;

    uint32_t generation = 0;
    uint64_t guest_fence_id = 0;
    if (!vgpu_virgl_take_completed_fence(false, 0, 0, fence, &generation,
                                         &guest_fence_id))
        return;

    struct vgpu_renderer_completion completion = {
        .type = VGPU_RENDERER_DONE_FENCE,
        .token = {.generation = generation},
        .context_fence = false,
        .fence_id = guest_fence_id,
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
    __atomic_store_n(&g_vgpu_virgl_fences.last_context_ctx_id, ctx_id,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_vgpu_virgl_fences.last_context_ring_idx, ring_idx,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_vgpu_virgl_fences.last_context_fence, fence_id,
                     __ATOMIC_RELEASE);
    if (!cookie)
        return;

    uint32_t generation = 0;
    uint64_t guest_fence_id = 0;
    if (!vgpu_virgl_take_completed_fence(true, ctx_id, ring_idx, fence_id,
                                         &generation, &guest_fence_id))
        return;

    struct vgpu_renderer_completion completion = {
        .type = VGPU_RENDERER_DONE_FENCE,
        .token = {.generation = generation},
        .context_fence = true,
        .ctx_id = ctx_id,
        .ring_idx = ring_idx,
        .fence_id = guest_fence_id,
    };
    if (!vgpu_renderer_complete(&completion))
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): dropped renderer context fence completion %" PRIu64 "\n",
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
    int ret = virgl_renderer_init(vgpu, VIRGL_RENDERER_THREAD_SYNC,
                                  &g_vgpu_virgl_callbacks);
    if (ret) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): failed to initialize virglrenderer (%d)\n",
                __func__, ret);
        exit(EXIT_FAILURE);
    }

    vgpu_virgl_publish_capsets(vgpu);

    /* virglrenderer initializes ctx0 on this SDL/main thread. Keep all later
     * renderer work on the same owner through the renderer request queue.
     */
    vgpu_window_virgl_make_current(0, NULL);
}

static void vgpu_virgl_poll(virtio_gpu_state_t *vgpu)
{
    (void) vgpu;
    vgpu_virgl_request_poll();
}

static void vgpu_virgl_delegate_reset(virtio_gpu_state_t *vgpu)
{
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_RESET,
        .payload = vgpu,
    };
    if (!vgpu_renderer_submit(&request))
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): dropped renderer reset request\n",
                __func__);
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
    *plen = virtio_gpu_write_ctrl_response(vgpu, request, response_desc, type);
}

static void vgpu_virgl_complete_ctrl_work(
    const struct vgpu_renderer_request *request,
    struct vgpu_virgl_ctrl_work *work,
    uint32_t response_type,
    void *response,
    size_t response_size)
{
    if (response_type == VIRTIO_GPU_RESP_OK_NODATA &&
        (work->hdr.flags & VIRTIO_GPU_FLAG_FENCE)) {
        int ret;
        bool context_fence =
            (work->hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) != 0;
        uint32_t fence_ctx_id = context_fence ? work->hdr.ctx_id : 0;
        uint32_t fence_ring_idx = context_fence ? work->hdr.ring_idx : 0;
        uint64_t guest_fence_id = work->hdr.fence_id;
        /* Guest fence ids can restart after a device reset. Use a monotonic
         * renderer-local id for callbacks, then map it back to the guest id.
         */
        uint64_t renderer_fence_id =
            context_fence ? vgpu_virgl_next_context_renderer_fence()
                          : vgpu_virgl_next_ctx0_renderer_fence();
        if (!vgpu_virgl_record_pending_fence(
                request->token.generation, context_fence, fence_ctx_id,
                fence_ring_idx, renderer_fence_id, guest_fence_id)) {
            free(response);
            response = NULL;
            response_size = 0;
            response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
            goto complete;
        }

        if (work->hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) {
            ret = virgl_renderer_context_create_fence(
                work->hdr.ctx_id, VIRGL_RENDERER_FENCE_FLAG_MERGEABLE,
                work->hdr.ring_idx, renderer_fence_id);
        } else {
            /* virgl_renderer_create_fence() does not switch contexts; it
             * expects ctx0 to already be current before calling glFenceSync().
             */
            virgl_renderer_force_ctx_0();
            ret = virgl_renderer_create_fence((int) renderer_fence_id, 0);
        }

        if (!ret) {
            vgpu_virgl_request_poll();
            free(response);
            return;
        }

        vgpu_virgl_cancel_pending_fence(context_fence, fence_ctx_id,
                                        fence_ring_idx, renderer_fence_id);
        free(response);
        response = NULL;
        response_size = 0;
        response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
    }

complete:
    struct vgpu_renderer_completion completion = {
        .type = VGPU_RENDERER_DONE_CTRL,
        .token = request->token,
        .response_type = response_type,
        .response = response,
        .response_size = response_size,
        .release_response = free,
    };
    if (!vgpu_renderer_complete(&completion))
        free(response);
}

static bool vgpu_virgl_submit_ctrl_work(virtio_gpu_state_t *vgpu,
                                        const struct virtq_desc *response_desc,
                                        struct vgpu_virgl_ctrl_work *work,
                                        uint32_t response_type,
                                        uint32_t *plen)
{
    uint32_t generation = virtio_gpu_ctrl_generation(vgpu);
    uint32_t token = vgpu_virgl_next_token();
    if (!virtio_gpu_defer_ctrl_response_token(vgpu, &work->hdr, response_desc,
                                              response_type, generation,
                                              token)) {
        struct virtio_gpu_ctrl_hdr hdr = work->hdr;
        vgpu_virgl_free_ctrl_work(work);
        *plen = virtio_gpu_write_ctrl_response(vgpu, &hdr, response_desc,
                                               VIRTIO_GPU_RESP_ERR_UNSPEC);
        return false;
    }

    work->response_capacity = response_desc->len;
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.id = token, .generation = generation},
        .command_type = work->hdr.type,
        .payload = work,
        .payload_size = sizeof(*work),
        .release_payload = vgpu_virgl_release_ctrl_payload,
    };
    if (!vgpu_renderer_submit(&request)) {
        struct virtio_gpu_ctrl_hdr hdr = work->hdr;
        virtio_gpu_cancel_ctrl_response_token(vgpu, generation, token);
        *plen = virtio_gpu_write_ctrl_response(vgpu, &hdr, response_desc,
                                               VIRTIO_GPU_RESP_ERR_UNSPEC);
        vgpu_virgl_free_ctrl_work(work);
        return false;
    }

    *plen = VIRTIO_GPU_RESPONSE_DEFERRED;
    return true;
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
    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    if (!work) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY, plen);
        return;
    }

    work->vgpu = vgpu;
    work->hdr = request->hdr;
    work->hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    work->cmd.ctx_create = *request;
    work->cmd.ctx_create.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    vgpu_virgl_submit_ctrl_work(vgpu, response_desc, work,
                                VIRTIO_GPU_RESP_OK_NODATA, plen);
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
    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    if (!work) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY, plen);
        return;
    }

    work->vgpu = vgpu;
    work->hdr = request->hdr;
    work->hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    work->cmd.ctx_destroy = *request;
    work->cmd.ctx_destroy.hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    vgpu_virgl_submit_ctrl_work(vgpu, response_desc, work,
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
    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    if (!work) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY, plen);
        return;
    }

    work->vgpu = vgpu;
    work->hdr = request->hdr;
    work->hdr.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
    work->cmd.ctx_resource = *request;
    work->cmd.ctx_resource.hdr.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
    vgpu_virgl_submit_ctrl_work(vgpu, response_desc, work,
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
    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    if (!work) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY, plen);
        return;
    }

    work->vgpu = vgpu;
    work->hdr = request->hdr;
    work->hdr.type = VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE;
    work->cmd.ctx_resource = *request;
    work->cmd.ctx_resource.hdr.type = VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE;
    vgpu_virgl_submit_ctrl_work(vgpu, response_desc, work,
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

    res->resource_id = request->resource_id;
    vgpu_virgl_insert_resource(res);

    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    if (!work) {
        vgpu_virgl_remove_resource(request->resource_id);
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY, plen);
        return;
    }

    work->vgpu = vgpu;
    work->hdr = request->hdr;
    work->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    work->cmd.resource_create_3d = *request;
    work->cmd.resource_create_3d.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    if (!vgpu_virgl_submit_ctrl_work(vgpu, response_desc, work,
                                     VIRTIO_GPU_RESP_OK_NODATA, plen))
        vgpu_virgl_remove_resource(request->resource_id);
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

    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    if (!work) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY, plen);
        return;
    }

    work->vgpu = vgpu;
    work->hdr = request->hdr;
    work->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    work->cmd.resource_unref = *request;
    work->cmd.resource_unref.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    vgpu_virgl_submit_ctrl_work(vgpu, response_desc, work,
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
    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    if (!work) {
        free(iov);
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY, plen);
        return;
    }

    work->vgpu = vgpu;
    work->hdr = request->hdr;
    work->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    work->cmd.attach_backing = *request;
    work->cmd.attach_backing.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    work->iov = iov;
    work->iov_count = (int) nr_entries;
    vgpu_virgl_submit_ctrl_work(vgpu, response_desc, work,
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

    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    if (!work) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY, plen);
        return;
    }

    work->vgpu = vgpu;
    work->hdr = request->hdr;
    work->hdr.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    work->cmd.detach_backing = *request;
    work->cmd.detach_backing.hdr.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    vgpu_virgl_submit_ctrl_work(vgpu, response_desc, work,
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

    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    if (!work) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY, plen);
        return;
    }

    work->vgpu = vgpu;
    work->hdr = request->hdr;
    work->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    work->cmd.transfer_to_host_2d = *request;
    work->cmd.transfer_to_host_2d.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    vgpu_virgl_submit_ctrl_work(vgpu, response_desc, work,
                                VIRTIO_GPU_RESP_OK_NODATA, plen);
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

    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    if (!work) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY, plen);
        return;
    }

    work->vgpu = vgpu;
    work->hdr = request->hdr;
    work->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    work->cmd.transfer_3d = *request;
    work->cmd.transfer_3d.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    vgpu_virgl_submit_ctrl_work(vgpu, response_desc, work,
                                VIRTIO_GPU_RESP_OK_NODATA, plen);
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

    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    if (!work) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY, plen);
        return;
    }

    work->vgpu = vgpu;
    work->hdr = request->hdr;
    work->hdr.type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    work->cmd.transfer_3d = *request;
    work->cmd.transfer_3d.hdr.type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    vgpu_virgl_submit_ctrl_work(vgpu, response_desc, work,
                                VIRTIO_GPU_RESP_OK_NODATA, plen);
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
    if (!vgpu_display_can_publish()) {
        __atomic_add_fetch(&g_vgpu_virgl_debug_scanouts_dropped, 1,
                           __ATOMIC_RELAXED);
        return true;
    }

    struct virgl_renderer_resource_info info = {0};
    int ret = virgl_renderer_resource_get_info((int) resource_id, &info);
    if (ret) {
        __atomic_add_fetch(&g_vgpu_virgl_debug_scanouts_dropped, 1,
                           __ATOMIC_RELAXED);
        return false;
    }

    struct vgpu_display_payload *payload =
        vgpu_virgl_create_gl_payload(resource_id, scanout, &info);
    if (!payload) {
        __atomic_add_fetch(&g_vgpu_virgl_debug_scanouts_dropped, 1,
                           __ATOMIC_RELAXED);
        return false;
    }
    vgpu_display_publish_primary_set(scanout_id, payload);
    __atomic_add_fetch(&g_vgpu_virgl_debug_scanouts_published, 1,
                       __ATOMIC_RELAXED);
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

    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    if (!work) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY, plen);
        return;
    }

    work->vgpu = vgpu;
    work->hdr = request->hdr;
    work->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    work->cmd.set_scanout = *request;
    work->cmd.set_scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    vgpu_virgl_submit_ctrl_work(vgpu, response_desc, work,
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

    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    if (!work) {
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY, plen);
        return;
    }

    work->vgpu = vgpu;
    work->hdr = request->hdr;
    work->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    work->cmd.resource_flush = *request;
    work->cmd.resource_flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    vgpu_virgl_submit_ctrl_work(vgpu, response_desc, work,
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
    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    if (!work) {
        free(buffer);
        vgpu_virgl_write_response(vgpu, &request->hdr, response_desc,
                                  VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY, plen);
        return;
    }

    work->vgpu = vgpu;
    work->hdr = request->hdr;
    work->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    work->cmd.submit_3d = *request;
    work->cmd.submit_3d.hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    work->data = buffer;
    work->data_size = request->size;
    vgpu_virgl_submit_ctrl_work(vgpu, response_desc, work,
                                VIRTIO_GPU_RESP_OK_NODATA, plen);
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

static void vgpu_virgl_execute_ctrl_request(
    const struct vgpu_renderer_request *request,
    struct vgpu_virgl_ctrl_work *work)
{
    uint32_t response_type = VIRTIO_GPU_RESP_OK_NODATA;
    void *response = NULL;
    size_t response_size = 0;

    __atomic_add_fetch(&g_vgpu_virgl_debug_ctrl_requests_started, 1,
                       __ATOMIC_RELAXED);

    /* SDL presentation can detach the real current GL context behind
     * virglrenderer's cached current_ctx/current_hw_ctx. Reset to ctx0 before
     * every renderer command so virglrenderer performs the needed switch.
     */
    virgl_renderer_force_ctx_0();

    switch (request->command_type) {
    case VIRTIO_GPU_CMD_CTX_CREATE: {
        const struct virtio_gpu_ctx_create *cmd = &work->cmd.ctx_create;
        int ret = virgl_renderer_context_create(cmd->hdr.ctx_id, cmd->nlen,
                                                cmd->debug_name);
        response_type =
            ret ? VIRTIO_GPU_RESP_ERR_UNSPEC : VIRTIO_GPU_RESP_OK_NODATA;
        break;
    }
    case VIRTIO_GPU_CMD_CTX_DESTROY:
        virgl_renderer_context_destroy(work->cmd.ctx_destroy.hdr.ctx_id);
        break;
    case VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE:
        virgl_renderer_ctx_attach_resource(work->cmd.ctx_resource.hdr.ctx_id,
                                           work->cmd.ctx_resource.resource_id);
        break;
    case VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE:
        virgl_renderer_ctx_detach_resource(work->cmd.ctx_resource.hdr.ctx_id,
                                           work->cmd.ctx_resource.resource_id);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_3D: {
        const struct virtio_gpu_resource_create_3d *cmd =
            &work->cmd.resource_create_3d;
        struct virgl_renderer_resource_create_args args = {
            .handle = cmd->resource_id,
            .target = cmd->target,
            .format = cmd->format,
            .bind = cmd->bind,
            .width = cmd->width,
            .height = cmd->height,
            .depth = cmd->depth,
            .array_size = cmd->array_size,
            .last_level = cmd->last_level,
            .nr_samples = cmd->nr_samples,
            .flags = cmd->flags,
        };
        int ret = virgl_renderer_resource_create(&args, NULL, 0);
        if (ret) {
            vgpu_virgl_remove_resource(cmd->resource_id);
            response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
        }
        break;
    }
    case VIRTIO_GPU_CMD_RESOURCE_UNREF: {
        const struct virtio_gpu_res_unref *cmd = &work->cmd.resource_unref;
        struct vgpu_virgl_resource *res =
            vgpu_virgl_find_resource(cmd->resource_id);
        if (!res) {
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
            break;
        }
        vgpu_virgl_clear_resource_scanouts(work->vgpu, cmd->resource_id);
        if (res->backing_attached)
            vgpu_virgl_detach_iov(cmd->resource_id);
        virgl_renderer_resource_unref(cmd->resource_id);
        vgpu_virgl_remove_resource(cmd->resource_id);
        break;
    }
    case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING: {
        const struct virtio_gpu_res_attach_backing *cmd =
            &work->cmd.attach_backing;
        struct vgpu_virgl_resource *res =
            vgpu_virgl_find_resource(cmd->resource_id);
        if (!res || res->backing_attached) {
            response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
            break;
        }
        int ret = virgl_renderer_resource_attach_iov(
            cmd->resource_id, work->iov, work->iov_count);
        if (ret) {
            response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
            break;
        }
        work->iov = NULL;
        work->iov_count = 0;
        res->backing_attached = true;
        break;
    }
    case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING: {
        const struct virtio_gpu_res_detach_backing *cmd =
            &work->cmd.detach_backing;
        struct vgpu_virgl_resource *res =
            vgpu_virgl_find_resource(cmd->resource_id);
        if (!res || !res->backing_attached) {
            response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
            break;
        }
        vgpu_virgl_detach_iov(cmd->resource_id);
        res->backing_attached = false;
        break;
    }
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D: {
        const struct virtio_gpu_trans_to_host_2d *cmd =
            &work->cmd.transfer_to_host_2d;
        struct vgpu_virgl_box box = vgpu_virgl_box_from_rect(&cmd->r);
        int ret = virgl_renderer_transfer_write_iov(
            cmd->resource_id, 0, 0, 0, 0, (struct virgl_box *) &box,
            cmd->offset, NULL, 0);
        response_type =
            ret ? VIRTIO_GPU_RESP_ERR_UNSPEC : VIRTIO_GPU_RESP_OK_NODATA;
        break;
    }
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D: {
        const struct virtio_gpu_transfer_host_3d *cmd = &work->cmd.transfer_3d;
        struct vgpu_virgl_box box = vgpu_virgl_box_from_virtio(&cmd->box);
        int ret = virgl_renderer_transfer_write_iov(
            cmd->resource_id, cmd->hdr.ctx_id, (int) cmd->level, cmd->stride,
            cmd->layer_stride, (struct virgl_box *) &box, cmd->offset, NULL, 0);
        response_type =
            ret ? VIRTIO_GPU_RESP_ERR_UNSPEC : VIRTIO_GPU_RESP_OK_NODATA;
        break;
    }
    case VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D: {
        const struct virtio_gpu_transfer_host_3d *cmd = &work->cmd.transfer_3d;
        struct vgpu_virgl_box box = vgpu_virgl_box_from_virtio(&cmd->box);
        int ret = virgl_renderer_transfer_read_iov(
            cmd->resource_id, cmd->hdr.ctx_id, cmd->level, cmd->stride,
            cmd->layer_stride, (struct virgl_box *) &box, cmd->offset, NULL, 0);
        response_type =
            ret ? VIRTIO_GPU_RESP_ERR_UNSPEC : VIRTIO_GPU_RESP_OK_NODATA;
        break;
    }
    case VIRTIO_GPU_CMD_SUBMIT_3D: {
        const struct virtio_gpu_cmd_submit *cmd = &work->cmd.submit_3d;
        int ret = virgl_renderer_submit_cmd(
            work->data, cmd->hdr.ctx_id, (int) (cmd->size / sizeof(uint32_t)));
        response_type =
            ret ? VIRTIO_GPU_RESP_ERR_UNSPEC : VIRTIO_GPU_RESP_OK_NODATA;
        break;
    }
    case VIRTIO_GPU_CMD_SET_SCANOUT: {
        const struct virtio_gpu_set_scanout *cmd = &work->cmd.set_scanout;
        struct virtio_gpu_scanout_info *scanout =
            vgpu_virgl_get_scanout(work->vgpu, cmd->scanout_id);
        if (!scanout) {
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
            break;
        }

        if (cmd->resource_id == 0) {
            scanout->primary_resource_id = 0;
            scanout->src_x = scanout->src_y = 0;
            scanout->src_w = scanout->src_h = 0;
            vgpu_display_publish_primary_clear(cmd->scanout_id);
            break;
        }

        struct virgl_renderer_resource_info info = {0};
        int ret =
            virgl_renderer_resource_get_info((int) cmd->resource_id, &info);
        if (ret) {
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
            break;
        }

        if (!vgpu_virgl_rect_fits(info.width, info.height, &cmd->r) ||
            cmd->r.width > scanout->width || cmd->r.height > scanout->height) {
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            break;
        }

        scanout->primary_resource_id = cmd->resource_id;
        scanout->src_x = cmd->r.x;
        scanout->src_y = cmd->r.y;
        scanout->src_w = cmd->r.width;
        scanout->src_h = cmd->r.height;

        if (vgpu_display_can_publish()) {
            struct vgpu_display_payload *payload =
                vgpu_virgl_create_gl_payload(cmd->resource_id, scanout, &info);
            if (!payload) {
                response_type = VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
                break;
            }
            vgpu_display_publish_primary_set(cmd->scanout_id, payload);
            __atomic_add_fetch(&g_vgpu_virgl_debug_scanouts_published, 1,
                               __ATOMIC_RELAXED);
        } else {
            __atomic_add_fetch(&g_vgpu_virgl_debug_scanouts_dropped, 1,
                               __ATOMIC_RELAXED);
        }
        break;
    }
    case VIRTIO_GPU_CMD_RESOURCE_FLUSH: {
        const struct virtio_gpu_res_flush *cmd = &work->cmd.resource_flush;
        for (uint32_t i = 0; i < PRIV(work->vgpu)->num_scanouts; i++) {
            struct virtio_gpu_scanout_info *scanout =
                &PRIV(work->vgpu)->scanouts[i];
            if (!scanout->enabled ||
                scanout->primary_resource_id != cmd->resource_id)
                continue;

            if (!vgpu_virgl_publish_gl_scanout(i, cmd->resource_id, scanout)) {
                response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
                break;
            }
        }
        break;
    }
    case VIRTIO_GPU_CMD_GET_CAPSET_INFO: {
        const struct virtio_gpu_get_capset_info *cmd =
            &work->cmd.get_capset_info;
        struct virtio_gpu_resp_capset_info *out = calloc(1, sizeof(*out));
        if (!out) {
            response_type = VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
            break;
        }
        out->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET_INFO;
        if (cmd->hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
            out->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
            out->hdr.fence_id = cmd->hdr.fence_id;
        }
        out->capset_id = vgpu_virgl_capset_id_for_index(cmd->capset_index);
        if (out->capset_id) {
            uint32_t max_version = 0;
            uint32_t max_size = 0;
            virgl_renderer_get_cap_set(out->capset_id, &max_version, &max_size);
            out->capset_max_version = max_version;
            out->capset_max_size = max_size;
        }
        response = out;
        response_size = sizeof(*out);
        response_type = VIRTIO_GPU_RESP_OK_CAPSET_INFO;
        break;
    }
    case VIRTIO_GPU_CMD_GET_CAPSET: {
        const struct virtio_gpu_get_capset *cmd = &work->cmd.get_capset;
        uint32_t max_version = 0;
        uint32_t max_size = 0;
        virgl_renderer_get_cap_set(cmd->capset_id, &max_version, &max_size);
        if (!max_version || !max_size || cmd->capset_version > max_version) {
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            break;
        }

        response_size = sizeof(struct virtio_gpu_resp_capset) + max_size;
        if (response_size < max_size || response_size > UINT32_MAX ||
            work->response_capacity < response_size) {
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            response_size = 0;
            break;
        }

        struct virtio_gpu_resp_capset *out = calloc(1, response_size);
        if (!out) {
            response_type = VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
            response_size = 0;
            break;
        }
        out->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET;
        if (cmd->hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
            out->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
            out->hdr.fence_id = cmd->hdr.fence_id;
        }
        virgl_renderer_fill_caps(cmd->capset_id, cmd->capset_version,
                                 out->capset_data);
        response = out;
        response_type = VIRTIO_GPU_RESP_OK_CAPSET;
        break;
    }
    default:
        response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
        break;
    }

    vgpu_virgl_complete_ctrl_work(request, work, response_type, response,
                                  response_size);
    __atomic_add_fetch(&g_vgpu_virgl_debug_ctrl_requests_completed, 1,
                       __ATOMIC_RELAXED);
}

void vgpu_virgl_execute_renderer_request(
    const struct vgpu_renderer_request *request)
{
    if (!request)
        return;

    switch (request->type) {
    case VGPU_RENDERER_REQ_RESET: {
        virtio_gpu_state_t *vgpu = request->payload;
        vgpu_virgl_reset_poll_state();
        vgpu_virgl_reset_fence_state();
        if (g_virtio_gpu_sw_backend.reset)
            g_virtio_gpu_sw_backend.reset(vgpu);
        virgl_renderer_reset();
        vgpu_virgl_publish_capsets(vgpu);
        while (g_vgpu_virgl_resources) {
            struct vgpu_virgl_resource *res = g_vgpu_virgl_resources;
            g_vgpu_virgl_resources = res->next;
            free(res);
        }
        break;
    }
    case VGPU_RENDERER_REQ_POLL:
        virgl_renderer_poll();
        __atomic_add_fetch(&g_vgpu_virgl_debug_poll_requests_executed, 1,
                           __ATOMIC_RELAXED);
        __atomic_store_n(&g_vgpu_virgl_poll_request_pending, false,
                         __ATOMIC_RELEASE);
        break;
    case VGPU_RENDERER_REQ_CTRL:
        vgpu_virgl_execute_ctrl_request(
            request, (struct vgpu_virgl_ctrl_work *) request->payload);
        vgpu_virgl_free_ctrl_work(
            (struct vgpu_virgl_ctrl_work *) request->payload);
        break;
    default:
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): unsupported renderer request type %u\n",
                __func__, request->type);
        break;
    }
}

void vgpu_virgl_debug_snapshot(struct vgpu_virgl_debug_stats *stats)
{
    if (!stats)
        return;

    *stats = (struct vgpu_virgl_debug_stats) {
        .pending_fences =
            __atomic_load_n(&g_vgpu_virgl_pending_fences, __ATOMIC_ACQUIRE),
        .poll_request_pending = __atomic_load_n(
            &g_vgpu_virgl_poll_request_pending, __ATOMIC_ACQUIRE),
        .poll_requests_submitted = __atomic_load_n(
            &g_vgpu_virgl_debug_poll_requests_submitted, __ATOMIC_RELAXED),
        .poll_requests_dropped = __atomic_load_n(
            &g_vgpu_virgl_debug_poll_requests_dropped, __ATOMIC_RELAXED),
        .poll_requests_executed = __atomic_load_n(
            &g_vgpu_virgl_debug_poll_requests_executed, __ATOMIC_RELAXED),
        .fences_created = __atomic_load_n(&g_vgpu_virgl_debug_fences_created,
                                          __ATOMIC_RELAXED),
        .fences_completed = __atomic_load_n(
            &g_vgpu_virgl_debug_fences_completed, __ATOMIC_RELAXED),
        .ctrl_requests_started = __atomic_load_n(
            &g_vgpu_virgl_debug_ctrl_requests_started, __ATOMIC_RELAXED),
        .ctrl_requests_completed = __atomic_load_n(
            &g_vgpu_virgl_debug_ctrl_requests_completed, __ATOMIC_RELAXED),
        .scanouts_published = __atomic_load_n(
            &g_vgpu_virgl_debug_scanouts_published, __ATOMIC_RELAXED),
        .scanouts_dropped = __atomic_load_n(
            &g_vgpu_virgl_debug_scanouts_dropped, __ATOMIC_RELAXED),
        .last_ctx0_fence = __atomic_load_n(&g_vgpu_virgl_fences.last_ctx0_fence,
                                           __ATOMIC_RELAXED),
        .last_context_ctx_id = __atomic_load_n(
            &g_vgpu_virgl_fences.last_context_ctx_id, __ATOMIC_RELAXED),
        .last_context_ring_idx = __atomic_load_n(
            &g_vgpu_virgl_fences.last_context_ring_idx, __ATOMIC_RELAXED),
        .last_context_fence = __atomic_load_n(
            &g_vgpu_virgl_fences.last_context_fence, __ATOMIC_RELAXED),
    };
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

    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    if (!work) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, &vq_desc[resp_idx],
            VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
        return;
    }

    work->vgpu = vgpu;
    work->hdr = request->hdr;
    work->hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
    work->cmd.get_capset_info = *request;
    work->cmd.get_capset_info.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
    vgpu_virgl_submit_ctrl_work(vgpu, &vq_desc[resp_idx], work,
                                VIRTIO_GPU_RESP_OK_CAPSET_INFO, plen);
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

    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    if (!work) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, &vq_desc[resp_idx],
            VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
        return;
    }

    work->vgpu = vgpu;
    work->hdr = request->hdr;
    work->hdr.type = VIRTIO_GPU_CMD_GET_CAPSET;
    work->cmd.get_capset = *request;
    work->cmd.get_capset.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET;
    vgpu_virgl_submit_ctrl_work(vgpu, &vq_desc[resp_idx], work,
                                VIRTIO_GPU_RESP_OK_CAPSET, plen);
}

const struct virtio_gpu_cmd_backend g_virtio_gpu_backend = {
    .init = vgpu_virgl_init,
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
