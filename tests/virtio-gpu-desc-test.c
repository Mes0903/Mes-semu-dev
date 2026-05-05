#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../virtio-gpu.h"

#define TEST_RAM_SIZE 512U
#define REQ_ADDR 16U
#define DATA_ADDR 128U
#define RESP_ADDR 240U

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static uint8_t g_ram[TEST_RAM_SIZE];

static virtio_gpu_state_t test_vgpu(void)
{
    virtio_gpu_state_t vgpu = {0};
    vgpu.ram = (uint32_t *) g_ram;
    return vgpu;
}

static void clear_ram(void)
{
    memset(g_ram, 0, sizeof(g_ram));
}

static int test_split_request_and_payload_descs(void)
{
    clear_ram();

    virtio_gpu_state_t vgpu = test_vgpu();
    struct virtio_gpu_res_attach_backing request = {
        .resource_id = 7,
        .nr_entries = 2,
    };
    struct virtio_gpu_mem_entry entries[2] = {
        {.addr = 0x40, .length = 16},
        {.addr = 0x80, .length = 32},
    };
    struct virtio_gpu_mem_entry copied[2] = {0};
    size_t readable_size = 0;

    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    memcpy(&g_ram[DATA_ADDR], entries, sizeof(entries));

    struct virtq_desc desc[3] = {
        {.addr = REQ_ADDR, .len = sizeof(request), .flags = VIRTIO_DESC_F_NEXT},
        {.addr = DATA_ADDR,
         .len = sizeof(entries),
         .flags = VIRTIO_DESC_F_NEXT},
        {.addr = RESP_ADDR, .len = 64, .flags = VIRTIO_DESC_F_WRITE},
    };

    CHECK(virtio_gpu_desc_readable_size(desc, 3, &readable_size));
    CHECK(readable_size == sizeof(request) + sizeof(entries));
    CHECK(virtio_gpu_desc_copy_from_readable(&vgpu, desc, 3, sizeof(request),
                                             copied, sizeof(copied)) ==
          VIRTIO_GPU_DESC_COPY_OK);
    CHECK(memcmp(copied, entries, sizeof(entries)) == 0);

    return 0;
}

static int test_payload_packed_after_request(void)
{
    clear_ram();

    virtio_gpu_state_t vgpu = test_vgpu();
    struct virtio_gpu_res_attach_backing request = {
        .resource_id = 9,
        .nr_entries = 1,
    };
    struct virtio_gpu_mem_entry entry = {.addr = 0x120, .length = 64};
    struct virtio_gpu_mem_entry copied = {0};

    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    memcpy(&g_ram[REQ_ADDR + sizeof(request)], &entry, sizeof(entry));

    struct virtq_desc desc[3] = {
        {.addr = REQ_ADDR,
         .len = sizeof(request) + sizeof(entry),
         .flags = VIRTIO_DESC_F_NEXT},
        {.addr = RESP_ADDR, .len = 64, .flags = VIRTIO_DESC_F_WRITE},
    };

    CHECK(virtio_gpu_desc_copy_from_readable(&vgpu, desc, 3, sizeof(request),
                                             &copied, sizeof(copied)) ==
          VIRTIO_GPU_DESC_COPY_OK);
    CHECK(memcmp(&copied, &entry, sizeof(entry)) == 0);

    return 0;
}

static int test_short_payload_reports_short_copy(void)
{
    clear_ram();

    virtio_gpu_state_t vgpu = test_vgpu();
    struct virtio_gpu_res_attach_backing request = {
        .resource_id = 11,
        .nr_entries = 1,
    };
    struct virtio_gpu_mem_entry copied = {0};

    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));

    struct virtq_desc desc[3] = {
        {.addr = REQ_ADDR, .len = sizeof(request), .flags = VIRTIO_DESC_F_NEXT},
        {.addr = RESP_ADDR, .len = 64, .flags = VIRTIO_DESC_F_WRITE},
    };

    CHECK(virtio_gpu_desc_copy_from_readable(&vgpu, desc, 3, sizeof(request),
                                             &copied, sizeof(copied)) ==
          VIRTIO_GPU_DESC_COPY_SHORT);

    return 0;
}

static int test_writable_desc_ends_readable_stream(void)
{
    clear_ram();

    virtio_gpu_state_t vgpu = test_vgpu();
    struct virtio_gpu_res_attach_backing request = {
        .resource_id = 13,
        .nr_entries = 1,
    };
    struct virtio_gpu_mem_entry entry = {.addr = 0x140, .length = 64};
    struct virtio_gpu_mem_entry copied = {0};

    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    memcpy(&g_ram[DATA_ADDR], &entry, sizeof(entry));

    struct virtq_desc desc[3] = {
        {.addr = REQ_ADDR, .len = sizeof(request), .flags = VIRTIO_DESC_F_NEXT},
        {.addr = RESP_ADDR, .len = 64, .flags = VIRTIO_DESC_F_WRITE},
        {.addr = DATA_ADDR, .len = sizeof(entry), .flags = 0},
    };

    CHECK(virtio_gpu_desc_copy_from_readable(&vgpu, desc, 3, sizeof(request),
                                             &copied, sizeof(copied)) ==
          VIRTIO_GPU_DESC_COPY_SHORT);

    return 0;
}

static int test_high_addr_reports_invalid_copy(void)
{
    clear_ram();

    virtio_gpu_state_t vgpu = test_vgpu();
    struct virtio_gpu_ctrl_hdr copied = {0};
    struct virtq_desc desc[1] = {
        {.addr = (uint64_t) UINT32_MAX + 1U, .len = sizeof(copied), .flags = 0},
    };

    CHECK(virtio_gpu_desc_copy_from_readable(&vgpu, desc, 1, 0, &copied,
                                             sizeof(copied)) ==
          VIRTIO_GPU_DESC_COPY_INVALID);

    return 0;
}

int main(void)
{
    CHECK(test_split_request_and_payload_descs() == 0);
    CHECK(test_payload_packed_after_request() == 0);
    CHECK(test_short_payload_reports_short_copy() == 0);
    CHECK(test_writable_desc_ends_readable_stream() == 0);
    CHECK(test_high_addr_reports_invalid_copy() == 0);
    return 0;
}
