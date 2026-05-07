#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../vgpu-renderer.h"
#include "../virtio-gpu.h"

#define TEST_RAM_SIZE 4096U
#define DESC_WORD 64U
#define AVAIL_WORD 128U
#define USED_WORD 160U
#define REQ_ADDR 1024U
#define REQ2_ADDR 1152U
#define RESP_ADDR 2048U
#define RESP2_ADDR 2176U

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static uint8_t g_ram[TEST_RAM_SIZE];
static bool g_defer_unfenced;
static bool g_defer_tokenized;
static bool g_complete_during_submit;
static bool g_cancel_after_defer;
static bool g_cancel_result;

static void fake_submit_3d(virtio_gpu_state_t *vgpu,
                           struct virtq_desc *vq_desc,
                           uint32_t *plen);

const struct virtio_gpu_cmd_backend g_virtio_gpu_backend = {
    .submit_3d = fake_submit_3d,
};

uint32_t virtio_gpu_backend_get_num_capsets(void)
{
    return 0;
}

void vm_set_exception(hart_t *vm, uint32_t cause, uint32_t val)
{
    (void) vm;
    (void) cause;
    (void) val;
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

#include "../virtio-gpu.c"

static uint32_t *ram_words(void)
{
    return (uint32_t *) g_ram;
}

static struct virtio_gpu_ctrl_hdr *response_hdr(void)
{
    return (struct virtio_gpu_ctrl_hdr *) &g_ram[RESP_ADDR];
}

static struct virtio_gpu_ctrl_hdr *response2_hdr(void)
{
    return (struct virtio_gpu_ctrl_hdr *) &g_ram[RESP2_ADDR];
}

static uint16_t used_idx(void)
{
    return (uint16_t) (ram_words()[USED_WORD] >> 16);
}

static uint32_t used_elem_id(uint16_t slot)
{
    return ram_words()[USED_WORD + 1U + slot * 2U];
}

static uint32_t used_elem_len(uint16_t slot)
{
    return ram_words()[USED_WORD + 2U + slot * 2U];
}

static void write_desc(uint16_t index,
                       uint64_t addr,
                       uint32_t len,
                       uint16_t flags,
                       uint16_t next)
{
    uint32_t *ram = ram_words();
    uint32_t base = DESC_WORD + index * 4U;
    ram[base] = (uint32_t) addr;
    ram[base + 1U] = (uint32_t) (addr >> 32);
    ram[base + 2U] = len;
    ram[base + 3U] = (uint32_t) flags | ((uint32_t) next << 16);
}

static virtio_gpu_state_t fresh_vgpu(void)
{
    memset(g_ram, 0, sizeof(g_ram));
    memset(&virtio_gpu_data, 0, sizeof(virtio_gpu_data));
    g_defer_unfenced = false;
    g_defer_tokenized = false;
    g_complete_during_submit = false;
    g_cancel_after_defer = false;
    g_cancel_result = false;

    virtio_gpu_state_t vgpu = {0};
    vgpu.ram = ram_words();
    vgpu.priv = &virtio_gpu_data;
    vgpu.Status = VIRTIO_STATUS__DRIVER_OK;

    virtio_gpu_queue_t *queue = &vgpu.queues[VIRTIO_GPU_CONTROLQ];
    queue->QueueNum = 8;
    queue->QueueDesc = DESC_WORD;
    queue->QueueAvail = AVAIL_WORD;
    queue->QueueUsed = USED_WORD;
    queue->ready = true;
    queue->last_avail = 0;
    return vgpu;
}

static void queue_one_submit(uint32_t flags,
                             uint64_t fence_id,
                             uint32_t ctx_id,
                             uint8_t ring_idx)
{
    struct virtio_gpu_ctrl_hdr request = {
        .type = VIRTIO_GPU_CMD_SUBMIT_3D,
        .flags = flags,
        .fence_id = fence_id,
        .ctx_id = ctx_id,
        .ring_idx = ring_idx,
    };

    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    write_desc(0, REQ_ADDR, sizeof(request), VIRTIO_DESC_F_NEXT, 1);
    write_desc(1, RESP_ADDR, sizeof(struct virtio_gpu_ctrl_hdr),
               VIRTIO_DESC_F_WRITE, 0);

    ram_words()[AVAIL_WORD] = 1U << 16;
    ram_words()[AVAIL_WORD + 1U] = 0;
}

static void queue_two_unfenced_submits(void)
{
    struct virtio_gpu_ctrl_hdr first = {
        .type = VIRTIO_GPU_CMD_SUBMIT_3D,
        .ctx_id = 101,
    };
    struct virtio_gpu_ctrl_hdr second = {
        .type = VIRTIO_GPU_CMD_SUBMIT_3D,
        .ctx_id = 202,
    };

    memcpy(&g_ram[REQ_ADDR], &first, sizeof(first));
    memcpy(&g_ram[REQ2_ADDR], &second, sizeof(second));
    write_desc(0, REQ_ADDR, sizeof(first), VIRTIO_DESC_F_NEXT, 1);
    write_desc(1, RESP_ADDR, sizeof(struct virtio_gpu_ctrl_hdr),
               VIRTIO_DESC_F_WRITE, 0);
    write_desc(2, REQ2_ADDR, sizeof(second), VIRTIO_DESC_F_NEXT, 3);
    write_desc(3, RESP2_ADDR, sizeof(struct virtio_gpu_ctrl_hdr),
               VIRTIO_DESC_F_WRITE, 0);

    ram_words()[AVAIL_WORD] = 2U << 16;
    ram_words()[AVAIL_WORD + 1U] = 2U << 16;
}

static void fake_submit_3d(virtio_gpu_state_t *vgpu,
                           struct virtq_desc *vq_desc,
                           uint32_t *plen)
{
    struct virtio_gpu_ctrl_hdr *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    int resp_idx = virtio_gpu_get_response_desc(
        vq_desc, VIRTIO_GPU_MAX_DESC, sizeof(struct virtio_gpu_ctrl_hdr));
    if (resp_idx < 0) {
        *plen = 0;
        return;
    }

    uint32_t generation = virtio_gpu_ctrl_generation(vgpu);
    if (g_defer_tokenized &&
        virtio_gpu_defer_ctrl_response_token(vgpu, request, &vq_desc[resp_idx],
                                             VIRTIO_GPU_RESP_OK_NODATA,
                                             generation, request->ctx_id)) {
        *plen = VIRTIO_GPU_RESPONSE_DEFERRED;
        return;
    }

    if ((g_defer_unfenced || (request->flags & VIRTIO_GPU_FLAG_FENCE)) &&
        virtio_gpu_defer_ctrl_response(vgpu, request, &vq_desc[resp_idx],
                                       VIRTIO_GPU_RESP_OK_NODATA, generation)) {
        if (g_complete_during_submit) {
            bool context_fence =
                (request->flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) != 0;
            virtio_gpu_complete_ctrl_response(
                vgpu, generation, request->fence_id, context_fence,
                request->ctx_id, request->ring_idx);
        }
        if (g_cancel_after_defer) {
            g_cancel_result =
                virtio_gpu_cancel_ctrl_response(vgpu, generation, request);
            *plen = virtio_gpu_write_ctrl_response(
                vgpu, request, &vq_desc[resp_idx], VIRTIO_GPU_RESP_ERR_UNSPEC);
            return;
        }
        *plen = VIRTIO_GPU_RESPONSE_DEFERRED;
        return;
    }

    *plen = virtio_gpu_write_ctrl_response(vgpu, request, &vq_desc[resp_idx],
                                           VIRTIO_GPU_RESP_OK_NODATA);
}

static int test_fenced_submit_completes_used_ring_after_fence(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    queue_one_submit(VIRTIO_GPU_FLAG_FENCE, 7, 0, 0);

    virtio_gpu_queue_notify_handler(&vgpu, VIRTIO_GPU_CONTROLQ);

    CHECK(vgpu.queues[VIRTIO_GPU_CONTROLQ].last_avail == 1);
    CHECK(used_idx() == 0);
    CHECK(response_hdr()->type == 0);
    CHECK(vgpu.InterruptStatus == 0);

    virtio_gpu_complete_ctrl_response(&vgpu, virtio_gpu_ctrl_generation(&vgpu),
                                      7, false, 0, 0);

    CHECK(used_idx() == 1);
    CHECK(used_elem_id(0) == 0);
    CHECK(used_elem_len(0) == sizeof(struct virtio_gpu_ctrl_hdr));
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(response_hdr()->flags == VIRTIO_GPU_FLAG_FENCE);
    CHECK(response_hdr()->fence_id == 7);
    CHECK(vgpu.InterruptStatus == VIRTIO_INT__USED_RING);

    return 0;
}

static int test_context_fence_requires_matching_context_and_ring(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    queue_one_submit(VIRTIO_GPU_FLAG_FENCE | VIRTIO_GPU_FLAG_INFO_RING_IDX,
                     0x123456789ULL, 11, 3);

    virtio_gpu_queue_notify_handler(&vgpu, VIRTIO_GPU_CONTROLQ);

    CHECK(used_idx() == 0);
    virtio_gpu_complete_ctrl_response(&vgpu, virtio_gpu_ctrl_generation(&vgpu),
                                      0x123456789ULL, true, 11, 4);
    CHECK(used_idx() == 0);
    CHECK(response_hdr()->type == 0);

    virtio_gpu_complete_ctrl_response(&vgpu, virtio_gpu_ctrl_generation(&vgpu),
                                      0x123456789ULL, true, 11, 3);

    CHECK(used_idx() == 1);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(response_hdr()->flags == VIRTIO_GPU_FLAG_FENCE);
    CHECK(response_hdr()->fence_id == 0x123456789ULL);

    return 0;
}

static int test_reset_drops_pending_fence_without_writing_stale_response(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    queue_one_submit(VIRTIO_GPU_FLAG_FENCE, 9, 0, 0);

    virtio_gpu_queue_notify_handler(&vgpu, VIRTIO_GPU_CONTROLQ);
    CHECK(used_idx() == 0);

    uint32_t stale_generation = virtio_gpu_ctrl_generation(&vgpu);
    virtio_gpu_update_status(&vgpu, 0);
    virtio_gpu_complete_ctrl_response(&vgpu, stale_generation, 9, false, 0, 0);

    CHECK(response_hdr()->type == 0);

    return 0;
}

static int test_unfenced_deferred_submit_completes_generically(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    g_defer_unfenced = true;
    queue_one_submit(0, 0, 0, 0);

    virtio_gpu_queue_notify_handler(&vgpu, VIRTIO_GPU_CONTROLQ);

    CHECK(vgpu.queues[VIRTIO_GPU_CONTROLQ].last_avail == 1);
    CHECK(used_idx() == 0);
    CHECK(response_hdr()->type == 0);

    virtio_gpu_complete_ctrl_response(&vgpu, virtio_gpu_ctrl_generation(&vgpu),
                                      0, false, 0, 0);

    CHECK(used_idx() == 1);
    CHECK(used_elem_id(0) == 0);
    CHECK(used_elem_len(0) == sizeof(struct virtio_gpu_ctrl_hdr));
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(response_hdr()->flags == 0);
    CHECK(response_hdr()->fence_id == 0);
    CHECK(vgpu.InterruptStatus == VIRTIO_INT__USED_RING);

    return 0;
}

static int test_tokenized_unfenced_completions_complete_only_matching_ctrl(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    g_defer_tokenized = true;
    queue_two_unfenced_submits();

    virtio_gpu_queue_notify_handler(&vgpu, VIRTIO_GPU_CONTROLQ);

    CHECK(vgpu.queues[VIRTIO_GPU_CONTROLQ].last_avail == 2);
    CHECK(used_idx() == 0);
    CHECK(response_hdr()->type == 0);
    CHECK(response2_hdr()->type == 0);

    virtio_gpu_complete_ctrl_response_token(
        &vgpu, virtio_gpu_ctrl_generation(&vgpu), 202,
        VIRTIO_GPU_RESP_ERR_UNSPEC, NULL, 0);

    CHECK(used_idx() == 1);
    CHECK(used_elem_id(0) == 2);
    CHECK(response_hdr()->type == 0);
    CHECK(response2_hdr()->type == VIRTIO_GPU_RESP_ERR_UNSPEC);

    virtio_gpu_complete_ctrl_response_token(
        &vgpu, virtio_gpu_ctrl_generation(&vgpu), 101,
        VIRTIO_GPU_RESP_OK_NODATA, NULL, 0);

    CHECK(used_idx() == 2);
    CHECK(used_elem_id(1) == 0);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    return 0;
}

static int test_tokenized_cancel_drops_only_matching_ctrl(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    g_defer_tokenized = true;
    queue_two_unfenced_submits();

    virtio_gpu_queue_notify_handler(&vgpu, VIRTIO_GPU_CONTROLQ);

    CHECK(vgpu.queues[VIRTIO_GPU_CONTROLQ].last_avail == 2);
    CHECK(used_idx() == 0);

    CHECK(virtio_gpu_cancel_ctrl_response_token(
        &vgpu, virtio_gpu_ctrl_generation(&vgpu), 202));

    virtio_gpu_complete_ctrl_response_token(
        &vgpu, virtio_gpu_ctrl_generation(&vgpu), 101,
        VIRTIO_GPU_RESP_OK_NODATA, NULL, 0);
    virtio_gpu_complete_ctrl_response_token(
        &vgpu, virtio_gpu_ctrl_generation(&vgpu), 202,
        VIRTIO_GPU_RESP_ERR_UNSPEC, NULL, 0);

    CHECK(used_idx() == 1);
    CHECK(used_elem_id(0) == 0);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(response2_hdr()->type == 0);

    return 0;
}

static int test_sync_completion_after_defer_is_not_lost(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    g_complete_during_submit = true;
    queue_one_submit(VIRTIO_GPU_FLAG_FENCE, 12, 0, 0);

    virtio_gpu_queue_notify_handler(&vgpu, VIRTIO_GPU_CONTROLQ);

    CHECK(vgpu.queues[VIRTIO_GPU_CONTROLQ].last_avail == 1);
    CHECK(used_idx() == 1);
    CHECK(used_elem_id(0) == 0);
    CHECK(used_elem_len(0) == sizeof(struct virtio_gpu_ctrl_hdr));
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(response_hdr()->flags == VIRTIO_GPU_FLAG_FENCE);
    CHECK(response_hdr()->fence_id == 12);

    return 0;
}

static int test_deferred_ctrl_can_be_cancelled_before_error_response(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    g_cancel_after_defer = true;
    queue_one_submit(VIRTIO_GPU_FLAG_FENCE, 13, 0, 0);

    virtio_gpu_queue_notify_handler(&vgpu, VIRTIO_GPU_CONTROLQ);

    CHECK(g_cancel_result);
    CHECK(vgpu.queues[VIRTIO_GPU_CONTROLQ].last_avail == 1);
    CHECK(used_idx() == 1);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_ERR_UNSPEC);
    CHECK(response_hdr()->flags == VIRTIO_GPU_FLAG_FENCE);
    CHECK(response_hdr()->fence_id == 13);

    virtio_gpu_complete_ctrl_response(&vgpu, virtio_gpu_ctrl_generation(&vgpu),
                                      13, false, 0, 0);
    CHECK(used_idx() == 1);

    return 0;
}

static int test_renderer_fence_completion_drains_from_gpu_poll(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    uint32_t generation = virtio_gpu_ctrl_generation(&vgpu);
    vgpu_renderer_reset_queues(generation);
    queue_one_submit(VIRTIO_GPU_FLAG_FENCE, 14, 0, 0);

    virtio_gpu_queue_notify_handler(&vgpu, VIRTIO_GPU_CONTROLQ);

    struct vgpu_renderer_completion completion = {
        .type = VGPU_RENDERER_DONE_FENCE,
        .token = {.id = 1, .generation = generation},
        .context_fence = false,
        .fence_id = 14,
    };
    CHECK(vgpu_renderer_complete(&completion));
    CHECK(used_idx() == 0);

    virtio_gpu_poll(&vgpu);

    CHECK(used_idx() == 1);
    CHECK(used_elem_id(0) == 0);
    CHECK(used_elem_len(0) == sizeof(struct virtio_gpu_ctrl_hdr));
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(response_hdr()->flags == VIRTIO_GPU_FLAG_FENCE);
    CHECK(response_hdr()->fence_id == 14);

    return 0;
}

static int test_num_capsets_config_read_uses_cached_device_state(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    uint32_t value = 0;

    virtio_gpu_set_num_capsets(&vgpu, 5);

    CHECK(virtio_gpu_reg_read(
        &vgpu,
        VIRTIO_Config +
            offsetof(struct virtio_gpu_config, num_capsets) / sizeof(uint32_t),
        &value));
    CHECK(value == 5);

    return 0;
}

static int test_pending_ctrl_capacity_covers_full_virtqueue(void)
{
    CHECK(VIRTIO_GPU_PENDING_CTRLS_MAX >= VIRTIO_GPU_QUEUE_NUM_MAX);
    return 0;
}

int main(void)
{
    CHECK(test_unfenced_deferred_submit_completes_generically() == 0);
    CHECK(test_tokenized_unfenced_completions_complete_only_matching_ctrl() ==
          0);
    CHECK(test_tokenized_cancel_drops_only_matching_ctrl() == 0);
    CHECK(test_fenced_submit_completes_used_ring_after_fence() == 0);
    CHECK(test_context_fence_requires_matching_context_and_ring() == 0);
    CHECK(test_reset_drops_pending_fence_without_writing_stale_response() == 0);
    CHECK(test_sync_completion_after_defer_is_not_lost() == 0);
    CHECK(test_deferred_ctrl_can_be_cancelled_before_error_response() == 0);
    CHECK(test_renderer_fence_completion_drains_from_gpu_poll() == 0);
    CHECK(test_num_capsets_config_read_uses_cached_device_state() == 0);
    CHECK(test_pending_ctrl_capacity_covers_full_virtqueue() == 0);
    return 0;
}
