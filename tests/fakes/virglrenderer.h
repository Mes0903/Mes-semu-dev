#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/uio.h>

#define VIRGL_RENDERER_CALLBACKS_VERSION 4
#define VIRGL_RENDERER_THREAD_SYNC 2
#define VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK 0xff
#define VIRGL_RENDERER_FENCE_FLAG_MERGEABLE (1 << 0)

struct virgl_box {
    uint32_t x, y, z;
    uint32_t w, h, d;
};

typedef void *virgl_renderer_gl_context;

struct virgl_renderer_gl_ctx_param {
    int version;
    bool shared;
    int major_ver;
    int minor_ver;
    int compat_ctx;
};

struct virgl_renderer_callbacks {
    int version;
    void (*write_fence)(void *cookie, uint32_t fence);
    virgl_renderer_gl_context (*create_gl_context)(
        void *cookie,
        int scanout_idx,
        struct virgl_renderer_gl_ctx_param *param);
    void (*destroy_gl_context)(void *cookie, virgl_renderer_gl_context ctx);
    int (*make_current)(void *cookie,
                        int scanout_idx,
                        virgl_renderer_gl_context ctx);
    void (*write_context_fence)(void *cookie,
                                uint32_t ctx_id,
                                uint32_t ring_idx,
                                uint64_t fence_id);
};

struct virgl_renderer_resource_create_args {
    uint32_t handle;
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
};

struct virgl_renderer_resource_info {
    uint32_t handle;
    uint32_t virgl_format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t flags;
    uint32_t tex_id;
    uint32_t stride;
    int drm_fourcc;
    int fd;
};

int virgl_renderer_init(void *cookie,
                        int flags,
                        struct virgl_renderer_callbacks *cb);
void virgl_renderer_poll(void);
int virgl_renderer_create_fence(int client_fence_id, uint32_t ctx_id);
int virgl_renderer_context_create_fence(uint32_t ctx_id,
                                        uint32_t flags,
                                        uint32_t ring_idx,
                                        uint64_t fence_id);
void virgl_renderer_get_cap_set(uint32_t set,
                                uint32_t *max_ver,
                                uint32_t *max_size);
void virgl_renderer_fill_caps(uint32_t set, uint32_t version, void *caps);
void virgl_renderer_reset(void);
int virgl_renderer_context_create(uint32_t handle,
                                  uint32_t nlen,
                                  const char *name);
void virgl_renderer_context_destroy(uint32_t handle);
int virgl_renderer_context_create_with_flags(uint32_t ctx_id,
                                             uint32_t ctx_flags,
                                             uint32_t nlen,
                                             const char *name);
void virgl_renderer_ctx_attach_resource(int ctx_id, int res_handle);
void virgl_renderer_ctx_detach_resource(int ctx_id, int res_handle);
int virgl_renderer_resource_create(
    struct virgl_renderer_resource_create_args *args,
    struct iovec *iov,
    uint32_t num_iovs);
void virgl_renderer_resource_unref(uint32_t res_handle);
int virgl_renderer_resource_get_info(int res_handle,
                                     struct virgl_renderer_resource_info *info);
int virgl_renderer_resource_attach_iov(int res_handle,
                                       struct iovec *iov,
                                       int num_iovs);
void virgl_renderer_resource_detach_iov(int res_handle,
                                        struct iovec **iov,
                                        int *num_iovs);
int virgl_renderer_transfer_write_iov(uint32_t handle,
                                      uint32_t ctx_id,
                                      int level,
                                      uint32_t stride,
                                      uint32_t layer_stride,
                                      struct virgl_box *box,
                                      uint64_t offset,
                                      struct iovec *iovec,
                                      unsigned int iovec_cnt);
int virgl_renderer_transfer_read_iov(uint32_t handle,
                                     uint32_t ctx_id,
                                     uint32_t level,
                                     uint32_t stride,
                                     uint32_t layer_stride,
                                     struct virgl_box *box,
                                     uint64_t offset,
                                     struct iovec *iov,
                                     int iovec_cnt);
int virgl_renderer_submit_cmd(void *buffer, int ctx_id, int ndw);
void virgl_renderer_force_ctx_0(void);
