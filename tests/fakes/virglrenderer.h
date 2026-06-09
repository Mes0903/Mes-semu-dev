#pragma once

#include <stdint.h>

#define VIRGL_RENDERER_CALLBACKS_VERSION 4
#define VIRGL_RENDERER_THREAD_SYNC 2
#define VIRGL_RENDERER_FENCE_FLAG_MERGEABLE (1 << 0)

struct virgl_renderer_callbacks {
    int version;
    void (*write_fence)(void *cookie, uint32_t fence);
    void *create_gl_context;
    void *destroy_gl_context;
    void *make_current;
    void (*write_context_fence)(void *cookie,
                                uint32_t ctx_id,
                                uint32_t ring_idx,
                                uint64_t fence_id);
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
void virgl_renderer_reset(void);
