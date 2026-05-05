#pragma once

#include <stdint.h>
#include <sys/uio.h>

#define VIRGL_RENDERER_CALLBACKS_VERSION 4
#define VIRGL_RENDERER_THREAD_SYNC 2
#define VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK 0xff

struct virgl_box {
    uint32_t x, y, z;
    uint32_t w, h, d;
};

struct virgl_renderer_callbacks {
    int version;
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
