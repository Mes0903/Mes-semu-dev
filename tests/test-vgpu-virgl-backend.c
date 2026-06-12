#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <virglrenderer.h>

#include "../platform.h"
#include "../riscv_private.h"
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
static int fake_create_fence_result;
static uint32_t fake_last_create_fence_ctx_id;
static int fake_context_create_fence_count;
static uint32_t fake_last_context_fence_ctx_id;
static uint32_t fake_last_context_fence_flags;
static uint32_t fake_last_context_fence_ring_idx;
static uint64_t fake_last_context_fence_id;
static int fake_context_create_fence_result;
static int fake_poll_count;
static int fake_force_ctx0_count;
static int fake_reset_count;
static int fake_get_cap_set_count;
static bool fake_virgl2_supported;
static int fake_set_num_capsets_count;
static uint32_t fake_last_num_capsets;
static int fake_fill_caps_count;
static uint32_t fake_last_fill_caps_set;
static uint32_t fake_last_fill_caps_version;
static int fake_context_create_count;
static uint32_t fake_last_context_create_handle;
static uint32_t fake_last_context_create_nlen;
static char fake_last_context_create_name[64];
static int fake_context_create_with_flags_count;
static uint32_t fake_last_context_create_with_flags_ctx_id;
static uint32_t fake_last_context_create_with_flags_flags;
static uint32_t fake_last_context_create_with_flags_nlen;
static char fake_last_context_create_with_flags_name[64];
static int fake_context_destroy_count;
static uint32_t fake_last_context_destroy_handle;
static int fake_context_attach_count;
static int fake_last_context_attach_ctx_id;
static int fake_last_context_attach_resource_id;
static int fake_context_detach_count;
static int fake_last_context_detach_ctx_id;
static int fake_last_context_detach_resource_id;
static int fake_resource_create_count;
static struct virgl_renderer_resource_create_args
    fake_last_resource_create_args;
static struct iovec *fake_last_resource_create_iov;
static uint32_t fake_last_resource_create_num_iovs;
static int fake_resource_create_result;
static int fake_resource_create_blob_count;
static struct virgl_renderer_resource_create_blob_args
    fake_last_resource_create_blob_args;
static int fake_resource_create_blob_result;
static int fake_resource_get_info_count;
static int fake_last_resource_get_info_handle;
static struct virgl_renderer_resource_info fake_resource_get_info;
static int fake_resource_get_info_result;
static int fake_resource_attach_iov_count;
static int fake_last_resource_attach_iov_handle;
static struct iovec *fake_last_resource_attach_iov;
static int fake_last_resource_attach_iov_count;
static int fake_resource_attach_iov_result;
static int fake_resource_detach_iov_count;
static int fake_last_resource_detach_iov_handle;
static struct iovec *fake_attached_iov;
static int fake_attached_iov_count;
static struct iovec *fake_last_detached_iov;
static int fake_last_resource_detach_iov_count;
static int fake_resource_map_count;
static uint32_t fake_last_resource_map_handle;
static void *fake_resource_map_ptr = (void *) (uintptr_t) 0x12340000;
static uint64_t fake_resource_map_size = 0x2000;
static int fake_resource_map_result;
static int fake_resource_unmap_count;
static uint32_t fake_last_resource_unmap_handle;
static int fake_resource_unmap_result;
static int fake_resource_get_map_info_count;
static uint32_t fake_last_resource_get_map_info_handle;
static uint32_t fake_resource_get_map_info = VIRTIO_GPU_MAP_CACHE_WC;
static int fake_resource_get_map_info_result;
static int fake_resource_unref_count;
static uint32_t fake_last_resource_unref_handle;
static int fake_transfer_write_iov_count;
static uint32_t fake_last_transfer_write_handle;
static uint32_t fake_last_transfer_write_ctx_id;
static int fake_last_transfer_write_level;
static uint32_t fake_last_transfer_write_stride;
static uint32_t fake_last_transfer_write_layer_stride;
static struct virgl_box fake_last_transfer_write_box;
static uint64_t fake_last_transfer_write_offset;
static struct iovec *fake_last_transfer_write_iov;
static unsigned int fake_last_transfer_write_iov_count;
static int fake_transfer_write_iov_result;
static int fake_transfer_read_iov_count;
static uint32_t fake_last_transfer_read_handle;
static uint32_t fake_last_transfer_read_ctx_id;
static uint32_t fake_last_transfer_read_level;
static uint32_t fake_last_transfer_read_stride;
static uint32_t fake_last_transfer_read_layer_stride;
static struct virgl_box fake_last_transfer_read_box;
static uint64_t fake_last_transfer_read_offset;
static struct iovec *fake_last_transfer_read_iov;
static int fake_last_transfer_read_iov_count;
static int fake_transfer_read_iov_result;
static int fake_submit_cmd_count;
static int fake_last_submit_cmd_ctx_id;
static int fake_last_submit_cmd_ndw;
static uint32_t fake_last_submit_cmd_words[16];
static int fake_submit_cmd_result;
static int wake_renderer_count;
static int wake_frontend_count;
static int fake_cookie_marker;
static int fake_window_create_count;
static int fake_window_create_scanout_idx;
static struct virgl_renderer_gl_ctx_param fake_window_create_param;
static struct virgl_renderer_gl_ctx_param *fake_window_create_param_ptr;
static virgl_renderer_gl_context fake_window_create_result =
    (virgl_renderer_gl_context) (uintptr_t) 0x12345678;
static int fake_window_destroy_count;
static virgl_renderer_gl_context fake_window_destroy_ctx;
static int fake_window_make_current_count;
static int fake_window_make_current_scanout_idx;
static virgl_renderer_gl_context fake_window_make_current_ctx;
static int fake_window_make_current_result;

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

void virgl_renderer_force_ctx_0(void)
{
    fake_force_ctx0_count++;
}

int virgl_renderer_create_fence(int client_fence_id, uint32_t ctx_id)
{
    fake_create_fence_count++;
    fake_last_create_fence_id = client_fence_id;
    fake_last_create_fence_ctx_id = ctx_id;
    return fake_create_fence_result;
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
    return fake_context_create_fence_result;
}

void virgl_renderer_get_cap_set(uint32_t set,
                                uint32_t *max_ver,
                                uint32_t *max_size)
{
    fake_get_cap_set_count++;
    if (set == VIRTIO_GPU_CAPSET_VIRGL) {
        *max_ver = 2;
        *max_size = 4;
    } else if (set == VIRTIO_GPU_CAPSET_VIRGL2) {
        *max_ver = fake_virgl2_supported ? 3 : 0;
        *max_size = fake_virgl2_supported ? 8 : 0;
    } else {
        *max_ver = 0;
        *max_size = 0;
    }
}

void virgl_renderer_fill_caps(uint32_t set, uint32_t version, void *caps)
{
    uint8_t *bytes = caps;

    fake_fill_caps_count++;
    fake_last_fill_caps_set = set;
    fake_last_fill_caps_version = version;
    for (uint32_t i = 0; i < 4; i++)
        bytes[i] = (uint8_t) (0xa0 + i);
}

int virgl_renderer_context_create(uint32_t handle,
                                  uint32_t nlen,
                                  const char *name)
{
    fake_context_create_count++;
    fake_last_context_create_handle = handle;
    fake_last_context_create_nlen = nlen;
    memset(fake_last_context_create_name, 0,
           sizeof(fake_last_context_create_name));
    if (name && nlen < sizeof(fake_last_context_create_name))
        memcpy(fake_last_context_create_name, name, nlen);
    return 0;
}

int virgl_renderer_context_create_with_flags(uint32_t ctx_id,
                                             uint32_t ctx_flags,
                                             uint32_t nlen,
                                             const char *name)
{
    fake_context_create_with_flags_count++;
    fake_last_context_create_with_flags_ctx_id = ctx_id;
    fake_last_context_create_with_flags_flags = ctx_flags;
    fake_last_context_create_with_flags_nlen = nlen;
    memset(fake_last_context_create_with_flags_name, 0,
           sizeof(fake_last_context_create_with_flags_name));
    if (name && nlen < sizeof(fake_last_context_create_with_flags_name))
        memcpy(fake_last_context_create_with_flags_name, name, nlen);
    return 0;
}

void virgl_renderer_context_destroy(uint32_t handle)
{
    fake_context_destroy_count++;
    fake_last_context_destroy_handle = handle;
}

void virgl_renderer_ctx_attach_resource(int ctx_id, int res_handle)
{
    fake_context_attach_count++;
    fake_last_context_attach_ctx_id = ctx_id;
    fake_last_context_attach_resource_id = res_handle;
}

void virgl_renderer_ctx_detach_resource(int ctx_id, int res_handle)
{
    fake_context_detach_count++;
    fake_last_context_detach_ctx_id = ctx_id;
    fake_last_context_detach_resource_id = res_handle;
}

int virgl_renderer_resource_create(
    struct virgl_renderer_resource_create_args *args,
    struct iovec *iov,
    uint32_t num_iovs)
{
    fake_resource_create_count++;
    if (args)
        fake_last_resource_create_args = *args;
    fake_last_resource_create_iov = iov;
    fake_last_resource_create_num_iovs = num_iovs;
    return fake_resource_create_result;
}

int virgl_renderer_resource_create_blob(
    const struct virgl_renderer_resource_create_blob_args *args)
{
    fake_resource_create_blob_count++;
    if (args)
        fake_last_resource_create_blob_args = *args;
    return fake_resource_create_blob_result;
}

int virgl_renderer_resource_get_info(int res_handle,
                                     struct virgl_renderer_resource_info *info)
{
    fake_resource_get_info_count++;
    fake_last_resource_get_info_handle = res_handle;
    if (fake_resource_get_info_result == 0 && info) {
        *info = fake_resource_get_info;
        if (info->handle == 0)
            info->handle = (uint32_t) res_handle;
    }
    return fake_resource_get_info_result;
}

int virgl_renderer_resource_attach_iov(int res_handle,
                                       struct iovec *iov,
                                       int num_iovs)
{
    fake_resource_attach_iov_count++;
    fake_last_resource_attach_iov_handle = res_handle;
    fake_last_resource_attach_iov = iov;
    fake_last_resource_attach_iov_count = num_iovs;
    if (fake_resource_attach_iov_result == 0) {
        fake_attached_iov = iov;
        fake_attached_iov_count = num_iovs;
    }
    return fake_resource_attach_iov_result;
}

void virgl_renderer_resource_detach_iov(int res_handle,
                                        struct iovec **iov,
                                        int *num_iovs)
{
    fake_resource_detach_iov_count++;
    fake_last_resource_detach_iov_handle = res_handle;
    fake_last_detached_iov = fake_attached_iov;
    fake_last_resource_detach_iov_count = fake_attached_iov_count;
    if (num_iovs)
        *num_iovs = fake_attached_iov_count;
    if (iov)
        *iov = fake_attached_iov;
    fake_attached_iov = NULL;
    fake_attached_iov_count = 0;
}

int virgl_renderer_resource_map(uint32_t res_handle, void **map, uint64_t *size)
{
    fake_resource_map_count++;
    fake_last_resource_map_handle = res_handle;
    if (fake_resource_map_result == 0) {
        if (map)
            *map = fake_resource_map_ptr;
        if (size)
            *size = fake_resource_map_size;
    }
    return fake_resource_map_result;
}

int virgl_renderer_resource_unmap(uint32_t res_handle)
{
    fake_resource_unmap_count++;
    fake_last_resource_unmap_handle = res_handle;
    return fake_resource_unmap_result;
}

int virgl_renderer_resource_get_map_info(uint32_t res_handle,
                                         uint32_t *map_info)
{
    fake_resource_get_map_info_count++;
    fake_last_resource_get_map_info_handle = res_handle;
    if (fake_resource_get_map_info_result == 0 && map_info)
        *map_info = fake_resource_get_map_info;
    return fake_resource_get_map_info_result;
}

void virgl_renderer_resource_unref(uint32_t res_handle)
{
    fake_resource_unref_count++;
    fake_last_resource_unref_handle = res_handle;
}

int virgl_renderer_transfer_write_iov(uint32_t handle,
                                      uint32_t ctx_id,
                                      int level,
                                      uint32_t stride,
                                      uint32_t layer_stride,
                                      struct virgl_box *box,
                                      uint64_t offset,
                                      struct iovec *iov,
                                      unsigned int iovec_cnt)
{
    fake_transfer_write_iov_count++;
    fake_last_transfer_write_handle = handle;
    fake_last_transfer_write_ctx_id = ctx_id;
    fake_last_transfer_write_level = level;
    fake_last_transfer_write_stride = stride;
    fake_last_transfer_write_layer_stride = layer_stride;
    fake_last_transfer_write_box = box ? *box : (struct virgl_box) {0};
    fake_last_transfer_write_offset = offset;
    fake_last_transfer_write_iov = iov;
    fake_last_transfer_write_iov_count = iovec_cnt;
    return fake_transfer_write_iov_result;
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
    fake_transfer_read_iov_count++;
    fake_last_transfer_read_handle = handle;
    fake_last_transfer_read_ctx_id = ctx_id;
    fake_last_transfer_read_level = level;
    fake_last_transfer_read_stride = stride;
    fake_last_transfer_read_layer_stride = layer_stride;
    fake_last_transfer_read_box = box ? *box : (struct virgl_box) {0};
    fake_last_transfer_read_offset = offset;
    fake_last_transfer_read_iov = iov;
    fake_last_transfer_read_iov_count = iovec_cnt;
    return fake_transfer_read_iov_result;
}

int virgl_renderer_submit_cmd(void *buffer, int ctx_id, int ndw)
{
    fake_submit_cmd_count++;
    fake_last_submit_cmd_ctx_id = ctx_id;
    fake_last_submit_cmd_ndw = ndw;
    memset(fake_last_submit_cmd_words, 0, sizeof(fake_last_submit_cmd_words));
    if (buffer && ndw > 0) {
        int copy_ndw = ndw;
        if (copy_ndw > (int) (sizeof(fake_last_submit_cmd_words) /
                              sizeof(fake_last_submit_cmd_words[0])))
            copy_ndw = (int) (sizeof(fake_last_submit_cmd_words) /
                              sizeof(fake_last_submit_cmd_words[0]));
        memcpy(fake_last_submit_cmd_words, buffer,
               (size_t) copy_ndw * sizeof(uint32_t));
    }
    return fake_submit_cmd_result;
}

void virgl_renderer_reset(void)
{
    fake_reset_count++;
}

virgl_renderer_gl_context vgpu_window_virgl_create_context(
    int scanout_idx,
    struct virgl_renderer_gl_ctx_param *param)
{
    fake_window_create_count++;
    fake_window_create_scanout_idx = scanout_idx;
    fake_window_create_param_ptr = param;
    if (param)
        fake_window_create_param = *param;
    return fake_window_create_result;
}

void vgpu_window_virgl_destroy_context(virgl_renderer_gl_context ctx)
{
    fake_window_destroy_count++;
    fake_window_destroy_ctx = ctx;
}

int vgpu_window_virgl_make_current(int scanout_idx,
                                   virgl_renderer_gl_context ctx)
{
    fake_window_make_current_count++;
    fake_window_make_current_scanout_idx = scanout_idx;
    fake_window_make_current_ctx = ctx;
    return fake_window_make_current_result;
}

void virtio_gpu_set_num_capsets(virtio_gpu_state_t *vgpu, uint32_t num_capsets)
{
    (void) vgpu;
    fake_set_num_capsets_count++;
    fake_last_num_capsets = num_capsets;
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
    fake_create_fence_result = 0;
    fake_context_create_fence_count = 0;
    fake_last_context_fence_ctx_id = 0;
    fake_last_context_fence_flags = 0;
    fake_last_context_fence_ring_idx = 0;
    fake_last_context_fence_id = 0;
    fake_context_create_fence_result = 0;
    fake_poll_count = 0;
    fake_force_ctx0_count = 0;
    fake_reset_count = 0;
    fake_get_cap_set_count = 0;
    fake_virgl2_supported = false;
    fake_set_num_capsets_count = 0;
    fake_last_num_capsets = 0;
    fake_fill_caps_count = 0;
    fake_last_fill_caps_set = 0;
    fake_last_fill_caps_version = 0;
    fake_context_create_count = 0;
    fake_last_context_create_handle = 0;
    fake_last_context_create_nlen = 0;
    memset(fake_last_context_create_name, 0,
           sizeof(fake_last_context_create_name));
    fake_context_create_with_flags_count = 0;
    fake_last_context_create_with_flags_ctx_id = 0;
    fake_last_context_create_with_flags_flags = 0;
    fake_last_context_create_with_flags_nlen = 0;
    memset(fake_last_context_create_with_flags_name, 0,
           sizeof(fake_last_context_create_with_flags_name));
    fake_context_destroy_count = 0;
    fake_last_context_destroy_handle = 0;
    fake_context_attach_count = 0;
    fake_last_context_attach_ctx_id = 0;
    fake_last_context_attach_resource_id = 0;
    fake_context_detach_count = 0;
    fake_last_context_detach_ctx_id = 0;
    fake_last_context_detach_resource_id = 0;
    fake_resource_create_count = 0;
    fake_last_resource_create_args =
        (struct virgl_renderer_resource_create_args) {0};
    fake_last_resource_create_iov = NULL;
    fake_last_resource_create_num_iovs = 0;
    fake_resource_create_result = 0;
    fake_resource_create_blob_count = 0;
    fake_last_resource_create_blob_args =
        (struct virgl_renderer_resource_create_blob_args) {0};
    fake_resource_create_blob_result = 0;
    fake_resource_get_info_count = 0;
    fake_last_resource_get_info_handle = 0;
    fake_resource_get_info = (struct virgl_renderer_resource_info) {
        .width = 320,
        .height = 240,
        .tex_id = 0x4567,
    };
    fake_resource_get_info_result = 0;
    fake_resource_attach_iov_count = 0;
    fake_last_resource_attach_iov_handle = 0;
    fake_last_resource_attach_iov = NULL;
    fake_last_resource_attach_iov_count = 0;
    fake_resource_attach_iov_result = 0;
    fake_resource_detach_iov_count = 0;
    fake_last_resource_detach_iov_handle = 0;
    free(fake_attached_iov);
    fake_attached_iov = NULL;
    fake_attached_iov_count = 0;
    fake_last_detached_iov = NULL;
    fake_last_resource_detach_iov_count = 0;
    fake_resource_map_count = 0;
    fake_last_resource_map_handle = 0;
    fake_resource_map_ptr = (void *) (uintptr_t) 0x12340000;
    fake_resource_map_size = 0x2000;
    fake_resource_map_result = 0;
    fake_resource_unmap_count = 0;
    fake_last_resource_unmap_handle = 0;
    fake_resource_unmap_result = 0;
    fake_resource_get_map_info_count = 0;
    fake_last_resource_get_map_info_handle = 0;
    fake_resource_get_map_info = VIRTIO_GPU_MAP_CACHE_WC;
    fake_resource_get_map_info_result = 0;
    fake_resource_unref_count = 0;
    fake_last_resource_unref_handle = 0;
    fake_transfer_write_iov_count = 0;
    fake_last_transfer_write_handle = 0;
    fake_last_transfer_write_ctx_id = 0;
    fake_last_transfer_write_level = 0;
    fake_last_transfer_write_stride = 0;
    fake_last_transfer_write_layer_stride = 0;
    fake_last_transfer_write_box = (struct virgl_box) {0};
    fake_last_transfer_write_offset = 0;
    fake_last_transfer_write_iov = NULL;
    fake_last_transfer_write_iov_count = 0;
    fake_transfer_write_iov_result = 0;
    fake_transfer_read_iov_count = 0;
    fake_last_transfer_read_handle = 0;
    fake_last_transfer_read_ctx_id = 0;
    fake_last_transfer_read_level = 0;
    fake_last_transfer_read_stride = 0;
    fake_last_transfer_read_layer_stride = 0;
    fake_last_transfer_read_box = (struct virgl_box) {0};
    fake_last_transfer_read_offset = 0;
    fake_last_transfer_read_iov = NULL;
    fake_last_transfer_read_iov_count = 0;
    fake_transfer_read_iov_result = 0;
    fake_submit_cmd_count = 0;
    fake_last_submit_cmd_ctx_id = 0;
    fake_last_submit_cmd_ndw = 0;
    memset(fake_last_submit_cmd_words, 0, sizeof(fake_last_submit_cmd_words));
    fake_submit_cmd_result = 0;
    wake_renderer_count = 0;
    wake_frontend_count = 0;
    fake_window_create_count = 0;
    fake_window_create_scanout_idx = -1;
    fake_window_create_param = (struct virgl_renderer_gl_ctx_param) {0};
    fake_window_create_param_ptr = NULL;
    fake_window_destroy_count = 0;
    fake_window_destroy_ctx = NULL;
    fake_window_make_current_count = 0;
    fake_window_make_current_scanout_idx = -1;
    fake_window_make_current_ctx = NULL;
    fake_window_make_current_result = 0;

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
    CHECK(fake_callbacks.create_gl_context != NULL);
    CHECK(fake_callbacks.destroy_gl_context != NULL);
    CHECK(fake_callbacks.make_current != NULL);
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

static void test_init_renderer_installs_gl_context_callbacks(void)
{
    reset_test_state(3);
    init_renderer_for_test();

    CHECK(fake_callbacks.create_gl_context != NULL);
    CHECK(fake_callbacks.destroy_gl_context != NULL);
    CHECK(fake_callbacks.make_current != NULL);
}

static void test_create_gl_context_callback_delegates_to_window_hook(void)
{
    struct virgl_renderer_gl_ctx_param param = {
        .version = 1,
        .shared = true,
        .major_ver = 4,
        .minor_ver = 3,
        .compat_ctx = 1,
    };

    reset_test_state(5);
    init_renderer_for_test();

    virgl_renderer_gl_context ctx =
        fake_callbacks.create_gl_context(fake_init_cookie, 2, &param);

    CHECK(ctx == fake_window_create_result);
    CHECK(fake_window_create_count == 1);
    CHECK(fake_window_create_scanout_idx == 2);
    CHECK(fake_window_create_param_ptr == &param);
    CHECK(fake_window_create_param.version == param.version);
    CHECK(fake_window_create_param.shared == param.shared);
    CHECK(fake_window_create_param.major_ver == param.major_ver);
    CHECK(fake_window_create_param.minor_ver == param.minor_ver);
    CHECK(fake_window_create_param.compat_ctx == param.compat_ctx);
}

static void test_destroy_gl_context_callback_delegates_to_window_hook(void)
{
    int marker;
    virgl_renderer_gl_context ctx = &marker;

    reset_test_state(6);
    init_renderer_for_test();

    fake_callbacks.destroy_gl_context(fake_init_cookie, ctx);

    CHECK(fake_window_destroy_count == 1);
    CHECK(fake_window_destroy_ctx == ctx);
}

static void test_make_current_callback_delegates_to_window_hook(void)
{
    int marker;
    virgl_renderer_gl_context ctx = &marker;

    reset_test_state(8);
    fake_window_make_current_result = -17;
    init_renderer_for_test();

    CHECK(fake_callbacks.make_current(fake_init_cookie, 4, ctx) == -17);
    CHECK(fake_window_make_current_count == 1);
    CHECK(fake_window_make_current_scanout_idx == 4);
    CHECK(fake_window_make_current_ctx == ctx);
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
    CHECK(fake_force_ctx0_count == 1);
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
    CHECK(fake_force_ctx0_count == 0);

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
    CHECK(stats.pending_fences == 2);
    CHECK(!stats.poll_request_pending);
    CHECK(stats.poll_requests_submitted == before.poll_requests_submitted + 1);
    CHECK(stats.poll_requests_executed == before.poll_requests_executed + 1);

    queue_stats = renderer_stats();
    CHECK(queue_stats.request_depth == 0);

    vgpu_virgl_request_poll();
    stats = virgl_stats();
    CHECK(stats.poll_request_pending);
    CHECK(stats.poll_requests_submitted == before.poll_requests_submitted + 2);
    queue_stats = renderer_stats();
    CHECK(queue_stats.request_depth == 1);

    fake_callbacks.write_fence(fake_init_cookie, fake_last_create_fence_id);
    struct vgpu_renderer_completion completion;
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.fence_id == 1);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.fence_id == 2);
    CHECK(!vgpu_renderer_pop_completion(&completion));

    CHECK(vgpu_renderer_pop_request(&request));
    CHECK(request.type == VGPU_RENDERER_REQ_POLL);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(fake_poll_count == 2);

    stats = virgl_stats();
    CHECK(stats.pending_fences == 0);
    CHECK(!stats.poll_request_pending);
    CHECK(stats.poll_requests_executed == before.poll_requests_executed + 2);
}

static void test_ctrl_request_executes_get_capset_info_completion(void)
{
    reset_test_state(31);

    struct vgpu_renderer_ctrl_payload payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO},
        .cmd.get_capset_info =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO},
                .capset_index = 0,
            },
        .response_capacity = sizeof(struct virtio_gpu_resp_capset_info),
        .response_type = VIRTIO_GPU_RESP_OK_CAPSET_INFO,
        .ctrl_completion =
            {
                .queue_index = VIRTIO_GPU_CONTROLQ,
                .desc_head = 11,
                .actor_generation = 22,
                .common_generation = 31,
                .trigger_irq = true,
            },
        .response_desc =
            {
                .addr = 0x80,
                .len = sizeof(struct virtio_gpu_resp_capset_info),
                .flags = VIRTIO_DESC_F_WRITE,
            },
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 31},
        .command_type = VIRTIO_GPU_CMD_GET_CAPSET_INFO,
        .payload = &payload,
        .payload_size = sizeof(payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.type == VGPU_RENDERER_DONE_CTRL);
    CHECK(completion.token.generation == 31);
    CHECK(completion.has_ctrl_completion);
    CHECK(completion.ctrl_completion.desc_head == 11);
    CHECK(completion.has_response_desc);
    CHECK(completion.response_size ==
          sizeof(struct virtio_gpu_resp_capset_info));

    struct virtio_gpu_resp_capset_info *response = completion.response;
    CHECK(response->hdr.type == VIRTIO_GPU_RESP_OK_CAPSET_INFO);
    CHECK(response->capset_id == VIRTIO_GPU_CAPSET_VIRGL);
    CHECK(response->capset_max_version == 2);
    CHECK(response->capset_max_size == 4);
    completion.release_response(completion.response);
}

static void test_ctrl_request_executes_get_capset_completion(void)
{
    reset_test_state(32);

    struct vgpu_renderer_ctrl_payload payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_GET_CAPSET},
        .cmd.get_capset =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_GET_CAPSET},
                .capset_id = VIRTIO_GPU_CAPSET_VIRGL,
                .capset_version = 2,
            },
        .response_capacity = sizeof(struct virtio_gpu_resp_capset) + 4,
        .response_type = VIRTIO_GPU_RESP_OK_CAPSET,
        .ctrl_completion =
            {
                .queue_index = VIRTIO_GPU_CONTROLQ,
                .desc_head = 12,
                .actor_generation = 23,
                .common_generation = 32,
                .trigger_irq = true,
            },
        .response_desc =
            {
                .addr = 0x90,
                .len = sizeof(struct virtio_gpu_resp_capset) + 4,
                .flags = VIRTIO_DESC_F_WRITE,
            },
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 32},
        .command_type = VIRTIO_GPU_CMD_GET_CAPSET,
        .payload = &payload,
        .payload_size = sizeof(payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.type == VGPU_RENDERER_DONE_CTRL);
    CHECK(completion.response_size ==
          sizeof(struct virtio_gpu_resp_capset) + 4);
    CHECK(fake_fill_caps_count == 1);
    CHECK(fake_last_fill_caps_set == VIRTIO_GPU_CAPSET_VIRGL);
    CHECK(fake_last_fill_caps_version == 2);

    struct virtio_gpu_resp_capset *response = completion.response;
    CHECK(response->hdr.type == VIRTIO_GPU_RESP_OK_CAPSET);
    CHECK(response->capset_data[0] == 0xa0);
    CHECK(response->capset_data[3] == 0xa3);
    completion.release_response(completion.response);
}

static void test_ctrl_request_exposes_supported_virgl2_capset(void)
{
    reset_test_state(132);
    fake_virgl2_supported = true;

    struct vgpu_renderer_ctrl_payload payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO},
        .cmd.get_capset_info =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO},
                .capset_index = 1,
            },
        .response_capacity = sizeof(struct virtio_gpu_resp_capset_info),
        .response_type = VIRTIO_GPU_RESP_OK_CAPSET_INFO,
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 132},
        .command_type = VIRTIO_GPU_CMD_GET_CAPSET_INFO,
        .payload = &payload,
        .payload_size = sizeof(payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_CAPSET_INFO);
    CHECK(completion.response_size ==
          sizeof(struct virtio_gpu_resp_capset_info));
    struct virtio_gpu_resp_capset_info *capset_info = completion.response;
    CHECK(capset_info->capset_id == VIRTIO_GPU_CAPSET_VIRGL2);
    CHECK(capset_info->capset_max_version == 3);
    CHECK(capset_info->capset_max_size == 8);
    completion.release_response(completion.response);

    payload = (struct vgpu_renderer_ctrl_payload) {
        .hdr = {.type = VIRTIO_GPU_CMD_GET_CAPSET},
        .cmd.get_capset =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_GET_CAPSET},
                .capset_id = VIRTIO_GPU_CAPSET_VIRGL2,
                .capset_version = 3,
            },
        .response_capacity = sizeof(struct virtio_gpu_resp_capset) + 8,
        .response_type = VIRTIO_GPU_RESP_OK_CAPSET,
    };
    request.command_type = VIRTIO_GPU_CMD_GET_CAPSET;
    request.payload = &payload;

    vgpu_virgl_execute_renderer_request(&request);

    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_CAPSET);
    CHECK(completion.response_size ==
          sizeof(struct virtio_gpu_resp_capset) + 8);
    CHECK(fake_fill_caps_count == 1);
    CHECK(fake_last_fill_caps_set == VIRTIO_GPU_CAPSET_VIRGL2);
    CHECK(fake_last_fill_caps_version == 3);
    completion.release_response(completion.response);
}

static void test_ctrl_request_executes_context_create_destroy(void)
{
    reset_test_state(33);

    struct vgpu_renderer_ctrl_payload payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_CTX_CREATE, .ctx_id = 77},
        .cmd.ctx_create =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_CTX_CREATE, .ctx_id = 77},
                .nlen = 4,
            },
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
        .ctrl_completion =
            {
                .queue_index = VIRTIO_GPU_CONTROLQ,
                .desc_head = 13,
                .actor_generation = 24,
                .common_generation = 33,
                .trigger_irq = true,
            },
        .response_desc =
            {
                .addr = 0xa0,
                .len = sizeof(struct virtio_gpu_ctrl_hdr),
                .flags = VIRTIO_DESC_F_WRITE,
            },
    };
    memcpy(payload.cmd.ctx_create.debug_name, "ctxB", 4);
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 33},
        .command_type = VIRTIO_GPU_CMD_CTX_CREATE,
        .payload = &payload,
        .payload_size = sizeof(payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(completion.response == NULL);
    CHECK(fake_context_create_count == 1);
    CHECK(fake_last_context_create_handle == 77);
    CHECK(fake_last_context_create_nlen == 4);
    CHECK(memcmp(fake_last_context_create_name, "ctxB", 4) == 0);
    CHECK(fake_context_create_with_flags_count == 0);

    payload = (struct vgpu_renderer_ctrl_payload) {
        .hdr = {.type = VIRTIO_GPU_CMD_CTX_CREATE, .ctx_id = 78},
        .cmd.ctx_create =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_CTX_CREATE, .ctx_id = 78},
                .nlen = 4,
                .context_init = VIRTIO_GPU_CAPSET_VIRGL,
            },
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    memcpy(payload.cmd.ctx_create.debug_name, "ctxC", 4);
    request.command_type = VIRTIO_GPU_CMD_CTX_CREATE;
    request.payload = &payload;

    vgpu_virgl_execute_renderer_request(&request);

    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(fake_context_create_count == 1);
    CHECK(fake_context_create_with_flags_count == 1);
    CHECK(fake_last_context_create_with_flags_ctx_id == 78);
    CHECK(fake_last_context_create_with_flags_flags == VIRTIO_GPU_CAPSET_VIRGL);
    CHECK(fake_last_context_create_with_flags_nlen == 4);
    CHECK(memcmp(fake_last_context_create_with_flags_name, "ctxC", 4) == 0);

    payload = (struct vgpu_renderer_ctrl_payload) {
        .hdr = {.type = VIRTIO_GPU_CMD_CTX_DESTROY, .ctx_id = 77},
        .cmd.ctx_destroy =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_CTX_DESTROY, .ctx_id = 77},
            },
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
        .ctrl_completion =
            {
                .queue_index = VIRTIO_GPU_CONTROLQ,
                .desc_head = 14,
                .actor_generation = 24,
                .common_generation = 33,
                .trigger_irq = true,
            },
        .response_desc =
            {
                .addr = 0xb0,
                .len = sizeof(struct virtio_gpu_ctrl_hdr),
                .flags = VIRTIO_DESC_F_WRITE,
            },
    };
    request.command_type = VIRTIO_GPU_CMD_CTX_DESTROY;
    request.payload = &payload;

    vgpu_virgl_execute_renderer_request(&request);

    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(completion.response == NULL);
    CHECK(fake_context_destroy_count == 1);
    CHECK(fake_last_context_destroy_handle == 77);
}

static void test_ctrl_request_executes_virgl2_context_init(void)
{
    reset_test_state(233);
    fake_virgl2_supported = true;

    struct vgpu_renderer_ctrl_payload payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_CTX_CREATE, .ctx_id = 79},
        .cmd.ctx_create =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_CTX_CREATE, .ctx_id = 79},
                .nlen = 4,
                .context_init = VIRTIO_GPU_CAPSET_VIRGL2,
            },
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    memcpy(payload.cmd.ctx_create.debug_name, "ctxD", 4);
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 233},
        .command_type = VIRTIO_GPU_CMD_CTX_CREATE,
        .payload = &payload,
        .payload_size = sizeof(payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(completion.response == NULL);
    CHECK(fake_context_create_count == 0);
    CHECK(fake_context_create_with_flags_count == 1);
    CHECK(fake_last_context_create_with_flags_ctx_id == 79);
    CHECK(fake_last_context_create_with_flags_flags ==
          VIRTIO_GPU_CAPSET_VIRGL2);
    CHECK(fake_last_context_create_with_flags_nlen == 4);
    CHECK(memcmp(fake_last_context_create_with_flags_name, "ctxD", 4) == 0);
}

static void test_ctrl_request_executes_context_resource_attach_detach(void)
{
    reset_test_state(133);

    struct vgpu_renderer_ctrl_payload payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, .ctx_id = 77},
        .cmd.ctx_resource =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
                        .ctx_id = 77},
                .resource_id = 88,
            },
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 133},
        .command_type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
        .payload = &payload,
        .payload_size = sizeof(payload),
    };
    struct vgpu_renderer_completion completion = {0};

    vgpu_virgl_execute_renderer_request(&request);

    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(completion.response == NULL);
    CHECK(fake_context_attach_count == 1);
    CHECK(fake_last_context_attach_ctx_id == 77);
    CHECK(fake_last_context_attach_resource_id == 88);
    CHECK(fake_context_detach_count == 0);

    payload = (struct vgpu_renderer_ctrl_payload) {
        .hdr = {.type = VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, .ctx_id = 77},
        .cmd.ctx_resource =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,
                        .ctx_id = 77},
                .resource_id = 88,
            },
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    request.command_type = VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE;
    request.payload = &payload;
    request.payload_size = sizeof(payload);

    vgpu_virgl_execute_renderer_request(&request);

    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(completion.response == NULL);
    CHECK(fake_context_detach_count == 1);
    CHECK(fake_last_context_detach_ctx_id == 77);
    CHECK(fake_last_context_detach_resource_id == 88);
}

static void test_ctrl_request_executes_resource_create_3d_completion(void)
{
    reset_test_state(34);

    struct vgpu_renderer_ctrl_payload payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
        .cmd.resource_create_3d =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
                .resource_id = 88,
                .target = 2,
                .format = 3,
                .bind = 4,
                .width = 640,
                .height = 480,
                .depth = 1,
                .array_size = 1,
                .last_level = 0,
                .nr_samples = 1,
                .flags = 0x5a,
            },
        .resource_generation = 0x1234,
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
        .ctrl_completion =
            {
                .queue_index = VIRTIO_GPU_CONTROLQ,
                .desc_head = 15,
                .actor_generation = 25,
                .common_generation = 34,
                .trigger_irq = true,
            },
        .response_desc =
            {
                .addr = 0xc0,
                .len = sizeof(struct virtio_gpu_ctrl_hdr),
                .flags = VIRTIO_DESC_F_WRITE,
            },
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 34},
        .command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D,
        .payload = &payload,
        .payload_size = sizeof(payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(completion.virgl_resource.type ==
          VGPU_VIRGL_RESOURCE_SIDE_EFFECT_NONE);
    CHECK(fake_resource_create_count == 1);
    CHECK(fake_last_resource_create_args.handle == 88);
    CHECK(fake_last_resource_create_args.target == 2);
    CHECK(fake_last_resource_create_args.format == 3);
    CHECK(fake_last_resource_create_args.bind == 4);
    CHECK(fake_last_resource_create_args.width == 640);
    CHECK(fake_last_resource_create_args.height == 480);
    CHECK(fake_last_resource_create_args.flags == 0x5a);
    CHECK(fake_last_resource_create_iov == NULL);
    CHECK(fake_last_resource_create_num_iovs == 0);
}


static void test_ctrl_request_executes_resource_create_blob_completion(void)
{
    struct iovec iov[2] = {
        {.iov_base = (void *) (uintptr_t) 0x1000, .iov_len = 16},
        {.iov_base = (void *) (uintptr_t) 0x2000, .iov_len = 32},
    };

    reset_test_state(135);

    struct vgpu_renderer_ctrl_payload payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB, .ctx_id = 44},
        .cmd.resource_create_blob =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB,
                        .ctx_id = 44},
                .resource_id = 188,
                .blob_mem = VIRTIO_GPU_BLOB_MEM_GUEST,
                .blob_flags = VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE,
                .nr_entries = 2,
                .blob_id = UINT64_C(0x0102030405060708),
                .size = 48,
            },
        .iov = iov,
        .iov_count = 2,
        .resource_generation = 0x123456,
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
        .ctrl_completion =
            {
                .queue_index = VIRTIO_GPU_CONTROLQ,
                .desc_head = 115,
                .actor_generation = 125,
                .common_generation = 135,
                .trigger_irq = true,
            },
        .response_desc =
            {
                .addr = 0xc8,
                .len = sizeof(struct virtio_gpu_ctrl_hdr),
                .flags = VIRTIO_DESC_F_WRITE,
            },
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 135},
        .command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB,
        .payload = &payload,
        .payload_size = sizeof(payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(completion.virgl_resource.type ==
          VGPU_VIRGL_RESOURCE_SIDE_EFFECT_NONE);
    CHECK(fake_resource_create_blob_count == 1);
    CHECK(fake_last_resource_create_blob_args.res_handle == 188);
    CHECK(fake_last_resource_create_blob_args.ctx_id == 44);
    CHECK(fake_last_resource_create_blob_args.blob_mem ==
          VIRTIO_GPU_BLOB_MEM_GUEST);
    CHECK(fake_last_resource_create_blob_args.blob_flags ==
          VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE);
    CHECK(fake_last_resource_create_blob_args.blob_id ==
          UINT64_C(0x0102030405060708));
    CHECK(fake_last_resource_create_blob_args.size == 48);
    CHECK(fake_last_resource_create_blob_args.iovecs == iov);
    CHECK(fake_last_resource_create_blob_args.num_iovs == 2);
    CHECK(fake_resource_create_count == 0);

    fake_resource_create_blob_result = -1;
    payload.cmd.resource_create_blob.resource_id = 189;
    payload.resource_generation = 0x789abc;
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;

    vgpu_virgl_execute_renderer_request(&request);

    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_UNSPEC);
    CHECK(completion.virgl_resource.type ==
          VGPU_VIRGL_RESOURCE_SIDE_EFFECT_CREATE_3D_ROLLBACK);
    CHECK(completion.virgl_resource.resource_id == 189);
    CHECK(completion.virgl_resource.resource_generation == 0x789abc);
    CHECK(fake_resource_create_blob_count == 2);
    CHECK(fake_resource_unref_count == 0);
}

static void test_ctrl_request_maps_and_unmaps_blob_resources(void)
{
    reset_test_state(136);

    struct vgpu_renderer_ctrl_payload map_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB,
                .flags = VIRTIO_GPU_FLAG_FENCE | VIRTIO_GPU_FLAG_INFO_RING_IDX,
                .fence_id = UINT64_C(0x0102030405060708),
                .ctx_id = 7,
                .ring_idx = 3},
        .cmd.resource_map_blob =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB,
                        .flags = VIRTIO_GPU_FLAG_FENCE |
                                 VIRTIO_GPU_FLAG_INFO_RING_IDX,
                        .fence_id = UINT64_C(0x0102030405060708),
                        .ctx_id = 7,
                        .ring_idx = 3},
                .resource_id = 404,
            },
        .response_capacity = sizeof(struct virtio_gpu_resp_map_info),
        .response_type = VIRTIO_GPU_RESP_OK_MAP_INFO,
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 136},
        .command_type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB,
        .payload = &map_payload,
        .payload_size = sizeof(map_payload),
    };
    struct vgpu_renderer_completion completion = {0};

    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    CHECK(fake_resource_map_count == 0);

    struct vgpu_renderer_ctrl_payload create_3d = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
        .cmd.resource_create_3d =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
                .resource_id = 190,
                .target = 2,
                .format = 3,
                .bind = 4,
                .width = 64,
                .height = 64,
                .depth = 1,
                .array_size = 1,
                .nr_samples = 1,
            },
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    request.payload = &create_3d;
    request.payload_size = sizeof(create_3d);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);

    map_payload.cmd.resource_map_blob.resource_id = 190;
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    request.payload = &map_payload;
    request.payload_size = sizeof(map_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    CHECK(fake_resource_map_count == 0);

    struct vgpu_renderer_ctrl_payload create_blob = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB, .ctx_id = 8},
        .cmd.resource_create_blob =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB,
                        .ctx_id = 8},
                .resource_id = 191,
                .blob_mem = VIRTIO_GPU_BLOB_MEM_HOST3D,
                .blob_flags = VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE,
                .size = 4096,
            },
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    request.payload = &create_blob;
    request.payload_size = sizeof(create_blob);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);

    map_payload.cmd.resource_map_blob.resource_id = 191;
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    request.payload = &map_payload;
    request.payload_size = sizeof(map_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_MAP_INFO);
    CHECK(completion.response_size == sizeof(struct virtio_gpu_resp_map_info));
    struct virtio_gpu_resp_map_info *map_response = completion.response;
    CHECK(map_response != NULL);
    CHECK(map_response->hdr.type == VIRTIO_GPU_RESP_OK_MAP_INFO);
    CHECK(map_response->hdr.flags ==
          (VIRTIO_GPU_FLAG_FENCE | VIRTIO_GPU_FLAG_INFO_RING_IDX));
    CHECK(map_response->hdr.fence_id == UINT64_C(0x0102030405060708));
    CHECK(map_response->hdr.ctx_id == 7);
    CHECK(map_response->hdr.ring_idx == 3);
    CHECK(map_response->map_info == VIRTIO_GPU_MAP_CACHE_WC);
    completion.release_response(completion.response);
    CHECK(fake_resource_map_count == 1);
    CHECK(fake_last_resource_map_handle == 191);
    CHECK(fake_resource_get_map_info_count == 1);
    CHECK(fake_last_resource_get_map_info_handle == 191);

    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_UNSPEC);
    CHECK(fake_resource_map_count == 1);

    struct vgpu_renderer_ctrl_payload unmap_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB},
        .cmd.resource_unmap_blob =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB},
                .resource_id = 191,
            },
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB;
    request.payload = &unmap_payload;
    request.payload_size = sizeof(unmap_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(fake_resource_unmap_count == 1);
    CHECK(fake_last_resource_unmap_handle == 191);

    unmap_payload.cmd.resource_unmap_blob.resource_id = 404;
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);

    unmap_payload.cmd.resource_unmap_blob.resource_id = 190;
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    CHECK(fake_resource_unmap_count == 1);

    create_blob.cmd.resource_create_blob.resource_id = 192;
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    request.payload = &create_blob;
    request.payload_size = sizeof(create_blob);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);

    map_payload.cmd.resource_map_blob.resource_id = 192;
    map_payload.response_capacity = sizeof(struct virtio_gpu_ctrl_hdr);
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    request.payload = &map_payload;
    request.payload_size = sizeof(map_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    CHECK(fake_resource_map_count == 1);
    CHECK(fake_resource_get_map_info_count == 1);
    map_payload.response_capacity = sizeof(struct virtio_gpu_resp_map_info);

    create_blob.cmd.resource_create_blob.resource_id = 193;
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    request.payload = &create_blob;
    request.payload_size = sizeof(create_blob);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);

    fake_resource_map_result = -1;
    map_payload.cmd.resource_map_blob.resource_id = 193;
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    request.payload = &map_payload;
    request.payload_size = sizeof(map_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_UNSPEC);
    CHECK(fake_resource_map_count == 2);
    CHECK(fake_last_resource_map_handle == 193);
    CHECK(fake_resource_get_map_info_count == 1);
    fake_resource_map_result = 0;

    create_blob.cmd.resource_create_blob.resource_id = 194;
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    request.payload = &create_blob;
    request.payload_size = sizeof(create_blob);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);

    int unmaps_before = fake_resource_unmap_count;
    fake_resource_get_map_info_result = -1;
    map_payload.cmd.resource_map_blob.resource_id = 194;
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    request.payload = &map_payload;
    request.payload_size = sizeof(map_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_UNSPEC);
    CHECK(fake_resource_map_count == 3);
    CHECK(fake_resource_get_map_info_count == 2);
    CHECK(fake_resource_unmap_count == unmaps_before + 1);
    CHECK(fake_last_resource_unmap_handle == 194);
    fake_resource_get_map_info_result = 0;

    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_MAP_INFO);
    CHECK(fake_resource_map_count == 4);
    CHECK(fake_resource_get_map_info_count == 3);
    completion.release_response(completion.response);

    unmap_payload.cmd.resource_unmap_blob.resource_id = 194;
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB;
    request.payload = &unmap_payload;
    request.payload_size = sizeof(unmap_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(fake_resource_unmap_count == unmaps_before + 2);
    CHECK(fake_last_resource_unmap_handle == 194);

    create_blob.cmd.resource_create_blob.resource_id = 195;
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    request.payload = &create_blob;
    request.payload_size = sizeof(create_blob);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);

    map_payload.cmd.resource_map_blob.resource_id = 195;
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    request.payload = &map_payload;
    request.payload_size = sizeof(map_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_MAP_INFO);
    completion.release_response(completion.response);

    unmaps_before = fake_resource_unmap_count;
    int unrefs_before = fake_resource_unref_count;
    struct vgpu_renderer_ctrl_payload unref_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_UNREF},
        .cmd.resource_unref =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_UNREF},
                .resource_id = 195,
            },
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    request.payload = &unref_payload;
    request.payload_size = sizeof(unref_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(fake_resource_unmap_count == unmaps_before + 1);
    CHECK(fake_last_resource_unmap_handle == 195);
    CHECK(fake_resource_unref_count == unrefs_before + 1);
    CHECK(fake_last_resource_unref_handle == 195);

    create_blob.cmd.resource_create_blob.resource_id = 196;
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    request.payload = &create_blob;
    request.payload_size = sizeof(create_blob);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);

    map_payload.cmd.resource_map_blob.resource_id = 196;
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    request.payload = &map_payload;
    request.payload_size = sizeof(map_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_MAP_INFO);
    completion.release_response(completion.response);

    unmaps_before = fake_resource_unmap_count;
    fake_resource_unmap_result = -1;
    unmap_payload.cmd.resource_unmap_blob.resource_id = 196;
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB;
    request.payload = &unmap_payload;
    request.payload_size = sizeof(unmap_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_UNSPEC);
    CHECK(fake_resource_unmap_count == unmaps_before + 1);
    CHECK(fake_last_resource_unmap_handle == 196);
    fake_resource_unmap_result = 0;

    int maps_before = fake_resource_map_count;
    map_payload.cmd.resource_map_blob.resource_id = 196;
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    request.payload = &map_payload;
    request.payload_size = sizeof(map_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_UNSPEC);
    CHECK(fake_resource_map_count == maps_before);

    unmaps_before = fake_resource_unmap_count;
    int resets_before = fake_reset_count;
    vgpu_virgl_reset_renderer();
    CHECK(fake_resource_unmap_count == unmaps_before + 1);
    CHECK(fake_last_resource_unmap_handle == 196);
    CHECK(fake_reset_count == resets_before + 1);
}

static void test_blob_hostmem_aperture_bounds_and_access(void)
{
    reset_test_state(137);

    uint8_t blob_data[0x2000];
    for (size_t i = 0; i < sizeof(blob_data); i++)
        blob_data[i] = (uint8_t) (0x10 + i);
    blob_data[2] = 0x92;
    blob_data[3] = 0xf1;
    blob_data[4] = 0x34;
    blob_data[5] = 0x56;
    blob_data[6] = 0x78;
    blob_data[7] = 0x9a;
    fake_resource_map_ptr = blob_data;
    fake_resource_map_size = sizeof(blob_data);

    struct vgpu_renderer_completion completion = {0};
    struct vgpu_renderer_ctrl_payload create_blob = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB},
        .cmd.resource_create_blob =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB},
                .resource_id = 501,
                .blob_mem = VIRTIO_GPU_BLOB_MEM_HOST3D,
                .blob_flags = VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE,
                .size = 0x1000,
            },
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 137},
        .command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB,
        .payload = &create_blob,
        .payload_size = sizeof(create_blob),
    };

    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);

    struct vgpu_renderer_ctrl_payload map_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB},
        .cmd.resource_map_blob =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB},
                .resource_id = 501,
                .offset = SEMU_PLATFORM_VGPU_HOSTMEM_SIZE - 0x800,
            },
        .response_capacity = sizeof(struct virtio_gpu_resp_map_info),
        .response_type = VIRTIO_GPU_RESP_OK_MAP_INFO,
    };
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    request.payload = &map_payload;
    request.payload_size = sizeof(map_payload);

    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    CHECK(fake_resource_map_count == 0);

    map_payload.cmd.resource_map_blob.offset = UINT64_MAX - 0x7ff;
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    CHECK(fake_resource_map_count == 0);

    map_payload.cmd.resource_map_blob.offset = 0x3000;
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_MAP_INFO);
    CHECK(fake_resource_map_count == 1);
    completion.release_response(completion.response);

    uint32_t value = 0;
    CHECK(!vgpu_virgl_hostmem_read(0x2fff, RV_MEM_LBU, &value));
    CHECK(vgpu_virgl_hostmem_read(0x3002, RV_MEM_LBU, &value));
    CHECK(value == blob_data[2]);
    CHECK(vgpu_virgl_hostmem_read(0x3002, RV_MEM_LB, &value));
    CHECK(value == (uint32_t) (int32_t) (int8_t) blob_data[2]);
    CHECK(vgpu_virgl_hostmem_read(0x3002, RV_MEM_LHU, &value));
    CHECK(value == ((uint32_t) blob_data[2] | ((uint32_t) blob_data[3] << 8)));
    CHECK(vgpu_virgl_hostmem_read(0x3002, RV_MEM_LH, &value));
    CHECK(value ==
          (uint32_t) (int32_t) (int16_t) ((uint16_t) blob_data[2] |
                                          ((uint16_t) blob_data[3] << 8)));
    CHECK(vgpu_virgl_hostmem_read(0x3004, RV_MEM_LW, &value));
    CHECK(value ==
          ((uint32_t) blob_data[4] | ((uint32_t) blob_data[5] << 8) |
           ((uint32_t) blob_data[6] << 16) | ((uint32_t) blob_data[7] << 24)));
    CHECK(!vgpu_virgl_hostmem_read(0x3fff, RV_MEM_LHU, &value));
    CHECK(!vgpu_virgl_hostmem_read(0x3000, 8, &value));

    CHECK(vgpu_virgl_hostmem_write(0x3008, RV_MEM_SB, 0xaa));
    CHECK(blob_data[8] == 0xaa);
    CHECK(vgpu_virgl_hostmem_write(0x300a, RV_MEM_SH, 0xb1c2));
    CHECK(blob_data[10] == 0xc2);
    CHECK(blob_data[11] == 0xb1);
    CHECK(vgpu_virgl_hostmem_write(0x300c, RV_MEM_SW, 0xd1e2f304));
    CHECK(blob_data[12] == 0x04);
    CHECK(blob_data[13] == 0xf3);
    CHECK(blob_data[14] == 0xe2);
    CHECK(blob_data[15] == 0xd1);
    CHECK(!vgpu_virgl_hostmem_write(0x3fff, RV_MEM_SH, 0x55));
    CHECK(!vgpu_virgl_hostmem_write(0x3000, 8, 0x55));

    struct vgpu_renderer_ctrl_payload unmap_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB},
        .cmd.resource_unmap_blob =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB},
                .resource_id = 501,
            },
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB;
    request.payload = &unmap_payload;
    request.payload_size = sizeof(unmap_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(!vgpu_virgl_hostmem_read(0x3002, RV_MEM_LBU, &value));
    CHECK(!vgpu_virgl_hostmem_write(0x3002, RV_MEM_SB, 0x11));

    map_payload.cmd.resource_map_blob.offset = 0x4000;
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    request.payload = &map_payload;
    request.payload_size = sizeof(map_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_MAP_INFO);
    completion.release_response(completion.response);
    CHECK(vgpu_virgl_hostmem_read(0x4002, RV_MEM_LBU, &value));

    struct vgpu_renderer_ctrl_payload unref_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_UNREF},
        .cmd.resource_unref =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_UNREF},
                .resource_id = 501,
            },
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    request.payload = &unref_payload;
    request.payload_size = sizeof(unref_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(!vgpu_virgl_hostmem_read(0x4002, RV_MEM_LBU, &value));

    create_blob.cmd.resource_create_blob.resource_id = 502;
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    request.payload = &create_blob;
    request.payload_size = sizeof(create_blob);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    map_payload.cmd.resource_map_blob.resource_id = 502;
    map_payload.cmd.resource_map_blob.offset = 0x5000;
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    request.payload = &map_payload;
    request.payload_size = sizeof(map_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_MAP_INFO);
    completion.release_response(completion.response);
    CHECK(vgpu_virgl_hostmem_read(0x5002, RV_MEM_LBU, &value));

    vgpu_virgl_reset_renderer();
    CHECK(!vgpu_virgl_hostmem_read(0x5002, RV_MEM_LBU, &value));
}

static void test_ctrl_request_resource_create_3d_failure_rolls_back_frontend(
    void)
{
    reset_test_state(35);
    fake_resource_create_result = -1;

    struct vgpu_renderer_ctrl_payload payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
        .cmd.resource_create_3d =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
                .resource_id = 89,
            },
        .resource_generation = 0x5678,
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
        .ctrl_completion =
            {
                .queue_index = VIRTIO_GPU_CONTROLQ,
                .desc_head = 16,
                .actor_generation = 26,
                .common_generation = 35,
                .trigger_irq = true,
            },
        .response_desc =
            {
                .addr = 0xd0,
                .len = sizeof(struct virtio_gpu_ctrl_hdr),
                .flags = VIRTIO_DESC_F_WRITE,
            },
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 35},
        .command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D,
        .payload = &payload,
        .payload_size = sizeof(payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_UNSPEC);
    CHECK(completion.virgl_resource.type ==
          VGPU_VIRGL_RESOURCE_SIDE_EFFECT_CREATE_3D_ROLLBACK);
    CHECK(completion.virgl_resource.resource_id == 89);
    CHECK(completion.virgl_resource.resource_generation == 0x5678);
    CHECK(fake_resource_create_count == 1);
    CHECK(fake_resource_unref_count == 0);
}

static void test_ctrl_request_records_set_scanout_gl_payload_completion(void)
{
    reset_test_state(41);

    struct vgpu_renderer_ctrl_payload create_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
        .cmd.resource_create_3d =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
                .resource_id = 91,
                .target = 2,
                .format = 3,
                .bind = 4,
                .width = 320,
                .height = 240,
                .depth = 1,
                .array_size = 1,
                .nr_samples = 1,
            },
        .resource_generation = 0x1111,
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 41},
        .command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D,
        .payload = &create_payload,
        .payload_size = sizeof(create_payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(fake_resource_create_count == 1);

    struct vgpu_renderer_ctrl_payload scanout_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_SET_SCANOUT},
        .cmd.set_scanout =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_SET_SCANOUT},
                .r = {.x = 4, .y = 8, .width = 160, .height = 120},
                .scanout_id = 0,
                .resource_id = 91,
            },
        .scanout_generation = 0x2222,
        .scanout =
            {
                .enabled = 1,
                .width = 1024,
                .height = 768,
            },
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    request.command_type = VIRTIO_GPU_CMD_SET_SCANOUT;
    request.payload = &scanout_payload;
    request.payload_size = sizeof(scanout_payload);

    vgpu_virgl_execute_renderer_request(&request);

    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(completion.virgl_resource.type ==
          VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT);
    CHECK(completion.virgl_resource.scanout_count == 1);
    CHECK(completion.virgl_resource.scanouts[0].scanout_id == 0);
    CHECK(completion.virgl_resource.scanouts[0].scanout_generation == 0x2222);
    CHECK(completion.virgl_resource.scanouts[0].scanout.primary_resource_id ==
          91);
    CHECK(completion.virgl_resource.scanouts[0].scanout.src_x == 4);
    CHECK(completion.virgl_resource.scanouts[0].scanout.src_y == 8);
    CHECK(completion.virgl_resource.scanouts[0].scanout.src_w == 160);
    CHECK(completion.virgl_resource.scanouts[0].scanout.src_h == 120);
    CHECK(completion.virgl_resource.scanouts[0].has_gl_payload);
    CHECK(completion.virgl_resource.scanouts[0].gl_payload.texture_id ==
          0x4567);
    CHECK(completion.virgl_resource.scanouts[0].gl_payload.width == 320);
    CHECK(completion.virgl_resource.scanouts[0].gl_payload.height == 240);
    CHECK(completion.virgl_resource.scanouts[0].gl_payload.src_x == 4);
    CHECK(completion.virgl_resource.scanouts[0].gl_payload.src_y == 8);
    CHECK(completion.virgl_resource.scanouts[0].gl_payload.src_width == 160);
    CHECK(completion.virgl_resource.scanouts[0].gl_payload.src_height == 120);
    CHECK(!completion.virgl_resource.scanouts[0].gl_payload.y_0_top);
    CHECK(fake_resource_get_info_count == 1);
    CHECK(fake_last_resource_get_info_handle == 91);
    CHECK(fake_window_create_count == 0);
    CHECK(fake_window_make_current_count == 0);
    CHECK(!vgpu_renderer_pop_completion(&completion));
}

static void test_ctrl_request_records_set_scanout_blob_gl_payload_completion(
    void)
{
    reset_test_state(37);

    struct vgpu_renderer_ctrl_payload missing_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_SET_SCANOUT_BLOB},
        .cmd.set_scanout_blob =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_SET_SCANOUT_BLOB},
                .r = {.x = 0, .y = 0, .width = 160, .height = 120},
                .scanout_id = 0,
                .resource_id = 999,
                .width = 320,
                .height = 240,
                .format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                .strides = {1280, 0, 0, 0},
            },
        .scanout_generation = 0x1111,
        .scanout =
            {
                .enabled = 1,
                .width = 1024,
                .height = 768,
            },
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 37},
        .command_type = VIRTIO_GPU_CMD_SET_SCANOUT_BLOB,
        .payload = &missing_payload,
        .payload_size = sizeof(missing_payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    CHECK(completion.virgl_resource.type ==
          VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT_ROLLBACK);
    CHECK(completion.virgl_resource.scanout_count == 1);
    CHECK(fake_resource_get_info_count == 0);

    struct vgpu_renderer_ctrl_payload create_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB, .ctx_id = 44},
        .cmd.resource_create_blob =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB,
                        .ctx_id = 44},
                .resource_id = 92,
                .blob_mem = VIRTIO_GPU_BLOB_MEM_HOST3D,
                .blob_flags = VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE,
                .size = 4096,
            },
        .resource_generation = 0x2222,
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    request.payload = &create_payload;
    request.payload_size = sizeof(create_payload);

    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(fake_resource_create_blob_count == 1);

    struct vgpu_renderer_ctrl_payload scanout_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_SET_SCANOUT_BLOB},
        .cmd.set_scanout_blob =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_SET_SCANOUT_BLOB},
                .r = {.x = 4, .y = 8, .width = 160, .height = 120},
                .scanout_id = 0,
                .resource_id = 92,
                .width = 320,
                .height = 240,
                .format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                .strides = {1280, 0, 0, 0},
                .offsets = {16, 0, 0, 0},
            },
        .resource_generation = 0x2222,
        .scanout_generation = 0x3333,
        .scanout =
            {
                .enabled = 1,
                .width = 1024,
                .height = 768,
            },
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    request.command_type = VIRTIO_GPU_CMD_SET_SCANOUT_BLOB;
    request.payload = &scanout_payload;
    request.payload_size = sizeof(scanout_payload);

    vgpu_virgl_execute_renderer_request(&request);

    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(completion.virgl_resource.type ==
          VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT);
    CHECK(completion.virgl_resource.scanout_count == 1);
    CHECK(completion.virgl_resource.scanouts[0].scanout_id == 0);
    CHECK(completion.virgl_resource.scanouts[0].scanout_generation == 0x3333);
    CHECK(completion.virgl_resource.scanouts[0].resource_generation == 0x2222);
    CHECK(completion.virgl_resource.scanouts[0].scanout.primary_resource_id ==
          92);
    CHECK(completion.virgl_resource.scanouts[0].scanout.src_x == 4);
    CHECK(completion.virgl_resource.scanouts[0].scanout.src_y == 8);
    CHECK(completion.virgl_resource.scanouts[0].scanout.src_w == 160);
    CHECK(completion.virgl_resource.scanouts[0].scanout.src_h == 120);
    CHECK(completion.virgl_resource.scanouts[0].has_gl_payload);
    CHECK(completion.virgl_resource.scanouts[0].gl_payload.texture_id ==
          0x4567);
    CHECK(completion.virgl_resource.scanouts[0].gl_payload.width == 320);
    CHECK(completion.virgl_resource.scanouts[0].gl_payload.height == 240);
    CHECK(completion.virgl_resource.scanouts[0].gl_payload.src_x == 4);
    CHECK(completion.virgl_resource.scanouts[0].gl_payload.src_y == 8);
    CHECK(completion.virgl_resource.scanouts[0].gl_payload.src_width == 160);
    CHECK(completion.virgl_resource.scanouts[0].gl_payload.src_height == 120);
    CHECK(!completion.virgl_resource.scanouts[0].gl_payload.y_0_top);
    CHECK(fake_resource_get_info_count == 1);
    CHECK(fake_last_resource_get_info_handle == 92);
    CHECK(!vgpu_renderer_pop_completion(&completion));
}

static void test_ctrl_request_executes_resource_unref_completion(void)
{
    reset_test_state(36);

    struct vgpu_renderer_ctrl_payload create_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
        .cmd.resource_create_3d =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
                .resource_id = 90,
                .target = 2,
                .format = 3,
                .bind = 4,
                .width = 64,
                .height = 64,
                .depth = 1,
                .array_size = 1,
                .nr_samples = 1,
            },
        .resource_generation = 0x1111,
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
        .ctrl_completion =
            {
                .queue_index = VIRTIO_GPU_CONTROLQ,
                .desc_head = 17,
                .actor_generation = 27,
                .common_generation = 36,
                .trigger_irq = true,
            },
        .response_desc =
            {
                .addr = 0xe0,
                .len = sizeof(struct virtio_gpu_ctrl_hdr),
                .flags = VIRTIO_DESC_F_WRITE,
            },
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 36},
        .command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D,
        .payload = &create_payload,
        .payload_size = sizeof(create_payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(fake_resource_create_count == 1);
    CHECK(fake_resource_unref_count == 0);

    struct vgpu_renderer_ctrl_payload unref_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_UNREF},
        .cmd.resource_unref =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_UNREF},
                .resource_id = 90,
            },
        .resource_generation = 0x2222,
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
        .ctrl_completion =
            {
                .queue_index = VIRTIO_GPU_CONTROLQ,
                .desc_head = 18,
                .actor_generation = 28,
                .common_generation = 36,
                .trigger_irq = true,
            },
        .response_desc =
            {
                .addr = 0xf0,
                .len = sizeof(struct virtio_gpu_ctrl_hdr),
                .flags = VIRTIO_DESC_F_WRITE,
            },
    };
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    request.payload = &unref_payload;

    vgpu_virgl_execute_renderer_request(&request);

    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(completion.virgl_resource.type ==
          VGPU_VIRGL_RESOURCE_SIDE_EFFECT_UNREF);
    CHECK(completion.virgl_resource.resource_id == 90);
    CHECK(completion.virgl_resource.resource_generation == 0x2222);
    CHECK(fake_resource_unref_count == 1);
    CHECK(fake_last_resource_unref_handle == 90);
}

static void test_ctrl_request_resource_unref_missing_resource_rolls_back(void)
{
    reset_test_state(37);

    struct vgpu_renderer_ctrl_payload payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_UNREF},
        .cmd.resource_unref =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_UNREF},
                .resource_id = 91,
            },
        .resource_generation = 0x3333,
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
        .ctrl_completion =
            {
                .queue_index = VIRTIO_GPU_CONTROLQ,
                .desc_head = 19,
                .actor_generation = 29,
                .common_generation = 37,
                .trigger_irq = true,
            },
        .response_desc =
            {
                .addr = 0x100,
                .len = sizeof(struct virtio_gpu_ctrl_hdr),
                .flags = VIRTIO_DESC_F_WRITE,
            },
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 37},
        .command_type = VIRTIO_GPU_CMD_RESOURCE_UNREF,
        .payload = &payload,
        .payload_size = sizeof(payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    CHECK(completion.virgl_resource.type ==
          VGPU_VIRGL_RESOURCE_SIDE_EFFECT_UNREF_ROLLBACK);
    CHECK(completion.virgl_resource.resource_id == 91);
    CHECK(completion.virgl_resource.resource_generation == 0x3333);
    CHECK(fake_resource_unref_count == 0);
}

static void test_ctrl_request_executes_resource_backing_lifecycle(void)
{
    reset_test_state(38);

    struct vgpu_renderer_ctrl_payload create_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
        .cmd.resource_create_3d =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
                .resource_id = 92,
                .target = 2,
                .format = 3,
                .bind = 4,
                .width = 64,
                .height = 64,
                .depth = 1,
                .array_size = 1,
                .nr_samples = 1,
            },
        .resource_generation = 0x4444,
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 38},
        .command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D,
        .payload = &create_payload,
        .payload_size = sizeof(create_payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(fake_resource_create_count == 1);

    uint8_t backing_a[16] = {0};
    uint8_t backing_b[32] = {0};
    struct iovec *iov = malloc(2 * sizeof(*iov));
    CHECK(iov != NULL);
    iov[0] =
        (struct iovec) {.iov_base = backing_a, .iov_len = sizeof(backing_a)};
    iov[1] =
        (struct iovec) {.iov_base = backing_b, .iov_len = sizeof(backing_b)};
    struct vgpu_renderer_ctrl_payload attach_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING},
        .cmd.resource_attach_backing =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING},
                .resource_id = 92,
                .nr_entries = 2,
            },
        .iov = iov,
        .iov_count = 2,
        .resource_generation = 0x5555,
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    request.payload = &attach_payload;
    request.payload_size = sizeof(attach_payload);

    vgpu_virgl_execute_renderer_request(&request);

    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(completion.virgl_resource.type ==
          VGPU_VIRGL_RESOURCE_SIDE_EFFECT_ATTACH_BACKING);
    CHECK(completion.virgl_resource.resource_id == 92);
    CHECK(completion.virgl_resource.resource_generation == 0x5555);
    CHECK(completion.virgl_resource.backing_transition_success);
    CHECK(fake_resource_attach_iov_count == 1);
    CHECK(fake_last_resource_attach_iov_handle == 92);
    CHECK(fake_last_resource_attach_iov == iov);
    CHECK(fake_last_resource_attach_iov_count == 2);
    CHECK(attach_payload.iov == NULL);
    CHECK(attach_payload.iov_count == 0);

    attach_payload.resource_generation = 0x5556;
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_UNSPEC);
    CHECK(completion.virgl_resource.type ==
          VGPU_VIRGL_RESOURCE_SIDE_EFFECT_ATTACH_BACKING);
    CHECK(!completion.virgl_resource.backing_transition_success);
    CHECK(fake_resource_attach_iov_count == 1);

    struct vgpu_renderer_ctrl_payload detach_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING},
        .cmd.resource_detach_backing =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING},
                .resource_id = 92,
            },
        .resource_generation = 0x6666,
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    request.payload = &detach_payload;
    request.payload_size = sizeof(detach_payload);

    vgpu_virgl_execute_renderer_request(&request);

    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(completion.virgl_resource.type ==
          VGPU_VIRGL_RESOURCE_SIDE_EFFECT_DETACH_BACKING);
    CHECK(completion.virgl_resource.resource_id == 92);
    CHECK(completion.virgl_resource.resource_generation == 0x6666);
    CHECK(completion.virgl_resource.backing_transition_success);
    CHECK(fake_resource_detach_iov_count == 1);
    CHECK(fake_last_resource_detach_iov_handle == 92);
    CHECK(fake_last_resource_detach_iov_count == 2);
    CHECK(fake_last_detached_iov == iov);
    CHECK(fake_attached_iov == NULL);

    detach_payload.resource_generation = 0x6667;
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_UNSPEC);
    CHECK(completion.virgl_resource.type ==
          VGPU_VIRGL_RESOURCE_SIDE_EFFECT_DETACH_BACKING);
    CHECK(!completion.virgl_resource.backing_transition_success);
    CHECK(fake_resource_detach_iov_count == 1);
}

static void test_ctrl_request_attach_backing_failure_leaves_renderer_detached(
    void)
{
    reset_test_state(39);

    struct vgpu_renderer_ctrl_payload create_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
        .cmd.resource_create_3d =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
                .resource_id = 93,
                .target = 2,
                .format = 3,
                .bind = 4,
                .width = 64,
                .height = 64,
                .depth = 1,
                .array_size = 1,
                .nr_samples = 1,
            },
        .resource_generation = 0x7777,
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 39},
        .command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D,
        .payload = &create_payload,
        .payload_size = sizeof(create_payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);

    uint8_t backing[16] = {0};
    struct iovec *iov = malloc(sizeof(*iov));
    CHECK(iov != NULL);
    *iov = (struct iovec) {.iov_base = backing, .iov_len = sizeof(backing)};
    struct vgpu_renderer_ctrl_payload attach_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING},
        .cmd.resource_attach_backing =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING},
                .resource_id = 93,
                .nr_entries = 1,
            },
        .iov = iov,
        .iov_count = 1,
        .resource_generation = 0x8888,
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    request.payload = &attach_payload;
    request.payload_size = sizeof(attach_payload);

    fake_resource_attach_iov_result = -1;
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_UNSPEC);
    CHECK(completion.virgl_resource.type ==
          VGPU_VIRGL_RESOURCE_SIDE_EFFECT_ATTACH_BACKING);
    CHECK(!completion.virgl_resource.backing_transition_success);
    CHECK(fake_resource_attach_iov_count == 1);
    CHECK(attach_payload.iov == iov);

    fake_resource_attach_iov_result = 0;
    attach_payload.resource_generation = 0x8889;
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(completion.virgl_resource.backing_transition_success);
    CHECK(fake_resource_attach_iov_count == 2);
    CHECK(attach_payload.iov == NULL);

    struct vgpu_renderer_ctrl_payload detach_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING},
        .cmd.resource_detach_backing =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING},
                .resource_id = 93,
            },
        .resource_generation = 0x8890,
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    request.payload = &detach_payload;
    request.payload_size = sizeof(detach_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(fake_last_detached_iov == iov);
}

static void test_reset_detaches_attached_resource_iov(void)
{
    reset_test_state(40);

    struct vgpu_renderer_ctrl_payload create_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
        .cmd.resource_create_3d =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
                .resource_id = 94,
                .target = 2,
                .format = 3,
                .bind = 4,
                .width = 64,
                .height = 64,
                .depth = 1,
                .array_size = 1,
                .nr_samples = 1,
            },
        .resource_generation = 0x9990,
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 40},
        .command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D,
        .payload = &create_payload,
        .payload_size = sizeof(create_payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);

    uint8_t backing[16] = {0};
    struct iovec *iov = malloc(sizeof(*iov));
    CHECK(iov != NULL);
    *iov = (struct iovec) {.iov_base = backing, .iov_len = sizeof(backing)};
    struct vgpu_renderer_ctrl_payload attach_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING},
        .cmd.resource_attach_backing =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING},
                .resource_id = 94,
                .nr_entries = 1,
            },
        .iov = iov,
        .iov_count = 1,
        .resource_generation = 0x9991,
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    request.payload = &attach_payload;
    request.payload_size = sizeof(attach_payload);

    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(fake_attached_iov == iov);

    vgpu_virgl_reset_renderer();
    CHECK(fake_resource_detach_iov_count == 1);
    CHECK(fake_last_resource_detach_iov_handle == 94);
    CHECK(fake_last_detached_iov == iov);
    CHECK(fake_attached_iov == NULL);
    CHECK(fake_reset_count == 1);

    struct vgpu_renderer_ctrl_payload unref_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_UNREF},
        .cmd.resource_unref =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_UNREF},
                .resource_id = 94,
            },
        .resource_generation = 0x9992,
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    request.payload = &unref_payload;
    request.payload_size = sizeof(unref_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
}


static void test_ctrl_request_executes_transfer_3d_completion(void)
{
    reset_test_state(42);

    struct vgpu_renderer_ctrl_payload create_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
        .cmd.resource_create_3d =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D},
                .resource_id = 95,
                .target = 2,
                .format = 3,
                .bind = 4,
                .width = 64,
                .height = 64,
                .depth = 1,
                .array_size = 1,
                .nr_samples = 1,
            },
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 42},
        .command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D,
        .payload = &create_payload,
        .payload_size = sizeof(create_payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);

    uint8_t backing[16] = {0};
    struct iovec *iov = malloc(sizeof(*iov));
    CHECK(iov != NULL);
    *iov = (struct iovec) {.iov_base = backing, .iov_len = sizeof(backing)};
    struct vgpu_renderer_ctrl_payload attach_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING},
        .cmd.resource_attach_backing =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING},
                .resource_id = 95,
                .nr_entries = 1,
            },
        .iov = iov,
        .iov_count = 1,
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    request.payload = &attach_payload;
    request.payload_size = sizeof(attach_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);

    struct vgpu_renderer_ctrl_payload transfer_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D, .ctx_id = 33},
        .cmd.transfer_3d =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D,
                        .ctx_id = 33},
                .box = {.x = 1, .y = 2, .z = 3, .w = 4, .h = 5, .d = 6},
                .offset = UINT64_C(0x123456789),
                .resource_id = 95,
                .level = 2,
                .stride = 64,
                .layer_stride = 128,
            },
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
    };
    request.command_type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    request.payload = &transfer_payload;
    request.payload_size = sizeof(transfer_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(completion.virgl_resource.type ==
          VGPU_VIRGL_RESOURCE_SIDE_EFFECT_NONE);
    CHECK(fake_transfer_write_iov_count == 1);
    CHECK(fake_last_transfer_write_handle == 95);
    CHECK(fake_last_transfer_write_ctx_id == 33);
    CHECK(fake_last_transfer_write_level == 2);
    CHECK(fake_last_transfer_write_stride == 64);
    CHECK(fake_last_transfer_write_layer_stride == 128);
    CHECK(fake_last_transfer_write_box.x == 1);
    CHECK(fake_last_transfer_write_box.y == 2);
    CHECK(fake_last_transfer_write_box.z == 3);
    CHECK(fake_last_transfer_write_box.w == 4);
    CHECK(fake_last_transfer_write_box.h == 5);
    CHECK(fake_last_transfer_write_box.d == 6);
    CHECK(fake_last_transfer_write_offset == UINT64_C(0x123456789));
    CHECK(fake_last_transfer_write_iov == NULL);
    CHECK(fake_last_transfer_write_iov_count == 0);

    fake_transfer_read_iov_result = -1;
    transfer_payload.hdr.type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    transfer_payload.hdr.ctx_id = 34;
    transfer_payload.cmd.transfer_3d.hdr.type =
        VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    transfer_payload.cmd.transfer_3d.hdr.ctx_id = 34;
    transfer_payload.cmd.transfer_3d.level = 3;
    request.command_type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_UNSPEC);
    CHECK(completion.virgl_resource.type ==
          VGPU_VIRGL_RESOURCE_SIDE_EFFECT_NONE);
    CHECK(fake_transfer_read_iov_count == 1);
    CHECK(fake_last_transfer_read_handle == 95);
    CHECK(fake_last_transfer_read_ctx_id == 34);
    CHECK(fake_last_transfer_read_level == 3);
    CHECK(fake_last_transfer_read_stride == 64);
    CHECK(fake_last_transfer_read_layer_stride == 128);
    CHECK(fake_last_transfer_read_box.x == 1);
    CHECK(fake_last_transfer_read_box.y == 2);
    CHECK(fake_last_transfer_read_box.z == 3);
    CHECK(fake_last_transfer_read_box.w == 4);
    CHECK(fake_last_transfer_read_box.h == 5);
    CHECK(fake_last_transfer_read_box.d == 6);
    CHECK(fake_last_transfer_read_offset == UINT64_C(0x123456789));
    CHECK(fake_last_transfer_read_iov == NULL);
    CHECK(fake_last_transfer_read_iov_count == 0);

    fake_transfer_read_iov_result = 0;
    transfer_payload.cmd.transfer_3d.resource_id = 12345;
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    CHECK(fake_transfer_read_iov_count == 1);

    create_payload.cmd.resource_create_3d.resource_id = 96;
    request.command_type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    request.payload = &create_payload;
    request.payload_size = sizeof(create_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);

    transfer_payload.cmd.transfer_3d.resource_id = 96;
    request.command_type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    request.payload = &transfer_payload;
    request.payload_size = sizeof(transfer_payload);
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_UNSPEC);
    CHECK(fake_transfer_read_iov_count == 1);

    transfer_payload.cmd.transfer_3d.resource_id = 95;
    transfer_payload.cmd.transfer_3d.level = UINT32_MAX;
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    CHECK(fake_transfer_read_iov_count == 1);

    vgpu_virgl_reset_renderer();
}

static void test_ctrl_request_executes_submit_3d_completion(void)
{
    reset_test_state(41);
    init_renderer_for_test();

    uint32_t command_stream[3] = {0x01020304, 0x11121314, 0x21222324};
    struct vgpu_renderer_ctrl_payload submit_payload = {
        .hdr = {.type = VIRTIO_GPU_CMD_SUBMIT_3D, .ctx_id = 77},
        .cmd.submit_3d =
            {
                .hdr = {.type = VIRTIO_GPU_CMD_SUBMIT_3D, .ctx_id = 77},
                .size = sizeof(command_stream),
            },
        .submit_data = command_stream,
        .submit_data_size = sizeof(command_stream),
        .response_capacity = sizeof(struct virtio_gpu_ctrl_hdr),
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
        .ctrl_completion =
            {
                .queue_index = VIRTIO_GPU_CONTROLQ,
                .desc_head = 15,
                .actor_generation = 25,
                .common_generation = 41,
                .trigger_irq = true,
            },
        .response_desc =
            {
                .addr = 0xb0,
                .len = sizeof(struct virtio_gpu_ctrl_hdr),
                .flags = VIRTIO_DESC_F_WRITE,
            },
    };
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = 41},
        .command_type = VIRTIO_GPU_CMD_SUBMIT_3D,
        .payload = &submit_payload,
        .payload_size = sizeof(submit_payload),
    };

    vgpu_virgl_execute_renderer_request(&request);

    struct vgpu_renderer_completion completion = {0};
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(fake_submit_cmd_count == 1);
    CHECK(fake_last_submit_cmd_ctx_id == 77);
    CHECK(fake_last_submit_cmd_ndw == 3);
    CHECK(fake_last_submit_cmd_words[0] == 0x01020304);
    CHECK(fake_last_submit_cmd_words[1] == 0x11121314);
    CHECK(fake_last_submit_cmd_words[2] == 0x21222324);
    CHECK(fake_force_ctx0_count == 1);

    fake_submit_cmd_result = -1;
    command_stream[0] = 0xaabbccdd;
    submit_payload.hdr.ctx_id = 78;
    submit_payload.cmd.submit_3d.hdr.ctx_id = 78;
    submit_payload.cmd.submit_3d.size = sizeof(uint32_t);
    submit_payload.submit_data_size = sizeof(uint32_t);

    vgpu_virgl_execute_renderer_request(&request);

    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_UNSPEC);
    CHECK(fake_submit_cmd_count == 2);
    CHECK(fake_last_submit_cmd_ctx_id == 78);
    CHECK(fake_last_submit_cmd_ndw == 1);
    CHECK(fake_last_submit_cmd_words[0] == 0xaabbccdd);

    fake_submit_cmd_result = 0;
    submit_payload.cmd.submit_3d.num_in_fences = 1;
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    CHECK(fake_submit_cmd_count == 2);

    submit_payload.cmd.submit_3d.num_in_fences = 0;
    submit_payload.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    submit_payload.hdr.fence_id = UINT64_C(0xf00d00000000abcd);
    submit_payload.hdr.ctx_id = 0;
    submit_payload.cmd.submit_3d.hdr = submit_payload.hdr;
    int force_ctx0_before_fence = fake_force_ctx0_count;
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(!vgpu_renderer_pop_completion(&completion));
    CHECK(fake_submit_cmd_count == 3);
    CHECK(fake_create_fence_count == 1);
    CHECK(fake_last_create_fence_ctx_id == 0);
    CHECK(fake_force_ctx0_count == force_ctx0_before_fence + 2);
    CHECK(virgl_stats().pending_fences == 1);

    fake_callbacks.write_fence(fake_init_cookie, fake_last_create_fence_id);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.type == VGPU_RENDERER_DONE_FENCE);
    CHECK(completion.token.generation == 41);
    CHECK(!completion.context_fence);
    CHECK(completion.fence_id == UINT64_C(0xf00d00000000abcd));
    CHECK(completion.has_ctrl_completion);
    CHECK(completion.ctrl_completion.desc_head == 15);
    CHECK(completion.has_response_desc);
    CHECK(completion.response_type == VIRTIO_GPU_RESP_OK_NODATA);
    CHECK(completion.request_hdr.flags == VIRTIO_GPU_FLAG_FENCE);
    CHECK(completion.request_hdr.fence_id == UINT64_C(0xf00d00000000abcd));
    CHECK(!vgpu_renderer_pop_completion(&completion));

    submit_payload.hdr.flags =
        VIRTIO_GPU_FLAG_FENCE | VIRTIO_GPU_FLAG_INFO_RING_IDX;
    submit_payload.hdr.fence_id = UINT64_C(0xabcddcba11223344);
    submit_payload.hdr.ctx_id = 0;
    submit_payload.hdr.ring_idx = 6;
    submit_payload.cmd.submit_3d.hdr = submit_payload.hdr;
    submit_payload.ctrl_completion.desc_head = 16;
    submit_payload.response_desc.addr = 0xc0;
    force_ctx0_before_fence = fake_force_ctx0_count;
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(!vgpu_renderer_pop_completion(&completion));
    CHECK(fake_submit_cmd_count == 4);
    CHECK(fake_context_create_fence_count == 1);
    CHECK(fake_last_context_fence_ctx_id == 0);
    CHECK(fake_last_context_fence_flags == VIRGL_RENDERER_FENCE_FLAG_MERGEABLE);
    CHECK(fake_last_context_fence_ring_idx == 6);
    CHECK(fake_force_ctx0_count == force_ctx0_before_fence + 1);
    CHECK(virgl_stats().pending_fences == 1);

    fake_callbacks.write_context_fence(fake_init_cookie, 0, 6,
                                       fake_last_context_fence_id);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.type == VGPU_RENDERER_DONE_FENCE);
    CHECK(completion.token.generation == 41);
    CHECK(completion.context_fence);
    CHECK(completion.ctx_id == 0);
    CHECK(completion.ring_idx == 6);
    CHECK(completion.fence_id == UINT64_C(0xabcddcba11223344));
    CHECK(completion.has_ctrl_completion);
    CHECK(completion.ctrl_completion.desc_head == 16);
    CHECK(completion.response_desc.addr == 0xc0);
    CHECK(completion.request_hdr.flags ==
          (VIRTIO_GPU_FLAG_FENCE | VIRTIO_GPU_FLAG_INFO_RING_IDX));
    CHECK(!vgpu_renderer_pop_completion(&completion));

    fake_submit_cmd_result = -1;
    submit_payload.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    submit_payload.hdr.fence_id = UINT64_C(0x4444555566667777);
    submit_payload.hdr.ctx_id = 0;
    submit_payload.hdr.ring_idx = 0;
    submit_payload.cmd.submit_3d.hdr = submit_payload.hdr;
    submit_payload.ctrl_completion.desc_head = 17;
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_UNSPEC);
    CHECK(fake_submit_cmd_count == 5);
    CHECK(fake_create_fence_count == 1);
    CHECK(virgl_stats().pending_fences == 0);

    fake_submit_cmd_result = 0;
    fake_create_fence_result = -1;
    submit_payload.ctrl_completion.desc_head = 18;
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_UNSPEC);
    CHECK(completion.request_hdr.fence_id == UINT64_C(0x4444555566667777));
    CHECK(fake_submit_cmd_count == 6);
    CHECK(fake_create_fence_count == 2);
    CHECK(virgl_stats().pending_fences == 0);

    fake_create_fence_result = 0;
    fake_context_create_fence_result = -1;
    submit_payload.hdr.flags =
        VIRTIO_GPU_FLAG_FENCE | VIRTIO_GPU_FLAG_INFO_RING_IDX;
    submit_payload.hdr.fence_id = UINT64_C(0x7777666655554444);
    submit_payload.hdr.ctx_id = 0;
    submit_payload.hdr.ring_idx = 7;
    submit_payload.cmd.submit_3d.hdr = submit_payload.hdr;
    submit_payload.ctrl_completion.desc_head = 19;
    vgpu_virgl_execute_renderer_request(&request);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.response_type == VIRTIO_GPU_RESP_ERR_UNSPEC);
    CHECK(completion.request_hdr.fence_id == UINT64_C(0x7777666655554444));
    CHECK(fake_submit_cmd_count == 7);
    CHECK(fake_context_create_fence_count == 2);
    CHECK(virgl_stats().pending_fences == 0);
    fake_context_create_fence_result = 0;
}

static void test_callback_completes_all_matching_fences_in_order(void)
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
    CHECK(completion.fence_id == 100);
    CHECK(vgpu_renderer_pop_completion(&completion));
    CHECK(completion.type == VGPU_RENDERER_DONE_FENCE);
    CHECK(completion.token.generation == 19);
    CHECK(completion.fence_id == 200);
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

static void test_reset_request_republishes_capset_count(void)
{
    reset_test_state(234);
    fake_virgl2_supported = true;

    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_RESET,
        .payload = (void *) (uintptr_t) 0x1234,
    };

    vgpu_virgl_execute_renderer_request(&request);

    CHECK(fake_reset_count == 1);
    CHECK(fake_set_num_capsets_count == 1);
    CHECK(fake_last_num_capsets == 2);
}

int main(void)
{
    test_init_renderer_uses_thread_sync_and_callbacks();
    test_init_renderer_installs_gl_context_callbacks();
    test_create_gl_context_callback_delegates_to_window_hook();
    test_destroy_gl_context_callback_delegates_to_window_hook();
    test_make_current_callback_delegates_to_window_hook();
    test_ctx0_fence_completion_preserves_guest_id_and_generation();
    test_context_fence_completion_preserves_stream_metadata();
    test_poll_requests_coalesce_until_executed();
    test_ctrl_request_executes_get_capset_info_completion();
    test_ctrl_request_executes_get_capset_completion();
    test_ctrl_request_exposes_supported_virgl2_capset();
    test_ctrl_request_executes_context_create_destroy();
    test_ctrl_request_executes_virgl2_context_init();
    test_ctrl_request_executes_context_resource_attach_detach();
    test_ctrl_request_executes_resource_create_3d_completion();
    test_ctrl_request_executes_resource_create_blob_completion();
    test_ctrl_request_maps_and_unmaps_blob_resources();
    test_blob_hostmem_aperture_bounds_and_access();
    test_ctrl_request_resource_create_3d_failure_rolls_back_frontend();
    test_ctrl_request_records_set_scanout_gl_payload_completion();
    test_ctrl_request_records_set_scanout_blob_gl_payload_completion();
    test_ctrl_request_executes_resource_unref_completion();
    test_ctrl_request_resource_unref_missing_resource_rolls_back();
    test_ctrl_request_executes_resource_backing_lifecycle();
    test_ctrl_request_attach_backing_failure_leaves_renderer_detached();
    test_reset_detaches_attached_resource_iov();
    test_ctrl_request_executes_transfer_3d_completion();
    test_ctrl_request_executes_submit_3d_completion();
    test_callback_completes_all_matching_fences_in_order();
    test_reset_drops_pending_fences_and_ignores_stale_callbacks();
    test_reset_request_republishes_capset_count();
    return 0;
}
