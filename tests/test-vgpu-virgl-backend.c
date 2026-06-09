#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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
static int wake_renderer_count;
static int wake_frontend_count;
static int fake_cookie_marker;

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

void virgl_renderer_reset(void)
{
    fake_reset_count++;
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
    wake_renderer_count = 0;
    wake_frontend_count = 0;

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
    test_ctx0_fence_completion_preserves_guest_id_and_generation();
    test_context_fence_completion_preserves_stream_metadata();
    test_poll_requests_coalesce_until_executed();
    test_callback_completes_newest_matching_fence_and_clears_older();
    test_reset_drops_pending_fences_and_ignores_stale_callbacks();
    return 0;
}
