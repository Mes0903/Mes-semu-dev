#pragma once

#if !SEMU_HAS(VIRGL)
#error Only valid when VirGL is enabled.
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "virtio-gpu.h"

enum vgpu_renderer_request_type {
    VGPU_RENDERER_REQ_INIT = 0,
    VGPU_RENDERER_REQ_RESET,
    VGPU_RENDERER_REQ_POLL,
    VGPU_RENDERER_REQ_CTRL,
    VGPU_RENDERER_REQ_SHUTDOWN,
};

enum vgpu_renderer_completion_type {
    VGPU_RENDERER_DONE_CTRL = 0,
    VGPU_RENDERER_DONE_FENCE,
    VGPU_RENDERER_DONE_FATAL,
};

struct vgpu_renderer_token {
    uint32_t id;
    uint32_t generation;
};

struct vgpu_renderer_request {
    enum vgpu_renderer_request_type type;
    struct vgpu_renderer_token token;
    uint32_t command_type;
    void *payload;
    size_t payload_size;
    void (*release_payload)(void *payload);
};

struct vgpu_renderer_completion {
    enum vgpu_renderer_completion_type type;
    struct vgpu_renderer_token token;
    uint32_t response_type;
    void *response;
    size_t response_size;
    void (*release_response)(void *response);
    bool context_fence;
    uint32_t ctx_id;
    uint32_t ring_idx;
    uint64_t fence_id;
};

struct vgpu_renderer_debug_stats {
    uint32_t active_generation;
    uint32_t request_head;
    uint32_t request_tail;
    uint32_t request_depth;
    uint32_t completion_head;
    uint32_t completion_tail;
    uint32_t completion_depth;
    uint64_t requests_submitted;
    uint64_t requests_dropped;
    uint64_t requests_popped;
    uint64_t completions_submitted;
    uint64_t completions_dropped;
    uint64_t completions_popped;
    uint64_t queue_resets;
    uint64_t execute_started;
    uint64_t execute_finished;
    uint64_t current_execute_seq;
    enum vgpu_renderer_request_type current_request_type;
    uint32_t current_command_type;
    uint32_t current_token_id;
    uint32_t current_generation;
};

void vgpu_renderer_set_wake_frontend(void (*wake_frontend)(void));
void vgpu_renderer_set_wake_backend(void (*wake_backend)(void));
bool vgpu_renderer_submit(const struct vgpu_renderer_request *request);
bool vgpu_renderer_pop_request(struct vgpu_renderer_request *request);
bool vgpu_renderer_complete(const struct vgpu_renderer_completion *completion);
bool vgpu_renderer_pop_completion(struct vgpu_renderer_completion *completion);
void vgpu_renderer_reset_queues(uint32_t generation);
void vgpu_renderer_debug_note_execute_begin(
    const struct vgpu_renderer_request *request);
void vgpu_renderer_debug_note_execute_end(void);
void vgpu_renderer_debug_snapshot(struct vgpu_renderer_debug_stats *stats);
