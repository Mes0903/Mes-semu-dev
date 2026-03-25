#include <stdio.h>
#include <stdlib.h>

#include "device.h"
#include "vgpu-display.h"

/* PRIMARY_SET/CURSOR_SET own CPU-frame snapshots, so each queued command can
 * retain significantly more memory than an input event. Keep this backlog
 * deliberately small: display updates are lossy and quickly become stale, and
 * the emulator thread must be able to drop them rather than accumulate a
 * large queue of old frames.
 */
#define VGPU_DISPLAY_CMD_QUEUE_SIZE 64U
#define VGPU_DISPLAY_CMD_QUEUE_MASK (VGPU_DISPLAY_CMD_QUEUE_SIZE - 1U)

/* The GPU backend is the only producer and the window backend is the only
 * consumer. Keeping this queue SPSC preserves single-owner device state while
 * leaving room for upstream emulation to funnel into one display path.
 */
static struct vgpu_display_cmd
    vgpu_display_cmd_queue[VGPU_DISPLAY_CMD_QUEUE_SIZE];
static uint32_t vgpu_display_cmd_head;
static uint32_t vgpu_display_cmd_tail;
static bool vgpu_display_unavailable;

void vgpu_display_cmd_dispose(struct vgpu_display_cmd *cmd)
{
    switch (cmd->type) {
    case VGPU_DISPLAY_CMD_PRIMARY_SET:
        free(cmd->u.primary_set.payload);
        break;
    case VGPU_DISPLAY_CMD_CURSOR_SET:
        free(cmd->u.cursor_set.payload);
        break;
    default:
        break;
    }
}

static bool vgpu_display_cmd_queue_full(void)
{
    uint32_t head = __atomic_load_n(&vgpu_display_cmd_head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&vgpu_display_cmd_tail, __ATOMIC_ACQUIRE);
    uint32_t next = (head + 1U) & VGPU_DISPLAY_CMD_QUEUE_MASK;

    return next == tail;
}

static void vgpu_display_cmd_push(struct vgpu_display_cmd *cmd)
{
    uint32_t head = __atomic_load_n(&vgpu_display_cmd_head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&vgpu_display_cmd_tail, __ATOMIC_ACQUIRE);
    uint32_t next = (head + 1U) & VGPU_DISPLAY_CMD_QUEUE_MASK;

    /* Keep the producer non-blocking. If the window backend falls behind,
     * prefer dropping the newest display update over stalling guest/device
     * execution on the emulator thread.
     */
    if (next == tail) {
        vgpu_display_cmd_dispose(cmd);
        return;
    }

    vgpu_display_cmd_queue[head] = *cmd;
    __atomic_store_n(&vgpu_display_cmd_head, next, __ATOMIC_RELEASE);
}

bool vgpu_display_cmd_pop(struct vgpu_display_cmd *cmd)
{
    uint32_t tail = __atomic_load_n(&vgpu_display_cmd_tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&vgpu_display_cmd_head, __ATOMIC_ACQUIRE);

    if (tail == head)
        return false;

    *cmd = vgpu_display_cmd_queue[tail];
    tail = (tail + 1U) & VGPU_DISPLAY_CMD_QUEUE_MASK;
    __atomic_store_n(&vgpu_display_cmd_tail, tail, __ATOMIC_RELEASE);
    return true;
}

void vgpu_display_set_unavailable(void)
{
    struct vgpu_display_cmd cmd;

    /* Stop accepting new display updates, then dispose anything still queued
     * because the window backend will no longer drain and render it. In the
     * current tree this latch is set only when window-sw initialization fails
     * and semu falls back to headless mode.
     */
    vgpu_display_unavailable = true;
    while (vgpu_display_cmd_pop(&cmd))
        vgpu_display_cmd_dispose(&cmd);
}

bool vgpu_display_can_publish(void)
{
    return !vgpu_display_unavailable && !vgpu_display_cmd_queue_full();
}

void vgpu_display_publish_primary_set(uint32_t scanout_id,
                                      struct vgpu_display_payload *payload)
{
    if (vgpu_display_unavailable) {
        free(payload);
        return;
    }

    struct vgpu_display_cmd cmd = {
        .type = VGPU_DISPLAY_CMD_PRIMARY_SET,
        .scanout_id = scanout_id,
        .u.primary_set = {.payload = payload},
    };
    vgpu_display_cmd_push(&cmd);
}

void vgpu_display_publish_primary_clear(uint32_t scanout_id)
{
    if (vgpu_display_unavailable)
        return;

    struct vgpu_display_cmd cmd = {
        .type = VGPU_DISPLAY_CMD_PRIMARY_CLEAR,
        .scanout_id = scanout_id,
    };
    vgpu_display_cmd_push(&cmd);
}

void vgpu_display_publish_cursor_set(uint32_t scanout_id,
                                     struct vgpu_display_payload *payload,
                                     int x,
                                     int y,
                                     int hot_x,
                                     int hot_y)
{
    if (vgpu_display_unavailable) {
        free(payload);
        return;
    }

    struct vgpu_display_cmd cmd = {
        .type = VGPU_DISPLAY_CMD_CURSOR_SET,
        .scanout_id = scanout_id,
        .u.cursor_set = {.payload = payload,
                         .x = x,
                         .y = y,
                         .hot_x = hot_x,
                         .hot_y = hot_y},
    };
    vgpu_display_cmd_push(&cmd);
}

void vgpu_display_publish_cursor_clear(uint32_t scanout_id)
{
    if (vgpu_display_unavailable)
        return;

    struct vgpu_display_cmd cmd = {
        .type = VGPU_DISPLAY_CMD_CURSOR_CLEAR,
        .scanout_id = scanout_id,
    };
    vgpu_display_cmd_push(&cmd);
}

void vgpu_display_publish_cursor_move(uint32_t scanout_id, int x, int y)
{
    if (vgpu_display_unavailable)
        return;

    struct vgpu_display_cmd cmd = {
        .type = VGPU_DISPLAY_CMD_CURSOR_MOVE,
        .scanout_id = scanout_id,
        .u.cursor_move = {.x = x, .y = y},
    };
    vgpu_display_cmd_push(&cmd);
}
