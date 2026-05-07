#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../vgpu-renderer.h"

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static int wake_count;
static int backend_wake_count;
static int released_payload_count;
static void *last_released_payload;
static int released_response_count;
static void *last_released_response;
static bool submit_during_reset_result;
static bool complete_during_reset_result;
static bool pop_request_during_reset_result;
static bool pop_completion_during_reset_result;

#define CONCURRENT_PRODUCERS 16U
#define CONCURRENT_REQUESTS_PER_PRODUCER 24U
#define CONCURRENT_REQUESTS_TOTAL \
    (CONCURRENT_PRODUCERS * CONCURRENT_REQUESTS_PER_PRODUCER)

struct submit_worker {
    pthread_barrier_t *barrier;
    uint32_t base_id;
    int failed;
};

static void test_wake_frontend(void)
{
    wake_count++;
}

static void test_wake_backend(void)
{
    backend_wake_count++;
}

static struct vgpu_renderer_request test_request(uint32_t id)
{
    return (struct vgpu_renderer_request) {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.id = id, .generation = 1},
        .command_type = VIRTIO_GPU_CMD_SUBMIT_3D,
        .payload = (void *) (uintptr_t) (id + 1),
        .payload_size = id + 16,
    };
}

static struct vgpu_renderer_completion test_completion(uint32_t id,
                                                       uint32_t generation)
{
    return (struct vgpu_renderer_completion) {
        .type = VGPU_RENDERER_DONE_CTRL,
        .token = {.id = id, .generation = generation},
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
}

static void release_payload(void *payload)
{
    released_payload_count++;
    last_released_payload = payload;
}

static void release_response(void *response)
{
    released_response_count++;
    last_released_response = response;
}

static void release_payload_and_submit_during_reset(void *payload)
{
    released_payload_count++;
    last_released_payload = payload;

    struct vgpu_renderer_request nested = test_request(90);
    submit_during_reset_result = vgpu_renderer_submit(&nested);
}

static void release_response_and_complete_during_reset(void *response)
{
    released_response_count++;
    last_released_response = response;

    struct vgpu_renderer_completion nested = test_completion(91, 1);
    complete_during_reset_result = vgpu_renderer_complete(&nested);
}

static void release_payload_and_pop_during_reset(void *payload)
{
    released_payload_count++;
    last_released_payload = payload;

    struct vgpu_renderer_request out;
    pop_request_during_reset_result = vgpu_renderer_pop_request(&out);
}

static void release_response_and_pop_during_reset(void *response)
{
    released_response_count++;
    last_released_response = response;

    struct vgpu_renderer_completion out;
    pop_completion_during_reset_result = vgpu_renderer_pop_completion(&out);
}

static void *submit_worker_main(void *arg)
{
    struct submit_worker *worker = arg;

    pthread_barrier_wait(worker->barrier);
    for (uint32_t i = 0; i < CONCURRENT_REQUESTS_PER_PRODUCER; i++) {
        struct vgpu_renderer_request request =
            test_request(worker->base_id + i);
        if (!vgpu_renderer_submit(&request))
            worker->failed++;
    }

    return NULL;
}

static void *complete_worker_main(void *arg)
{
    struct submit_worker *worker = arg;

    pthread_barrier_wait(worker->barrier);
    for (uint32_t i = 0; i < CONCURRENT_REQUESTS_PER_PRODUCER; i++) {
        struct vgpu_renderer_completion completion =
            test_completion(worker->base_id + i, 1);
        if (!vgpu_renderer_complete(&completion))
            worker->failed++;
    }

    return NULL;
}

static int test_request_fifo_ordering(void)
{
    vgpu_renderer_reset_queues(1);
    wake_count = 0;
    vgpu_renderer_set_wake_frontend(test_wake_frontend);

    struct vgpu_renderer_request first = test_request(10);
    struct vgpu_renderer_request second = test_request(11);
    CHECK(vgpu_renderer_submit(&first));
    CHECK(vgpu_renderer_submit(&second));
    CHECK(wake_count == 2);

    struct vgpu_renderer_request out;
    CHECK(vgpu_renderer_pop_request(&out));
    CHECK(out.token.id == 10);
    CHECK(out.payload == first.payload);
    CHECK(out.payload_size == first.payload_size);

    CHECK(vgpu_renderer_pop_request(&out));
    CHECK(out.token.id == 11);
    CHECK(out.payload == second.payload);
    CHECK(out.payload_size == second.payload_size);

    CHECK(!vgpu_renderer_pop_request(&out));
    return 0;
}

static int test_completion_fifo_ordering(void)
{
    vgpu_renderer_reset_queues(1);

    struct vgpu_renderer_completion first = test_completion(20, 1);
    struct vgpu_renderer_completion second = test_completion(21, 1);
    CHECK(vgpu_renderer_complete(&first));
    CHECK(vgpu_renderer_complete(&second));

    struct vgpu_renderer_completion out;
    CHECK(vgpu_renderer_pop_completion(&out));
    CHECK(out.token.id == 20);
    CHECK(out.response_type == VIRTIO_GPU_RESP_OK_NODATA);

    CHECK(vgpu_renderer_pop_completion(&out));
    CHECK(out.token.id == 21);

    CHECK(!vgpu_renderer_pop_completion(&out));
    return 0;
}

static int test_completion_wakes_backend(void)
{
    vgpu_renderer_reset_queues(1);
    backend_wake_count = 0;
    vgpu_renderer_set_wake_backend(test_wake_backend);

    struct vgpu_renderer_completion current = test_completion(22, 1);
    struct vgpu_renderer_completion stale = test_completion(23, 0);
    CHECK(vgpu_renderer_complete(&current));
    CHECK(backend_wake_count == 1);
    CHECK(!vgpu_renderer_complete(&stale));
    CHECK(backend_wake_count == 1);

    vgpu_renderer_set_wake_backend(NULL);
    return 0;
}

static int test_full_queue_rejects_newest_request(void)
{
    vgpu_renderer_reset_queues(1);

    uint32_t accepted = 0;
    for (uint32_t i = 0; i < VIRTIO_GPU_QUEUE_NUM_MAX * 4U; i++) {
        struct vgpu_renderer_request request = test_request(i);
        if (!vgpu_renderer_submit(&request))
            break;
        accepted++;
    }

    CHECK(accepted >= VIRTIO_GPU_QUEUE_NUM_MAX);
    CHECK(accepted < VIRTIO_GPU_QUEUE_NUM_MAX * 4U);

    struct vgpu_renderer_request rejected = test_request(accepted);
    CHECK(!vgpu_renderer_submit(&rejected));

    for (uint32_t i = 0; i < accepted; i++) {
        struct vgpu_renderer_request out;
        CHECK(vgpu_renderer_pop_request(&out));
        CHECK(out.token.id == i);
    }

    struct vgpu_renderer_request out;
    CHECK(!vgpu_renderer_pop_request(&out));
    return 0;
}

static int test_request_queue_accepts_full_virtqueue_burst(void)
{
    vgpu_renderer_reset_queues(1);

    for (uint32_t i = 0; i < VIRTIO_GPU_QUEUE_NUM_MAX; i++) {
        struct vgpu_renderer_request request = test_request(i);
        CHECK(vgpu_renderer_submit(&request));
    }

    for (uint32_t i = 0; i < VIRTIO_GPU_QUEUE_NUM_MAX; i++) {
        struct vgpu_renderer_request out;
        CHECK(vgpu_renderer_pop_request(&out));
        CHECK(out.token.id == i);
    }

    struct vgpu_renderer_request out;
    CHECK(!vgpu_renderer_pop_request(&out));
    return 0;
}

static int test_concurrent_request_submitters_do_not_lose_requests(void)
{
    vgpu_renderer_reset_queues(1);
    vgpu_renderer_set_wake_frontend(NULL);

    pthread_t threads[CONCURRENT_PRODUCERS];
    struct submit_worker workers[CONCURRENT_PRODUCERS];
    pthread_barrier_t barrier;
    CHECK(pthread_barrier_init(&barrier, NULL, CONCURRENT_PRODUCERS) == 0);

    for (uint32_t i = 0; i < CONCURRENT_PRODUCERS; i++) {
        workers[i] = (struct submit_worker) {
            .barrier = &barrier,
            .base_id = i * CONCURRENT_REQUESTS_PER_PRODUCER,
        };
        CHECK(pthread_create(&threads[i], NULL, submit_worker_main,
                             &workers[i]) == 0);
    }

    for (uint32_t i = 0; i < CONCURRENT_PRODUCERS; i++)
        CHECK(pthread_join(threads[i], NULL) == 0);
    pthread_barrier_destroy(&barrier);

    for (uint32_t i = 0; i < CONCURRENT_PRODUCERS; i++)
        CHECK(workers[i].failed == 0);

    bool seen[CONCURRENT_REQUESTS_TOTAL];
    memset(seen, 0, sizeof(seen));

    uint32_t popped = 0;
    struct vgpu_renderer_request out;
    while (vgpu_renderer_pop_request(&out)) {
        CHECK(out.token.id < CONCURRENT_REQUESTS_TOTAL);
        CHECK(!seen[out.token.id]);
        seen[out.token.id] = true;
        popped++;
    }

    CHECK(popped == CONCURRENT_REQUESTS_TOTAL);
    for (uint32_t i = 0; i < CONCURRENT_REQUESTS_TOTAL; i++)
        CHECK(seen[i]);

    return 0;
}

static int test_concurrent_completion_submitters_do_not_lose_completions(void)
{
    vgpu_renderer_reset_queues(1);
    vgpu_renderer_set_wake_backend(NULL);

    pthread_t threads[CONCURRENT_PRODUCERS];
    struct submit_worker workers[CONCURRENT_PRODUCERS];
    pthread_barrier_t barrier;
    CHECK(pthread_barrier_init(&barrier, NULL, CONCURRENT_PRODUCERS) == 0);

    for (uint32_t i = 0; i < CONCURRENT_PRODUCERS; i++) {
        workers[i] = (struct submit_worker) {
            .barrier = &barrier,
            .base_id = i * CONCURRENT_REQUESTS_PER_PRODUCER,
        };
        CHECK(pthread_create(&threads[i], NULL, complete_worker_main,
                             &workers[i]) == 0);
    }

    for (uint32_t i = 0; i < CONCURRENT_PRODUCERS; i++)
        CHECK(pthread_join(threads[i], NULL) == 0);
    pthread_barrier_destroy(&barrier);

    for (uint32_t i = 0; i < CONCURRENT_PRODUCERS; i++)
        CHECK(workers[i].failed == 0);

    bool seen[CONCURRENT_REQUESTS_TOTAL];
    memset(seen, 0, sizeof(seen));

    uint32_t popped = 0;
    struct vgpu_renderer_completion out;
    while (vgpu_renderer_pop_completion(&out)) {
        CHECK(out.token.id < CONCURRENT_REQUESTS_TOTAL);
        CHECK(!seen[out.token.id]);
        seen[out.token.id] = true;
        popped++;
    }

    CHECK(popped == CONCURRENT_REQUESTS_TOTAL);
    for (uint32_t i = 0; i < CONCURRENT_REQUESTS_TOTAL; i++)
        CHECK(seen[i]);

    return 0;
}

static int test_reset_generation_filters_stale_completions(void)
{
    vgpu_renderer_reset_queues(7);

    struct vgpu_renderer_completion stale = test_completion(30, 6);
    struct vgpu_renderer_completion current = test_completion(31, 7);
    CHECK(!vgpu_renderer_complete(&stale));
    CHECK(vgpu_renderer_complete(&current));

    struct vgpu_renderer_completion out;
    CHECK(vgpu_renderer_pop_completion(&out));
    CHECK(out.token.id == 31);
    CHECK(out.token.generation == 7);
    CHECK(!vgpu_renderer_pop_completion(&out));
    return 0;
}

static int test_reset_releases_queued_request_payloads(void)
{
    vgpu_renderer_reset_queues(1);
    released_payload_count = 0;
    last_released_payload = NULL;

    struct vgpu_renderer_request request = test_request(40);
    request.release_payload = release_payload;
    CHECK(vgpu_renderer_submit(&request));

    vgpu_renderer_reset_queues(2);

    CHECK(released_payload_count == 1);
    CHECK(last_released_payload == request.payload);

    struct vgpu_renderer_request out;
    CHECK(!vgpu_renderer_pop_request(&out));
    return 0;
}

static int test_reset_releases_queued_completion_responses(void)
{
    vgpu_renderer_reset_queues(1);
    released_response_count = 0;
    last_released_response = NULL;

    struct vgpu_renderer_completion completion = test_completion(41, 1);
    completion.response = (void *) (uintptr_t) 0x41410000U;
    completion.response_size = 32;
    completion.release_response = release_response;
    CHECK(vgpu_renderer_complete(&completion));

    vgpu_renderer_reset_queues(2);

    CHECK(released_response_count == 1);
    CHECK(last_released_response == completion.response);

    struct vgpu_renderer_completion out;
    CHECK(!vgpu_renderer_pop_completion(&out));
    return 0;
}

static int test_reset_rejects_request_submitters_until_queue_is_cleared(void)
{
    vgpu_renderer_reset_queues(1);
    submit_during_reset_result = true;

    struct vgpu_renderer_request request = test_request(42);
    request.release_payload = release_payload_and_submit_during_reset;
    CHECK(vgpu_renderer_submit(&request));

    vgpu_renderer_reset_queues(2);

    CHECK(released_payload_count >= 1);
    CHECK(!submit_during_reset_result);

    struct vgpu_renderer_request out;
    CHECK(!vgpu_renderer_pop_request(&out));
    return 0;
}

static int test_reset_rejects_completion_submitters_until_queue_is_cleared(void)
{
    vgpu_renderer_reset_queues(1);
    complete_during_reset_result = true;

    struct vgpu_renderer_completion completion = test_completion(43, 1);
    completion.response = (void *) (uintptr_t) 0x43430000U;
    completion.release_response = release_response_and_complete_during_reset;
    CHECK(vgpu_renderer_complete(&completion));

    vgpu_renderer_reset_queues(2);

    CHECK(released_response_count >= 1);
    CHECK(!complete_during_reset_result);

    struct vgpu_renderer_completion out;
    CHECK(!vgpu_renderer_pop_completion(&out));
    return 0;
}

static int test_reset_rejects_request_consumers_until_queue_is_cleared(void)
{
    vgpu_renderer_reset_queues(1);
    pop_request_during_reset_result = true;

    struct vgpu_renderer_request request = test_request(44);
    request.release_payload = release_payload_and_pop_during_reset;
    CHECK(vgpu_renderer_submit(&request));

    vgpu_renderer_reset_queues(2);

    CHECK(released_payload_count >= 1);
    CHECK(!pop_request_during_reset_result);

    struct vgpu_renderer_request out;
    CHECK(!vgpu_renderer_pop_request(&out));
    return 0;
}

static int test_reset_rejects_completion_consumers_until_queue_is_cleared(void)
{
    vgpu_renderer_reset_queues(1);
    pop_completion_during_reset_result = true;

    struct vgpu_renderer_completion completion = test_completion(45, 1);
    completion.response = (void *) (uintptr_t) 0x45450000U;
    completion.release_response = release_response_and_pop_during_reset;
    CHECK(vgpu_renderer_complete(&completion));

    vgpu_renderer_reset_queues(2);

    CHECK(released_response_count >= 1);
    CHECK(!pop_completion_during_reset_result);

    struct vgpu_renderer_completion out;
    CHECK(!vgpu_renderer_pop_completion(&out));
    return 0;
}

static int test_debug_snapshot_tracks_queue_progress(void)
{
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

    vgpu_renderer_debug_note_execute_end();
    vgpu_renderer_debug_snapshot(&stats);
    CHECK(stats.execute_started == stats.execute_finished);
    CHECK(stats.current_execute_seq == 0);

    struct vgpu_renderer_completion completion = test_completion(61, 50);
    CHECK(vgpu_renderer_complete(&completion));
    vgpu_renderer_debug_snapshot(&stats);
    CHECK(stats.completion_depth == 1);
    CHECK(stats.completions_submitted >= 1);

    struct vgpu_renderer_completion out_completion;
    CHECK(vgpu_renderer_pop_completion(&out_completion));
    vgpu_renderer_debug_snapshot(&stats);
    CHECK(stats.completion_depth == 0);
    CHECK(stats.completions_popped >= 1);
    return 0;
}

int main(void)
{
    CHECK(test_request_fifo_ordering() == 0);
    CHECK(test_completion_fifo_ordering() == 0);
    CHECK(test_completion_wakes_backend() == 0);
    CHECK(test_full_queue_rejects_newest_request() == 0);
    CHECK(test_request_queue_accepts_full_virtqueue_burst() == 0);
    CHECK(test_concurrent_request_submitters_do_not_lose_requests() == 0);
    CHECK(test_concurrent_completion_submitters_do_not_lose_completions() == 0);
    CHECK(test_reset_generation_filters_stale_completions() == 0);
    CHECK(test_reset_releases_queued_request_payloads() == 0);
    CHECK(test_reset_releases_queued_completion_responses() == 0);
    CHECK(test_reset_rejects_request_submitters_until_queue_is_cleared() == 0);
    CHECK(test_reset_rejects_completion_submitters_until_queue_is_cleared() ==
          0);
    CHECK(test_reset_rejects_request_consumers_until_queue_is_cleared() == 0);
    CHECK(test_reset_rejects_completion_consumers_until_queue_is_cleared() ==
          0);
    CHECK(test_debug_snapshot_tracks_queue_progress() == 0);
    return 0;
}
