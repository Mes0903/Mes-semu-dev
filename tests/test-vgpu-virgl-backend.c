#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <virglrenderer.h>

#include "../vgpu-renderer.h"
#include "../virtio-gpu-virgl.h"

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static void *fake_init_cookie;
static int fake_init_flags;
static int fake_init_count;
static struct virgl_renderer_callbacks fake_callbacks;
static int fake_create_fence_count;
static int fake_last_create_fence_id;
static uint32_t fake_last_create_fence_ctx_id;
static int fake_context_create_fence_count;
static uint32_t fake_last_context_fence_ctx_id;
static uint32_t fake_last_context_fence_flags;
static uint32_t fake_last_context_fence_ring_idx;
static uint64_t fake_last_context_fence_id;
static int fake_poll_count;
static int fake_reset_count;
static int fake_get_cap_set_count;
static int fake_fill_caps_count;
static uint32_t fake_last_fill_caps_set;
static uint32_t fake_last_fill_caps_version;
static int fake_context_create_count;
static uint32_t fake_last_context_create_handle;
static uint32_t fake_last_context_create_nlen;
static char fake_last_context_create_name[64];
static int fake_context_destroy_count;
static uint32_t fake_last_context_destroy_handle;
static int fake_resource_create_count;
static struct virgl_renderer_resource_create_args
    fake_last_resource_create_args;
static struct iovec *fake_last_resource_create_iov;
static uint32_t fake_last_resource_create_num_iovs;
static int fake_resource_create_result;
static int fake_resource_unref_count;
static uint32_t fake_last_resource_unref_handle;
static int wake_renderer_count;
static int wake_frontend_count;
static int fake_cookie_marker;
static int fake_window_create_count;
static int fake_window_create_scanout_idx;
static struct virgl_renderer_gl_ctx_param fake_window_create_param;
static struct virgl_renderer_gl_ctx_param *fake_window_create_param_ptr;
static virgl_renderer_gl_context fake_window_create_result =
    (virgl_renderer_gl_context) (uintptr_t) 0x12345678;
static int fake_window_destroy_count;
static virgl_renderer_gl_context fake_window_destroy_ctx;
static int fake_window_make_current_count;
static int fake_window_make_current_scanout_idx;
static virgl_renderer_gl_context fake_window_make_current_ctx;
static int fake_window_make_current_result;

int virgl_renderer_init(void *cookie,
                        int flags,
                        struct virgl_renderer_callbacks *cb)
{
    fake_init_cookie = cookie;
    fake_init_flags = flags;
    fake_init_count++;
    fake_callbacks = *cb;
    return 0;
}

void virgl_renderer_poll(void)
{
    fake_poll_count++;
}

int virgl_renderer_create_fence(int client_fence_id, uint32_t ctx_id)
{
    fake_create_fence_count++;
    fake_last_create_fence_id = client_fence_id;
    fake_last_create_fence_ctx_id = ctx_id;
    return 0;
}

int virgl_renderer_context_create_fence(uint32_t ctx_id,
                                        uint32_t flags,
                                        uint32_t ring_idx,
                                        uint64_t fence_id)
{
    fake_context_create_fence_count++;
    fake_last_context_fence_ctx_id = ctx_id;
    fake_last_context_fence_flags = flags;
    fake_last_context_fence_ring_idx = ring_idx;
    fake_last_context_fence_id = fence_id;
    return 0;
}

void virgl_renderer_get_cap_set(uint32_t set,
                                uint32_t *max_ver,
                                uint32_t *max_size)
{
    fake_get_cap_set_count++;
    if (set == VIRTIO_GPU_CAPSET_VIRGL) {
        *max_ver = 2;
        *max_size = 4;
    } else if (set == VIRTIO_GPU_CAPSET_VIRGL2) {
        *max_ver = 0;
        *max_size = 0;
    } else {
        *max_ver = 0;
        *max_size = 0;
    }
}

void virgl_renderer_fill_caps(uint32_t set, uint32_t version, void *caps)
{
    uint8_t *bytes = caps;

    fake_fill_caps_count++;
    fake_last_fill_caps_set = set;
    fake_last_fill_caps_version = version;
    for (uint32_t i = 0; i < 4; i++)
        bytes[i] = (uint8_t) (0xa0 + i);
}

int virgl_renderer_context_create(uint32_t handle,
                                  uint32_t nlen,
                                  const char *name)
{
    fake_context_create_count++;
    fake_last_context_create_handle = handle;
    fake_last_context_create_nlen = nlen;
    memset(fake_last_context_create_name, 0,
           sizeof(fake_last_context_create_name));
    if (name && nlen < sizeof(fake_last_context_create_name))
        memcpy(fake_last_context_create_name, name, nlen);
    return 0;
}

void virgl_renderer_context_destroy(uint32_t handle)
{
    fake_context_destroy_count++;
    fake_last_context_destroy_handle = handle;
}

int virgl_renderer_resource_create(
    struct virgl_renderer_resource_create_args *args,
    struct iovec *iov,
    uint32_t num_iovs)
{
    fake_resource_create_count++;
    if (args)
        fake_last_resource_create_args = *args;
    fake_last_resource_create_iov = iov;
    fake_last_resource_create_num_iovs = num_iovs;
    return fake_resource_create_result;
}

void virgl_renderer_resource_unref(uint32_t res_handle)
{
    fake_resource_unref_count++;
    fake_last_resource_unref_handle = res_handle;
}

void virgl_renderer_reset(void)
{
    fake_reset_count++;
}

virgl_renderer_gl_context vgpu_window_virgl_create_context(
    int scanout_idx,
    struct virgl_renderer_gl_ctx_param *param)
{
    fake_window_create_count++;
    fake_window_create_scanout_idx = scanout_idx;
    fake_window_create_param_ptr = param;
    if (param)
        fake_window_create_param = *param;
    return fake_window_create_result;
}

void vgpu_window_virgl_destroy_context(virgl_renderer_gl_context ctx)
{
    fake_window_destroy_count++;
    fake_window_destroy_ctx = ctx;
}

int vgpu_window_virgl_make_current(int scanout_idx,
                                   virgl_renderer_gl_context ctx)
{
    fake_window_make_current_count++;
    fake_window_make_current_scanout_idx = scanout_idx;
    fake_window_make_current_ctx = ctx;
    return fake_window_make_current_result;
}

static void wake_renderer(void)
{
    wake_renderer_count++;
}

static void wake_frontend(void)
{
    wake_frontend_count++;
}

static void reset_test_state(uint64_t generation)
{
    fake_init_cookie = NULL;
    fake_init_flags = 0;
    fake_init_count = 0;
    fake_callbacks = (struct virgl_renderer_callbacks) {0};
    fake_create_fence_count = 0;
    fake_last_create_fence_id = 0;
    fake_last_create_fence_ctx_id = 0;
    fake_context_create_fence_count = 0;
    fake_last_context_fence_ctx_id = 0;
    fake_last_context_fence_flags = 0;
    fake_last_context_fence_ring_idx = 0;
    fake_last_context_fence_id = 0;
    fake_poll_count = 0;
    fake_reset_count = 0;
    fake_get_cap_set_count = 0;
    fake_fill_caps_count = 0;
    fake_last_fill_caps_set = 0;
    fake_last_fill_caps_version = 0;
    fake_context_create_count = 0;
    fake_last_context_create_handle = 0;
    fake_last_context_create_nlen = 0;
    memset(fake_last_context_create_name, 0,
           sizeof(fake_last_context_create_name));
    fake_context_destroy_count = 0;
    fake_last_context_destroy_handle = 0;
    fake_resource_create_count = 0;
    fake_last_resource_create_args =
        (struct virgl_renderer_resource_create_args) {0};
    fake_last_resource_create_iov = NULL;
    fake_last_resource_create_num_iovs = 0;
    fake_resource_create_result = 0;
    fake_resource_unref_count = 0;
    fake_last_resource_unref_handle = 0;
    wake_renderer_count = 0;
    wake_frontend_count = 0;
    fake_window_create_count = 0;
    fake_window_create_scanout_idx = -1;
    fake_window_create_param = (struct virgl_renderer_gl_ctx_param) {0};
    fake_window_create_param_ptr = NULL;
    fake_window_destroy_count = 0;
    fake_window_destroy_ctx = NULL;
    fake_window_make_current_count = 0;
    fake_window_make_current_scanout_idx = -1;
    fake_window_make_current_ctx = NULL;
    fake_window_make_current_result = 0;

    vgpu_renderer_set_wake_renderer(wake_renderer);
    vgpu_renderer_set_wake_frontend(wake_frontend);
    vgpu_renderer_reset_queues(generation);
    vgpu_virgl_reset_renderer();
    fake_reset_count = 0;
}

static struct vgpu_virgl_debug_stats virgl_stats(void)
{
    struct vgpu_virgl_debug_stats stats = {0};
    vgpu_virgl_debug_snapshot(&stats);
    return stats;
}

static struct vgpu_renderer_debug_stats renderer_stats(void)
{
    struct vgpu_renderer_debug_stats stats = {0};
    vgpu_renderer_debug_snapshot(&stats);
    return stats;
}

static void init_renderer_for_test(void)
{
    CHECK(vgpu_virgl_init_renderer(&fake_cookie_marker) == 0);
    CHECK(fake_callbacks.write_fence != NULL);
    CHECK(fake_callbacks.write_context_fence != NULL);
    CHECK(fake_callbacks.create_gl_context != NULL);
    CHECK(fake_callbacks.destroy_gl_context != NULL);
    CHECK(fake_callbacks.make_current != NULL);
}

static void test_init_renderer_uses_thread_sync_and_callbacks(void)
{
    int marker;

    reset_test_state(1);
    CHECK(vgpu_virgl_init_renderer(&marker) == 0);
    CHECK(fake_init_count == 1);
    CHECK(fake_init_cookie == &marker);
    CHECK(fake_init_flags == VIRGL_RENDERER_THREAD_SYNC);
    CHECK(fake_callbacks.version == VIRGL_RENDERER_CALLBACKS_VERSION);
    CHECK(fake_callbacks.write_fence != NULL);
    CHECK(fake_callbacks.write_context_fence != NULL);
}

static void test_init_renderer_installs_gl_context_callbacks(void)
{
    reset_test_state(3);
    init_renderer_for_test();

    CHECK(fake_callbacks.create_gl_context != NULL);
    CHECK(fake_callbacks.destroy_gl_context != NULL);
    CHECK(fake_callbacks.make_current != NULL);
}

static void test_create_gl_context_callback_delegates_to_window_hook(void)
{
    struct virgl_renderer_gl_ctx_param param = {
        .version = 1,
        .shared = true,
        .major_ver = 4,
        .minor_ver = 3,
        .compat_ctx = 1,
    };

    reset_test_state(5);
    init_renderer_for_test();

    virgl_renderer_gl_context ctx =
        fake_callbacks.create_gl_context(fake_init_cookie, 2, &param);

    CHECK(ctx == fake_window_create_result);
    CHECK(fake_window_create_count == 1);
    CHECK(fake_window_create_scanout_idx == 2);
    CHECK(fake_window_create_param_ptr == &param);
    CHECK(fake_window_create_param.version == param.version);
    CHECK(fake_window_create_param.shared == param.shared);
    CHECK(fake_window_create_param.major_ver == param.major_ver);
    CHECK(fake_window_create_param.minor_ver == param.minor_ver);
    CHECK(fake_window_create_param.compat_ctx == param.compat_ctx);
}

static void test_destroy_gl_context_callback_delegates_to_window_hook(void)
{
    int marker;
    virgl_renderer_gl_context ctx = &marker;

    reset_test_state(6);
    init_renderer_for_test();

    fake_callbacks.destroy_gl_context(fake_init_cookie, ctx);

    CHECK(fake_window_destroy_count == 1);
    CHECK(fake_window_destroy_ctx == ctx);
}

static void test_make_current_callback_delegates_to_window_hook(void)
{
    int marker;
    virgl_renderer_gl_context ctx = &marker;

    reset_test_state(8);
    fake_window_make_current_result = -17;
    init_renderer_for_test();

    CHECK(fake_callbacks.make_current(fake_init_cookie, 4, ctx) == -17);
    CHECK(fake_window_make_current_count == 1);
    CHECK(fake_window_make_current_scanout_idx == 4);
    CHECK(fake_window_make_current_ctx == ctx);
}

static void test_ctx0_fence_completion_preserves_guest_id_and_generation(void)
{
    reset_test_state(7);
    init_renderer_for_test();
    struct vgpu_virgl_debug_stats before = virgl_stats();

    CHECK(
        vgpu_virgl_submit_fence(7, false, 99, 3, UINT64_C(0x1122334455667788)));

    struct vgpu_virgl_debug_stats stats = virgl_stats();
    CHECK(stats.pending_fences == 1);
    CHECK(stats.poll_request_pending);
    CHECK(stats.poll_requests_submitted == before.poll_requests_submitted + 1);
    CHECK(stats.fences_created == before.fences_created + 1);
    CHECK(fake_create_fence_count == 1);
    CHECK(fake_last_create_fence_id == 1);
    CHECK(fake_last_create_fence_ctx_id == 0);
    CHECK(wake_renderer_count == 1);

    fake_callbacks.write_fence(fake_init_cookie, 1);

    struct vgpu_renderer_completion completion;
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.type == VGPU_RENDERER_DONE_FENCE);
    CHECK(completion.token.generation == 7);
    CHECK(!completion.context_fence);
    CHECK(completion.ctx_id == 0);
    CHECK(completion.ring_idx == 0);
    CHECK(completion.fence_id == UINT64_C(0x1122334455667788));
    CHECK(!vgpu_renderer_pop_completion(&completion));
    CHECK(wake_frontend_count == 1);

    stats = virgl_stats();
    CHECK(stats.pending_fences == 0);
    CHECK(stats.fences_completed == before.fences_completed + 1);
    CHECK(stats.last_ctx0_fence == 1);
}

static void test_context_fence_completion_preserves_stream_metadata(void)
{
    reset_test_state(11);
    init_renderer_for_test();
    struct vgpu_virgl_debug_stats before = virgl_stats();

    CHECK(
        vgpu_virgl_submit_fence(11, true, 42, 5, UINT64_C(0x99aabbccddeeff00)));

    struct vgpu_virgl_debug_stats stats = virgl_stats();
    CHECK(stats.pending_fences == 1);
    CHECK(stats.poll_requests_submitted == before.poll_requests_submitted + 1);
    CHECK(fake_context_create_fence_count == 1);
    CHECK(fake_last_context_fence_ctx_id == 42);
    CHECK(fake_last_context_fence_flags == VIRGL_RENDERER_FENCE_FLAG_MERGEABLE);
    CHECK(fake_last_context_fence_ring_idx == 5);
    CHECK(fake_last_context_fence_id == 1);

    fake_callbacks.write_context_fence(fake_init_cookie, 42, 5, 1);

    struct vgpu_renderer_completion completion;
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.type == VGPU_RENDERER_DONE_FENCE);
    CHECK(completion.token.generation == 11);
    CHECK(completion.context_fence);
    CHECK(completion.ctx_id == 42);
    CHECK(completion.ring_idx == 5);
    CHECK(completion.fence_id == UINT64_C(0x99aabbccddeeff00));
    CHECK(!vgpu_renderer_pop_completion(&completion));

    stats = virgl_stats();
    CHECK(stats.pending_fences == 0);
    CHECK(stats.fences_completed == before.fences_completed + 1);
    CHECK(stats.last_context_ctx_id == 42);
    CHECK(stats.last_context_ring_idx == 5);
    CHECK(stats.last_context_fence == 1);
}

static void test_poll_requests_coalesce_until_executed(void)
{
    reset_test_state(17);
    init_renderer_for_test();
    struct vgpu_virgl_debug_stats before = virgl_stats();

    CHECK(vgpu_virgl_submit_fence(17, false, 0, 0, 1));
    CHECK(vgpu_virgl_submit_fence(17, false, 0, 0, 2));

    struct vgpu_virgl_debug_stats stats = virgl_stats();
    CHECK(stats.pending_fences == 2);
    CHECK(stats.poll_request_pending);
    CHECK(stats.poll_requests_submitted == before.poll_requests_submitted + 1);
    CHECK(stats.poll_requests_dropped == before.poll_requests_dropped);

    struct vgpu_renderer_debug_stats queue_stats = renderer_stats();
    CHECK(queue_stats.request_depth == 1);

    struct vgpu_renderer_request request;
    CHECK(vgpu_renderer_pop_request(&request));
    CHECK(request.type == VGPU_RENDERER_REQ_POLL);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(fake_poll_count == 1);

    stats = virgl_stats();
    CHECK(!stats.poll_request_pending);
    CHECK(stats.poll_requests_executed == before.poll_requests_executed + 1);
}

static void test_ctrl_request_executes_get_capset_info_completion(void)
{
    reset_test_state(31);

    struct vgpu_renderer_ctrl_payload payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO},
        .cmd.get_capset_info =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO},
                .capset_index = 0,
            },
        .response_capacity = sizeof(struct virtio_gpu_resp_capset_info),
        .response_type = VIRTIO_GPU_RESP_OK_CAPSET_INFO,
        .ctrl_completion =
            {
                .queue_index = VIRTIO_GPU_CONTROLQ,
                .desc_head = 11,
                .actor_generation = 22,
                .common_generation = 31,
                .trigger_irq = true,
            },
        .response_desc =
            {
                .addr = 0x80,
                .len = sizeof(struct virtio_gpu_resp_capset_info),
                .flags = VIRTIO_DESC_F_WRITE,
            },
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 31},
        .command_type = VIRTIO_GPU_CMD_GET_CAPSET_INFO,
        .payload = &payload,
        .payload_size = sizeof(payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.type == VGPU_RENDERER_DONE_CTRL);
    CHECK(completion.token.generation == 31);
    CHECK(completion.has_ctrl_completion);
    CHECK(completion.ctrl_completion.desc_head == 11);
    CHECK(completion.has_response_desc);
    CHECK(completion.response_size ==
          sizeof(struct virtio_gpu_resp_capset_info));

    struct virtio_gpu_resp_capset_info *response = completion.response;
    CHECK(response->hdr.type == VIRTIO_GPU_RESP_OK_CAPSET_INFO);
    CHECK(response->capset_id == VIRTIO_GPU_CAPSET_VIRGL);
    CHECK(response->capset_max_version == 2);
    CHECK(response->capset_max_size == 4);
    completion.release_response(completion.response);
}

static void test_ctrl_request_executes_get_capset_completion(void)
{
    reset_test_state(32);

    struct vgpu_renderer_ctrl_payload payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_GET_CAPSET},
        .cmd.get_capset =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_GET_CAPSET},
                .capset_id = VIRTIO_GPU_CAPSET_VIRGL,
                .capset_version = 2,
            },
        .response_capacity = sizeof(struct virtio_gpu_resp_capset) + 4,
        .response_type = VIRTIO_GPU_RESP_OK_CAPSET,
        .ctrl_completion =
            {
                .queue_index = VIRTIO_GPU_CONTROLQ,
                .desc_head = 12,
                .actor_generation = 23,
                .common_generation = 32,
                .trigger_irq = true,
            },
        .response_desc =
            {
                .addr = 0x90,
                .len = sizeof(struct virtio_gpu_resp_capset) + 4,
                .flags = VIRTIO_DESC_F_WRITE,
            },
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 32},
        .command_type = VIRTIO_GPU_CMD_GET_CAPSET,
        .payload = &payload,
        .payload_size = sizeof(payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.type == VGPU_RENDERER_DONE_CTRL);
    CHECK(completion.response_size ==
          sizeof(struct virtio_gpu_resp_capset) + 4);
    CHECK(fake_fill_caps_count == 1);
    CHECK(fake_last_fill_caps_set == VIRTIO_GPU_CAPSET_VIRGL);
    CHECK(fake_last_fill_caps_version == 2);

    struct virtio_gpu_resp_capset *response = completion.response;
    CHECK(response->hdr.type == VIRTIO_GPU_RESP_OK_CAPSET);
    CHECK(response->capset_data[0] == 0xa0);
    CHECK(response->capset_data[3] == 0xa3);
    completion.release_response(completion.response);
}

static void test_ctrl_request_executes_context_create_destroy(void)
{
    reset_test_state(33);

    struct vgpu_renderer_ctrl_payload payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_CTX_CREATE, .ctx_id = 77},
        .cmd.ctx_create =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_CTX_CREATE, .ctx_id = 77},
                .nlen = 4,
            },
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
        .ctrl_completion =
            {
                .queue_index = VIRTIO_GPU_CONTROLQ,
                .desc_head = 13,
                .actor_generation = 24,
                .common_generation = 33,
                .trigger_irq = true,
            },
        .response_desc =
            {
                .addr = 0xa0,
                .len = sizeof(struct virtio_gpu_ctrl_hdr),
                .flags = VIRTIO_DESC_F_WRITE,
            },
    };
    memcpy(payload.cmd.ctx_create.debug_name, "ctxB", 4);
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 33},
        .command_type = VIRTIO_GPU_CMD_CTX_CREATE,
        .payload = &payload,
        .payload_size = sizeof(payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(completion.response == NULL);
    CHECK(fake_context_create_count == 1);
    CHECK(fake_last_context_create_handle == 77);
    CHECK(fake_last_context_create_nlen == 4);
    CHECK(memcmp(fake_last_context_create_name, "ctxB", 4) == 0);

    payload = (struct vgpu_renderer_ctrl_payload) {
        .hdr = {.type = VIRTIO_GPU_CMD_CTX_DESTROY, .ctx_id = 77},
        .cmd.ctx_destroy =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_CTX_DESTROY, .ctx_id = 77},
            },
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
        .ctrl_completion =
            {
                .queue_index = VIRTIO_GPU_CONTROLQ,
                .desc_head = 14,
                .actor_generation = 24,
                .common_generation = 33,
                .trigger_irq = true,
            },
        .response_desc =
            {
                .addr = 0xb0,
                .len = sizeof(struct virtio_gpu_ctrl_hdr),
                .flags = VIRTIO_DESC_F_WRITE,
            },
    };
    request.command_type = VIRTIO_GPU_CMD_CTX_DESTROY;
    request.payload = &payload;

    vgpu_virgl_execute_renderer_request(&request);

    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(completion.response == NULL);
    CHECK(fake_context_destroy_count == 1);
    CHECK(fake_last_context_destroy_handle == 77);
}

static void test_ctrl_request_executes_resource_create_3d_completion(void)
{
    reset_test_state(34);

    struct vgpu_renderer_ctrl_payload payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
        .cmd.resource_create_3d =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
                .resource_id = 88,
                .target = 2,
                .format = 3,
                .bind = 4,
                .width = 640,
                .height = 480,
                .depth = 1,
                .array_size = 1,
                .last_level = 0,
                .nr_samples = 1,
                .flags = 0x5a,
            },
        .resource_generation = 0x1234,
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
        .ctrl_completion =
            {
                .queue_index = VIRTIO_GPU_CONTROLQ,
                .desc_head = 15,
                .actor_generation = 25,
                .common_generation = 34,
                .trigger_irq = true,
            },
        .response_desc =
            {
                .addr = 0xc0,
                .len = sizeof(struct virtio_gpu_ctrl_hdr),
                .flags = VIRTIO_DESC_F_WRITE,
            },
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 34},
        .command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D,
        .payload = &payload,
        .payload_size = sizeof(payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(completion.virgl_resource.type ==
          VGPU_VIRGL_RESOURCE_SIDE_EFFECT_NONE);
    CHECK(fake_resource_create_count == 1);
    CHECK(fake_last_resource_create_args.handle == 88);
    CHECK(fake_last_resource_create_args.target == 2);
    CHECK(fake_last_resource_create_args.format == 3);
    CHECK(fake_last_resource_create_args.bind == 4);
    CHECK(fake_last_resource_create_args.width == 640);
    CHECK(fake_last_resource_create_args.height == 480);
    CHECK(fake_last_resource_create_args.flags == 0x5a);
    CHECK(fake_last_resource_create_iov == NULL);
    CHECK(fake_last_resource_create_num_iovs == 0);
}

static void test_ctrl_request_resource_create_3d_failure_rolls_back_frontend(
    void)
{
    reset_test_state(35);
    fake_resource_create_result = -1;

    struct vgpu_renderer_ctrl_payload payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
        .cmd.resource_create_3d =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
                .resource_id = 89,
            },
        .resource_generation = 0x5678,
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
        .ctrl_completion =
            {
                .queue_index = VIRTIO_GPU_CONTROLQ,
                .desc_head = 16,
                .actor_generation = 26,
                .common_generation = 35,
                .trigger_irq = true,
            },
        .response_desc =
            {
                .addr = 0xd0,
                .len = sizeof(struct virtio_gpu_ctrl_hdr),
                .flags = VIRTIO_DESC_F_WRITE,
            },
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 35},
        .command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D,
        .payload = &payload,
        .payload_size = sizeof(payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_UNSPEC);
    CHECK(completion.virgl_resource.type ==
          VGPU_VIRGL_RESOURCE_SIDE_EFFECT_CREATE_3D_ROLLBACK);
    CHECK(completion.virgl_resource.resource_id == 89);
    CHECK(completion.virgl_resource.resource_generation == 0x5678);
    CHECK(fake_resource_create_count == 1);
    CHECK(fake_resource_unref_count == 0);
}

static void test_callback_completes_newest_matching_fence_and_clears_older(void)
{
    reset_test_state(19);
    init_renderer_for_test();

    CHECK(vgpu_virgl_submit_fence(19, false, 0, 0, 100));
    CHECK(vgpu_virgl_submit_fence(19, false, 0, 0, 200));
    CHECK(vgpu_virgl_submit_fence(19, false, 0, 0, 300));
    uint32_t completed_renderer_fence = (uint32_t) fake_last_create_fence_id;
    CHECK(virgl_stats().pending_fences == 3);

    fake_callbacks.write_fence(fake_init_cookie, completed_renderer_fence);

    struct vgpu_renderer_completion completion;
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.type == VGPU_RENDERER_DONE_FENCE);
    CHECK(completion.token.generation == 19);
    CHECK(completion.fence_id == 300);
    CHECK(!vgpu_renderer_pop_completion(&completion));
    CHECK(virgl_stats().pending_fences == 0);

    fake_callbacks.write_fence(fake_init_cookie, completed_renderer_fence - 1);
    CHECK(!vgpu_renderer_pop_completion(&completion));
}

static void test_reset_drops_pending_fences_and_ignores_stale_callbacks(void)
{
    reset_test_state(23);
    init_renderer_for_test();
    CHECK(
        vgpu_virgl_submit_fence(23, false, 0, 0, UINT64_C(0xabcdef0123456789)));
    CHECK(
        vgpu_virgl_submit_fence(23, true, 7, 2, UINT64_C(0x7777888899990000)));
    CHECK(virgl_stats().pending_fences == 2);
    uint64_t completed_before_reset = virgl_stats().fences_completed;

    vgpu_virgl_reset_renderer();
    CHECK(fake_reset_count == 1);

    struct vgpu_virgl_debug_stats stats = virgl_stats();
    CHECK(stats.pending_fences == 0);
    CHECK(!stats.poll_request_pending);
    CHECK(stats.last_ctx0_fence == 0);
    CHECK(stats.last_context_ctx_id == 0);
    CHECK(stats.last_context_ring_idx == 0);
    CHECK(stats.last_context_fence == 0);

    fake_callbacks.write_fence(fake_init_cookie, 1);
    fake_callbacks.write_context_fence(fake_init_cookie, 7, 2, 1);

    struct vgpu_renderer_completion completion;
    CHECK(!vgpu_renderer_pop_completion(&completion));
    stats = virgl_stats();
    CHECK(stats.pending_fences == 0);
    CHECK(stats.fences_completed == completed_before_reset);
}

int main(void)
{
    test_init_renderer_uses_thread_sync_and_callbacks();
    test_init_renderer_installs_gl_context_callbacks();
    test_create_gl_context_callback_delegates_to_window_hook();
    test_destroy_gl_context_callback_delegates_to_window_hook();
    test_make_current_callback_delegates_to_window_hook();
    test_ctx0_fence_completion_preserves_guest_id_and_generation();
    test_context_fence_completion_preserves_stream_metadata();
    test_poll_requests_coalesce_until_executed();
    test_ctrl_request_executes_get_capset_info_completion();
    test_ctrl_request_executes_get_capset_completion();
    test_ctrl_request_executes_context_create_destroy();
    test_ctrl_request_executes_resource_create_3d_completion();
    test_ctrl_request_resource_create_3d_failure_rolls_back_frontend();
    test_callback_completes_newest_matching_fence_and_clears_older();
    test_reset_drops_pending_fences_and_ignores_stale_callbacks();
    return 0;
}
