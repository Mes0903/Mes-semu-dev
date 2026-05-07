#pragma once

#if !SEMU_HAS(VIRTIOGPU)
#error Only valid when Virtio-GPU is enabled.
#endif

#include <stdbool.h>
#include <stdint.h>

#include "virtio-gpu.h"

/* Immutable CPU-frame payload published by the VirtIO GPU backend and later
 * consumed by the window backend when it uploads pixels into its own textures.
 */
struct vgpu_display_cpu_payload {
    enum virtio_gpu_formats format;
    uint32_t width, height;
    uint32_t stride;
    uint32_t bits_per_pixel;
    uint8_t *pixels;
};

struct vgpu_display_gl_scanout_payload {
    uint32_t texture_id;
    uint32_t width, height;
    uint32_t src_x, src_y, src_w, src_h;
    bool y_0_top;
};

enum vgpu_display_payload_kind {
    VGPU_DISPLAY_PAYLOAD_CPU = 0,
    VGPU_DISPLAY_PAYLOAD_GL_SCANOUT,
};

/* Owning payload object passed through the display queue. The bridge queues
 * and disposes this object, while GPU and window backends only fill or
 * consume the payload it carries.
 */
struct vgpu_display_payload {
    enum vgpu_display_payload_kind kind;
    union {
        struct vgpu_display_cpu_payload cpu;
        struct vgpu_display_gl_scanout_payload gl;
    };
};

/* Runtime display commands published by the GPU backend and consumed by the
 * window backend. 'PRIMARY_*' updates the main scanout image, while 'CURSOR_*'
 * updates or moves the separate cursor plane.
 *
 * Clear commands are reliable generation changes, frame/move commands are lossy
 * SPSC queue entries.
 */
enum vgpu_display_cmd_type {
    VGPU_DISPLAY_CMD_PRIMARY_SET = 0,
    VGPU_DISPLAY_CMD_PRIMARY_CLEAR,
    VGPU_DISPLAY_CMD_CURSOR_SET,
    VGPU_DISPLAY_CMD_CURSOR_CLEAR,
    VGPU_DISPLAY_CMD_CURSOR_MOVE,
};

/* One synthesized display bridge command. 'scanout_id' selects which scanout
 * to update, and the union carries the payload or coordinates required by the
 * specific command type above.
 */
struct vgpu_display_cmd {
    enum vgpu_display_cmd_type type;
    uint32_t scanout_id;
    uint32_t generation;
    const uint32_t *guard_generation;
    uint32_t guard_expected;
    union {
        struct {
            struct vgpu_display_payload *payload;
        } primary_set;
        struct {
            struct vgpu_display_payload *payload;
            int32_t x;
            int32_t y;
            uint32_t hot_x;
            uint32_t hot_y;
        } cursor_set;
        struct {
            int32_t x;
            int32_t y;
        } cursor_move;
    } u;
};

struct vgpu_display_debug_stats {
    uint32_t scanout_count;
    uint32_t queue_head;
    uint32_t queue_tail;
    uint32_t queue_depth;
    bool unavailable;
    uint64_t primary_clears;
    uint64_t cursor_clears;
    uint64_t primary_sets_published;
    uint64_t cursor_sets_published;
    uint64_t cursor_moves_published;
    uint64_t cmds_queued;
    uint64_t cmds_dropped;
    uint64_t cmds_popped;
    uint64_t stale_cmds_dropped;
};

void vgpu_display_set_scanout_count(uint32_t scanout_count);
void vgpu_display_publish_primary_clear(uint32_t scanout_id);
void vgpu_display_publish_cursor_clear(uint32_t scanout_id);

void vgpu_display_release_cmd(struct vgpu_display_cmd *cmd);
bool vgpu_display_pop_cmd(struct vgpu_display_cmd *cmd);
void vgpu_display_set_unavailable(void);
bool vgpu_display_can_publish(void);
void vgpu_display_debug_snapshot(struct vgpu_display_debug_stats *stats);
bool vgpu_display_publish_primary_set_guarded(
    uint32_t scanout_id,
    struct vgpu_display_payload *payload,
    const uint32_t *guard_generation,
    uint32_t guard_expected);
void vgpu_display_publish_primary_set(uint32_t scanout_id,
                                      struct vgpu_display_payload *payload);
void vgpu_display_publish_cursor_set(uint32_t scanout_id,
                                     struct vgpu_display_payload *payload,
                                     int32_t x,
                                     int32_t y,
                                     uint32_t hot_x,
                                     uint32_t hot_y);
void vgpu_display_publish_cursor_move(uint32_t scanout_id,
                                      int32_t x,
                                      int32_t y);
