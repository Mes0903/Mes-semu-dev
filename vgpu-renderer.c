#include "vgpu-renderer.h"

#define VGPU_RENDERER_QUEUE_SIZE 256U
#define VGPU_RENDERER_QUEUE_MASK (VGPU_RENDERER_QUEUE_SIZE - 1U)

static struct vgpu_renderer_request request_queue[VGPU_RENDERER_QUEUE_SIZE];
static uint32_t request_head;
static uint32_t request_tail;

static struct vgpu_renderer_completion completion_queue[VGPU_RENDERER_QUEUE_SIZE];
static uint32_t completion_head;
static uint32_t completion_tail;

static void (*wake_frontend_cb)(void);
static void (*wake_backend_cb)(void);
static uint32_t active_generation;

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
    if (!request || vgpu_renderer_request_queue_full())
        return false;

    uint32_t head = __atomic_load_n(&request_head, __ATOMIC_RELAXED);
    request_queue[head] = *request;
    __atomic_store_n(&request_head, (head + 1U) & VGPU_RENDERER_QUEUE_MASK,
                     __ATOMIC_RELEASE);

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
    return true;
}

bool vgpu_renderer_complete(const struct vgpu_renderer_completion *completion)
{
    if (!completion || vgpu_renderer_completion_queue_full())
        return false;

    uint32_t generation =
        __atomic_load_n(&active_generation, __ATOMIC_ACQUIRE);
    if (completion->token.generation != generation)
        return false;

    uint32_t head = __atomic_load_n(&completion_head, __ATOMIC_RELAXED);
    completion_queue[head] = *completion;
    __atomic_store_n(&completion_head,
                     (head + 1U) & VGPU_RENDERER_QUEUE_MASK,
                     __ATOMIC_RELEASE);

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

    uint32_t generation =
        __atomic_load_n(&active_generation, __ATOMIC_ACQUIRE);

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
            return true;
        }
    }
}

void vgpu_renderer_reset_queues(uint32_t generation)
{
    vgpu_renderer_release_queued_requests();
    vgpu_renderer_release_queued_completions();
    __atomic_store_n(&active_generation, generation, __ATOMIC_RELEASE);
    __atomic_store_n(&request_head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&request_tail, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&completion_head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&completion_tail, 0, __ATOMIC_RELEASE);
}
