#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../vgpu-renderer.h"

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static int wake_renderer_count;
static int wake_frontend_count;
static int released_payload_count;
static int released_response_count;
static bool submit_during_reset_result;
static bool complete_during_reset_result;
static bool pop_request_during_reset_result;
static bool pop_completion_during_reset_result;
static bool pop_completion_during_frontend_wake_result;
static struct vgpu_renderer_completion pop_completion_during_frontend_wake;
static pthread_t concurrent_reset_thread;
static bool concurrent_reset_thread_started;

static void reset_test_counters(void)
{
    wake_renderer_count = 0;
    wake_frontend_count = 0;
    released_payload_count = 0;
    released_response_count = 0;
    submit_during_reset_result = true;
    complete_during_reset_result = true;
    pop_request_during_reset_result = true;
    pop_completion_during_reset_result = true;
    pop_completion_during_frontend_wake_result = false;
    pop_completion_during_frontend_wake = (struct vgpu_renderer_completion) {0};
    concurrent_reset_thread_started = false;
    vgpu_renderer_set_wake_renderer(NULL);
    vgpu_renderer_set_wake_frontend(NULL);
}

static struct vgpu_renderer_request test_request(uint32_t id)
{
    return (struct vgpu_renderer_request) {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.id = id, .generation = 1},
        .command_type = VIRTIO_GPU_CMD_SUBMIT_3D,
        .payload = (void *) (uintptr_t) (id + 1U),
        .payload_size = id + 16U,
    };
}

static struct vgpu_renderer_completion test_completion(uint32_t id,
                                                       uint64_t generation)
{
    return (struct vgpu_renderer_completion) {
        .type = VGPU_RENDERER_DONE_CTRL,
        .token = {.id = id, .generation = generation},
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
        .response = (void *) (uintptr_t) (id + 0x1000U),
        .response_size = id + 8U,
    };
}

static void wake_renderer(void)
{
    wake_renderer_count++;
}

static void wake_frontend(void)
{
    wake_frontend_count++;
}

static void wake_frontend_and_pop(void)
{
    wake_frontend_count++;
    pop_completion_during_frontend_wake_result =
        vgpu_renderer_pop_completion(&pop_completion_during_frontend_wake);
}

static void release_response(void *response)
{
    (void) response;
    released_response_count++;
}

static void release_payload_and_reenter(void *payload)
{
    (void) payload;
    released_payload_count++;

    struct vgpu_renderer_request nested = test_request(900);
    submit_during_reset_result = vgpu_renderer_submit(&nested);
    pop_request_during_reset_result = vgpu_renderer_pop_request(&nested);
}

static void release_response_and_reenter(void *response)
{
    (void) response;
    released_response_count++;

    struct vgpu_renderer_completion nested = test_completion(901, 1);
    complete_during_reset_result = vgpu_renderer_complete(&nested);
    pop_completion_during_reset_result = vgpu_renderer_pop_completion(&nested);
}

static void test_request_fifo_and_wake(void)
{
    reset_test_counters();
    vgpu_renderer_reset_queues(1);
    vgpu_renderer_set_wake_renderer(wake_renderer);

    struct vgpu_renderer_request first = test_request(10);
    struct vgpu_renderer_request second = test_request(11);
    CHECK(vgpu_renderer_submit(&first));
    CHECK(vgpu_renderer_submit(&second));
    CHECK(wake_renderer_count == 2);

    struct vgpu_renderer_request out;
    CHECK(vgpu_renderer_pop_request(&out));
    CHECK(out.token.id == 10);
    CHECK(out.payload == first.payload);
    CHECK(vgpu_renderer_pop_request(&out));
    CHECK(out.token.id == 11);
    CHECK(out.payload == second.payload);
    CHECK(!vgpu_renderer_pop_request(&out));
}

static void test_completion_fifo_generation_and_wake(void)
{
    reset_test_counters();
    vgpu_renderer_reset_queues(7);
    vgpu_renderer_set_wake_frontend(wake_frontend);

    struct vgpu_renderer_completion stale = test_completion(19, 6);
    stale.release_response = release_response;
    CHECK(!vgpu_renderer_complete(&stale));
    CHECK(released_response_count == 1);
    CHECK(wake_frontend_count == 0);

    struct vgpu_renderer_completion first = test_completion(20, 7);
    struct vgpu_renderer_completion second = test_completion(21, 7);
    CHECK(vgpu_renderer_complete(&first));
    CHECK(vgpu_renderer_complete(&second));
    CHECK(wake_frontend_count == 2);

    struct vgpu_renderer_completion out;
    CHECK(vgpu_renderer_pop_completion(&out));
    CHECK(out.token.id == 20);
    CHECK(vgpu_renderer_pop_completion(&out));
    CHECK(out.token.id == 21);
    CHECK(!vgpu_renderer_pop_completion(&out));
}

static void test_completion_preserves_virgl_resource_side_effect_metadata(void)
{
    reset_test_counters();
    vgpu_renderer_reset_queues(9);

    struct vgpu_renderer_completion completion = test_completion(32, 9);
    completion.type = VGPU_RENDERER_DONE_VIRGL_RESOURCE;
    completion.virgl_resource.type =
        VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT;
    completion.virgl_resource.resource_id = 0x1234;
    completion.virgl_resource.resource_generation =
        UINT64_C(0x1122334455667788);
    completion.virgl_resource.backing_transition_success = true;
    completion.virgl_resource.scanout_id = 3;
    completion.virgl_resource.scanout_generation = UINT64_C(0x8877665544332211);
    completion.virgl_resource.rect = (struct virtio_gpu_rect) {
        .x = 10, .y = 20, .width = 640, .height = 480};
    completion.virgl_resource.scanout_count = 1;
    completion.virgl_resource.scanouts[0].scanout_id = 3;
    completion.virgl_resource.scanouts[0].scanout_generation =
        UINT64_C(0xaabbccddeeff0011);
    completion.virgl_resource.scanouts[0].scanout =
        (struct virtio_gpu_scanout_info) {
            .width = 800,
            .height = 600,
            .enabled = 1,
            .primary_resource_id = 0x1234,
            .cursor_resource_id = 0x5678,
            .src_x = 4,
            .src_y = 5,
            .src_w = 320,
            .src_h = 240,
        };

    CHECK(vgpu_renderer_complete(&completion));

    struct vgpu_renderer_completion out;
    CHECK(vgpu_renderer_pop_completion(&out));
    CHECK(out.type == VGPU_RENDERER_DONE_VIRGL_RESOURCE);
    CHECK(out.virgl_resource.type ==
          VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT);
    CHECK(out.virgl_resource.resource_id == 0x1234);
    CHECK(out.virgl_resource.resource_generation ==
          UINT64_C(0x1122334455667788));
    CHECK(out.virgl_resource.backing_transition_success);
    CHECK(out.virgl_resource.scanout_id == 3);
    CHECK(out.virgl_resource.scanout_generation ==
          UINT64_C(0x8877665544332211));
    CHECK(out.virgl_resource.rect.x == 10);
    CHECK(out.virgl_resource.rect.y == 20);
    CHECK(out.virgl_resource.rect.width == 640);
    CHECK(out.virgl_resource.rect.height == 480);
    CHECK(out.virgl_resource.scanout_count == 1);
    CHECK(out.virgl_resource.scanouts[0].scanout_id == 3);
    CHECK(out.virgl_resource.scanouts[0].scanout_generation ==
          UINT64_C(0xaabbccddeeff0011));
    CHECK(out.virgl_resource.scanouts[0].scanout.width == 800);
    CHECK(out.virgl_resource.scanouts[0].scanout.height == 600);
    CHECK(out.virgl_resource.scanouts[0].scanout.enabled == 1);
    CHECK(out.virgl_resource.scanouts[0].scanout.primary_resource_id == 0x1234);
    CHECK(out.virgl_resource.scanouts[0].scanout.cursor_resource_id == 0x5678);
    CHECK(out.virgl_resource.scanouts[0].scanout.src_x == 4);
    CHECK(out.virgl_resource.scanouts[0].scanout.src_y == 5);
    CHECK(out.virgl_resource.scanouts[0].scanout.src_w == 320);
    CHECK(out.virgl_resource.scanouts[0].scanout.src_h == 240);
    CHECK(!vgpu_renderer_pop_completion(&out));
}

static void test_frontend_wake_runs_outside_queue_lock(void)
{
    reset_test_counters();
    vgpu_renderer_reset_queues(3);
    vgpu_renderer_set_wake_frontend(wake_frontend_and_pop);

    struct vgpu_renderer_completion completion = test_completion(30, 3);
    CHECK(vgpu_renderer_complete(&completion));
    CHECK(wake_frontend_count == 1);
    CHECK(pop_completion_during_frontend_wake_result);
    CHECK(pop_completion_during_frontend_wake.token.id == 30);

    struct vgpu_renderer_completion out;
    CHECK(!vgpu_renderer_pop_completion(&out));
}

static void test_full_request_queue_rejects_newest(void)
{
    reset_test_counters();
    vgpu_renderer_reset_queues(1);

    for (uint32_t i = 0; i < VGPU_RENDERER_QUEUE_CAPACITY; i++) {
        struct vgpu_renderer_request request = test_request(i);
        CHECK(vgpu_renderer_submit(&request));
    }

    struct vgpu_renderer_request rejected =
        test_request(VGPU_RENDERER_QUEUE_CAPACITY);
    CHECK(!vgpu_renderer_submit(&rejected));

    for (uint32_t i = 0; i < VGPU_RENDERER_QUEUE_CAPACITY; i++) {
        struct vgpu_renderer_request out;
        CHECK(vgpu_renderer_pop_request(&out));
        CHECK(out.token.id == i);
    }
}

static void test_full_completion_queue_releases_newest_response(void)
{
    reset_test_counters();
    vgpu_renderer_reset_queues(5);

    for (uint32_t i = 0; i < VGPU_RENDERER_QUEUE_CAPACITY; i++) {
        struct vgpu_renderer_completion completion = test_completion(i, 5);
        CHECK(vgpu_renderer_complete(&completion));
    }

    struct vgpu_renderer_completion rejected =
        test_completion(VGPU_RENDERER_QUEUE_CAPACITY, 5);
    rejected.release_response = release_response;
    CHECK(!vgpu_renderer_complete(&rejected));
    CHECK(released_response_count == 1);

    for (uint32_t i = 0; i < VGPU_RENDERER_QUEUE_CAPACITY; i++) {
        struct vgpu_renderer_completion out;
        CHECK(vgpu_renderer_pop_completion(&out));
        CHECK(out.token.id == i);
    }
}

static void test_capacity_covers_full_gpu_queue_burst(void)
{
    CHECK(VGPU_RENDERER_QUEUE_CAPACITY >= VIRTIO_GPU_QUEUE_NUM_MAX);
}

static void test_reset_releases_queued_entries_and_rejects_reentrant_work(void)
{
    reset_test_counters();
    vgpu_renderer_reset_queues(1);

    struct vgpu_renderer_request request = test_request(40);
    request.release_payload = release_payload_and_reenter;
    CHECK(vgpu_renderer_submit(&request));

    struct vgpu_renderer_completion completion = test_completion(41, 1);
    completion.release_response = release_response_and_reenter;
    CHECK(vgpu_renderer_complete(&completion));

    vgpu_renderer_reset_queues(2);

    CHECK(released_payload_count == 1);
    CHECK(released_response_count == 1);
    CHECK(!submit_during_reset_result);
    CHECK(!complete_during_reset_result);
    CHECK(!pop_request_during_reset_result);
    CHECK(!pop_completion_during_reset_result);

    struct vgpu_renderer_debug_stats stats;
    vgpu_renderer_debug_snapshot(&stats);
    CHECK(stats.active_generation == 2);
    CHECK(stats.request_depth == 0);
    CHECK(stats.completion_depth == 0);
}

static void release_payload_and_reset(void *payload)
{
    (void) payload;
    released_payload_count++;
    vgpu_renderer_reset_queues(77);
}

static void test_reentrant_reset_does_not_reopen_gate_early(void)
{
    reset_test_counters();
    vgpu_renderer_reset_queues(10);

    struct vgpu_renderer_request request = test_request(70);
    request.release_payload = release_payload_and_reset;
    CHECK(vgpu_renderer_submit(&request));

    vgpu_renderer_reset_queues(11);

    struct vgpu_renderer_debug_stats stats;
    vgpu_renderer_debug_snapshot(&stats);
    CHECK(released_payload_count == 1);
    CHECK(stats.active_generation == 11);
    CHECK(stats.request_depth == 0);
    CHECK(stats.completion_depth == 0);
}

static void *concurrent_reset_main(void *arg)
{
    uint64_t generation = (uintptr_t) arg;
    vgpu_renderer_reset_queues(generation);
    return NULL;
}

static void release_payload_and_spawn_reset(void *payload)
{
    (void) payload;
    released_payload_count++;
    CHECK(pthread_create(&concurrent_reset_thread, NULL, concurrent_reset_main,
                         (void *) (uintptr_t) 88) == 0);
    concurrent_reset_thread_started = true;
}

static void test_concurrent_reset_waits_for_active_reset(void)
{
    reset_test_counters();
    vgpu_renderer_reset_queues(20);

    struct vgpu_renderer_request request = test_request(80);
    request.release_payload = release_payload_and_spawn_reset;
    CHECK(vgpu_renderer_submit(&request));

    vgpu_renderer_reset_queues(21);
    CHECK(concurrent_reset_thread_started);
    CHECK(pthread_join(concurrent_reset_thread, NULL) == 0);

    struct vgpu_renderer_debug_stats stats;
    vgpu_renderer_debug_snapshot(&stats);
    CHECK(released_payload_count == 1);
    CHECK(stats.active_generation == 88);
    CHECK(stats.request_depth == 0);
    CHECK(stats.completion_depth == 0);
}

static void test_debug_snapshot_tracks_queue_progress(void)
{
    reset_test_counters();
    vgpu_renderer_reset_queues(50);

    struct vgpu_renderer_debug_stats stats;
    vgpu_renderer_debug_snapshot(&stats);
    CHECK(stats.active_generation == 50);
    CHECK(stats.request_depth == 0);
    CHECK(stats.completion_depth == 0);

    struct vgpu_renderer_request request = test_request(60);
    CHECK(vgpu_renderer_submit(&request));
    vgpu_renderer_debug_snapshot(&stats);
    CHECK(stats.request_depth == 1);
    CHECK(stats.requests_submitted >= 1);

    struct vgpu_renderer_request out_request;
    CHECK(vgpu_renderer_pop_request(&out_request));
    vgpu_renderer_debug_note_execute_begin(&out_request);
    vgpu_renderer_debug_snapshot(&stats);
    CHECK(stats.request_depth == 0);
    CHECK(stats.requests_popped >= 1);
    CHECK(stats.execute_started == stats.execute_finished + 1);
    CHECK(stats.current_request_type == VGPU_RENDERER_REQ_CTRL);
    CHECK(stats.current_command_type == VIRTIO_GPU_CMD_SUBMIT_3D);
    CHECK(stats.current_token_id == 60);
    CHECK(stats.current_generation == 1);

    vgpu_renderer_debug_note_execute_end();
    vgpu_renderer_debug_snapshot(&stats);
    CHECK(stats.execute_started == stats.execute_finished);
    CHECK(stats.current_execute_seq == 0);
}

int main(void)
{
    test_capacity_covers_full_gpu_queue_burst();
    test_request_fifo_and_wake();
    test_completion_fifo_generation_and_wake();
    test_completion_preserves_virgl_resource_side_effect_metadata();
    test_frontend_wake_runs_outside_queue_lock();
    test_full_request_queue_rejects_newest();
    test_full_completion_queue_releases_newest_response();
    test_reset_releases_queued_entries_and_rejects_reentrant_work();
    test_reentrant_reset_does_not_reopen_gate_early();
    test_concurrent_reset_waits_for_active_reset();
    test_debug_snapshot_tracks_queue_progress();
    return 0;
}
