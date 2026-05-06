#include <stdint.h>
#include <stdio.h>

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

static void test_wake_frontend(void)
{
    wake_count++;
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

static int test_full_queue_rejects_newest_request(void)
{
    vgpu_renderer_reset_queues(1);

    uint32_t accepted = 0;
    for (uint32_t i = 0; i < 1024; i++) {
        struct vgpu_renderer_request request = test_request(i);
        if (!vgpu_renderer_submit(&request))
            break;
        accepted++;
    }

    CHECK(accepted > 0);
    CHECK(accepted < 1024);

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

int main(void)
{
    CHECK(test_request_fifo_ordering() == 0);
    CHECK(test_completion_fifo_ordering() == 0);
    CHECK(test_full_queue_rejects_newest_request() == 0);
    CHECK(test_reset_generation_filters_stale_completions() == 0);
    return 0;
}
