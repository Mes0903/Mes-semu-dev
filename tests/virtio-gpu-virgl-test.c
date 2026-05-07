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
#define TEST_SW_RESOURCE_ID 0x5a5aU

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
    int resource_create_ret;

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
    int submit_force_ctx_0_count;
    uint32_t submit_words[16];

    int force_ctx_0_count;
    int window_make_current_count;
    int window_make_current_null_count;
    virgl_renderer_gl_context window_make_current_last_ctx;
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
static bool g_renderer_submit_autorun;
static bool g_sw_interleaved_renderer_request_active;
static struct vgpu_renderer_request g_sw_interleaved_renderer_request;
static void (*g_after_resource_get_info_hook)(void);
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
    g_renderer_submit_autorun = true;
    g_sw_interleaved_renderer_request_active = false;
    memset(&g_sw_interleaved_renderer_request, 0,
           sizeof(g_sw_interleaved_renderer_request));
    g_after_resource_get_info_hook = NULL;
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

bool vgpu_display_publish_primary_set_guarded(
    uint32_t scanout_id,
    struct vgpu_display_payload *payload,
    const uint32_t *guard_generation,
    uint32_t guard_expected)
{
    if (guard_generation &&
        __atomic_load_n(guard_generation, __ATOMIC_ACQUIRE) != guard_expected) {
        free(payload);
        return false;
    }

    free(g_last_primary_payload);
    g_last_primary_payload = payload;
    g_last_primary_scanout = scanout_id;
    g_publish_primary_set_count++;
    return true;
}

void vgpu_display_publish_primary_set(uint32_t scanout_id,
                                      struct vgpu_display_payload *payload)
{
    (void) vgpu_display_publish_primary_set_guarded(scanout_id, payload, NULL,
                                                    0);
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
    if (!vgpu || !vgpu->priv)
        return 0;

    return ((virtio_gpu_data_t *) vgpu->priv)->ctrl_generation;
}

void virtio_gpu_set_num_capsets(virtio_gpu_state_t *vgpu, uint32_t num_capsets)
{
    if (!vgpu || !vgpu->priv)
        return;

    ((virtio_gpu_data_t *) vgpu->priv)->num_capsets = num_capsets;
}

bool virtio_gpu_cancel_ctrl_response(virtio_gpu_state_t *vgpu,
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
void vgpu_virgl_apply_renderer_side_effect(
    virtio_gpu_state_t *vgpu,
    const struct vgpu_renderer_completion *completion);

void virtio_gpu_set_primary_scanout_will_change_hook(
    virtio_gpu_state_t *vgpu,
    virtio_gpu_primary_scanout_will_change_func hook,
    void *opaque)
{
    virtio_gpu_data_t *data = vgpu->priv;
    data->primary_scanout_will_change = hook;
    data->primary_scanout_will_change_opaque = opaque;
}

void virtio_gpu_notify_primary_scanout_will_change(virtio_gpu_state_t *vgpu,
                                                   uint32_t scanout_id)
{
    virtio_gpu_data_t *data = vgpu->priv;
    if (!data->primary_scanout_will_change)
        return;

    data->primary_scanout_will_change(vgpu, scanout_id,
                                      data->primary_scanout_will_change_opaque);
}

static void test_run_sw_interleaved_renderer_request(void)
{
    if (!g_sw_interleaved_renderer_request_active)
        return;

    g_sw_interleaved_renderer_request_active = false;
    vgpu_virgl_execute_renderer_request(&g_sw_interleaved_renderer_request);
}

bool vgpu_renderer_complete(const struct vgpu_renderer_completion *completion)
{
    g_calls.renderer_complete_count++;
    if (completion)
        g_calls.last_renderer_completion = *completion;
    if (!completion)
        return false;
    if (completion->type == VGPU_RENDERER_DONE_VIRGL_RESOURCE) {
        for (size_t i = 0; i < ARRAY_SIZE(g_pending_ctrls); i++) {
            if (!g_pending_ctrls[i].active ||
                g_pending_ctrls[i].token_id != completion->token.id)
                continue;
            vgpu_virgl_apply_renderer_side_effect(g_pending_ctrls[i].vgpu,
                                                  completion);
            break;
        }
        return true;
    }
    if (completion->type != VGPU_RENDERER_DONE_CTRL || !completion->token.id)
        return completion != NULL;

    for (size_t i = 0; i < ARRAY_SIZE(g_pending_ctrls); i++) {
        if (!g_pending_ctrls[i].active ||
            g_pending_ctrls[i].token_id != completion->token.id)
            continue;

        vgpu_virgl_apply_renderer_side_effect(g_pending_ctrls[i].vgpu,
                                              completion);
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
        if (g_renderer_submit_autorun)
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

static const struct virtq_desc *test_response_desc(struct virtq_desc *vq_desc,
                                                   uint32_t *plen)
{
    int resp_idx = virtio_gpu_get_response_desc(
        vq_desc, VIRTIO_GPU_MAX_DESC, sizeof(struct virtio_gpu_ctrl_hdr));
    if (resp_idx < 0) {
        *plen = 0;
        return NULL;
    }
    return &vq_desc[resp_idx];
}

static void test_sw_resource_unref(virtio_gpu_state_t *vgpu,
                                   struct virtq_desc *vq_desc,
                                   uint32_t *plen)
{
    const struct virtio_gpu_res_unref *request =
        virtio_gpu_get_request(vgpu, vq_desc, sizeof(*request));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    const struct virtq_desc *response_desc = test_response_desc(vq_desc, plen);
    if (!response_desc)
        return;

    if (request->resource_id != TEST_SW_RESOURCE_ID) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        return;
    }

    virtio_gpu_data_t *data = vgpu->priv;
    for (uint32_t i = 0; i < data->num_scanouts; i++) {
        struct virtio_gpu_scanout_info *scanout = &data->scanouts[i];
        if (!scanout->enabled ||
            scanout->primary_resource_id != request->resource_id)
            continue;
        virtio_gpu_notify_primary_scanout_will_change(vgpu, i);
        scanout->primary_resource_id = 0;
        scanout->src_x = 0;
        scanout->src_y = 0;
        scanout->src_w = 0;
        scanout->src_h = 0;
        vgpu_display_publish_primary_clear(i);
        test_run_sw_interleaved_renderer_request();
    }

    *plen = virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_OK_NODATA);
}

static void test_sw_set_scanout(virtio_gpu_state_t *vgpu,
                                struct virtq_desc *vq_desc,
                                uint32_t *plen)
{
    const struct virtio_gpu_set_scanout *request =
        virtio_gpu_get_request(vgpu, vq_desc, sizeof(*request));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    const struct virtq_desc *response_desc = test_response_desc(vq_desc, plen);
    if (!response_desc)
        return;

    virtio_gpu_data_t *data = vgpu->priv;
    if (request->scanout_id >= data->num_scanouts ||
        request->resource_id != TEST_SW_RESOURCE_ID) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        return;
    }

    struct virtio_gpu_scanout_info *scanout =
        &data->scanouts[request->scanout_id];
    if (!scanout->enabled) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID);
        return;
    }

    virtio_gpu_notify_primary_scanout_will_change(vgpu, request->scanout_id);
    scanout->primary_resource_id = request->resource_id;
    scanout->src_x = request->r.x;
    scanout->src_y = request->r.y;
    scanout->src_w = request->r.width;
    scanout->src_h = request->r.height;
    test_run_sw_interleaved_renderer_request();
    *plen = virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_OK_NODATA);
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
    .resource_unref = test_sw_resource_unref,
    .set_scanout = test_sw_set_scanout,
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

int virgl_renderer_init(void *cookie,
                        int flags,
                        struct virgl_renderer_callbacks *cb)
{
    g_calls.renderer_init_count++;
    g_calls.renderer_init_cookie = cookie;
    g_calls.renderer_init_flags = flags;
    g_calls.renderer_init_callback_version = cb ? cb->version : 0;
    g_calls.renderer_init_lock_depth = 0;
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
    return g_calls.resource_create_ret;
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
    if (g_after_resource_get_info_hook)
        g_after_resource_get_info_hook();
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
    g_calls.submit_force_ctx_0_count = g_calls.force_ctx_0_count;
    memcpy(g_calls.submit_words, buffer, (size_t) ndw * sizeof(uint32_t));
    return 0;
}

void virgl_renderer_force_ctx_0(void)
{
    g_calls.force_ctx_0_count++;
}

#include "../virtio-gpu-virgl.c"

static void test_invalidate_scanout0_after_resource_get_info(void)
{
    g_after_resource_get_info_hook = NULL;
    __atomic_add_fetch(&g_vgpu_virgl_scanout_generation[0], 1,
                       __ATOMIC_ACQ_REL);
    g_vgpu_data.scanouts[0].primary_resource_id = 0;
    g_vgpu_data.scanouts[0].src_x = 0;
    g_vgpu_data.scanouts[0].src_y = 0;
    g_vgpu_data.scanouts[0].src_w = 0;
    g_vgpu_data.scanouts[0].src_h = 0;
    vgpu_virgl_set_committed_scanout(
        0, &g_vgpu_data.scanouts[0],
        __atomic_load_n(&g_vgpu_virgl_scanout_generation[0], __ATOMIC_ACQUIRE));
    vgpu_display_publish_primary_clear(0);
}

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

static int insert_virgl_frontend_resource(uint32_t resource_id)
{
    struct vgpu_virgl_resource *res = calloc(1, sizeof(*res));
    CHECK(res != NULL);
    res->resource_id = resource_id;
    vgpu_virgl_insert_resource(res);
    return 0;
}

static int arm_interleaved_virgl_set_scanout(virtio_gpu_state_t *vgpu,
                                             uint32_t resource_id,
                                             uint32_t generation)
{
    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    CHECK(work != NULL);
    work->vgpu = vgpu;
    work->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    work->cmd.set_scanout = (struct virtio_gpu_set_scanout) {
        .scanout_id = 0,
        .resource_id = resource_id,
        .r = {.x = 7, .y = 9, .width = 300, .height = 180},
    };
    work->cmd.set_scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    work->scanouts[0] = (struct vgpu_virgl_scanout_snapshot) {
        .scanout_id = 0,
        .generation = generation,
        .scanout = g_vgpu_data.scanouts[0],
    };
    work->scanout_count = 1;

    g_sw_interleaved_renderer_request = (struct vgpu_renderer_request) {
        .type = VGPU_RENDERER_REQ_CTRL,
        .command_type = VIRTIO_GPU_CMD_SET_SCANOUT,
        .payload = work,
    };
    g_sw_interleaved_renderer_request_active = true;
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
    CHECK(g_calls.submit_force_ctx_0_count == 1);
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
    CHECK(g_calls.create_fence_client_id > 0);
    CHECK(g_calls.create_fence_ctx_id == 0);
    CHECK(g_calls.context_create_fence_count == 0);
    CHECK(g_calls.poll_count == 1);
    CHECK(g_calls.renderer_submit_count == 2);
    CHECK(g_calls.last_renderer_request.type == VGPU_RENDERER_REQ_POLL);
    CHECK(ctrl_response_len_or_deferred(plen));
    CHECK(response_hdr()->type == 0);

    return 0;
}

static int test_ctx0_fence_forces_ctx0_current_before_create(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    struct virtio_gpu_cmd_submit request = {
        .hdr = {.flags = VIRTIO_GPU_FLAG_FENCE, .fence_id = 321, .ctx_id = 7},
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
    CHECK(g_calls.force_ctx_0_count == 2);
    CHECK(g_calls.create_fence_count == 1);
    CHECK(g_calls.context_create_fence_count == 0);

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

static int test_fence_callback_uses_generation_from_fence_creation(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    struct virtio_gpu_cmd_submit request = {
        .hdr = {.flags = VIRTIO_GPU_FLAG_FENCE, .fence_id = 123, .ctx_id = 7},
        .size = 16,
    };
    uint32_t commands[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    memcpy(&g_ram[PAYLOAD_ADDR], commands, sizeof(commands));

    g_vgpu_data.ctrl_generation = 1;
    g_virtio_gpu_backend.init(&vgpu);
    memset(&g_calls, 0, sizeof(g_calls));

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_with_payload(desc, sizeof(request), sizeof(commands));
    uint32_t plen = 0;

    g_virtio_gpu_backend.submit_3d(&vgpu, desc, &plen);
    CHECK(g_calls.create_fence_count == 1);
    CHECK(g_calls.renderer_complete_count == 0);
    uint32_t renderer_fence = (uint32_t) g_calls.create_fence_client_id;

    g_vgpu_data.ctrl_generation = 2;
    g_renderer_callbacks.write_fence(g_renderer_cookie, renderer_fence);

    CHECK(g_calls.renderer_complete_count == 1);
    CHECK(g_calls.last_renderer_completion.type == VGPU_RENDERER_DONE_FENCE);
    CHECK(g_calls.last_renderer_completion.token.generation == 1);
    CHECK(g_calls.last_renderer_completion.fence_id == 123);

    return 0;
}

static int test_stale_fence_callback_after_reset_is_dropped(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    struct virtio_gpu_cmd_submit request = {
        .hdr = {.flags = VIRTIO_GPU_FLAG_FENCE, .fence_id = 124, .ctx_id = 7},
        .size = 16,
    };
    uint32_t commands[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    memcpy(&g_ram[PAYLOAD_ADDR], commands, sizeof(commands));

    g_vgpu_data.ctrl_generation = 1;
    g_virtio_gpu_backend.init(&vgpu);
    memset(&g_calls, 0, sizeof(g_calls));

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_with_payload(desc, sizeof(request), sizeof(commands));
    uint32_t plen = 0;

    g_virtio_gpu_backend.submit_3d(&vgpu, desc, &plen);
    CHECK(g_calls.create_fence_count == 1);
    uint32_t renderer_fence = (uint32_t) g_calls.create_fence_client_id;

    g_vgpu_data.ctrl_generation = 2;
    g_virtio_gpu_backend.reset(&vgpu);
    int completed_after_reset = g_calls.renderer_complete_count;

    g_renderer_callbacks.write_fence(g_renderer_cookie, renderer_fence);

    CHECK(g_calls.renderer_complete_count == completed_after_reset);

    return 0;
}

static int test_stale_high_fence_after_reset_does_not_complete_new_low_fence(
    void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    struct virtio_gpu_cmd_submit request = {
        .hdr = {.flags = VIRTIO_GPU_FLAG_FENCE, .fence_id = 5000, .ctx_id = 7},
        .size = 16,
    };
    uint32_t commands[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    memcpy(&g_ram[PAYLOAD_ADDR], commands, sizeof(commands));

    g_vgpu_data.ctrl_generation = 1;
    g_virtio_gpu_backend.init(&vgpu);
    memset(&g_calls, 0, sizeof(g_calls));

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_with_payload(desc, sizeof(request), sizeof(commands));
    uint32_t plen = 0;

    g_virtio_gpu_backend.submit_3d(&vgpu, desc, &plen);
    CHECK(g_calls.create_fence_count == 1);
    uint32_t old_renderer_fence = (uint32_t) g_calls.create_fence_client_id;

    g_vgpu_data.ctrl_generation = 2;
    g_virtio_gpu_backend.reset(&vgpu);
    int completed_after_reset = g_calls.renderer_complete_count;

    request.hdr.fence_id = 1;
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    init_desc_with_payload(desc, sizeof(request), sizeof(commands));
    plen = 0;

    g_virtio_gpu_backend.submit_3d(&vgpu, desc, &plen);
    CHECK(g_calls.create_fence_count == 2);
    CHECK(g_calls.renderer_complete_count == completed_after_reset);

    uint32_t new_renderer_fence = (uint32_t) g_calls.create_fence_client_id;
    g_renderer_callbacks.write_fence(g_renderer_cookie, old_renderer_fence);
    CHECK(g_calls.renderer_complete_count == completed_after_reset);

    g_renderer_callbacks.write_fence(g_renderer_cookie, new_renderer_fence);
    CHECK(g_calls.renderer_complete_count == completed_after_reset + 1);
    CHECK(g_calls.last_renderer_completion.token.generation == 2);
    CHECK(g_calls.last_renderer_completion.fence_id == 1);

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

    g_virtio_gpu_backend.init(&vgpu);
    memset(&g_calls, 0, sizeof(g_calls));

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
    CHECK(g_calls.context_create_fence_id != 0);
    CHECK(g_calls.context_create_fence_id != 0x123456789ULL);
    CHECK(g_calls.poll_count == 1);
    CHECK(g_calls.renderer_submit_count == 2);
    CHECK(g_calls.last_renderer_request.type == VGPU_RENDERER_REQ_POLL);
    CHECK(response_hdr()->type == 0);

    g_renderer_callbacks.write_context_fence(g_renderer_cookie, 77, 3,
                                             g_calls.context_create_fence_id);
    CHECK(g_calls.renderer_complete_count == 1);
    CHECK(g_calls.last_renderer_completion.type == VGPU_RENDERER_DONE_FENCE);
    CHECK(g_calls.last_renderer_completion.context_fence == true);
    CHECK(g_calls.last_renderer_completion.ctx_id == 77);
    CHECK(g_calls.last_renderer_completion.ring_idx == 3);
    CHECK(g_calls.last_renderer_completion.fence_id == 0x123456789ULL);

    return 0;
}

static int test_fence_callbacks_record_state_and_reset_clears_it(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();

    vgpu_virgl_write_fence(&vgpu, 55);
    CHECK(g_calls.renderer_complete_count == 0);

    vgpu_virgl_write_context_fence(&vgpu, 77, 3, 0x123456789ULL);
    CHECK(g_calls.renderer_complete_count == 0);

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

static int test_direct_renderer_create_failure_preserves_frontend_resource(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(insert_virgl_frontend_resource(56) == 0);
    struct vgpu_virgl_resource *res = vgpu_virgl_find_resource(56);
    CHECK(res != NULL);
    g_calls.resource_create_ret = -1;

    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    CHECK(work != NULL);
    work->vgpu = &vgpu;
    work->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    work->cmd.resource_create_3d = (struct virtio_gpu_resource_create_3d) {
        .resource_id = 56,
        .target = 2,
        .format = VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM,
        .bind = 0x99,
        .width = 320,
        .height = 240,
        .depth = 1,
        .array_size = 1,
    };
    work->cmd.resource_create_3d.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    work->resource_generation = res->generation;
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D,
        .payload = work,
    };

    vgpu_virgl_execute_renderer_request(&request);

    CHECK(g_calls.resource_create_count == 1);
    CHECK(vgpu_virgl_find_resource(56) != NULL);
    CHECK(g_calls.last_renderer_completion.type == VGPU_RENDERER_DONE_CTRL);
    CHECK(g_calls.last_renderer_completion.response_type ==
          VIRTIO_GPU_RESP_ERR_UNSPEC);

    vgpu_virgl_apply_renderer_side_effect(&vgpu,
                                          &g_calls.last_renderer_completion);
    CHECK(vgpu_virgl_find_resource(56) == NULL);

    return 0;
}

static int test_direct_renderer_reset_preserves_frontend_resources(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(insert_virgl_frontend_resource(57) == 0);
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_RESET,
        .payload = &vgpu,
    };

    vgpu_virgl_execute_renderer_request(&request);

    CHECK(g_calls.reset_count == 1);
    CHECK(vgpu_virgl_find_resource(57) != NULL);

    return 0;
}

static int
test_direct_renderer_set_scanout_preserves_frontend_until_side_effect(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(insert_virgl_frontend_resource(58) == 0);

    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    CHECK(work != NULL);
    work->vgpu = &vgpu;
    work->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    work->cmd.set_scanout = (struct virtio_gpu_set_scanout) {
        .scanout_id = 0,
        .resource_id = 58,
        .r = {.x = 7, .y = 9, .width = 300, .height = 180},
    };
    work->cmd.set_scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    work->scanouts[0] = (struct vgpu_virgl_scanout_snapshot) {
        .scanout_id = 0,
        .generation = __atomic_load_n(&g_vgpu_virgl_scanout_generation[0],
                                      __ATOMIC_ACQUIRE),
        .scanout = g_vgpu_data.scanouts[0],
    };
    work->scanout_count = 1;
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .command_type = VIRTIO_GPU_CMD_SET_SCANOUT,
        .payload = work,
    };

    vgpu_virgl_execute_renderer_request(&request);

    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);
    CHECK(g_calls.last_renderer_completion.type == VGPU_RENDERER_DONE_CTRL);
    CHECK(g_calls.last_renderer_completion.response_type ==
          VIRTIO_GPU_RESP_OK_NODATA);

    vgpu_virgl_apply_renderer_side_effect(&vgpu,
                                          &g_calls.last_renderer_completion);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 58);
    CHECK(g_vgpu_data.scanouts[0].src_x == 7);
    CHECK(g_vgpu_data.scanouts[0].src_y == 9);
    CHECK(g_vgpu_data.scanouts[0].src_w == 300);
    CHECK(g_vgpu_data.scanouts[0].src_h == 180);

    return 0;
}

static int
test_renderer_flush_uses_prior_set_scanout_before_frontend_side_effect(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 59) == 0);
    memset(&g_calls, 0, sizeof(g_calls));

    struct vgpu_virgl_ctrl_work *set_work = calloc(1, sizeof(*set_work));
    CHECK(set_work != NULL);
    set_work->vgpu = &vgpu;
    set_work->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    set_work->cmd.set_scanout = (struct virtio_gpu_set_scanout) {
        .scanout_id = 0,
        .resource_id = 59,
        .r = {.x = 7, .y = 9, .width = 300, .height = 180},
    };
    set_work->cmd.set_scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    set_work->scanouts[0] = (struct vgpu_virgl_scanout_snapshot) {
        .scanout_id = 0,
        .generation = __atomic_load_n(&g_vgpu_virgl_scanout_generation[0],
                                      __ATOMIC_ACQUIRE),
        .scanout = g_vgpu_data.scanouts[0],
    };
    set_work->scanout_count = 1;
    struct vgpu_renderer_request set_request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .command_type = VIRTIO_GPU_CMD_SET_SCANOUT,
        .payload = set_work,
    };

    vgpu_virgl_execute_renderer_request(&set_request);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);

    struct vgpu_virgl_ctrl_work *flush_work = calloc(1, sizeof(*flush_work));
    CHECK(flush_work != NULL);
    flush_work->vgpu = &vgpu;
    flush_work->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush_work->cmd.resource_flush = (struct virtio_gpu_res_flush) {
        .resource_id = 59,
        .r = {.x = 0, .y = 0, .width = 300, .height = 180},
    };
    flush_work->cmd.resource_flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    struct vgpu_renderer_request flush_request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .command_type = VIRTIO_GPU_CMD_RESOURCE_FLUSH,
        .payload = flush_work,
    };

    g_publish_primary_set_count = 0;
    free(g_last_primary_payload);
    g_last_primary_payload = NULL;
    vgpu_virgl_execute_renderer_request(&flush_request);

    CHECK(g_publish_primary_set_count == 1);
    CHECK(g_last_primary_payload != NULL);
    CHECK(g_last_primary_payload->kind == VGPU_DISPLAY_PAYLOAD_GL_SCANOUT);
    CHECK(g_last_primary_payload->gl.src_x == 7);
    CHECK(g_last_primary_payload->gl.src_y == 9);
    CHECK(g_last_primary_payload->gl.src_w == 300);
    CHECK(g_last_primary_payload->gl.src_h == 180);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);

    return 0;
}

static int test_stale_set_scanout_side_effect_does_not_resurrect_clear(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(insert_virgl_frontend_resource(59) == 0);
    g_vgpu_data.scanouts[0].primary_resource_id = 59;
    g_vgpu_data.scanouts[0].src_x = 1;
    g_vgpu_data.scanouts[0].src_y = 2;
    g_vgpu_data.scanouts[0].src_w = 64;
    g_vgpu_data.scanouts[0].src_h = 32;
    uint32_t generation =
        __atomic_load_n(&g_vgpu_virgl_scanout_generation[0], __ATOMIC_ACQUIRE);

    struct vgpu_renderer_completion stale = {
        .type = VGPU_RENDERER_DONE_CTRL,
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT,
                .resource_id = 59,
                .scanout_id = 0,
                .scanout_generation = generation,
                .rect = {.x = 7, .y = 9, .width = 300, .height = 180},
            },
    };

    struct virtio_gpu_set_scanout clear = {
        .scanout_id = 0,
        .resource_id = 0,
    };
    memcpy(&g_ram[REQ_ADDR], &clear, sizeof(clear));
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(clear));
    uint32_t plen = 0;

    g_virtio_gpu_backend.set_scanout(&vgpu, desc, &plen);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);

    vgpu_virgl_apply_renderer_side_effect(&vgpu, &stale);

    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);
    CHECK(g_vgpu_data.scanouts[0].src_x == 0);
    CHECK(g_vgpu_data.scanouts[0].src_y == 0);
    CHECK(g_vgpu_data.scanouts[0].src_w == 0);
    CHECK(g_vgpu_data.scanouts[0].src_h == 0);

    return 0;
}

static int test_failed_sw_set_scanout_keeps_virgl_side_effect_current(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(insert_virgl_frontend_resource(59) == 0);
    uint32_t generation =
        __atomic_load_n(&g_vgpu_virgl_scanout_generation[0], __ATOMIC_ACQUIRE);

    struct vgpu_renderer_completion pending = {
        .type = VGPU_RENDERER_DONE_CTRL,
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT,
                .resource_id = 59,
                .scanout_id = 0,
                .scanout_generation = generation,
                .rect = {.x = 7, .y = 9, .width = 300, .height = 180},
            },
    };

    struct virtio_gpu_set_scanout bad_sw_set = {
        .scanout_id = 0,
        .resource_id = TEST_SW_RESOURCE_ID + 1,
        .r = {.x = 1, .y = 2, .width = 64, .height = 32},
    };
    memcpy(&g_ram[REQ_ADDR], &bad_sw_set, sizeof(bad_sw_set));
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(bad_sw_set));
    uint32_t plen = 0;

    g_virtio_gpu_backend.set_scanout(&vgpu, desc, &plen);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);

    vgpu_virgl_apply_renderer_side_effect(&vgpu, &pending);

    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 59);
    CHECK(g_vgpu_data.scanouts[0].src_x == 7);
    CHECK(g_vgpu_data.scanouts[0].src_y == 9);
    CHECK(g_vgpu_data.scanouts[0].src_w == 300);
    CHECK(g_vgpu_data.scanouts[0].src_h == 180);

    return 0;
}

static int test_sw_set_scanout_invalidates_before_mutating_primary(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(insert_virgl_frontend_resource(60) == 0);
    uint32_t generation =
        __atomic_load_n(&g_vgpu_virgl_scanout_generation[0], __ATOMIC_ACQUIRE);
    CHECK(arm_interleaved_virgl_set_scanout(&vgpu, 60, generation) == 0);

    struct virtio_gpu_set_scanout sw_set = {
        .scanout_id = 0,
        .resource_id = TEST_SW_RESOURCE_ID,
        .r = {.x = 1, .y = 2, .width = 64, .height = 32},
    };
    memcpy(&g_ram[REQ_ADDR], &sw_set, sizeof(sw_set));
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(sw_set));
    uint32_t plen = 0;

    g_virtio_gpu_backend.set_scanout(&vgpu, desc, &plen);

    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == TEST_SW_RESOURCE_ID);
    CHECK(g_publish_primary_set_count == 0);

    return 0;
}

static int test_stale_renderer_set_scanout_does_not_publish_after_clear(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(insert_virgl_frontend_resource(60) == 0);

    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    CHECK(work != NULL);
    work->vgpu = &vgpu;
    work->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    work->cmd.set_scanout = (struct virtio_gpu_set_scanout) {
        .scanout_id = 0,
        .resource_id = 60,
        .r = {.x = 0, .y = 0, .width = 64, .height = 32},
    };
    work->cmd.set_scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    work->scanouts[0] = (struct vgpu_virgl_scanout_snapshot) {
        .scanout_id = 0,
        .generation = __atomic_load_n(&g_vgpu_virgl_scanout_generation[0],
                                      __ATOMIC_ACQUIRE),
        .scanout = g_vgpu_data.scanouts[0],
    };
    work->scanout_count = 1;

    __atomic_add_fetch(&g_vgpu_virgl_scanout_generation[0], 1,
                       __ATOMIC_ACQ_REL);
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .command_type = VIRTIO_GPU_CMD_SET_SCANOUT,
        .payload = work,
    };

    vgpu_virgl_execute_renderer_request(&request);

    CHECK(g_publish_primary_set_count == 0);
    CHECK(g_calls.last_renderer_completion.type == VGPU_RENDERER_DONE_CTRL);
    CHECK(g_calls.last_renderer_completion.response_type ==
          VIRTIO_GPU_RESP_OK_NODATA);

    return 0;
}

static int test_set_scanout_rechecks_generation_before_gl_publish(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 60) == 0);
    memset(&g_calls, 0, sizeof(g_calls));

    struct virtio_gpu_set_scanout request = {
        .scanout_id = 0,
        .resource_id = 60,
        .r = {.x = 3, .y = 4, .width = 128, .height = 96},
    };
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(request));
    uint32_t plen = 0;

    g_after_resource_get_info_hook =
        test_invalidate_scanout0_after_resource_get_info;
    g_virtio_gpu_backend.set_scanout(&vgpu, desc, &plen);

    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(g_publish_primary_clear_count == 1);
    CHECK(g_publish_primary_set_count == 0);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);

    return 0;
}

static int test_sw_unref_invalidates_before_clearing_primary(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(insert_virgl_frontend_resource(61) == 0);
    uint32_t generation =
        __atomic_load_n(&g_vgpu_virgl_scanout_generation[0], __ATOMIC_ACQUIRE);
    CHECK(arm_interleaved_virgl_set_scanout(&vgpu, 61, generation) == 0);

    g_vgpu_data.scanouts[0].primary_resource_id = TEST_SW_RESOURCE_ID;
    g_vgpu_data.scanouts[0].src_x = 1;
    g_vgpu_data.scanouts[0].src_y = 2;
    g_vgpu_data.scanouts[0].src_w = 64;
    g_vgpu_data.scanouts[0].src_h = 32;
    struct virtio_gpu_res_unref unref = {
        .resource_id = TEST_SW_RESOURCE_ID,
    };
    memcpy(&g_ram[REQ_ADDR], &unref, sizeof(unref));
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(unref));
    uint32_t plen = 0;

    g_virtio_gpu_backend.resource_unref(&vgpu, desc, &plen);

    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);
    CHECK(g_publish_primary_clear_count == 1);
    CHECK(g_publish_primary_set_count == 0);

    return 0;
}

static int test_sw_unref_invalidates_stale_virgl_display_publish(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(insert_virgl_frontend_resource(60) == 0);

    struct vgpu_virgl_ctrl_work *work = calloc(1, sizeof(*work));
    CHECK(work != NULL);
    work->vgpu = &vgpu;
    work->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    work->cmd.set_scanout = (struct virtio_gpu_set_scanout) {
        .scanout_id = 0,
        .resource_id = 60,
        .r = {.x = 0, .y = 0, .width = 64, .height = 32},
    };
    work->cmd.set_scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    work->scanouts[0] = (struct vgpu_virgl_scanout_snapshot) {
        .scanout_id = 0,
        .generation = __atomic_load_n(&g_vgpu_virgl_scanout_generation[0],
                                      __ATOMIC_ACQUIRE),
        .scanout = g_vgpu_data.scanouts[0],
    };
    work->scanout_count = 1;

    g_vgpu_data.scanouts[0].primary_resource_id = TEST_SW_RESOURCE_ID;
    g_vgpu_data.scanouts[0].src_x = 1;
    g_vgpu_data.scanouts[0].src_y = 2;
    g_vgpu_data.scanouts[0].src_w = 64;
    g_vgpu_data.scanouts[0].src_h = 32;
    struct virtio_gpu_res_unref unref = {
        .resource_id = TEST_SW_RESOURCE_ID,
    };
    memcpy(&g_ram[REQ_ADDR], &unref, sizeof(unref));
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(unref));
    uint32_t plen = 0;
    g_virtio_gpu_backend.resource_unref(&vgpu, desc, &plen);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);

    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .command_type = VIRTIO_GPU_CMD_SET_SCANOUT,
        .payload = work,
    };

    vgpu_virgl_execute_renderer_request(&request);

    CHECK(g_publish_primary_set_count == 0);
    CHECK(g_calls.last_renderer_completion.type == VGPU_RENDERER_DONE_CTRL);
    CHECK(g_calls.last_renderer_completion.response_type ==
          VIRTIO_GPU_RESP_OK_NODATA);

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

static int test_pipelined_attach_detach_uses_optimistic_frontend_backing(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 62) == 0);
    memset(&g_calls, 0, sizeof(g_calls));
    g_renderer_submit_autorun = false;

    struct virtio_gpu_res_attach_backing attach = {
        .resource_id = 62,
        .nr_entries = 1,
    };
    struct virtio_gpu_mem_entry entry = {.addr = 1536, .length = 64};
    memcpy(&g_ram[REQ_ADDR], &attach, sizeof(attach));
    memcpy(&g_ram[PAYLOAD_ADDR], &entry, sizeof(entry));

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_with_payload(desc, sizeof(attach), sizeof(entry));
    uint32_t plen = 0;

    g_virtio_gpu_backend.resource_attach_backing(&vgpu, desc, &plen);
    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_virgl_resource *res = vgpu_virgl_find_resource(62);
    CHECK(res != NULL);
    CHECK(res->backing_attached);

    struct virtio_gpu_res_detach_backing detach = {
        .resource_id = 62,
    };
    memcpy(&g_ram[REQ_ADDR], &detach, sizeof(detach));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(detach));
    plen = 0;

    g_virtio_gpu_backend.resource_detach_backing(&vgpu, desc, &plen);

    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    CHECK(response_hdr()->type == 0);
    CHECK(!res->backing_attached);
    g_renderer_submit_autorun = true;

    return 0;
}

static int test_attach_failure_before_unref_failure_restores_backing_state(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 68) == 0);
    vgpu_virgl_remove_renderer_resource(68);
    memset(&g_calls, 0, sizeof(g_calls));
    g_renderer_submit_autorun = false;

    struct virtio_gpu_res_attach_backing attach = {
        .resource_id = 68,
        .nr_entries = 1,
    };
    struct virtio_gpu_mem_entry entry = {.addr = 1536, .length = 64};
    memcpy(&g_ram[REQ_ADDR], &attach, sizeof(attach));
    memcpy(&g_ram[PAYLOAD_ADDR], &entry, sizeof(entry));

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_with_payload(desc, sizeof(attach), sizeof(entry));
    uint32_t plen = 0;

    g_virtio_gpu_backend.resource_attach_backing(&vgpu, desc, &plen);
    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_virgl_resource *res = vgpu_virgl_find_resource(68);
    CHECK(res != NULL);
    uint64_t generation = res->generation;
    CHECK(res->backing_attached);
    struct vgpu_renderer_request attach_request = g_calls.last_renderer_request;

    struct virtio_gpu_res_unref unref = {
        .resource_id = 68,
    };
    memcpy(&g_ram[REQ_ADDR], &unref, sizeof(unref));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(unref));
    plen = 0;

    g_virtio_gpu_backend.resource_unref(&vgpu, desc, &plen);
    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    CHECK(vgpu_virgl_find_resource(68) == NULL);
    struct vgpu_renderer_request unref_request = g_calls.last_renderer_request;

    vgpu_virgl_execute_renderer_request(&attach_request);
    struct vgpu_virgl_resource *inactive =
        vgpu_virgl_find_frontend_resource_generation(68, generation);
    CHECK(inactive != NULL);
    CHECK(!inactive->active);
    CHECK(!inactive->backing_attached);

    vgpu_virgl_execute_renderer_request(&unref_request);
    res = vgpu_virgl_find_resource(68);
    CHECK(res != NULL);
    CHECK(res->generation == generation);
    CHECK(!res->backing_attached);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    g_renderer_submit_autorun = true;

    return 0;
}

static int test_attach_detach_failures_restore_committed_backing_state(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 69) == 0);
    vgpu_virgl_remove_renderer_resource(69);
    memset(&g_calls, 0, sizeof(g_calls));
    g_renderer_submit_autorun = false;

    struct virtio_gpu_res_attach_backing attach = {
        .resource_id = 69,
        .nr_entries = 1,
    };
    struct virtio_gpu_mem_entry entry = {.addr = 1536, .length = 64};
    memcpy(&g_ram[REQ_ADDR], &attach, sizeof(attach));
    memcpy(&g_ram[PAYLOAD_ADDR], &entry, sizeof(entry));

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_with_payload(desc, sizeof(attach), sizeof(entry));
    uint32_t plen = 0;

    g_virtio_gpu_backend.resource_attach_backing(&vgpu, desc, &plen);
    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_virgl_resource *res = vgpu_virgl_find_resource(69);
    CHECK(res != NULL);
    CHECK(res->backing_attached);
    CHECK(res->backing_pending_transitions == 1);
    struct vgpu_renderer_request attach_request = g_calls.last_renderer_request;

    struct virtio_gpu_res_detach_backing detach = {
        .resource_id = 69,
    };
    memcpy(&g_ram[REQ_ADDR], &detach, sizeof(detach));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(detach));
    plen = 0;

    g_virtio_gpu_backend.resource_detach_backing(&vgpu, desc, &plen);
    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    CHECK(!res->backing_attached);
    CHECK(res->backing_pending_transitions == 2);
    struct vgpu_renderer_request detach_request = g_calls.last_renderer_request;

    vgpu_virgl_execute_renderer_request(&attach_request);
    CHECK(!res->backing_attached);
    CHECK(!res->backing_committed_attached);
    CHECK(res->backing_pending_transitions == 1);

    vgpu_virgl_execute_renderer_request(&detach_request);
    CHECK(!res->backing_attached);
    CHECK(!res->backing_committed_attached);
    CHECK(res->backing_pending_transitions == 0);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_ERR_UNSPEC);
    g_renderer_submit_autorun = true;

    return 0;
}

static int test_pipelined_unref_create_allows_resource_id_reuse(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 62) == 0);
    memset(&g_calls, 0, sizeof(g_calls));
    g_renderer_submit_autorun = false;

    struct virtio_gpu_res_unref unref = {
        .resource_id = 62,
    };
    memcpy(&g_ram[REQ_ADDR], &unref, sizeof(unref));
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(unref));
    uint32_t plen = 0;

    g_virtio_gpu_backend.resource_unref(&vgpu, desc, &plen);
    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    CHECK(vgpu_virgl_find_resource(62) == NULL);

    struct virtio_gpu_resource_create_3d create = {
        .resource_id = 62,
        .target = 2,
        .format = VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM,
        .bind = 0x99,
        .width = 320,
        .height = 240,
        .depth = 1,
        .array_size = 1,
    };
    memcpy(&g_ram[REQ_ADDR], &create, sizeof(create));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(create));
    plen = 0;

    g_virtio_gpu_backend.resource_create_3d(&vgpu, desc, &plen);

    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    CHECK(response_hdr()->type == 0);
    CHECK(vgpu_virgl_find_resource(62) != NULL);
    g_renderer_submit_autorun = true;

    return 0;
}

static int test_unref_failure_restores_frontend_resource_and_scanout(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 65) == 0);
    vgpu_virgl_remove_renderer_resource(65);

    g_vgpu_data.scanouts[0].primary_resource_id = 65;
    g_vgpu_data.scanouts[0].src_x = 3;
    g_vgpu_data.scanouts[0].src_y = 4;
    g_vgpu_data.scanouts[0].src_w = 320;
    g_vgpu_data.scanouts[0].src_h = 200;
    vgpu_virgl_set_committed_scanout(
        0, &g_vgpu_data.scanouts[0],
        __atomic_load_n(&g_vgpu_virgl_scanout_generation[0], __ATOMIC_ACQUIRE));
    memset(&g_calls, 0, sizeof(g_calls));
    g_publish_primary_clear_count = 0;

    struct virtio_gpu_res_unref unref = {
        .resource_id = 65,
    };
    memcpy(&g_ram[REQ_ADDR], &unref, sizeof(unref));
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(unref));
    uint32_t plen = 0;

    g_virtio_gpu_backend.resource_unref(&vgpu, desc, &plen);

    struct vgpu_virgl_resource *res = vgpu_virgl_find_resource(65);
    CHECK(res != NULL);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 65);
    CHECK(g_vgpu_data.scanouts[0].src_x == 3);
    CHECK(g_vgpu_data.scanouts[0].src_y == 4);
    CHECK(g_vgpu_data.scanouts[0].src_w == 320);
    CHECK(g_vgpu_data.scanouts[0].src_h == 200);
    CHECK(g_publish_primary_clear_count == 0);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);

    return 0;
}

static int test_create_unref_create_rollbacks_are_generation_guarded(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    g_renderer_submit_autorun = false;

    struct virtio_gpu_resource_create_3d first_create = {
        .resource_id = 66,
        .target = 2,
        .format = VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM,
        .bind = 0x99,
        .width = 320,
        .height = 240,
        .depth = 1,
        .array_size = 1,
    };
    memcpy(&g_ram[REQ_ADDR], &first_create, sizeof(first_create));
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(first_create));
    uint32_t plen = 0;

    g_virtio_gpu_backend.resource_create_3d(&vgpu, desc, &plen);
    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_virgl_resource *first = vgpu_virgl_find_resource(66);
    CHECK(first != NULL);
    uint64_t first_generation = first->generation;

    struct virtio_gpu_res_unref unref = {
        .resource_id = 66,
    };
    memcpy(&g_ram[REQ_ADDR], &unref, sizeof(unref));
    init_desc_no_payload(desc, sizeof(unref));
    plen = 0;

    g_virtio_gpu_backend.resource_unref(&vgpu, desc, &plen);
    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    CHECK(vgpu_virgl_find_resource(66) == NULL);

    struct virtio_gpu_resource_create_3d second_create = first_create;
    memcpy(&g_ram[REQ_ADDR], &second_create, sizeof(second_create));
    init_desc_no_payload(desc, sizeof(second_create));
    plen = 0;

    g_virtio_gpu_backend.resource_create_3d(&vgpu, desc, &plen);
    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_virgl_resource *second = vgpu_virgl_find_resource(66);
    CHECK(second != NULL);
    uint64_t second_generation = second->generation;
    CHECK(second_generation != first_generation);

    struct vgpu_renderer_completion first_create_failed = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_CREATE_3D_ROLLBACK,
                .resource_id = 66,
                .resource_generation = first_generation,
            },
    };
    vgpu_virgl_apply_renderer_side_effect(&vgpu, &first_create_failed);
    CHECK(vgpu_virgl_find_resource(66) == second);

    struct vgpu_renderer_completion first_unref_failed = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_UNREF_ROLLBACK,
                .resource_id = 66,
                .resource_generation = first_generation,
            },
    };
    vgpu_virgl_apply_renderer_side_effect(&vgpu, &first_unref_failed);
    CHECK(vgpu_virgl_find_resource(66) == second);

    struct vgpu_renderer_completion second_create_failed = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_CREATE_3D_ROLLBACK,
                .resource_id = 66,
                .resource_generation = second_generation,
            },
    };
    vgpu_virgl_apply_renderer_side_effect(&vgpu, &second_create_failed);
    CHECK(vgpu_virgl_find_resource(66) == NULL);
    g_renderer_submit_autorun = true;

    return 0;
}

static int test_unref_failure_restores_after_new_same_id_create_rollback(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 67) == 0);
    struct vgpu_virgl_resource *old = vgpu_virgl_find_resource(67);
    CHECK(old != NULL);
    uint64_t old_generation = old->generation;
    g_renderer_submit_autorun = false;

    struct virtio_gpu_res_unref unref = {
        .resource_id = 67,
    };
    memcpy(&g_ram[REQ_ADDR], &unref, sizeof(unref));
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(unref));
    uint32_t plen = 0;
    g_virtio_gpu_backend.resource_unref(&vgpu, desc, &plen);
    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    CHECK(vgpu_virgl_find_resource(67) == NULL);

    struct virtio_gpu_resource_create_3d create = {
        .resource_id = 67,
        .target = 2,
        .format = VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM,
        .bind = 0x99,
        .width = 320,
        .height = 240,
        .depth = 1,
        .array_size = 1,
    };
    memcpy(&g_ram[REQ_ADDR], &create, sizeof(create));
    init_desc_no_payload(desc, sizeof(create));
    plen = 0;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, desc, &plen);
    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_virgl_resource *new_res = vgpu_virgl_find_resource(67);
    CHECK(new_res != NULL);
    uint64_t new_generation = new_res->generation;

    struct vgpu_renderer_completion unref_failed = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_UNREF_ROLLBACK,
                .resource_id = 67,
                .resource_generation = old_generation,
            },
    };
    vgpu_virgl_apply_renderer_side_effect(&vgpu, &unref_failed);
    CHECK(vgpu_virgl_find_resource(67) == new_res);

    struct vgpu_renderer_completion create_failed = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_CREATE_3D_ROLLBACK,
                .resource_id = 67,
                .resource_generation = new_generation,
            },
    };
    vgpu_virgl_apply_renderer_side_effect(&vgpu, &create_failed);
    struct vgpu_virgl_resource *restored = vgpu_virgl_find_resource(67);
    CHECK(restored != NULL);
    CHECK(restored->generation == old_generation);
    g_renderer_submit_autorun = true;

    return 0;
}

static int test_pipelined_set_scanout_unref_drops_stale_set_publish(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 70) == 0);
    memset(&g_calls, 0, sizeof(g_calls));
    g_renderer_submit_autorun = false;

    struct virtio_gpu_set_scanout set = {
        .scanout_id = 0,
        .resource_id = 70,
        .r = {.x = 5, .y = 6, .width = 320, .height = 200},
    };
    memcpy(&g_ram[REQ_ADDR], &set, sizeof(set));
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(set));
    uint32_t plen = 0;

    g_virtio_gpu_backend.set_scanout(&vgpu, desc, &plen);
    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 70);
    CHECK(g_vgpu_data.scanouts[0].src_x == 5);
    CHECK(g_vgpu_data.scanouts[0].src_y == 6);
    CHECK(g_vgpu_data.scanouts[0].src_w == 320);
    CHECK(g_vgpu_data.scanouts[0].src_h == 200);
    struct vgpu_renderer_request set_request = g_calls.last_renderer_request;

    struct virtio_gpu_res_unref unref = {
        .resource_id = 70,
    };
    memcpy(&g_ram[REQ_ADDR], &unref, sizeof(unref));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(unref));
    plen = 0;

    g_virtio_gpu_backend.resource_unref(&vgpu, desc, &plen);
    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    CHECK(vgpu_virgl_find_resource(70) == NULL);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);
    struct vgpu_renderer_request unref_request = g_calls.last_renderer_request;

    vgpu_virgl_execute_renderer_request(&set_request);
    CHECK(g_publish_primary_set_count == 0);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);

    vgpu_virgl_execute_renderer_request(&unref_request);
    CHECK(g_publish_primary_clear_count == 0);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);
    CHECK(vgpu_virgl_find_resource(70) == NULL);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);
    g_renderer_submit_autorun = true;

    return 0;
}

static struct vgpu_renderer_completion test_side_effect_for_work(
    struct vgpu_virgl_ctrl_work *work,
    uint32_t response_type)
{
    struct vgpu_renderer_completion completion = {0};
    vgpu_virgl_set_completion_side_effect(&completion, work, response_type);
    return completion;
}

static int test_completed_set_scanout_before_unref_success_publishes_clear(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 73) == 0);
    memset(&g_calls, 0, sizeof(g_calls));
    g_renderer_submit_autorun = false;

    struct virtio_gpu_set_scanout set = {
        .scanout_id = 0,
        .resource_id = 73,
        .r = {.x = 5, .y = 6, .width = 320, .height = 200},
    };
    memcpy(&g_ram[REQ_ADDR], &set, sizeof(set));
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(set));
    uint32_t plen = 0;

    g_virtio_gpu_backend.set_scanout(&vgpu, desc, &plen);
    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_virgl_ctrl_work *set_work =
        g_calls.last_renderer_request.payload;
    set_work->scanout_committed = true;
    struct vgpu_renderer_completion set_done =
        test_side_effect_for_work(set_work, VIRTIO_GPU_RESP_OK_NODATA);

    struct virtio_gpu_res_unref unref = {
        .resource_id = 73,
    };
    memcpy(&g_ram[REQ_ADDR], &unref, sizeof(unref));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(unref));
    plen = 0;

    g_virtio_gpu_backend.resource_unref(&vgpu, desc, &plen);
    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);
    struct vgpu_virgl_ctrl_work *unref_work =
        g_calls.last_renderer_request.payload;
    struct vgpu_renderer_completion unref_done =
        test_side_effect_for_work(unref_work, VIRTIO_GPU_RESP_OK_NODATA);

    vgpu_virgl_apply_renderer_side_effect(&vgpu, &set_done);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);
    CHECK(g_publish_primary_clear_count == 0);

    vgpu_virgl_apply_renderer_side_effect(&vgpu, &unref_done);
    CHECK(g_publish_primary_clear_count == 1);
    CHECK(vgpu_virgl_find_resource(73) == NULL);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);
    vgpu_virgl_free_ctrl_work(set_work);
    vgpu_virgl_free_ctrl_work(unref_work);
    g_renderer_submit_autorun = true;

    return 0;
}

static int test_completed_set_scanout_before_unref_failure_restores_set(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 74) == 0);
    vgpu_virgl_remove_renderer_resource(74);
    g_vgpu_data.scanouts[0].primary_resource_id = TEST_SW_RESOURCE_ID;
    g_vgpu_data.scanouts[0].src_x = 1;
    g_vgpu_data.scanouts[0].src_y = 2;
    g_vgpu_data.scanouts[0].src_w = 64;
    g_vgpu_data.scanouts[0].src_h = 32;
    vgpu_virgl_set_committed_scanout(
        0, &g_vgpu_data.scanouts[0],
        __atomic_load_n(&g_vgpu_virgl_scanout_generation[0], __ATOMIC_ACQUIRE));
    memset(&g_calls, 0, sizeof(g_calls));
    g_renderer_submit_autorun = false;

    struct virtio_gpu_set_scanout set = {
        .scanout_id = 0,
        .resource_id = 74,
        .r = {.x = 5, .y = 6, .width = 320, .height = 200},
    };
    memcpy(&g_ram[REQ_ADDR], &set, sizeof(set));
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(set));
    uint32_t plen = 0;

    g_virtio_gpu_backend.set_scanout(&vgpu, desc, &plen);
    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_virgl_ctrl_work *set_work =
        g_calls.last_renderer_request.payload;
    set_work->scanout_committed = true;
    struct vgpu_renderer_completion set_done =
        test_side_effect_for_work(set_work, VIRTIO_GPU_RESP_OK_NODATA);

    struct virtio_gpu_res_unref unref = {
        .resource_id = 74,
    };
    memcpy(&g_ram[REQ_ADDR], &unref, sizeof(unref));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(unref));
    plen = 0;

    g_virtio_gpu_backend.resource_unref(&vgpu, desc, &plen);
    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);
    struct vgpu_virgl_ctrl_work *unref_work =
        g_calls.last_renderer_request.payload;
    struct vgpu_renderer_completion unref_done = test_side_effect_for_work(
        unref_work, VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);

    vgpu_virgl_apply_renderer_side_effect(&vgpu, &set_done);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);

    vgpu_virgl_apply_renderer_side_effect(&vgpu, &unref_done);
    struct vgpu_virgl_resource *res = vgpu_virgl_find_resource(74);
    CHECK(res != NULL);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 74);
    CHECK(g_vgpu_data.scanouts[0].src_x == 5);
    CHECK(g_vgpu_data.scanouts[0].src_y == 6);
    CHECK(g_vgpu_data.scanouts[0].src_w == 320);
    CHECK(g_vgpu_data.scanouts[0].src_h == 200);
    vgpu_virgl_free_ctrl_work(set_work);
    vgpu_virgl_free_ctrl_work(unref_work);
    g_renderer_submit_autorun = true;

    return 0;
}

static int test_failed_set_scanout_rolls_back_optimistic_frontend_state(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 70) == 0);
    memset(&g_calls, 0, sizeof(g_calls));

    struct virtio_gpu_set_scanout set = {
        .scanout_id = 0,
        .resource_id = 70,
        .r = {.x = 0, .y = 0, .width = 2000, .height = 200},
    };
    memcpy(&g_ram[REQ_ADDR], &set, sizeof(set));
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(set));
    uint32_t plen = 0;

    g_virtio_gpu_backend.set_scanout(&vgpu, desc, &plen);

    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);
    CHECK(g_vgpu_data.scanouts[0].src_x == 0);
    CHECK(g_vgpu_data.scanouts[0].src_y == 0);
    CHECK(g_vgpu_data.scanouts[0].src_w == 0);
    CHECK(g_vgpu_data.scanouts[0].src_h == 0);
    CHECK(g_publish_primary_set_count == 0);

    return 0;
}

static int test_failed_set_scanout_unref_restores_committed_scanout(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 72) == 0);
    vgpu_virgl_remove_renderer_resource(72);
    g_vgpu_data.scanouts[0].primary_resource_id = TEST_SW_RESOURCE_ID;
    g_vgpu_data.scanouts[0].src_x = 1;
    g_vgpu_data.scanouts[0].src_y = 2;
    g_vgpu_data.scanouts[0].src_w = 64;
    g_vgpu_data.scanouts[0].src_h = 32;
    vgpu_virgl_set_committed_scanout(
        0, &g_vgpu_data.scanouts[0],
        __atomic_load_n(&g_vgpu_virgl_scanout_generation[0], __ATOMIC_ACQUIRE));
    memset(&g_calls, 0, sizeof(g_calls));
    g_renderer_submit_autorun = false;

    struct virtio_gpu_set_scanout set = {
        .scanout_id = 0,
        .resource_id = 72,
        .r = {.x = 5, .y = 6, .width = 320, .height = 200},
    };
    memcpy(&g_ram[REQ_ADDR], &set, sizeof(set));
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(set));
    uint32_t plen = 0;

    g_virtio_gpu_backend.set_scanout(&vgpu, desc, &plen);
    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 72);
    struct vgpu_renderer_request set_request = g_calls.last_renderer_request;

    struct virtio_gpu_res_unref unref = {
        .resource_id = 72,
    };
    memcpy(&g_ram[REQ_ADDR], &unref, sizeof(unref));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(unref));
    plen = 0;

    g_virtio_gpu_backend.resource_unref(&vgpu, desc, &plen);
    CHECK(plen == VIRTIO_GPU_RESPONSE_DEFERRED);
    CHECK(vgpu_virgl_find_resource(72) == NULL);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);
    struct vgpu_renderer_request unref_request = g_calls.last_renderer_request;

    vgpu_virgl_execute_renderer_request(&set_request);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == 0);

    vgpu_virgl_execute_renderer_request(&unref_request);
    struct vgpu_virgl_resource *res = vgpu_virgl_find_resource(72);
    CHECK(res != NULL);
    CHECK(g_vgpu_data.scanouts[0].primary_resource_id == TEST_SW_RESOURCE_ID);
    CHECK(g_vgpu_data.scanouts[0].src_x == 1);
    CHECK(g_vgpu_data.scanouts[0].src_y == 2);
    CHECK(g_vgpu_data.scanouts[0].src_w == 64);
    CHECK(g_vgpu_data.scanouts[0].src_h == 32);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    g_renderer_submit_autorun = true;

    return 0;
}

static int test_resource_unref_completion_clears_bound_gl_scanout(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 62) == 0);

    g_vgpu_data.scanouts[0].primary_resource_id = 62;
    g_vgpu_data.scanouts[0].src_x = 3;
    g_vgpu_data.scanouts[0].src_y = 4;
    g_vgpu_data.scanouts[0].src_w = 320;
    g_vgpu_data.scanouts[0].src_h = 200;
    vgpu_virgl_set_committed_scanout(
        0, &g_vgpu_data.scanouts[0],
        __atomic_load_n(&g_vgpu_virgl_scanout_generation[0], __ATOMIC_ACQUIRE));
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
    CHECK(g_calls.resource_unref_primary_clear_count == 0);
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

static int test_fenced_resource_side_effects_cover_all_completed_fences(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    g_virtio_gpu_backend.init(&vgpu);
    CHECK(create_virgl_resource(&vgpu, 63) == 0);
    CHECK(create_virgl_resource(&vgpu, 64) == 0);
    memset(&g_calls, 0, sizeof(g_calls));

    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    struct virtio_gpu_res_unref first = {
        .hdr = {.flags = VIRTIO_GPU_FLAG_FENCE, .fence_id = 11},
        .resource_id = 63,
    };
    memcpy(&g_ram[REQ_ADDR], &first, sizeof(first));
    init_desc_no_payload(desc, sizeof(first));
    uint32_t plen = 0;
    g_virtio_gpu_backend.resource_unref(&vgpu, desc, &plen);
    CHECK(ctrl_response_len_or_deferred(plen));
    CHECK(g_calls.create_fence_count == 1);

    struct virtio_gpu_res_unref second = {
        .hdr = {.flags = VIRTIO_GPU_FLAG_FENCE, .fence_id = 22},
        .resource_id = 64,
    };
    memcpy(&g_ram[REQ_ADDR], &second, sizeof(second));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(second));
    plen = 0;
    g_virtio_gpu_backend.resource_unref(&vgpu, desc, &plen);
    CHECK(ctrl_response_len_or_deferred(plen));
    CHECK(g_calls.create_fence_count == 2);
    uint32_t second_renderer_fence = (uint32_t) g_calls.create_fence_client_id;

    g_renderer_callbacks.write_fence(g_renderer_cookie, second_renderer_fence);
    CHECK(g_calls.last_renderer_completion.type == VGPU_RENDERER_DONE_FENCE);

    vgpu_virgl_apply_renderer_side_effect(&vgpu,
                                          &g_calls.last_renderer_completion);

    CHECK(vgpu_virgl_find_resource(63) == NULL);
    CHECK(vgpu_virgl_find_resource(64) == NULL);

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

    struct virtio_gpu_set_scanout set_scanout = {
        .r = {.x = 7, .y = 8, .width = 300, .height = 180},
        .scanout_id = 0,
        .resource_id = 92,
    };
    memcpy(&g_ram[REQ_ADDR], &set_scanout, sizeof(set_scanout));
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(set_scanout));
    uint32_t plen = 0;
    g_virtio_gpu_backend.set_scanout(&vgpu, desc, &plen);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);
    memset(&g_calls, 0, sizeof(g_calls));
    g_publish_primary_set_count = 0;
    free(g_last_primary_payload);
    g_last_primary_payload = NULL;

    struct virtio_gpu_res_flush request = {
        .r = {.x = 0, .y = 0, .width = 300, .height = 180},
        .resource_id = 92,
    };
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));

    init_desc_no_payload(desc, sizeof(request));
    plen = 0;

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

static int test_resource_flush_rechecks_generation_before_gl_publish(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 93) == 0);

    struct virtio_gpu_set_scanout set_scanout = {
        .r = {.x = 7, .y = 8, .width = 300, .height = 180},
        .scanout_id = 0,
        .resource_id = 93,
    };
    memcpy(&g_ram[REQ_ADDR], &set_scanout, sizeof(set_scanout));
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC];
    init_desc_no_payload(desc, sizeof(set_scanout));
    uint32_t plen = 0;
    g_virtio_gpu_backend.set_scanout(&vgpu, desc, &plen);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);
    memset(&g_calls, 0, sizeof(g_calls));
    g_publish_primary_set_count = 0;
    free(g_last_primary_payload);
    g_last_primary_payload = NULL;

    struct virtio_gpu_res_flush request = {
        .r = {.x = 0, .y = 0, .width = 300, .height = 180},
        .resource_id = 93,
    };
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));

    init_desc_no_payload(desc, sizeof(request));
    plen = 0;

    g_after_resource_get_info_hook =
        test_invalidate_scanout0_after_resource_get_info;
    g_virtio_gpu_backend.resource_flush(&vgpu, desc, &plen);

    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(g_publish_primary_clear_count == 1);
    CHECK(g_publish_primary_set_count == 0);

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
    CHECK(test_backend_poll_without_pending_fence_does_not_poll_renderer() ==
          0);
    CHECK(test_renderer_poll_request_executes_on_gl_owner() == 0);
    CHECK(test_ctx_create_calls_renderer() == 0);
    CHECK(test_context_lifecycle_handlers_call_renderer() == 0);
    CHECK(test_ctx_create_rejects_context_init_until_feature_enabled() == 0);
    CHECK(test_resource_create_3d_calls_renderer() == 0);
    CHECK(test_direct_renderer_create_failure_preserves_frontend_resource() ==
          0);
    CHECK(test_direct_renderer_reset_preserves_frontend_resources() == 0);
    CHECK(
        test_direct_renderer_set_scanout_preserves_frontend_until_side_effect() ==
        0);
    CHECK(
        test_renderer_flush_uses_prior_set_scanout_before_frontend_side_effect() ==
        0);
    CHECK(test_stale_set_scanout_side_effect_does_not_resurrect_clear() == 0);
    CHECK(test_failed_sw_set_scanout_keeps_virgl_side_effect_current() == 0);
    CHECK(test_sw_set_scanout_invalidates_before_mutating_primary() == 0);
    CHECK(test_stale_renderer_set_scanout_does_not_publish_after_clear() == 0);
    CHECK(test_set_scanout_rechecks_generation_before_gl_publish() == 0);
    CHECK(test_sw_unref_invalidates_before_clearing_primary() == 0);
    CHECK(test_sw_unref_invalidates_stale_virgl_display_publish() == 0);
    CHECK(test_resource_backing_attach_detach_and_unref() == 0);
    CHECK(test_pipelined_attach_detach_uses_optimistic_frontend_backing() == 0);
    CHECK(test_attach_failure_before_unref_failure_restores_backing_state() ==
          0);
    CHECK(test_attach_detach_failures_restore_committed_backing_state() == 0);
    CHECK(test_pipelined_unref_create_allows_resource_id_reuse() == 0);
    CHECK(test_unref_failure_restores_frontend_resource_and_scanout() == 0);
    CHECK(test_create_unref_create_rollbacks_are_generation_guarded() == 0);
    CHECK(test_unref_failure_restores_after_new_same_id_create_rollback() == 0);
    CHECK(test_pipelined_set_scanout_unref_drops_stale_set_publish() == 0);
    CHECK(test_completed_set_scanout_before_unref_success_publishes_clear() ==
          0);
    CHECK(test_completed_set_scanout_before_unref_failure_restores_set() == 0);
    CHECK(test_failed_set_scanout_rolls_back_optimistic_frontend_state() == 0);
    CHECK(test_failed_set_scanout_unref_restores_committed_scanout() == 0);
    CHECK(test_resource_unref_completion_clears_bound_gl_scanout() == 0);
    CHECK(test_fenced_resource_side_effects_cover_all_completed_fences() == 0);
    CHECK(test_reset_clears_scanout_before_renderer_reset() == 0);
    CHECK(test_3d_transfers_call_renderer() == 0);
    CHECK(test_transfer_to_host_2d_routes_by_resource_owner() == 0);
    CHECK(test_set_scanout_publishes_gl_payload_for_virgl_resource() == 0);
    CHECK(test_resource_flush_republishes_bound_gl_payload() == 0);
    CHECK(test_resource_flush_rechecks_generation_before_gl_publish() == 0);
    CHECK(test_submit_3d_copies_payload_to_renderer() == 0);
    CHECK(test_fenced_submit_creates_renderer_fence_from_request_flags() == 0);
    CHECK(test_ctx0_fence_forces_ctx0_current_before_create() == 0);
    CHECK(test_synchronous_fence_callback_does_not_leave_poll_pending() == 0);
    CHECK(test_fence_callback_uses_generation_from_fence_creation() == 0);
    CHECK(test_stale_fence_callback_after_reset_is_dropped() == 0);
    CHECK(test_stale_high_fence_after_reset_does_not_complete_new_low_fence() ==
          0);
    CHECK(test_descriptor_write_flag_does_not_create_renderer_fence() == 0);
    CHECK(test_ring_idx_fence_uses_context_fence_api() == 0);
    CHECK(test_fence_callbacks_record_state_and_reset_clears_it() == 0);
    CHECK(test_submit_3d_rejects_unaligned_size() == 0);
    return 0;
}
