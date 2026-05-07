#include "vgpu-renderer.h"

/* The ring keeps one slot empty to distinguish full from empty. Make it larger
 * than the guest virtqueue so one full control-queue burst can be deferred.
 */
#define VGPU_RENDERER_QUEUE_SIZE (VIRTIO_GPU_QUEUE_NUM_MAX * 2U)
#define VGPU_RENDERER_QUEUE_MASK (VGPU_RENDERER_QUEUE_SIZE - 1U)

static struct vgpu_renderer_request request_queue[VGPU_RENDERER_QUEUE_SIZE];
static uint32_t request_head;
static uint32_t request_tail;

static struct vgpu_renderer_completion
    completion_queue[VGPU_RENDERER_QUEUE_SIZE];
static uint32_t completion_head;
static uint32_t completion_tail;

static void (*wake_frontend_cb)(void);
static void (*wake_backend_cb)(void);
static uint32_t active_generation;

static uint64_t debug_requests_submitted;
static uint64_t debug_requests_dropped;
static uint64_t debug_requests_popped;
static uint64_t debug_completions_submitted;
static uint64_t debug_completions_dropped;
static uint64_t debug_completions_popped;
static uint64_t debug_queue_resets;
static uint64_t debug_execute_started;
static uint64_t debug_execute_finished;
static uint64_t debug_current_execute_seq;
static uint32_t debug_current_request_type;
static uint32_t debug_current_command_type;
static uint32_t debug_current_token_id;
static uint32_t debug_current_generation;

static uint32_t vgpu_renderer_queue_depth(uint32_t head, uint32_t tail)
{
    return (head - tail) & VGPU_RENDERER_QUEUE_MASK;
}

static bool vgpu_renderer_request_queue_full(void)
{
    uint32_t head = __atomic_load_n(&request_head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&request_tail, __ATOMIC_ACQUIRE);
    return ((head + 1U) & VGPU_RENDERER_QUEUE_MASK) == tail;
}

static bool vgpu_renderer_completion_queue_full(void)
{
    uint32_t head = __atomic_load_n(&completion_head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&completion_tail, __ATOMIC_ACQUIRE);
    return ((head + 1U) & VGPU_RENDERER_QUEUE_MASK) == tail;
}

static void vgpu_renderer_release_queued_requests(void)
{
    uint32_t tail = __atomic_load_n(&request_tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&request_head, __ATOMIC_ACQUIRE);

    while (tail != head) {
        struct vgpu_renderer_request request = request_queue[tail];
        if (request.release_payload)
            request.release_payload(request.payload);
        tail = (tail + 1U) & VGPU_RENDERER_QUEUE_MASK;
    }
}

static void vgpu_renderer_release_queued_completions(void)
{
    uint32_t tail = __atomic_load_n(&completion_tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&completion_head, __ATOMIC_ACQUIRE);

    while (tail != head) {
        struct vgpu_renderer_completion completion = completion_queue[tail];
        if (completion.response && completion.release_response)
            completion.release_response(completion.response);
        tail = (tail + 1U) & VGPU_RENDERER_QUEUE_MASK;
    }
}

void vgpu_renderer_set_wake_frontend(void (*wake_frontend)(void))
{
    __atomic_store_n(&wake_frontend_cb, wake_frontend, __ATOMIC_RELEASE);
}

void vgpu_renderer_set_wake_backend(void (*wake_backend)(void))
{
    __atomic_store_n(&wake_backend_cb, wake_backend, __ATOMIC_RELEASE);
}

bool vgpu_renderer_submit(const struct vgpu_renderer_request *request)
{
    if (!request || vgpu_renderer_request_queue_full()) {
        __atomic_add_fetch(&debug_requests_dropped, 1, __ATOMIC_RELAXED);
        return false;
    }

    uint32_t head = __atomic_load_n(&request_head, __ATOMIC_RELAXED);
    request_queue[head] = *request;
    __atomic_store_n(&request_head, (head + 1U) & VGPU_RENDERER_QUEUE_MASK,
                     __ATOMIC_RELEASE);
    __atomic_add_fetch(&debug_requests_submitted, 1, __ATOMIC_RELAXED);

    void (*wake_frontend)(void) =
        __atomic_load_n(&wake_frontend_cb, __ATOMIC_ACQUIRE);
    if (wake_frontend)
        wake_frontend();

    return true;
}

bool vgpu_renderer_pop_request(struct vgpu_renderer_request *request)
{
    if (!request)
        return false;

    uint32_t tail = __atomic_load_n(&request_tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&request_head, __ATOMIC_ACQUIRE);
    if (tail == head)
        return false;

    *request = request_queue[tail];
    __atomic_store_n(&request_tail, (tail + 1U) & VGPU_RENDERER_QUEUE_MASK,
                     __ATOMIC_RELEASE);
    __atomic_add_fetch(&debug_requests_popped, 1, __ATOMIC_RELAXED);
    return true;
}

bool vgpu_renderer_complete(const struct vgpu_renderer_completion *completion)
{
    if (!completion || vgpu_renderer_completion_queue_full()) {
        __atomic_add_fetch(&debug_completions_dropped, 1, __ATOMIC_RELAXED);
        return false;
    }

    uint32_t generation = __atomic_load_n(&active_generation, __ATOMIC_ACQUIRE);
    if (completion->token.generation != generation) {
        __atomic_add_fetch(&debug_completions_dropped, 1, __ATOMIC_RELAXED);
        return false;
    }

    uint32_t head = __atomic_load_n(&completion_head, __ATOMIC_RELAXED);
    completion_queue[head] = *completion;
    __atomic_store_n(&completion_head, (head + 1U) & VGPU_RENDERER_QUEUE_MASK,
                     __ATOMIC_RELEASE);
    __atomic_add_fetch(&debug_completions_submitted, 1, __ATOMIC_RELAXED);

    void (*wake_backend)(void) =
        __atomic_load_n(&wake_backend_cb, __ATOMIC_ACQUIRE);
    if (wake_backend)
        wake_backend();

    return true;
}

bool vgpu_renderer_pop_completion(struct vgpu_renderer_completion *completion)
{
    if (!completion)
        return false;

    uint32_t generation = __atomic_load_n(&active_generation, __ATOMIC_ACQUIRE);

    for (;;) {
        uint32_t tail = __atomic_load_n(&completion_tail, __ATOMIC_RELAXED);
        uint32_t head = __atomic_load_n(&completion_head, __ATOMIC_ACQUIRE);
        if (tail == head)
            return false;

        struct vgpu_renderer_completion out = completion_queue[tail];
        __atomic_store_n(&completion_tail,
                         (tail + 1U) & VGPU_RENDERER_QUEUE_MASK,
                         __ATOMIC_RELEASE);
        if (out.token.generation == generation) {
            *completion = out;
            __atomic_add_fetch(&debug_completions_popped, 1, __ATOMIC_RELAXED);
            return true;
        }
        __atomic_add_fetch(&debug_completions_dropped, 1, __ATOMIC_RELAXED);
    }
}

void vgpu_renderer_reset_queues(uint32_t generation)
{
    __atomic_add_fetch(&debug_queue_resets, 1, __ATOMIC_RELAXED);
    vgpu_renderer_release_queued_requests();
    vgpu_renderer_release_queued_completions();
    __atomic_store_n(&active_generation, generation, __ATOMIC_RELEASE);
    __atomic_store_n(&request_head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&request_tail, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&completion_head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&completion_tail, 0, __ATOMIC_RELEASE);
}

void vgpu_renderer_debug_note_execute_begin(
    const struct vgpu_renderer_request *request)
{
    if (!request)
        return;

    uint64_t seq =
        __atomic_add_fetch(&debug_execute_started, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&debug_current_request_type, (uint32_t) request->type,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&debug_current_command_type, request->command_type,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&debug_current_token_id, request->token.id,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&debug_current_generation, request->token.generation,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&debug_current_execute_seq, seq, __ATOMIC_RELEASE);
}

void vgpu_renderer_debug_note_execute_end(void)
{
    __atomic_add_fetch(&debug_execute_finished, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&debug_current_execute_seq, 0, __ATOMIC_RELEASE);
}

void vgpu_renderer_debug_snapshot(struct vgpu_renderer_debug_stats *stats)
{
    if (!stats)
        return;

    uint32_t req_head = __atomic_load_n(&request_head, __ATOMIC_ACQUIRE);
    uint32_t req_tail = __atomic_load_n(&request_tail, __ATOMIC_ACQUIRE);
    uint32_t done_head = __atomic_load_n(&completion_head, __ATOMIC_ACQUIRE);
    uint32_t done_tail = __atomic_load_n(&completion_tail, __ATOMIC_ACQUIRE);

    *stats = (struct vgpu_renderer_debug_stats) {
        .active_generation =
            __atomic_load_n(&active_generation, __ATOMIC_ACQUIRE),
        .request_head = req_head,
        .request_tail = req_tail,
        .request_depth = vgpu_renderer_queue_depth(req_head, req_tail),
        .completion_head = done_head,
        .completion_tail = done_tail,
        .completion_depth = vgpu_renderer_queue_depth(done_head, done_tail),
        .requests_submitted =
            __atomic_load_n(&debug_requests_submitted, __ATOMIC_RELAXED),
        .requests_dropped =
            __atomic_load_n(&debug_requests_dropped, __ATOMIC_RELAXED),
        .requests_popped =
            __atomic_load_n(&debug_requests_popped, __ATOMIC_RELAXED),
        .completions_submitted =
            __atomic_load_n(&debug_completions_submitted, __ATOMIC_RELAXED),
        .completions_dropped =
            __atomic_load_n(&debug_completions_dropped, __ATOMIC_RELAXED),
        .completions_popped =
            __atomic_load_n(&debug_completions_popped, __ATOMIC_RELAXED),
        .queue_resets = __atomic_load_n(&debug_queue_resets, __ATOMIC_RELAXED),
        .execute_started =
            __atomic_load_n(&debug_execute_started, __ATOMIC_RELAXED),
        .execute_finished =
            __atomic_load_n(&debug_execute_finished, __ATOMIC_RELAXED),
        .current_execute_seq =
            __atomic_load_n(&debug_current_execute_seq, __ATOMIC_ACQUIRE),
        .current_request_type =
            (enum vgpu_renderer_request_type) __atomic_load_n(
                &debug_current_request_type, __ATOMIC_RELAXED),
        .current_command_type =
            __atomic_load_n(&debug_current_command_type, __ATOMIC_RELAXED),
        .current_token_id =
            __atomic_load_n(&debug_current_token_id, __ATOMIC_RELAXED),
        .current_generation =
            __atomic_load_n(&debug_current_generation, __ATOMIC_RELAXED),
    };
}
