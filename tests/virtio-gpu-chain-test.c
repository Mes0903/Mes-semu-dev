#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../virtio-gpu.h"

#define TEST_RAM_SIZE 8192U
#define DESC_WORD 64U
#define AVAIL_WORD 128U
#define USED_WORD 160U
#define REQ_ADDR 1024U
#define PAYLOAD0_ADDR 2048U
#define PAYLOAD1_ADDR 3072U
#define RESP_ADDR 4096U

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static uint32_t g_ram_words[TEST_RAM_SIZE / sizeof(uint32_t)];
static int g_submit_count;
static uint32_t g_submit_ctx_id;
static uint32_t g_submit_words[4];

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

#include "../virtio-gpu-desc.c"
#include "../virtio-gpu.c"

static uint8_t *ram_bytes(void)
{
    return (uint8_t *) g_ram_words;
}

static struct virtio_gpu_ctrl_hdr *response_hdr(void)
{
    return (struct virtio_gpu_ctrl_hdr *) &ram_bytes()[RESP_ADDR];
}

static uint16_t used_idx(void)
{
    return (uint16_t) (g_ram_words[USED_WORD] >> 16);
}

static uint32_t used_elem_id(uint16_t slot)
{
    return g_ram_words[USED_WORD + 1U + slot * 2U];
}

static uint32_t used_elem_len(uint16_t slot)
{
    return g_ram_words[USED_WORD + 2U + slot * 2U];
}

static void write_desc(uint16_t index,
                       uint64_t addr,
                       uint32_t len,
                       uint16_t flags,
                       uint16_t next)
{
    uint32_t base = DESC_WORD + index * 4U;
    g_ram_words[base] = (uint32_t) addr;
    g_ram_words[base + 1U] = (uint32_t) (addr >> 32);
    g_ram_words[base + 2U] = len;
    g_ram_words[base + 3U] = (uint32_t) flags | ((uint32_t) next << 16);
}

static virtio_gpu_state_t fresh_vgpu(void)
{
    memset(g_ram_words, 0, sizeof(g_ram_words));
    memset(&virtio_gpu_data, 0, sizeof(virtio_gpu_data));
    g_submit_count = 0;
    g_submit_ctx_id = 0;
    memset(g_submit_words, 0, sizeof(g_submit_words));

    virtio_gpu_state_t vgpu = {0};
    vgpu.ram = g_ram_words;
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

static void queue_split_submit(void)
{
    struct virtio_gpu_cmd_submit request = {
        .hdr = {.type = VIRTIO_GPU_CMD_SUBMIT_3D, .ctx_id = 7},
        .size = sizeof(g_submit_words),
    };
    const uint32_t commands[4] = {
        0x11111111,
        0x22222222,
        0x33333333,
        0x44444444,
    };

    memcpy(&ram_bytes()[REQ_ADDR], &request, sizeof(request));
    memcpy(&ram_bytes()[PAYLOAD0_ADDR], commands, sizeof(commands) / 2U);
    memcpy(&ram_bytes()[PAYLOAD1_ADDR],
           &((const uint8_t *) commands)[sizeof(commands) / 2U],
           sizeof(commands) / 2U);

    write_desc(0, REQ_ADDR, sizeof(request), VIRTIO_DESC_F_NEXT, 1);
    write_desc(1, PAYLOAD0_ADDR, sizeof(commands) / 2U, VIRTIO_DESC_F_NEXT, 2);
    write_desc(2, PAYLOAD1_ADDR, sizeof(commands) / 2U, VIRTIO_DESC_F_NEXT, 3);
    write_desc(3, RESP_ADDR, sizeof(struct virtio_gpu_ctrl_hdr),
               VIRTIO_DESC_F_WRITE, 0);

    g_ram_words[AVAIL_WORD] = 1U << 16;
    g_ram_words[AVAIL_WORD + 1U] = 0;
}

static void fake_submit_3d(virtio_gpu_state_t *vgpu,
                           struct virtq_desc *vq_desc,
                           uint32_t *plen)
{
    struct virtio_gpu_cmd_submit *request =
        virtio_gpu_get_request(vgpu, vq_desc, sizeof(*request));
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

    enum virtio_gpu_desc_copy_result copy_result =
        virtio_gpu_desc_copy_from_readable(vgpu, vq_desc, VIRTIO_GPU_MAX_DESC,
                                           sizeof(*request), g_submit_words,
                                           request->size);
    if (copy_result != VIRTIO_GPU_DESC_COPY_OK) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, &vq_desc[resp_idx],
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        return;
    }

    g_submit_count++;
    g_submit_ctx_id = request->hdr.ctx_id;
    *plen = virtio_gpu_write_ctrl_response(
        vgpu, &request->hdr, &vq_desc[resp_idx], VIRTIO_GPU_RESP_OK_NODATA);
}

static int test_submit_3d_accepts_split_payload_descriptor_chain(void)
{
    const uint32_t expected_commands[4] = {
        0x11111111,
        0x22222222,
        0x33333333,
        0x44444444,
    };
    virtio_gpu_state_t vgpu = fresh_vgpu();
    queue_split_submit();

    virtio_gpu_queue_notify_handler(&vgpu, VIRTIO_GPU_CONTROLQ);

    CHECK((vgpu.Status & VIRTIO_STATUS__DEVICE_NEEDS_RESET) == 0);
    CHECK(g_submit_count == 1);
    CHECK(g_submit_ctx_id == 7);
    CHECK(memcmp(g_submit_words, expected_commands, sizeof(g_submit_words)) ==
          0);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(used_idx() == 1);
    CHECK(used_elem_id(0) == 0);
    CHECK(used_elem_len(0) == sizeof(struct virtio_gpu_ctrl_hdr));
    CHECK(vgpu.InterruptStatus == VIRTIO_INT__USED_RING);

    return 0;
}

int main(void)
{
    CHECK(test_submit_3d_accepts_split_payload_descriptor_chain() == 0);
    return 0;
}
