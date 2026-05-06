#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tests/fakes/virglrenderer.h"
#include "../vgpu-display.h"
#include "../vgpu-renderer.h"
#include "../virtio-gpu.h"

#define TEST_RAM_SIZE 4096U
#define REQ_ADDR 64U
#define PAYLOAD_ADDR 512U
#define RESP_ADDR 1024U

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            return 1;                                                        \
        }                                                                    \
    } while (0)

struct virgl_test_calls {
    int renderer_init_count;
    void *renderer_init_cookie;
    int renderer_init_flags;
    int renderer_init_callback_version;
    int renderer_init_lock_depth;

    int poll_count;

    int create_fence_count;
    int create_fence_client_id;
    uint32_t create_fence_ctx_id;

    int context_create_fence_count;
    uint32_t context_create_fence_ctx_id;
    uint32_t context_create_fence_flags;
    uint32_t context_create_fence_ring_idx;
    uint64_t context_create_fence_id;

    int reset_count;
    int reset_primary_clear_count;
    int reset_cursor_clear_count;
    uint32_t reset_primary_resource_id;
    uint32_t reset_cursor_resource_id;

    int context_create_count;
    uint32_t context_create_handle;
    uint32_t context_create_nlen;
    char context_create_name[64];

    int context_destroy_count;
    uint32_t context_destroy_handle;

    int ctx_attach_count;
    int ctx_attach_ctx_id;
    int ctx_attach_res_handle;

    int ctx_detach_count;
    int ctx_detach_ctx_id;
    int ctx_detach_res_handle;

    int resource_create_count;
    struct virgl_renderer_resource_create_args resource_create_args;

    int resource_unref_count;
    uint32_t resource_unref_handle;
    int resource_unref_primary_clear_count;
    uint32_t resource_unref_primary_resource_id;

    int resource_get_info_count;
    int resource_get_info_handle;

    int resource_attach_iov_count;
    int resource_attach_iov_handle;
    int resource_attach_iov_num_iovs;
    struct iovec *attached_iov;
    int attached_iov_count;
    void *attached_iov_base[4];
    size_t attached_iov_len[4];

    int resource_detach_iov_count;
    int resource_detach_iov_handle;

    int transfer_write_count;
    uint32_t transfer_write_handle;
    uint32_t transfer_write_ctx_id;
    int transfer_write_level;
    uint32_t transfer_write_stride;
    uint32_t transfer_write_layer_stride;
    struct virgl_box transfer_write_box;
    uint64_t transfer_write_offset;
    unsigned int transfer_write_iov_cnt;

    int transfer_read_count;
    uint32_t transfer_read_handle;
    uint32_t transfer_read_ctx_id;
    uint32_t transfer_read_level;
    uint32_t transfer_read_stride;
    uint32_t transfer_read_layer_stride;
    struct virgl_box transfer_read_box;
    uint64_t transfer_read_offset;
    int transfer_read_iov_cnt;

    int submit_count;
    int submit_ctx_id;
    int submit_ndw;
    uint32_t submit_words[16];

    int force_ctx_0_count;
    int force_ctx_0_lock_depth;
    int window_make_current_count;
    int window_make_current_null_count;
    virgl_renderer_gl_context window_make_current_last_ctx;
    int gl_lock_depth;
    int gl_lock_max_depth;
    int gl_lock_underflow_count;
    int renderer_complete_count;
    struct vgpu_renderer_completion last_renderer_completion;
    int renderer_submit_count;
    struct vgpu_renderer_request last_renderer_request;

    int undefined_count;
};

static uint8_t g_ram[TEST_RAM_SIZE];
static virtio_gpu_data_t g_vgpu_data;
static struct virgl_test_calls g_calls;
static struct vgpu_display_payload *g_last_primary_payload;
static uint32_t g_last_primary_scanout;
static int g_publish_primary_set_count;
static int g_publish_primary_clear_count;
static int g_publish_cursor_clear_count;
static bool g_create_fence_completes_sync;
static struct virgl_renderer_callbacks g_renderer_callbacks;
static void *g_renderer_cookie;
static struct {
    bool active;
    virtio_gpu_state_t *vgpu;
    struct virtio_gpu_ctrl_hdr request;
    struct virtq_desc response_desc;
    uint32_t token_id;
} g_pending_ctrls[16];

static void reset_fixture(void)
{
    free(g_calls.attached_iov);
    free(g_last_primary_payload);
    memset(g_ram, 0, sizeof(g_ram));
    memset(&g_vgpu_data, 0, sizeof(g_vgpu_data));
    memset(&g_calls, 0, sizeof(g_calls));
    g_last_primary_payload = NULL;
    g_last_primary_scanout = 0;
    g_publish_primary_set_count = 0;
    g_publish_primary_clear_count = 0;
    g_publish_cursor_clear_count = 0;
    g_create_fence_completes_sync = false;
    memset(&g_renderer_callbacks, 0, sizeof(g_renderer_callbacks));
    g_renderer_cookie = NULL;
    memset(g_pending_ctrls, 0, sizeof(g_pending_ctrls));
}

static virtio_gpu_state_t test_vgpu(void)
{
    virtio_gpu_state_t vgpu = {0};
    vgpu.ram = (uint32_t *) g_ram;
    vgpu.priv = &g_vgpu_data;
    g_vgpu_data.num_scanouts = 1;
    g_vgpu_data.scanouts[0].width = 1024;
    g_vgpu_data.scanouts[0].height = 768;
    g_vgpu_data.scanouts[0].enabled = 1;
    return vgpu;
}

bool vgpu_display_can_publish(void)
{
    return true;
}

void vgpu_display_publish_primary_set(uint32_t scanout_id,
                                      struct vgpu_display_payload *payload)
{
    free(g_last_primary_payload);
    g_last_primary_payload = payload;
    g_last_primary_scanout = scanout_id;
    g_publish_primary_set_count++;
}

void vgpu_display_publish_primary_clear(uint32_t scanout_id)
{
    (void) scanout_id;
    g_publish_primary_clear_count++;
}

void vgpu_display_publish_cursor_clear(uint32_t scanout_id)
{
    (void) scanout_id;
    g_publish_cursor_clear_count++;
}

void *virtio_gpu_mem_guest_to_host(virtio_gpu_state_t *vgpu,
                                   uint32_t addr,
                                   uint32_t size)
{
    (void) vgpu;
    if ((uint64_t) addr + size > sizeof(g_ram))
        return NULL;
    return &g_ram[addr];
}

void virtio_gpu_set_fail(virtio_gpu_state_t *vgpu)
{
    vgpu->Status |= VIRTIO_STATUS__DEVICE_NEEDS_RESET;
}

void *virtio_gpu_get_request(virtio_gpu_state_t *vgpu,
                             struct virtq_desc *vq_desc,
                             size_t request_size)
{
    if ((vq_desc[0].flags & VIRTIO_DESC_F_WRITE) ||
        vq_desc[0].len < request_size || request_size > UINT32_MAX)
        return NULL;

    return virtio_gpu_mem_guest_to_host(vgpu, (uint32_t) vq_desc[0].addr,
                                        (uint32_t) request_size);
}

int virtio_gpu_get_response_desc(struct virtq_desc *vq_desc,
                                 int max_desc,
                                 size_t response_size)
{
    if (response_size > UINT32_MAX)
        return -1;

    for (int i = 1; i < max_desc; i++) {
        if (!(vq_desc[i].flags & VIRTIO_DESC_F_WRITE))
            continue;
        if (vq_desc[i].len < response_size)
            return -1;
        return i;
    }

    return -1;
}

uint32_t virtio_gpu_write_ctrl_response(
    virtio_gpu_state_t *vgpu,
    const struct virtio_gpu_ctrl_hdr *request,
    const struct virtq_desc *response_desc,
    uint32_t type)
{
    struct virtio_gpu_ctrl_hdr *response = virtio_gpu_mem_guest_to_host(
        vgpu, (uint32_t) response_desc->addr, sizeof(*response));
    if (!response)
        return 0;

    memset(response, 0, sizeof(*response));
    response->type = type;
    if (request->flags & VIRTIO_GPU_FLAG_FENCE) {
        response->flags = VIRTIO_GPU_FLAG_FENCE;
        response->fence_id = request->fence_id;
    }
    return sizeof(*response);
}

bool virtio_gpu_defer_ctrl_response(virtio_gpu_state_t *vgpu,
                                    const struct virtio_gpu_ctrl_hdr *request,
                                    const struct virtq_desc *response_desc,
                                    uint32_t response_type,
                                    uint32_t generation)
{
    (void) vgpu;
    (void) request;
    (void) response_desc;
    (void) response_type;
    (void) generation;
    return false;
}

bool virtio_gpu_defer_ctrl_response_token(
    virtio_gpu_state_t *vgpu,
    const struct virtio_gpu_ctrl_hdr *request,
    const struct virtq_desc *response_desc,
    uint32_t response_type,
    uint32_t generation,
    uint32_t token_id)
{
    (void) response_type;
    (void) generation;

    for (size_t i = 0; i < ARRAY_SIZE(g_pending_ctrls); i++) {
        if (g_pending_ctrls[i].active)
            continue;
        g_pending_ctrls[i].active = true;
        g_pending_ctrls[i].vgpu = vgpu;
        g_pending_ctrls[i].request = *request;
        g_pending_ctrls[i].response_desc = *response_desc;
        g_pending_ctrls[i].token_id = token_id;
        return true;
    }

    return false;
}

uint32_t virtio_gpu_ctrl_generation(virtio_gpu_state_t *vgpu)
{
    (void) vgpu;
    return 0;
}

void virtio_gpu_set_num_capsets(virtio_gpu_state_t *vgpu,
                                uint32_t num_capsets)
{
    if (!vgpu || !vgpu->priv)
        return;

    ((virtio_gpu_data_t *) vgpu->priv)->num_capsets = num_capsets;
}

bool virtio_gpu_cancel_ctrl_response(
    virtio_gpu_state_t *vgpu,
    uint32_t generation,
    const struct virtio_gpu_ctrl_hdr *request)
{
    (void) vgpu;
    (void) generation;
    (void) request;
    return false;
}

bool virtio_gpu_cancel_ctrl_response_token(virtio_gpu_state_t *vgpu,
                                           uint32_t generation,
                                           uint32_t token_id)
{
    (void) vgpu;
    (void) generation;

    for (size_t i = 0; i < ARRAY_SIZE(g_pending_ctrls); i++) {
        if (!g_pending_ctrls[i].active ||
            g_pending_ctrls[i].token_id != token_id)
            continue;
        g_pending_ctrls[i].active = false;
        return true;
    }

    return false;
}

void virtio_gpu_complete_fence(virtio_gpu_state_t *vgpu,
                               bool context_fence,
                               uint32_t ctx_id,
                               uint32_t ring_idx,
                               uint64_t fence_id)
{
    (void) vgpu;
    (void) context_fence;
    (void) ctx_id;
    (void) ring_idx;
    (void) fence_id;
}

void vgpu_virgl_execute_renderer_request(
    const struct vgpu_renderer_request *request);

bool vgpu_renderer_complete(const struct vgpu_renderer_completion *completion)
{
    g_calls.renderer_complete_count++;
    if (completion)
        g_calls.last_renderer_completion = *completion;
    if (!completion || completion->type != VGPU_RENDERER_DONE_CTRL ||
        !completion->token.id)
        return completion != NULL;

    for (size_t i = 0; i < ARRAY_SIZE(g_pending_ctrls); i++) {
        if (!g_pending_ctrls[i].active ||
            g_pending_ctrls[i].token_id != completion->token.id)
            continue;

        if (completion->response) {
            void *dst = virtio_gpu_mem_guest_to_host(
                g_pending_ctrls[i].vgpu,
                (uint32_t) g_pending_ctrls[i].response_desc.addr,
                (uint32_t) completion->response_size);
            if (dst)
                memcpy(dst, completion->response, completion->response_size);
            if (completion->release_response)
                completion->release_response(completion->response);
        } else {
            virtio_gpu_write_ctrl_response(
                g_pending_ctrls[i].vgpu, &g_pending_ctrls[i].request,
                &g_pending_ctrls[i].response_desc, completion->response_type);
        }
        g_pending_ctrls[i].active = false;
        break;
    }
    return completion != NULL;
}

bool vgpu_renderer_submit(const struct vgpu_renderer_request *request)
{
    g_calls.renderer_submit_count++;
    if (request) {
        g_calls.last_renderer_request = *request;
        vgpu_virgl_execute_renderer_request(request);
    }
    return request != NULL;
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

    for (int i = 0; i < max_desc; i++) {
        if (vq_desc[i].flags & VIRTIO_DESC_F_WRITE)
            break;

        if (offset >= vq_desc[i].len) {
            offset -= vq_desc[i].len;
            continue;
        }

        size_t available = vq_desc[i].len - offset;
        size_t chunk = bytes - done;
        if (chunk > available)
            chunk = available;

        const void *src = virtio_gpu_mem_guest_to_host(
            vgpu, (uint32_t) (vq_desc[i].addr + offset), (uint32_t) chunk);
        if (!src)
            return VIRTIO_GPU_DESC_COPY_INVALID;

        memcpy((uint8_t *) buf + done, src, chunk);
        done += chunk;
        offset = 0;
        if (done == bytes)
            return VIRTIO_GPU_DESC_COPY_OK;
    }

    return VIRTIO_GPU_DESC_COPY_SHORT;
}

bool virtio_gpu_desc_readable_size(const struct virtq_desc *vq_desc,
                                   int max_desc,
                                   size_t *size)
{
    *size = 0;
    for (int i = 0; i < max_desc; i++) {
        if (vq_desc[i].flags & VIRTIO_DESC_F_WRITE)
            return true;
        *size += vq_desc[i].len;
    }
    return true;
}

void virtio_gpu_cmd_undefined_handler(virtio_gpu_state_t *vgpu,
                                      struct virtq_desc *vq_desc,
                                      uint32_t *plen)
{
    (void) vgpu;
    (void) vq_desc;
    g_calls.undefined_count++;
    *plen = 0;
}

static void test_sw_reset(virtio_gpu_state_t *vgpu)
{
    virtio_gpu_data_t *data = vgpu->priv;

    for (uint32_t i = 0; i < data->num_scanouts; i++) {
        struct virtio_gpu_scanout_info *scanout = &data->scanouts[i];
        if (!scanout->enabled)
            continue;

        scanout->primary_resource_id = 0;
        scanout->cursor_resource_id = 0;
        scanout->src_x = 0;
        scanout->src_y = 0;
        scanout->src_w = 0;
        scanout->src_h = 0;
        vgpu_display_publish_primary_clear(i);
        vgpu_display_publish_cursor_clear(i);
    }
}

const struct virtio_gpu_cmd_backend g_virtio_gpu_sw_backend = {
    .init = NULL,
    .poll = NULL,
    .reset = test_sw_reset,
    .get_display_info = virtio_gpu_cmd_undefined_handler,
    .resource_create_2d = virtio_gpu_cmd_undefined_handler,
    .resource_unref = virtio_gpu_cmd_undefined_handler,
    .set_scanout = virtio_gpu_cmd_undefined_handler,
    .resource_flush = virtio_gpu_cmd_undefined_handler,
    .transfer_to_host_2d = virtio_gpu_cmd_undefined_handler,
    .resource_attach_backing = virtio_gpu_cmd_undefined_handler,
    .resource_detach_backing = virtio_gpu_cmd_undefined_handler,
    .get_capset_info = virtio_gpu_cmd_undefined_handler,
    .get_capset = virtio_gpu_cmd_undefined_handler,
    .get_edid = virtio_gpu_cmd_undefined_handler,
    .resource_assign_uuid = virtio_gpu_cmd_undefined_handler,
    .resource_create_blob = virtio_gpu_cmd_undefined_handler,
    .set_scanout_blob = virtio_gpu_cmd_undefined_handler,
    .ctx_create = virtio_gpu_cmd_undefined_handler,
    .ctx_destroy = virtio_gpu_cmd_undefined_handler,
    .ctx_attach_resource = virtio_gpu_cmd_undefined_handler,
    .ctx_detach_resource = virtio_gpu_cmd_undefined_handler,
    .resource_create_3d = virtio_gpu_cmd_undefined_handler,
    .transfer_to_host_3d = virtio_gpu_cmd_undefined_handler,
    .transfer_from_host_3d = virtio_gpu_cmd_undefined_handler,
    .submit_3d = virtio_gpu_cmd_undefined_handler,
    .resource_map_blob = virtio_gpu_cmd_undefined_handler,
    .resource_unmap_blob = virtio_gpu_cmd_undefined_handler,
    .update_cursor = virtio_gpu_cmd_undefined_handler,
    .move_cursor = virtio_gpu_cmd_undefined_handler,
};

virgl_renderer_gl_context vgpu_window_virgl_create_context(
    int scanout_idx,
    struct virgl_renderer_gl_ctx_param *param)
{
    (void) scanout_idx;
    (void) param;
    return (void *) 0x1;
}

void vgpu_window_virgl_destroy_context(virgl_renderer_gl_context ctx)
{
    (void) ctx;
}

int vgpu_window_virgl_make_current(int scanout_idx,
                                   virgl_renderer_gl_context ctx)
{
    (void) scanout_idx;
    g_calls.window_make_current_count++;
    g_calls.window_make_current_last_ctx = ctx;
    if (!ctx)
        g_calls.window_make_current_null_count++;
    return 0;
}

void vgpu_gl_lock(void)
{
    g_calls.gl_lock_depth++;
    if (g_calls.gl_lock_depth > g_calls.gl_lock_max_depth)
        g_calls.gl_lock_max_depth = g_calls.gl_lock_depth;
}

void vgpu_gl_unlock(void)
{
    if (g_calls.gl_lock_depth <= 0) {
        g_calls.gl_lock_underflow_count++;
        return;
    }
    g_calls.gl_lock_depth--;
}

int virgl_renderer_init(void *cookie,
                        int flags,
                        struct virgl_renderer_callbacks *cb)
{
    g_calls.renderer_init_count++;
    g_calls.renderer_init_cookie = cookie;
    g_calls.renderer_init_flags = flags;
    g_calls.renderer_init_callback_version = cb ? cb->version : 0;
    g_calls.renderer_init_lock_depth = g_calls.gl_lock_depth;
    if (cb)
        g_renderer_callbacks = *cb;
    g_renderer_cookie = cookie;
    return 0;
}

void virgl_renderer_poll(void)
{
    g_calls.poll_count++;
}

int virgl_renderer_create_fence(int client_fence_id, uint32_t ctx_id)
{
    g_calls.create_fence_count++;
    g_calls.create_fence_client_id = client_fence_id;
    g_calls.create_fence_ctx_id = ctx_id;
    if (g_create_fence_completes_sync && g_renderer_callbacks.write_fence)
        g_renderer_callbacks.write_fence(g_renderer_cookie,
                                         (uint32_t) client_fence_id);
    return 0;
}

int virgl_renderer_context_create_fence(uint32_t ctx_id,
                                        uint32_t flags,
                                        uint32_t ring_idx,
                                        uint64_t fence_id)
{
    g_calls.context_create_fence_count++;
    g_calls.context_create_fence_ctx_id = ctx_id;
    g_calls.context_create_fence_flags = flags;
    g_calls.context_create_fence_ring_idx = ring_idx;
    g_calls.context_create_fence_id = fence_id;
    if (g_create_fence_completes_sync &&
        g_renderer_callbacks.write_context_fence)
        g_renderer_callbacks.write_context_fence(g_renderer_cookie, ctx_id,
                                                 ring_idx, fence_id);
    return 0;
}

void virgl_renderer_get_cap_set(uint32_t set,
                                uint32_t *max_ver,
                                uint32_t *max_size)
{
    *max_ver = set == VIRTIO_GPU_CAPSET_VIRGL2 ? 2 : 1;
    *max_size = 64;
}

void virgl_renderer_fill_caps(uint32_t set, uint32_t version, void *caps)
{
    memset(caps, (int) (0xa0u + set + version), 64);
}

void virgl_renderer_reset(void)
{
    g_calls.reset_primary_clear_count = g_publish_primary_clear_count;
    g_calls.reset_cursor_clear_count = g_publish_cursor_clear_count;
    g_calls.reset_primary_resource_id =
        g_vgpu_data.scanouts[0].primary_resource_id;
    g_calls.reset_cursor_resource_id =
        g_vgpu_data.scanouts[0].cursor_resource_id;
    g_calls.reset_count++;
}

int virgl_renderer_context_create(uint32_t handle,
                                  uint32_t nlen,
                                  const char *name)
{
    g_calls.context_create_count++;
    g_calls.context_create_handle = handle;
    g_calls.context_create_nlen = nlen;
    memcpy(g_calls.context_create_name, name, nlen);
    return 0;
}

void virgl_renderer_context_destroy(uint32_t handle)
{
    g_calls.context_destroy_count++;
    g_calls.context_destroy_handle = handle;
}

int virgl_renderer_context_create_with_flags(uint32_t ctx_id,
                                             uint32_t ctx_flags,
                                             uint32_t nlen,
                                             const char *name)
{
    (void) ctx_id;
    (void) ctx_flags;
    (void) nlen;
    (void) name;
    return 0;
}

void virgl_renderer_ctx_attach_resource(int ctx_id, int res_handle)
{
    g_calls.ctx_attach_count++;
    g_calls.ctx_attach_ctx_id = ctx_id;
    g_calls.ctx_attach_res_handle = res_handle;
}

void virgl_renderer_ctx_detach_resource(int ctx_id, int res_handle)
{
    g_calls.ctx_detach_count++;
    g_calls.ctx_detach_ctx_id = ctx_id;
    g_calls.ctx_detach_res_handle = res_handle;
}

int virgl_renderer_resource_create(
    struct virgl_renderer_resource_create_args *args,
    struct iovec *iov,
    uint32_t num_iovs)
{
    g_calls.resource_create_count++;
    g_calls.resource_create_args = *args;
    (void) iov;
    (void) num_iovs;
    return 0;
}

void virgl_renderer_resource_unref(uint32_t res_handle)
{
    g_calls.resource_unref_count++;
    g_calls.resource_unref_handle = res_handle;
    g_calls.resource_unref_primary_clear_count = g_publish_primary_clear_count;
    g_calls.resource_unref_primary_resource_id =
        g_vgpu_data.scanouts[0].primary_resource_id;
}

int virgl_renderer_resource_get_info(int res_handle,
                                     struct virgl_renderer_resource_info *info)
{
    g_calls.resource_get_info_count++;
    g_calls.resource_get_info_handle = res_handle;
    memset(info, 0, sizeof(*info));
    info->handle = (uint32_t) res_handle;
    info->width = 640;
    info->height = 480;
    info->flags = 1U;
    info->tex_id = 1234;
    return 0;
}

int virgl_renderer_resource_attach_iov(int res_handle,
                                       struct iovec *iov,
                                       int num_iovs)
{
    g_calls.resource_attach_iov_count++;
    g_calls.resource_attach_iov_handle = res_handle;
    g_calls.resource_attach_iov_num_iovs = num_iovs;
    g_calls.attached_iov = iov;
    g_calls.attached_iov_count = num_iovs;
    for (int i = 0; i < num_iovs && i < 4; i++) {
        g_calls.attached_iov_base[i] = iov[i].iov_base;
        g_calls.attached_iov_len[i] = iov[i].iov_len;
    }
    return num_iovs >= 0 ? 0 : -1;
}

void virgl_renderer_resource_detach_iov(int res_handle,
                                        struct iovec **iov,
                                        int *num_iovs)
{
    g_calls.resource_detach_iov_count++;
    g_calls.resource_detach_iov_handle = res_handle;
    *iov = g_calls.attached_iov;
    *num_iovs = g_calls.attached_iov_count;
    g_calls.attached_iov = NULL;
    g_calls.attached_iov_count = 0;
}

int virgl_renderer_transfer_write_iov(uint32_t handle,
                                      uint32_t ctx_id,
                                      int level,
                                      uint32_t stride,
                                      uint32_t layer_stride,
                                      struct virgl_box *box,
                                      uint64_t offset,
                                      struct iovec *iovec,
                                      unsigned int iovec_cnt)
{
    g_calls.transfer_write_count++;
    g_calls.transfer_write_handle = handle;
    g_calls.transfer_write_ctx_id = ctx_id;
    g_calls.transfer_write_level = level;
    g_calls.transfer_write_stride = stride;
    g_calls.transfer_write_layer_stride = layer_stride;
    g_calls.transfer_write_box = *box;
    g_calls.transfer_write_offset = offset;
    (void) iovec;
    g_calls.transfer_write_iov_cnt = iovec_cnt;
    return 0;
}

int virgl_renderer_transfer_read_iov(uint32_t handle,
                                     uint32_t ctx_id,
                                     uint32_t level,
                                     uint32_t stride,
                                     uint32_t layer_stride,
                                     struct virgl_box *box,
                                     uint64_t offset,
                                     struct iovec *iov,
                                     int iovec_cnt)
{
    g_calls.transfer_read_count++;
    g_calls.transfer_read_handle = handle;
    g_calls.transfer_read_ctx_id = ctx_id;
    g_calls.transfer_read_level = level;
    g_calls.transfer_read_stride = stride;
    g_calls.transfer_read_layer_stride = layer_stride;
    g_calls.transfer_read_box = *box;
    g_calls.transfer_read_offset = offset;
    (void) iov;
    g_calls.transfer_read_iov_cnt = iovec_cnt;
    return 0;
}

int virgl_renderer_submit_cmd(void *buffer, int ctx_id, int ndw)
{
    g_calls.submit_count++;
    g_calls.submit_ctx_id = ctx_id;
    g_calls.submit_ndw = ndw;
    memcpy(g_calls.submit_words, buffer, (size_t) ndw * sizeof(uint32_t));
    return 0;
}

void virgl_renderer_force_ctx_0(void)
{
    g_calls.force_ctx_0_count++;
    g_calls.force_ctx_0_lock_depth = g_calls.gl_lock_depth;
}

#include "../virtio-gpu-virgl.c"

static virtio_gpu_state_t fresh_vgpu(void)
{
    reset_fixture();

    virtio_gpu_state_t vgpu = test_vgpu();
    if (g_virtio_gpu_backend.reset)
        g_virtio_gpu_backend.reset(&vgpu);
    memset(&g_calls, 0, sizeof(g_calls));
    g_publish_primary_clear_count = 0;
    g_publish_cursor_clear_count = 0;
    return vgpu;
}

static struct virtio_gpu_ctrl_hdr *response_hdr(void)
{
    return (struct virtio_gpu_ctrl_hdr *) &g_ram[RESP_ADDR];
}

static bool ctrl_response_len_or_deferred(uint32_t plen)
{
    return plen == sizeof(struct virtio_gpu_ctrl_hdr) ||
           plen == VIRTIO_GPU_RESPONSE_DEFERRED;
}

static void init_desc_no_payload(struct virtq_desc desc[VIRTIO_GPU_MAX_DESC],
                                 size_t request_size)
{
    memset(desc, 0, sizeof(struct virtq_desc) * VIRTIO_GPU_MAX_DESC);
    desc[0] = (struct virtq_desc) {
        .addr = REQ_ADDR,
        .len = (uint32_t) request_size,
        .flags = VIRTIO_DESC_F_NEXT,
    };
    desc[1] = (struct virtq_desc) {
        .addr = RESP_ADDR,
        .len = sizeof(struct virtio_gpu_ctrl_hdr),
        .flags = VIRTIO_DESC_F_WRITE,
    };
}

static void init_desc_with_payload(struct virtq_desc desc[VIRTIO_GPU_MAX_DESC],
                                   size_t request_size,
                                   size_t payload_size)
{
    memset(desc, 0, sizeof(struct virtq_desc) * VIRTIO_GPU_MAX_DESC);
    desc[0] = (struct virtq_desc) {
        .addr = REQ_ADDR,
        .len = (uint32_t) request_size,
        .flags = VIRTIO_DESC_F_NEXT,
    };
    desc[1] = (struct virtq_desc) {
        .addr = PAYLOAD_ADDR,
        .len = (uint32_t) payload_size,
        .flags = VIRTIO_DESC_F_NEXT,
    };
    desc[2] = (struct virtq_desc) {
        .addr = RESP_ADDR,
        .len = sizeof(struct virtio_gpu_ctrl_hdr),
        .flags = VIRTIO_DESC_F_WRITE,
    };
}

static int create_virgl_resource(virtio_gpu_state_t *vgpu, uint32_t resource_id)
{
    struct virtio_gpu_resource_create_3d request = {
        .resource_id = resource_id,
        .target = 2,
        .format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
        .bind = 0x40,
        .width = 64,
        .height = 32,
        .depth = 1,
        .array_size = 1,
        .last_level = 0,
        .nr_samples = 0,
        .flags = 0x10,
    };
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(request));
    uint32_t plen = 0;

    g_virtio_gpu_backend.resource_create_3d(vgpu, desc, &plen);

    CHECK(ctrl_response_len_or_deferred(plen));
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);
    return 0;
}

static int test_ctx_create_calls_renderer(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    struct virtio_gpu_ctx_create request = {
        .hdr = {.ctx_id = 42},
        .nlen = 4,
        .debug_name = "mesa",
    };
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(request));
    uint32_t plen = 0;

    g_virtio_gpu_backend.ctx_create(&vgpu, desc, &plen);

    CHECK(g_calls.context_create_count == 1);
    CHECK(g_calls.context_create_handle == 42);
    CHECK(g_calls.context_create_nlen == 4);
    CHECK(memcmp(g_calls.context_create_name, "mesa", 4) == 0);
    CHECK(g_calls.undefined_count == 0);
    CHECK(ctrl_response_len_or_deferred(plen));
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    return 0;
}

static int test_renderer_init_calls_virgl_renderer_init(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();

    g_virtio_gpu_backend.init(&vgpu);

    CHECK(g_calls.renderer_init_count == 1);
    CHECK(g_calls.renderer_init_cookie == &vgpu);
    CHECK(g_calls.renderer_init_flags == VIRGL_RENDERER_THREAD_SYNC);
    CHECK(g_calls.renderer_init_callback_version ==
          VIRGL_RENDERER_CALLBACKS_VERSION);
    CHECK(g_calls.renderer_init_lock_depth == 0);
    CHECK(g_vgpu_data.num_capsets == 2);
    CHECK(g_calls.gl_lock_depth == 0);
    CHECK(g_calls.gl_lock_underflow_count == 0);

    return 0;
}

static int test_renderer_init_stays_on_gl_owner_without_backend_lock_hooks(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();

    g_virtio_gpu_backend.init(&vgpu);

    CHECK(g_calls.window_make_current_count == 1);
    CHECK(g_calls.window_make_current_null_count == 1);
    CHECK(g_calls.window_make_current_last_ctx == NULL);
    CHECK(g_calls.force_ctx_0_count == 0);
    CHECK(g_calls.gl_lock_depth == 0);
    CHECK(g_calls.gl_lock_underflow_count == 0);

    return 0;
}

static int test_backend_reports_available_capsets(void)
{
    reset_fixture();

    CHECK(virtio_gpu_backend_get_num_capsets() == 2);

    return 0;
}

static int test_backend_poll_without_pending_fence_does_not_poll_renderer(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();

    g_virtio_gpu_backend.poll(&vgpu);

    CHECK(g_calls.poll_count == 0);
    CHECK(g_calls.renderer_submit_count == 0);

    return 0;
}

static int test_renderer_poll_request_executes_on_gl_owner(void)
{
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_POLL,
    };

    vgpu_virgl_execute_renderer_request(&request);

    CHECK(g_calls.poll_count == 1);

    return 0;
}

static int test_submit_3d_copies_payload_to_renderer(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    struct virtio_gpu_cmd_submit request = {
        .hdr = {.ctx_id = 7},
        .size = 16,
    };
    uint32_t commands[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    memcpy(&g_ram[PAYLOAD_ADDR], commands, sizeof(commands));

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_with_payload(desc, sizeof(request), sizeof(commands));
    uint32_t plen = 0;

    g_virtio_gpu_backend.submit_3d(&vgpu, desc, &plen);

    CHECK(g_calls.submit_count == 1);
    CHECK(g_calls.submit_ctx_id == 7);
    CHECK(g_calls.submit_ndw == 4);
    CHECK(memcmp(g_calls.submit_words, commands, sizeof(commands)) == 0);
    CHECK(g_calls.undefined_count == 0);
    CHECK(ctrl_response_len_or_deferred(plen));
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    return 0;
}

static int test_fenced_submit_creates_renderer_fence_from_request_flags(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    struct virtio_gpu_cmd_submit request = {
        .hdr = {.flags = VIRTIO_GPU_FLAG_FENCE, .fence_id = 123, .ctx_id = 7},
        .size = 16,
    };
    uint32_t commands[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    memcpy(&g_ram[PAYLOAD_ADDR], commands, sizeof(commands));

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_with_payload(desc, sizeof(request), sizeof(commands));
    uint32_t plen = 0;

    g_virtio_gpu_backend.submit_3d(&vgpu, desc, &plen);

    CHECK(g_calls.submit_count == 1);
    CHECK(g_calls.create_fence_count == 1);
    CHECK(g_calls.create_fence_client_id == 123);
    CHECK(g_calls.create_fence_ctx_id == 0);
    CHECK(g_calls.context_create_fence_count == 0);
    CHECK(g_calls.poll_count == 1);
    CHECK(g_calls.renderer_submit_count == 2);
    CHECK(g_calls.last_renderer_request.type == VGPU_RENDERER_REQ_POLL);
    CHECK(ctrl_response_len_or_deferred(plen));
    CHECK(response_hdr()->type == 0);

    return 0;
}

static int test_synchronous_fence_callback_does_not_leave_poll_pending(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    struct virtio_gpu_cmd_submit request = {
        .hdr = {.flags = VIRTIO_GPU_FLAG_FENCE, .fence_id = 123, .ctx_id = 7},
        .size = 16,
    };
    uint32_t commands[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    memcpy(&g_ram[PAYLOAD_ADDR], commands, sizeof(commands));

    g_virtio_gpu_backend.init(&vgpu);
    memset(&g_calls, 0, sizeof(g_calls));
    g_create_fence_completes_sync = true;

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_with_payload(desc, sizeof(request), sizeof(commands));
    uint32_t plen = 0;

    g_virtio_gpu_backend.submit_3d(&vgpu, desc, &plen);

    CHECK(g_calls.submit_count == 1);
    CHECK(g_calls.create_fence_count == 1);
    CHECK(g_calls.renderer_complete_count == 1);
    CHECK(g_calls.last_renderer_completion.type == VGPU_RENDERER_DONE_FENCE);
    CHECK(g_calls.poll_count == 0);
    CHECK(g_calls.renderer_submit_count == 1);
    CHECK(ctrl_response_len_or_deferred(plen));
    CHECK(response_hdr()->type == 0);

    g_virtio_gpu_backend.poll(&vgpu);
    CHECK(g_calls.poll_count == 0);
    CHECK(g_calls.renderer_submit_count == 1);

    return 0;
}

static int test_descriptor_write_flag_does_not_create_renderer_fence(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    struct virtio_gpu_cmd_submit request = {
        .hdr = {.ctx_id = 7},
        .size = 16,
    };
    uint32_t commands[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    memcpy(&g_ram[PAYLOAD_ADDR], commands, sizeof(commands));

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_with_payload(desc, sizeof(request), sizeof(commands));
    uint32_t plen = 0;

    g_virtio_gpu_backend.submit_3d(&vgpu, desc, &plen);

    CHECK(g_calls.submit_count == 1);
    CHECK(g_calls.create_fence_count == 0);
    CHECK(g_calls.context_create_fence_count == 0);
    CHECK(g_calls.poll_count == 0);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(response_hdr()->flags == 0);

    return 0;
}

static int test_ring_idx_fence_uses_context_fence_api(void)
{
    const uint32_t test_info_ring_idx_flag = 1U << 1;
    virtio_gpu_state_t vgpu = fresh_vgpu();
    struct virtio_gpu_cmd_submit request = {
        .hdr = {.flags = VIRTIO_GPU_FLAG_FENCE | test_info_ring_idx_flag,
                .fence_id = 0x123456789ULL,
                .ctx_id = 77,
                .ring_idx = 3},
        .size = 16,
    };
    uint32_t commands[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    memcpy(&g_ram[PAYLOAD_ADDR], commands, sizeof(commands));

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_with_payload(desc, sizeof(request), sizeof(commands));
    uint32_t plen = 0;

    g_virtio_gpu_backend.submit_3d(&vgpu, desc, &plen);

    CHECK(g_calls.submit_count == 1);
    CHECK(g_calls.create_fence_count == 0);
    CHECK(g_calls.context_create_fence_count == 1);
    CHECK(g_calls.context_create_fence_ctx_id == 77);
    CHECK(g_calls.context_create_fence_flags ==
          VIRGL_RENDERER_FENCE_FLAG_MERGEABLE);
    CHECK(g_calls.context_create_fence_ring_idx == 3);
    CHECK(g_calls.context_create_fence_id == 0x123456789ULL);
    CHECK(g_calls.poll_count == 1);
    CHECK(g_calls.renderer_submit_count == 2);
    CHECK(g_calls.last_renderer_request.type == VGPU_RENDERER_REQ_POLL);
    CHECK(response_hdr()->type == 0);

    return 0;
}

static int test_fence_callbacks_record_state_and_reset_clears_it(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();

    vgpu_virgl_write_fence(&vgpu, 55);
    CHECK(g_calls.renderer_complete_count == 1);
    CHECK(g_calls.last_renderer_completion.type == VGPU_RENDERER_DONE_FENCE);
    CHECK(g_calls.last_renderer_completion.context_fence == false);
    CHECK(g_calls.last_renderer_completion.fence_id == 55);

    vgpu_virgl_write_context_fence(&vgpu, 77, 3, 0x123456789ULL);
    CHECK(g_calls.renderer_complete_count == 2);
    CHECK(g_calls.last_renderer_completion.type == VGPU_RENDERER_DONE_FENCE);
    CHECK(g_calls.last_renderer_completion.context_fence == true);
    CHECK(g_calls.last_renderer_completion.ctx_id == 77);
    CHECK(g_calls.last_renderer_completion.ring_idx == 3);
    CHECK(g_calls.last_renderer_completion.fence_id == 0x123456789ULL);

    CHECK(g_vgpu_virgl_fences.last_ctx0_fence == 55);
    CHECK(g_vgpu_virgl_fences.last_context_ctx_id == 77);
    CHECK(g_vgpu_virgl_fences.last_context_ring_idx == 3);
    CHECK(g_vgpu_virgl_fences.last_context_fence == 0x123456789ULL);

    g_virtio_gpu_backend.reset(&vgpu);

    CHECK(g_vgpu_virgl_fences.last_ctx0_fence == 0);
    CHECK(g_vgpu_virgl_fences.last_context_ctx_id == 0);
    CHECK(g_vgpu_virgl_fences.last_context_ring_idx == 0);
    CHECK(g_vgpu_virgl_fences.last_context_fence == 0);

    return 0;
}

static int test_context_lifecycle_handlers_call_renderer(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();

    struct virtio_gpu_ctx_destroy destroy = {
        .hdr = {.ctx_id = 11},
    };
    memcpy(&g_ram[REQ_ADDR], &destroy, sizeof(destroy));
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(destroy));
    uint32_t plen = 0;

    g_virtio_gpu_backend.ctx_destroy(&vgpu, desc, &plen);
    CHECK(g_calls.context_destroy_count == 1);
    CHECK(g_calls.context_destroy_handle == 11);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    struct virtio_gpu_ctx_resource attach = {
        .hdr = {.ctx_id = 12},
        .resource_id = 88,
    };
    memcpy(&g_ram[REQ_ADDR], &attach, sizeof(attach));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(attach));
    plen = 0;

    g_virtio_gpu_backend.ctx_attach_resource(&vgpu, desc, &plen);
    CHECK(g_calls.ctx_attach_count == 1);
    CHECK(g_calls.ctx_attach_ctx_id == 12);
    CHECK(g_calls.ctx_attach_res_handle == 88);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    struct virtio_gpu_ctx_resource detach = {
        .hdr = {.ctx_id = 13},
        .resource_id = 89,
    };
    memcpy(&g_ram[REQ_ADDR], &detach, sizeof(detach));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(detach));
    plen = 0;

    g_virtio_gpu_backend.ctx_detach_resource(&vgpu, desc, &plen);
    CHECK(g_calls.ctx_detach_count == 1);
    CHECK(g_calls.ctx_detach_ctx_id == 13);
    CHECK(g_calls.ctx_detach_res_handle == 89);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    return 0;
}

static int test_ctx_create_rejects_context_init_until_feature_enabled(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    struct virtio_gpu_ctx_create request = {
        .hdr = {.ctx_id = 43},
        .nlen = 4,
        .context_init = VIRTIO_GPU_CAPSET_VIRGL,
        .debug_name = "mesa",
    };
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(request));
    uint32_t plen = 0;

    g_virtio_gpu_backend.ctx_create(&vgpu, desc, &plen);

    CHECK(g_calls.context_create_count == 0);
    CHECK(plen == sizeof(struct virtio_gpu_ctrl_hdr));
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_ERR_UNSPEC);

    return 0;
}

static int test_resource_create_3d_calls_renderer(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    struct virtio_gpu_resource_create_3d request = {
        .resource_id = 55,
        .target = 2,
        .format = VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM,
        .bind = 0x99,
        .width = 320,
        .height = 240,
        .depth = 1,
        .array_size = 2,
        .last_level = 3,
        .nr_samples = 4,
        .flags = 0x10,
    };
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(request));
    uint32_t plen = 0;

    g_virtio_gpu_backend.resource_create_3d(&vgpu, desc, &plen);

    CHECK(g_calls.resource_create_count == 1);
    CHECK(g_calls.resource_create_args.handle == 55);
    CHECK(g_calls.resource_create_args.target == 2);
    CHECK(g_calls.resource_create_args.format ==
          VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM);
    CHECK(g_calls.resource_create_args.bind == 0x99);
    CHECK(g_calls.resource_create_args.width == 320);
    CHECK(g_calls.resource_create_args.height == 240);
    CHECK(g_calls.resource_create_args.depth == 1);
    CHECK(g_calls.resource_create_args.array_size == 2);
    CHECK(g_calls.resource_create_args.last_level == 3);
    CHECK(g_calls.resource_create_args.nr_samples == 4);
    CHECK(g_calls.resource_create_args.flags == 0x10);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    return 0;
}

static int test_resource_backing_attach_detach_and_unref(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 61) == 0);
    memset(&g_calls, 0, sizeof(g_calls));

    struct virtio_gpu_res_attach_backing request = {
        .resource_id = 61,
        .nr_entries = 2,
    };
    struct virtio_gpu_mem_entry entries[2] = {
        {.addr = 1536, .length = 64},
        {.addr = 2048, .length = 128},
    };
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    memcpy(&g_ram[PAYLOAD_ADDR], entries, sizeof(entries));

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_with_payload(desc, sizeof(request), sizeof(entries));
    uint32_t plen = 0;

    g_virtio_gpu_backend.resource_attach_backing(&vgpu, desc, &plen);

    CHECK(g_calls.resource_attach_iov_count == 1);
    CHECK(g_calls.resource_attach_iov_handle == 61);
    CHECK(g_calls.resource_attach_iov_num_iovs == 2);
    CHECK(g_calls.attached_iov_base[0] == &g_ram[1536]);
    CHECK(g_calls.attached_iov_len[0] == 64);
    CHECK(g_calls.attached_iov_base[1] == &g_ram[2048]);
    CHECK(g_calls.attached_iov_len[1] == 128);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    struct virtio_gpu_res_detach_backing detach = {
        .resource_id = 61,
    };
    memcpy(&g_ram[REQ_ADDR], &detach, sizeof(detach));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(detach));
    plen = 0;

    g_virtio_gpu_backend.resource_detach_backing(&vgpu, desc, &plen);

    CHECK(g_calls.resource_detach_iov_count == 1);
    CHECK(g_calls.resource_detach_iov_handle == 61);
    CHECK(g_calls.attached_iov == NULL);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    struct virtio_gpu_res_unref unref = {
        .resource_id = 61,
    };
    memcpy(&g_ram[REQ_ADDR], &unref, sizeof(unref));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(unref));
    plen = 0;

    g_virtio_gpu_backend.resource_unref(&vgpu, desc, &plen);

    CHECK(g_calls.resource_unref_count == 1);
    CHECK(g_calls.resource_unref_handle == 61);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    return 0;
}

static int test_resource_unref_clears_bound_gl_scanout_before_unref(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 62) == 0);

    g_vgpu_data.scanouts[0].primary_resource_id = 62;
    g_vgpu_data.scanouts[0].src_x = 3;
    g_vgpu_data.scanouts[0].src_y = 4;
    g_vgpu_data.scanouts[0].src_w = 320;
    g_vgpu_data.scanouts[0].src_h = 200;
    memset(&g_calls, 0, sizeof(g_calls));
    g_publish_primary_clear_count = 0;

    struct virtio_gpu_res_unref request = {
        .resource_id = 62,
    };
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(request));
    uint32_t plen = 0;

    g_virtio_gpu_backend.resource_unref(&vgpu, desc, &plen);

    CHECK(g_calls.resource_unref_count == 1);
    CHECK(g_calls.resource_unref_handle == 62);
    CHECK(g_calls.resource_unref_primary_clear_count == 1);
    CHECK(g_calls.resource_unref_primary_resource_id == 0);
    CHECK(g_publish_primary_clear_count == 1);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);
    CHECK(g_vgpu_data.scanouts[0].src_x == 0);
    CHECK(g_vgpu_data.scanouts[0].src_y == 0);
    CHECK(g_vgpu_data.scanouts[0].src_w == 0);
    CHECK(g_vgpu_data.scanouts[0].src_h == 0);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    return 0;
}

static int test_reset_clears_scanout_before_renderer_reset(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 63) == 0);

    g_vgpu_data.scanouts[0].primary_resource_id = 63;
    g_vgpu_data.scanouts[0].cursor_resource_id = 44;
    g_vgpu_data.scanouts[0].src_x = 5;
    g_vgpu_data.scanouts[0].src_y = 6;
    g_vgpu_data.scanouts[0].src_w = 640;
    g_vgpu_data.scanouts[0].src_h = 480;
    g_vgpu_data.num_capsets = 99;
    memset(&g_calls, 0, sizeof(g_calls));
    g_publish_primary_clear_count = 0;
    g_publish_cursor_clear_count = 0;

    g_virtio_gpu_backend.reset(&vgpu);

    CHECK(g_calls.reset_count == 1);
    CHECK(g_calls.reset_primary_clear_count == 1);
    CHECK(g_calls.reset_cursor_clear_count == 1);
    CHECK(g_calls.reset_primary_resource_id == 0);
    CHECK(g_calls.reset_cursor_resource_id == 0);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);
    CHECK(g_vgpu_data.scanouts[0].cursor_resource_id == 0);
    CHECK(g_vgpu_data.scanouts[0].src_x == 0);
    CHECK(g_vgpu_data.scanouts[0].src_y == 0);
    CHECK(g_vgpu_data.scanouts[0].src_w == 0);
    CHECK(g_vgpu_data.scanouts[0].src_h == 0);
    CHECK(g_vgpu_data.num_capsets == 2);

    return 0;
}

static int test_3d_transfers_call_renderer(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 71) == 0);
    memset(&g_calls, 0, sizeof(g_calls));

    struct virtio_gpu_transfer_host_3d to_host = {
        .hdr = {.ctx_id = 5},
        .box = {.x = 1, .y = 2, .z = 3, .w = 4, .h = 5, .d = 6},
        .offset = 0x123456789ULL,
        .resource_id = 71,
        .level = 7,
        .stride = 256,
        .layer_stride = 4096,
    };
    memcpy(&g_ram[REQ_ADDR], &to_host, sizeof(to_host));
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(to_host));
    uint32_t plen = 0;

    g_virtio_gpu_backend.transfer_to_host_3d(&vgpu, desc, &plen);

    CHECK(g_calls.transfer_write_count == 1);
    CHECK(g_calls.transfer_write_handle == 71);
    CHECK(g_calls.transfer_write_ctx_id == 5);
    CHECK(g_calls.transfer_write_level == 7);
    CHECK(g_calls.transfer_write_stride == 256);
    CHECK(g_calls.transfer_write_layer_stride == 4096);
    CHECK(g_calls.transfer_write_box.x == 1);
    CHECK(g_calls.transfer_write_box.y == 2);
    CHECK(g_calls.transfer_write_box.z == 3);
    CHECK(g_calls.transfer_write_box.w == 4);
    CHECK(g_calls.transfer_write_box.h == 5);
    CHECK(g_calls.transfer_write_box.d == 6);
    CHECK(g_calls.transfer_write_offset == 0x123456789ULL);
    CHECK(g_calls.transfer_write_iov_cnt == 0);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    struct virtio_gpu_transfer_host_3d from_host = {
        .hdr = {.ctx_id = 6},
        .box = {.x = 9, .y = 8, .z = 7, .w = 6, .h = 5, .d = 4},
        .offset = 0xfeed,
        .resource_id = 71,
        .level = 3,
        .stride = 512,
        .layer_stride = 8192,
    };
    memcpy(&g_ram[REQ_ADDR], &from_host, sizeof(from_host));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(from_host));
    plen = 0;

    g_virtio_gpu_backend.transfer_from_host_3d(&vgpu, desc, &plen);

    CHECK(g_calls.transfer_read_count == 1);
    CHECK(g_calls.transfer_read_handle == 71);
    CHECK(g_calls.transfer_read_ctx_id == 6);
    CHECK(g_calls.transfer_read_level == 3);
    CHECK(g_calls.transfer_read_stride == 512);
    CHECK(g_calls.transfer_read_layer_stride == 8192);
    CHECK(g_calls.transfer_read_box.x == 9);
    CHECK(g_calls.transfer_read_box.y == 8);
    CHECK(g_calls.transfer_read_box.z == 7);
    CHECK(g_calls.transfer_read_box.w == 6);
    CHECK(g_calls.transfer_read_box.h == 5);
    CHECK(g_calls.transfer_read_box.d == 4);
    CHECK(g_calls.transfer_read_offset == 0xfeed);
    CHECK(g_calls.transfer_read_iov_cnt == 0);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    return 0;
}

static int test_transfer_to_host_2d_routes_by_resource_owner(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 81) == 0);
    memset(&g_calls, 0, sizeof(g_calls));

    struct virtio_gpu_trans_to_host_2d request = {
        .r = {.x = 10, .y = 11, .width = 12, .height = 13},
        .offset = 0x44,
        .resource_id = 81,
    };
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(request));
    uint32_t plen = 0;

    g_virtio_gpu_backend.transfer_to_host_2d(&vgpu, desc, &plen);

    CHECK(g_calls.transfer_write_count == 1);
    CHECK(g_calls.transfer_write_handle == 81);
    CHECK(g_calls.transfer_write_ctx_id == 0);
    CHECK(g_calls.transfer_write_level == 0);
    CHECK(g_calls.transfer_write_stride == 0);
    CHECK(g_calls.transfer_write_layer_stride == 0);
    CHECK(g_calls.transfer_write_box.x == 10);
    CHECK(g_calls.transfer_write_box.y == 11);
    CHECK(g_calls.transfer_write_box.z == 0);
    CHECK(g_calls.transfer_write_box.w == 12);
    CHECK(g_calls.transfer_write_box.h == 13);
    CHECK(g_calls.transfer_write_box.d == 1);
    CHECK(g_calls.transfer_write_offset == 0x44);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    request.resource_id = 999;
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(request));
    plen = 0;

    g_virtio_gpu_backend.transfer_to_host_2d(&vgpu, desc, &plen);

    CHECK(g_calls.undefined_count == 1);
    CHECK(plen == 0);

    return 0;
}

static int test_set_scanout_publishes_gl_payload_for_virgl_resource(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 91) == 0);
    memset(&g_calls, 0, sizeof(g_calls));

    struct virtio_gpu_set_scanout request = {
        .r = {.x = 4, .y = 5, .width = 320, .height = 200},
        .scanout_id = 0,
        .resource_id = 91,
    };
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(request));
    uint32_t plen = 0;

    g_virtio_gpu_backend.set_scanout(&vgpu, desc, &plen);

    CHECK(g_calls.resource_get_info_count == 1);
    CHECK(g_calls.resource_get_info_handle == 91);
    CHECK(g_calls.force_ctx_0_count == 1);
    CHECK(g_publish_primary_set_count == 1);
    CHECK(g_last_primary_scanout == 0);
    CHECK(g_last_primary_payload != NULL);
    CHECK(g_last_primary_payload->kind == VGPU_DISPLAY_PAYLOAD_GL_SCANOUT);
    CHECK(g_last_primary_payload->gl.texture_id == 1234);
    CHECK(g_last_primary_payload->gl.width == 640);
    CHECK(g_last_primary_payload->gl.height == 480);
    CHECK(g_last_primary_payload->gl.src_x == 4);
    CHECK(g_last_primary_payload->gl.src_y == 5);
    CHECK(g_last_primary_payload->gl.src_w == 320);
    CHECK(g_last_primary_payload->gl.src_h == 200);
    CHECK(g_last_primary_payload->gl.y_0_top);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 91);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    return 0;
}

static int test_resource_flush_republishes_bound_gl_payload(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 92) == 0);

    g_vgpu_data.scanouts[0].primary_resource_id = 92;
    g_vgpu_data.scanouts[0].src_x = 7;
    g_vgpu_data.scanouts[0].src_y = 8;
    g_vgpu_data.scanouts[0].src_w = 300;
    g_vgpu_data.scanouts[0].src_h = 180;
    memset(&g_calls, 0, sizeof(g_calls));

    struct virtio_gpu_res_flush request = {
        .r = {.x = 0, .y = 0, .width = 300, .height = 180},
        .resource_id = 92,
    };
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(request));
    uint32_t plen = 0;

    g_virtio_gpu_backend.resource_flush(&vgpu, desc, &plen);

    CHECK(g_calls.resource_get_info_count == 1);
    CHECK(g_calls.resource_get_info_handle == 92);
    CHECK(g_calls.force_ctx_0_count == 1);
    CHECK(g_publish_primary_set_count == 1);
    CHECK(g_last_primary_payload != NULL);
    CHECK(g_last_primary_payload->kind == VGPU_DISPLAY_PAYLOAD_GL_SCANOUT);
    CHECK(g_last_primary_payload->gl.texture_id == 1234);
    CHECK(g_last_primary_payload->gl.src_x == 7);
    CHECK(g_last_primary_payload->gl.src_y == 8);
    CHECK(g_last_primary_payload->gl.src_w == 300);
    CHECK(g_last_primary_payload->gl.src_h == 180);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    return 0;
}

static int test_submit_3d_rejects_unaligned_size(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    struct virtio_gpu_cmd_submit request = {
        .hdr = {.ctx_id = 7},
        .size = 15,
    };
    uint32_t commands[4] = {0};
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    memcpy(&g_ram[PAYLOAD_ADDR], commands, sizeof(commands));

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_with_payload(desc, sizeof(request), sizeof(commands));
    uint32_t plen = 0;

    g_virtio_gpu_backend.submit_3d(&vgpu, desc, &plen);

    CHECK(g_calls.submit_count == 0);
    CHECK(plen == sizeof(struct virtio_gpu_ctrl_hdr));
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);

    return 0;
}

int main(void)
{
    CHECK(test_renderer_init_calls_virgl_renderer_init() == 0);
    CHECK(test_renderer_init_stays_on_gl_owner_without_backend_lock_hooks() ==
          0);
    CHECK(test_backend_reports_available_capsets() == 0);
    CHECK(test_backend_poll_without_pending_fence_does_not_poll_renderer() == 0);
    CHECK(test_renderer_poll_request_executes_on_gl_owner() == 0);
    CHECK(test_ctx_create_calls_renderer() == 0);
    CHECK(test_context_lifecycle_handlers_call_renderer() == 0);
    CHECK(test_ctx_create_rejects_context_init_until_feature_enabled() == 0);
    CHECK(test_resource_create_3d_calls_renderer() == 0);
    CHECK(test_resource_backing_attach_detach_and_unref() == 0);
    CHECK(test_resource_unref_clears_bound_gl_scanout_before_unref() == 0);
    CHECK(test_reset_clears_scanout_before_renderer_reset() == 0);
    CHECK(test_3d_transfers_call_renderer() == 0);
    CHECK(test_transfer_to_host_2d_routes_by_resource_owner() == 0);
    CHECK(test_set_scanout_publishes_gl_payload_for_virgl_resource() == 0);
    CHECK(test_resource_flush_republishes_bound_gl_payload() == 0);
    CHECK(test_submit_3d_copies_payload_to_renderer() == 0);
    CHECK(test_fenced_submit_creates_renderer_fence_from_request_flags() == 0);
    CHECK(test_synchronous_fence_callback_does_not_leave_poll_pending() == 0);
    CHECK(test_descriptor_write_flag_does_not_create_renderer_fence() == 0);
    CHECK(test_ring_idx_fence_uses_context_fence_api() == 0);
    CHECK(test_fence_callbacks_record_state_and_reset_clears_it() == 0);
    CHECK(test_submit_3d_rejects_unaligned_size() == 0);
    return 0;
}
