#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tests/fakes/virglrenderer.h"
#include "../virtio-gpu.h"

#define TEST_RAM_SIZE 4096U
#define REQ_ADDR 64U
#define PAYLOAD_ADDR 512U
#define RESP_ADDR 1024U

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            return 1;                                                        \
        }                                                                    \
    } while (0)

struct virgl_test_calls {
    int reset_count;

    int context_create_count;
    uint32_t context_create_handle;
    uint32_t context_create_nlen;
    char context_create_name[64];

    int context_destroy_count;
    uint32_t context_destroy_handle;

    int ctx_attach_count;
    int ctx_attach_ctx_id;
    int ctx_attach_res_handle;

    int ctx_detach_count;
    int ctx_detach_ctx_id;
    int ctx_detach_res_handle;

    int resource_create_count;
    struct virgl_renderer_resource_create_args resource_create_args;

    int resource_unref_count;
    uint32_t resource_unref_handle;

    int resource_attach_iov_count;
    int resource_attach_iov_handle;
    int resource_attach_iov_num_iovs;
    struct iovec *attached_iov;
    int attached_iov_count;
    void *attached_iov_base[4];
    size_t attached_iov_len[4];

    int resource_detach_iov_count;
    int resource_detach_iov_handle;

    int transfer_write_count;
    uint32_t transfer_write_handle;
    uint32_t transfer_write_ctx_id;
    int transfer_write_level;
    uint32_t transfer_write_stride;
    uint32_t transfer_write_layer_stride;
    struct virgl_box transfer_write_box;
    uint64_t transfer_write_offset;
    unsigned int transfer_write_iov_cnt;

    int transfer_read_count;
    uint32_t transfer_read_handle;
    uint32_t transfer_read_ctx_id;
    uint32_t transfer_read_level;
    uint32_t transfer_read_stride;
    uint32_t transfer_read_layer_stride;
    struct virgl_box transfer_read_box;
    uint64_t transfer_read_offset;
    int transfer_read_iov_cnt;

    int submit_count;
    int submit_ctx_id;
    int submit_ndw;
    uint32_t submit_words[16];

    int undefined_count;
};

static uint8_t g_ram[TEST_RAM_SIZE];
static struct virgl_test_calls g_calls;

static void reset_fixture(void)
{
    free(g_calls.attached_iov);
    memset(g_ram, 0, sizeof(g_ram));
    memset(&g_calls, 0, sizeof(g_calls));
}

static virtio_gpu_state_t test_vgpu(void)
{
    virtio_gpu_state_t vgpu = {0};
    vgpu.ram = (uint32_t *) g_ram;
    return vgpu;
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

void virtio_gpu_set_fail(virtio_gpu_state_t *vgpu)
{
    vgpu->Status |= VIRTIO_STATUS__DEVICE_NEEDS_RESET;
}

void *virtio_gpu_get_request(virtio_gpu_state_t *vgpu,
                             struct virtq_desc *vq_desc,
                             size_t request_size)
{
    if ((vq_desc[0].flags & VIRTIO_DESC_F_WRITE) ||
        vq_desc[0].len < request_size || request_size > UINT32_MAX)
        return NULL;

    return virtio_gpu_mem_guest_to_host(vgpu, (uint32_t) vq_desc[0].addr,
                                        (uint32_t) request_size);
}

int virtio_gpu_get_response_desc(struct virtq_desc *vq_desc,
                                 int max_desc,
                                 size_t response_size)
{
    if (response_size > UINT32_MAX)
        return -1;

    for (int i = 1; i < max_desc; i++) {
        if (!(vq_desc[i].flags & VIRTIO_DESC_F_WRITE))
            continue;
        if (vq_desc[i].len < response_size)
            return -1;
        return i;
    }

    return -1;
}

uint32_t virtio_gpu_write_ctrl_response(
    virtio_gpu_state_t *vgpu,
    const struct virtio_gpu_ctrl_hdr *request,
    const struct virtq_desc *response_desc,
    uint32_t type)
{
    struct virtio_gpu_ctrl_hdr *response = virtio_gpu_mem_guest_to_host(
        vgpu, (uint32_t) response_desc->addr, sizeof(*response));
    if (!response)
        return 0;

    memset(response, 0, sizeof(*response));
    response->type = type;
    if (request->flags & VIRTIO_GPU_FLAG_FENCE) {
        response->flags = VIRTIO_GPU_FLAG_FENCE;
        response->fence_id = request->fence_id;
    }
    return sizeof(*response);
}

enum virtio_gpu_desc_copy_result virtio_gpu_desc_copy_from_readable(
    virtio_gpu_state_t *vgpu,
    const struct virtq_desc *vq_desc,
    int max_desc,
    size_t offset,
    void *buf,
    size_t bytes)
{
    size_t done = 0;

    for (int i = 0; i < max_desc; i++) {
        if (vq_desc[i].flags & VIRTIO_DESC_F_WRITE)
            break;

        if (offset >= vq_desc[i].len) {
            offset -= vq_desc[i].len;
            continue;
        }

        size_t available = vq_desc[i].len - offset;
        size_t chunk = bytes - done;
        if (chunk > available)
            chunk = available;

        const void *src = virtio_gpu_mem_guest_to_host(
            vgpu, (uint32_t) (vq_desc[i].addr + offset), (uint32_t) chunk);
        if (!src)
            return VIRTIO_GPU_DESC_COPY_INVALID;

        memcpy((uint8_t *) buf + done, src, chunk);
        done += chunk;
        offset = 0;
        if (done == bytes)
            return VIRTIO_GPU_DESC_COPY_OK;
    }

    return VIRTIO_GPU_DESC_COPY_SHORT;
}

bool virtio_gpu_desc_readable_size(const struct virtq_desc *vq_desc,
                                   int max_desc,
                                   size_t *size)
{
    *size = 0;
    for (int i = 0; i < max_desc; i++) {
        if (vq_desc[i].flags & VIRTIO_DESC_F_WRITE)
            return true;
        *size += vq_desc[i].len;
    }
    return true;
}

void virtio_gpu_cmd_undefined_handler(virtio_gpu_state_t *vgpu,
                                      struct virtq_desc *vq_desc,
                                      uint32_t *plen)
{
    (void) vgpu;
    (void) vq_desc;
    g_calls.undefined_count++;
    *plen = 0;
}

const struct virtio_gpu_cmd_backend g_virtio_gpu_sw_backend = {
    .reset = NULL,
    .get_display_info = virtio_gpu_cmd_undefined_handler,
    .resource_create_2d = virtio_gpu_cmd_undefined_handler,
    .resource_unref = virtio_gpu_cmd_undefined_handler,
    .set_scanout = virtio_gpu_cmd_undefined_handler,
    .resource_flush = virtio_gpu_cmd_undefined_handler,
    .transfer_to_host_2d = virtio_gpu_cmd_undefined_handler,
    .resource_attach_backing = virtio_gpu_cmd_undefined_handler,
    .resource_detach_backing = virtio_gpu_cmd_undefined_handler,
    .get_capset_info = virtio_gpu_cmd_undefined_handler,
    .get_capset = virtio_gpu_cmd_undefined_handler,
    .get_edid = virtio_gpu_cmd_undefined_handler,
    .resource_assign_uuid = virtio_gpu_cmd_undefined_handler,
    .resource_create_blob = virtio_gpu_cmd_undefined_handler,
    .set_scanout_blob = virtio_gpu_cmd_undefined_handler,
    .ctx_create = virtio_gpu_cmd_undefined_handler,
    .ctx_destroy = virtio_gpu_cmd_undefined_handler,
    .ctx_attach_resource = virtio_gpu_cmd_undefined_handler,
    .ctx_detach_resource = virtio_gpu_cmd_undefined_handler,
    .resource_create_3d = virtio_gpu_cmd_undefined_handler,
    .transfer_to_host_3d = virtio_gpu_cmd_undefined_handler,
    .transfer_from_host_3d = virtio_gpu_cmd_undefined_handler,
    .submit_3d = virtio_gpu_cmd_undefined_handler,
    .resource_map_blob = virtio_gpu_cmd_undefined_handler,
    .resource_unmap_blob = virtio_gpu_cmd_undefined_handler,
    .update_cursor = virtio_gpu_cmd_undefined_handler,
    .move_cursor = virtio_gpu_cmd_undefined_handler,
};

void virgl_renderer_get_cap_set(uint32_t set,
                                uint32_t *max_ver,
                                uint32_t *max_size)
{
    *max_ver = set == VIRTIO_GPU_CAPSET_VIRGL2 ? 2 : 1;
    *max_size = 64;
}

void virgl_renderer_fill_caps(uint32_t set, uint32_t version, void *caps)
{
    memset(caps, (int) (0xa0u + set + version), 64);
}

void virgl_renderer_reset(void)
{
    g_calls.reset_count++;
}

int virgl_renderer_context_create(uint32_t handle,
                                  uint32_t nlen,
                                  const char *name)
{
    g_calls.context_create_count++;
    g_calls.context_create_handle = handle;
    g_calls.context_create_nlen = nlen;
    memcpy(g_calls.context_create_name, name, nlen);
    return 0;
}

void virgl_renderer_context_destroy(uint32_t handle)
{
    g_calls.context_destroy_count++;
    g_calls.context_destroy_handle = handle;
}

int virgl_renderer_context_create_with_flags(uint32_t ctx_id,
                                             uint32_t ctx_flags,
                                             uint32_t nlen,
                                             const char *name)
{
    (void) ctx_id;
    (void) ctx_flags;
    (void) nlen;
    (void) name;
    return 0;
}

void virgl_renderer_ctx_attach_resource(int ctx_id, int res_handle)
{
    g_calls.ctx_attach_count++;
    g_calls.ctx_attach_ctx_id = ctx_id;
    g_calls.ctx_attach_res_handle = res_handle;
}

void virgl_renderer_ctx_detach_resource(int ctx_id, int res_handle)
{
    g_calls.ctx_detach_count++;
    g_calls.ctx_detach_ctx_id = ctx_id;
    g_calls.ctx_detach_res_handle = res_handle;
}

int virgl_renderer_resource_create(
    struct virgl_renderer_resource_create_args *args,
    struct iovec *iov,
    uint32_t num_iovs)
{
    g_calls.resource_create_count++;
    g_calls.resource_create_args = *args;
    (void) iov;
    (void) num_iovs;
    return 0;
}

void virgl_renderer_resource_unref(uint32_t res_handle)
{
    g_calls.resource_unref_count++;
    g_calls.resource_unref_handle = res_handle;
}

int virgl_renderer_resource_attach_iov(int res_handle,
                                       struct iovec *iov,
                                       int num_iovs)
{
    g_calls.resource_attach_iov_count++;
    g_calls.resource_attach_iov_handle = res_handle;
    g_calls.resource_attach_iov_num_iovs = num_iovs;
    g_calls.attached_iov = iov;
    g_calls.attached_iov_count = num_iovs;
    for (int i = 0; i < num_iovs && i < 4; i++) {
        g_calls.attached_iov_base[i] = iov[i].iov_base;
        g_calls.attached_iov_len[i] = iov[i].iov_len;
    }
    return num_iovs >= 0 ? 0 : -1;
}

void virgl_renderer_resource_detach_iov(int res_handle,
                                        struct iovec **iov,
                                        int *num_iovs)
{
    g_calls.resource_detach_iov_count++;
    g_calls.resource_detach_iov_handle = res_handle;
    *iov = g_calls.attached_iov;
    *num_iovs = g_calls.attached_iov_count;
    g_calls.attached_iov = NULL;
    g_calls.attached_iov_count = 0;
}

int virgl_renderer_transfer_write_iov(uint32_t handle,
                                      uint32_t ctx_id,
                                      int level,
                                      uint32_t stride,
                                      uint32_t layer_stride,
                                      struct virgl_box *box,
                                      uint64_t offset,
                                      struct iovec *iovec,
                                      unsigned int iovec_cnt)
{
    g_calls.transfer_write_count++;
    g_calls.transfer_write_handle = handle;
    g_calls.transfer_write_ctx_id = ctx_id;
    g_calls.transfer_write_level = level;
    g_calls.transfer_write_stride = stride;
    g_calls.transfer_write_layer_stride = layer_stride;
    g_calls.transfer_write_box = *box;
    g_calls.transfer_write_offset = offset;
    (void) iovec;
    g_calls.transfer_write_iov_cnt = iovec_cnt;
    return 0;
}

int virgl_renderer_transfer_read_iov(uint32_t handle,
                                     uint32_t ctx_id,
                                     uint32_t level,
                                     uint32_t stride,
                                     uint32_t layer_stride,
                                     struct virgl_box *box,
                                     uint64_t offset,
                                     struct iovec *iov,
                                     int iovec_cnt)
{
    g_calls.transfer_read_count++;
    g_calls.transfer_read_handle = handle;
    g_calls.transfer_read_ctx_id = ctx_id;
    g_calls.transfer_read_level = level;
    g_calls.transfer_read_stride = stride;
    g_calls.transfer_read_layer_stride = layer_stride;
    g_calls.transfer_read_box = *box;
    g_calls.transfer_read_offset = offset;
    (void) iov;
    g_calls.transfer_read_iov_cnt = iovec_cnt;
    return 0;
}

int virgl_renderer_submit_cmd(void *buffer, int ctx_id, int ndw)
{
    g_calls.submit_count++;
    g_calls.submit_ctx_id = ctx_id;
    g_calls.submit_ndw = ndw;
    memcpy(g_calls.submit_words, buffer, (size_t) ndw * sizeof(uint32_t));
    return 0;
}

#include "../virtio-gpu-virgl.c"

static virtio_gpu_state_t fresh_vgpu(void)
{
    reset_fixture();

    virtio_gpu_state_t vgpu = test_vgpu();
    if (g_virtio_gpu_backend.reset)
        g_virtio_gpu_backend.reset(&vgpu);
    memset(&g_calls, 0, sizeof(g_calls));
    return vgpu;
}

static struct virtio_gpu_ctrl_hdr *response_hdr(void)
{
    return (struct virtio_gpu_ctrl_hdr *) &g_ram[RESP_ADDR];
}

static void init_desc_no_payload(struct virtq_desc desc[3], size_t request_size)
{
    memset(desc, 0, sizeof(struct virtq_desc) * 3);
    desc[0] = (struct virtq_desc) {
        .addr = REQ_ADDR,
        .len = (uint32_t) request_size,
        .flags = VIRTIO_DESC_F_NEXT,
    };
    desc[1] = (struct virtq_desc) {
        .addr = RESP_ADDR,
        .len = sizeof(struct virtio_gpu_ctrl_hdr),
        .flags = VIRTIO_DESC_F_WRITE,
    };
}

static void init_desc_with_payload(struct virtq_desc desc[3],
                                   size_t request_size,
                                   size_t payload_size)
{
    memset(desc, 0, sizeof(struct virtq_desc) * 3);
    desc[0] = (struct virtq_desc) {
        .addr = REQ_ADDR,
        .len = (uint32_t) request_size,
        .flags = VIRTIO_DESC_F_NEXT,
    };
    desc[1] = (struct virtq_desc) {
        .addr = PAYLOAD_ADDR,
        .len = (uint32_t) payload_size,
        .flags = VIRTIO_DESC_F_NEXT,
    };
    desc[2] = (struct virtq_desc) {
        .addr = RESP_ADDR,
        .len = sizeof(struct virtio_gpu_ctrl_hdr),
        .flags = VIRTIO_DESC_F_WRITE,
    };
}

static int create_virgl_resource(virtio_gpu_state_t *vgpu, uint32_t resource_id)
{
    struct virtio_gpu_resource_create_3d request = {
        .resource_id = resource_id,
        .target = 2,
        .format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
        .bind = 0x40,
        .width = 64,
        .height = 32,
        .depth = 1,
        .array_size = 1,
        .last_level = 0,
        .nr_samples = 0,
        .flags = 0x10,
    };
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));

    struct virtq_desc desc[3];
    init_desc_no_payload(desc, sizeof(request));
    uint32_t plen = 0;

    g_virtio_gpu_backend.resource_create_3d(vgpu, desc, &plen);

    CHECK(plen == sizeof(struct virtio_gpu_ctrl_hdr));
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);
    return 0;
}

static int test_ctx_create_calls_renderer(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    struct virtio_gpu_ctx_create request = {
        .hdr = {.ctx_id = 42},
        .nlen = 4,
        .debug_name = "mesa",
    };
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));

    struct virtq_desc desc[3];
    init_desc_no_payload(desc, sizeof(request));
    uint32_t plen = 0;

    g_virtio_gpu_backend.ctx_create(&vgpu, desc, &plen);

    CHECK(g_calls.context_create_count == 1);
    CHECK(g_calls.context_create_handle == 42);
    CHECK(g_calls.context_create_nlen == 4);
    CHECK(memcmp(g_calls.context_create_name, "mesa", 4) == 0);
    CHECK(g_calls.undefined_count == 0);
    CHECK(plen == sizeof(struct virtio_gpu_ctrl_hdr));
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    return 0;
}

static int test_submit_3d_copies_payload_to_renderer(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    struct virtio_gpu_cmd_submit request = {
        .hdr = {.ctx_id = 7},
        .size = 16,
    };
    uint32_t commands[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    memcpy(&g_ram[PAYLOAD_ADDR], commands, sizeof(commands));

    struct virtq_desc desc[3];
    init_desc_with_payload(desc, sizeof(request), sizeof(commands));
    uint32_t plen = 0;

    g_virtio_gpu_backend.submit_3d(&vgpu, desc, &plen);

    CHECK(g_calls.submit_count == 1);
    CHECK(g_calls.submit_ctx_id == 7);
    CHECK(g_calls.submit_ndw == 4);
    CHECK(memcmp(g_calls.submit_words, commands, sizeof(commands)) == 0);
    CHECK(g_calls.undefined_count == 0);
    CHECK(plen == sizeof(struct virtio_gpu_ctrl_hdr));
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    return 0;
}

static int test_context_lifecycle_handlers_call_renderer(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();

    struct virtio_gpu_ctx_destroy destroy = {
        .hdr = {.ctx_id = 11},
    };
    memcpy(&g_ram[REQ_ADDR], &destroy, sizeof(destroy));
    struct virtq_desc desc[3];
    init_desc_no_payload(desc, sizeof(destroy));
    uint32_t plen = 0;

    g_virtio_gpu_backend.ctx_destroy(&vgpu, desc, &plen);
    CHECK(g_calls.context_destroy_count == 1);
    CHECK(g_calls.context_destroy_handle == 11);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    struct virtio_gpu_ctx_resource attach = {
        .hdr = {.ctx_id = 12},
        .resource_id = 88,
    };
    memcpy(&g_ram[REQ_ADDR], &attach, sizeof(attach));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(attach));
    plen = 0;

    g_virtio_gpu_backend.ctx_attach_resource(&vgpu, desc, &plen);
    CHECK(g_calls.ctx_attach_count == 1);
    CHECK(g_calls.ctx_attach_ctx_id == 12);
    CHECK(g_calls.ctx_attach_res_handle == 88);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    struct virtio_gpu_ctx_resource detach = {
        .hdr = {.ctx_id = 13},
        .resource_id = 89,
    };
    memcpy(&g_ram[REQ_ADDR], &detach, sizeof(detach));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(detach));
    plen = 0;

    g_virtio_gpu_backend.ctx_detach_resource(&vgpu, desc, &plen);
    CHECK(g_calls.ctx_detach_count == 1);
    CHECK(g_calls.ctx_detach_ctx_id == 13);
    CHECK(g_calls.ctx_detach_res_handle == 89);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    return 0;
}

static int test_ctx_create_rejects_context_init_until_feature_enabled(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    struct virtio_gpu_ctx_create request = {
        .hdr = {.ctx_id = 43},
        .nlen = 4,
        .context_init = VIRTIO_GPU_CAPSET_VIRGL,
        .debug_name = "mesa",
    };
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));

    struct virtq_desc desc[3];
    init_desc_no_payload(desc, sizeof(request));
    uint32_t plen = 0;

    g_virtio_gpu_backend.ctx_create(&vgpu, desc, &plen);

    CHECK(g_calls.context_create_count == 0);
    CHECK(plen == sizeof(struct virtio_gpu_ctrl_hdr));
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_ERR_UNSPEC);

    return 0;
}

static int test_resource_create_3d_calls_renderer(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    struct virtio_gpu_resource_create_3d request = {
        .resource_id = 55,
        .target = 2,
        .format = VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM,
        .bind = 0x99,
        .width = 320,
        .height = 240,
        .depth = 1,
        .array_size = 2,
        .last_level = 3,
        .nr_samples = 4,
        .flags = 0x10,
    };
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));

    struct virtq_desc desc[3];
    init_desc_no_payload(desc, sizeof(request));
    uint32_t plen = 0;

    g_virtio_gpu_backend.resource_create_3d(&vgpu, desc, &plen);

    CHECK(g_calls.resource_create_count == 1);
    CHECK(g_calls.resource_create_args.handle == 55);
    CHECK(g_calls.resource_create_args.target == 2);
    CHECK(g_calls.resource_create_args.format ==
          VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM);
    CHECK(g_calls.resource_create_args.bind == 0x99);
    CHECK(g_calls.resource_create_args.width == 320);
    CHECK(g_calls.resource_create_args.height == 240);
    CHECK(g_calls.resource_create_args.depth == 1);
    CHECK(g_calls.resource_create_args.array_size == 2);
    CHECK(g_calls.resource_create_args.last_level == 3);
    CHECK(g_calls.resource_create_args.nr_samples == 4);
    CHECK(g_calls.resource_create_args.flags == 0x10);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    return 0;
}

static int test_resource_backing_attach_detach_and_unref(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 61) == 0);
    memset(&g_calls, 0, sizeof(g_calls));

    struct virtio_gpu_res_attach_backing request = {
        .resource_id = 61,
        .nr_entries = 2,
    };
    struct virtio_gpu_mem_entry entries[2] = {
        {.addr = 1536, .length = 64},
        {.addr = 2048, .length = 128},
    };
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    memcpy(&g_ram[PAYLOAD_ADDR], entries, sizeof(entries));

    struct virtq_desc desc[3];
    init_desc_with_payload(desc, sizeof(request), sizeof(entries));
    uint32_t plen = 0;

    g_virtio_gpu_backend.resource_attach_backing(&vgpu, desc, &plen);

    CHECK(g_calls.resource_attach_iov_count == 1);
    CHECK(g_calls.resource_attach_iov_handle == 61);
    CHECK(g_calls.resource_attach_iov_num_iovs == 2);
    CHECK(g_calls.attached_iov_base[0] == &g_ram[1536]);
    CHECK(g_calls.attached_iov_len[0] == 64);
    CHECK(g_calls.attached_iov_base[1] == &g_ram[2048]);
    CHECK(g_calls.attached_iov_len[1] == 128);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    struct virtio_gpu_res_detach_backing detach = {
        .resource_id = 61,
    };
    memcpy(&g_ram[REQ_ADDR], &detach, sizeof(detach));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(detach));
    plen = 0;

    g_virtio_gpu_backend.resource_detach_backing(&vgpu, desc, &plen);

    CHECK(g_calls.resource_detach_iov_count == 1);
    CHECK(g_calls.resource_detach_iov_handle == 61);
    CHECK(g_calls.attached_iov == NULL);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    struct virtio_gpu_res_unref unref = {
        .resource_id = 61,
    };
    memcpy(&g_ram[REQ_ADDR], &unref, sizeof(unref));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(unref));
    plen = 0;

    g_virtio_gpu_backend.resource_unref(&vgpu, desc, &plen);

    CHECK(g_calls.resource_unref_count == 1);
    CHECK(g_calls.resource_unref_handle == 61);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    return 0;
}

static int test_3d_transfers_call_renderer(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 71) == 0);
    memset(&g_calls, 0, sizeof(g_calls));

    struct virtio_gpu_transfer_host_3d to_host = {
        .hdr = {.ctx_id = 5},
        .box = {.x = 1, .y = 2, .z = 3, .w = 4, .h = 5, .d = 6},
        .offset = 0x123456789ULL,
        .resource_id = 71,
        .level = 7,
        .stride = 256,
        .layer_stride = 4096,
    };
    memcpy(&g_ram[REQ_ADDR], &to_host, sizeof(to_host));
    struct virtq_desc desc[3];
    init_desc_no_payload(desc, sizeof(to_host));
    uint32_t plen = 0;

    g_virtio_gpu_backend.transfer_to_host_3d(&vgpu, desc, &plen);

    CHECK(g_calls.transfer_write_count == 1);
    CHECK(g_calls.transfer_write_handle == 71);
    CHECK(g_calls.transfer_write_ctx_id == 5);
    CHECK(g_calls.transfer_write_level == 7);
    CHECK(g_calls.transfer_write_stride == 256);
    CHECK(g_calls.transfer_write_layer_stride == 4096);
    CHECK(g_calls.transfer_write_box.x == 1);
    CHECK(g_calls.transfer_write_box.y == 2);
    CHECK(g_calls.transfer_write_box.z == 3);
    CHECK(g_calls.transfer_write_box.w == 4);
    CHECK(g_calls.transfer_write_box.h == 5);
    CHECK(g_calls.transfer_write_box.d == 6);
    CHECK(g_calls.transfer_write_offset == 0x123456789ULL);
    CHECK(g_calls.transfer_write_iov_cnt == 0);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    struct virtio_gpu_transfer_host_3d from_host = {
        .hdr = {.ctx_id = 6},
        .box = {.x = 9, .y = 8, .z = 7, .w = 6, .h = 5, .d = 4},
        .offset = 0xfeed,
        .resource_id = 71,
        .level = 3,
        .stride = 512,
        .layer_stride = 8192,
    };
    memcpy(&g_ram[REQ_ADDR], &from_host, sizeof(from_host));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(from_host));
    plen = 0;

    g_virtio_gpu_backend.transfer_from_host_3d(&vgpu, desc, &plen);

    CHECK(g_calls.transfer_read_count == 1);
    CHECK(g_calls.transfer_read_handle == 71);
    CHECK(g_calls.transfer_read_ctx_id == 6);
    CHECK(g_calls.transfer_read_level == 3);
    CHECK(g_calls.transfer_read_stride == 512);
    CHECK(g_calls.transfer_read_layer_stride == 8192);
    CHECK(g_calls.transfer_read_box.x == 9);
    CHECK(g_calls.transfer_read_box.y == 8);
    CHECK(g_calls.transfer_read_box.z == 7);
    CHECK(g_calls.transfer_read_box.w == 6);
    CHECK(g_calls.transfer_read_box.h == 5);
    CHECK(g_calls.transfer_read_box.d == 4);
    CHECK(g_calls.transfer_read_offset == 0xfeed);
    CHECK(g_calls.transfer_read_iov_cnt == 0);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    return 0;
}

static int test_transfer_to_host_2d_routes_by_resource_owner(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    CHECK(create_virgl_resource(&vgpu, 81) == 0);
    memset(&g_calls, 0, sizeof(g_calls));

    struct virtio_gpu_trans_to_host_2d request = {
        .r = {.x = 10, .y = 11, .width = 12, .height = 13},
        .offset = 0x44,
        .resource_id = 81,
    };
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    struct virtq_desc desc[3];
    init_desc_no_payload(desc, sizeof(request));
    uint32_t plen = 0;

    g_virtio_gpu_backend.transfer_to_host_2d(&vgpu, desc, &plen);

    CHECK(g_calls.transfer_write_count == 1);
    CHECK(g_calls.transfer_write_handle == 81);
    CHECK(g_calls.transfer_write_ctx_id == 0);
    CHECK(g_calls.transfer_write_level == 0);
    CHECK(g_calls.transfer_write_stride == 0);
    CHECK(g_calls.transfer_write_layer_stride == 0);
    CHECK(g_calls.transfer_write_box.x == 10);
    CHECK(g_calls.transfer_write_box.y == 11);
    CHECK(g_calls.transfer_write_box.z == 0);
    CHECK(g_calls.transfer_write_box.w == 12);
    CHECK(g_calls.transfer_write_box.h == 13);
    CHECK(g_calls.transfer_write_box.d == 1);
    CHECK(g_calls.transfer_write_offset == 0x44);
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_OK_NODATA);

    request.resource_id = 999;
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    memset(response_hdr(), 0, sizeof(*response_hdr()));
    init_desc_no_payload(desc, sizeof(request));
    plen = 0;

    g_virtio_gpu_backend.transfer_to_host_2d(&vgpu, desc, &plen);

    CHECK(g_calls.undefined_count == 1);
    CHECK(plen == 0);

    return 0;
}

static int test_submit_3d_rejects_unaligned_size(void)
{
    virtio_gpu_state_t vgpu = fresh_vgpu();
    struct virtio_gpu_cmd_submit request = {
        .hdr = {.ctx_id = 7},
        .size = 15,
    };
    uint32_t commands[4] = {0};
    memcpy(&g_ram[REQ_ADDR], &request, sizeof(request));
    memcpy(&g_ram[PAYLOAD_ADDR], commands, sizeof(commands));

    struct virtq_desc desc[3];
    init_desc_with_payload(desc, sizeof(request), sizeof(commands));
    uint32_t plen = 0;

    g_virtio_gpu_backend.submit_3d(&vgpu, desc, &plen);

    CHECK(g_calls.submit_count == 0);
    CHECK(plen == sizeof(struct virtio_gpu_ctrl_hdr));
    CHECK(response_hdr()->type == VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);

    return 0;
}

int main(void)
{
    CHECK(test_ctx_create_calls_renderer() == 0);
    CHECK(test_context_lifecycle_handlers_call_renderer() == 0);
    CHECK(test_ctx_create_rejects_context_init_until_feature_enabled() == 0);
    CHECK(test_resource_create_3d_calls_renderer() == 0);
    CHECK(test_resource_backing_attach_detach_and_unref() == 0);
    CHECK(test_3d_transfers_call_renderer() == 0);
    CHECK(test_transfer_to_host_2d_routes_by_resource_owner() == 0);
    CHECK(test_submit_3d_copies_payload_to_renderer() == 0);
    CHECK(test_submit_3d_rejects_unaligned_size() == 0);
    return 0;
}
