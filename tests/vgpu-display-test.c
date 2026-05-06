#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../vgpu-display.h"
#include "../virtio-gpu.h"

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static struct vgpu_display_payload *test_payload(void)
{
    struct vgpu_display_payload *payload = calloc(1, sizeof(*payload));
    if (!payload)
        return NULL;

    payload->kind = VGPU_DISPLAY_PAYLOAD_CPU;
    payload->cpu.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    payload->cpu.width = 1;
    payload->cpu.height = 1;
    payload->cpu.stride = 4;
    payload->cpu.bits_per_pixel = 32;
    return payload;
}

static int test_cursor_moves_are_queued_in_order(void)
{
    for (int32_t i = 0; i < 3; i++)
        vgpu_display_publish_cursor_move(0, i, i + 1000);

    struct vgpu_display_cmd cmd;
    for (int32_t i = 0; i < 3; i++) {
        CHECK(vgpu_display_pop_cmd(&cmd));
        CHECK(cmd.type == VGPU_DISPLAY_CMD_CURSOR_MOVE);
        CHECK(cmd.u.cursor_move.x == i);
        CHECK(cmd.u.cursor_move.y == i + 1000);
        vgpu_display_release_cmd(&cmd);
    }

    CHECK(!vgpu_display_pop_cmd(&cmd));
    return 0;
}

static int test_cursor_set_keeps_chronological_order_after_move(void)
{
    vgpu_display_publish_cursor_move(0, 10, 20);

    struct vgpu_display_payload *payload = test_payload();
    CHECK(payload != NULL);
    vgpu_display_publish_cursor_set(0, payload, 30, 40, 1, 2);

    struct vgpu_display_cmd cmd;
    CHECK(vgpu_display_pop_cmd(&cmd));
    CHECK(cmd.type == VGPU_DISPLAY_CMD_CURSOR_MOVE);
    CHECK(cmd.u.cursor_move.x == 10);
    CHECK(cmd.u.cursor_move.y == 20);
    vgpu_display_release_cmd(&cmd);

    CHECK(vgpu_display_pop_cmd(&cmd));
    CHECK(cmd.type == VGPU_DISPLAY_CMD_CURSOR_SET);
    CHECK(cmd.u.cursor_set.x == 30);
    CHECK(cmd.u.cursor_set.y == 40);
    vgpu_display_release_cmd(&cmd);

    CHECK(!vgpu_display_pop_cmd(&cmd));
    return 0;
}

static int test_cursor_moves_before_set_keep_fifo_order(void)
{
    for (int32_t i = 0; i < 3; i++)
        vgpu_display_publish_cursor_move(0, 100 + i, 200 + i);

    struct vgpu_display_payload *payload = test_payload();
    CHECK(payload != NULL);
    vgpu_display_publish_cursor_set(0, payload, 300, 400, 5, 6);

    struct vgpu_display_cmd cmd;
    for (int32_t i = 0; i < 3; i++) {
        CHECK(vgpu_display_pop_cmd(&cmd));
        CHECK(cmd.type == VGPU_DISPLAY_CMD_CURSOR_MOVE);
        CHECK(cmd.u.cursor_move.x == 100 + i);
        CHECK(cmd.u.cursor_move.y == 200 + i);
        vgpu_display_release_cmd(&cmd);
    }

    CHECK(vgpu_display_pop_cmd(&cmd));
    CHECK(cmd.type == VGPU_DISPLAY_CMD_CURSOR_SET);
    CHECK(cmd.u.cursor_set.x == 300);
    CHECK(cmd.u.cursor_set.y == 400);
    CHECK(cmd.u.cursor_set.hot_x == 5);
    CHECK(cmd.u.cursor_set.hot_y == 6);
    vgpu_display_release_cmd(&cmd);

    CHECK(!vgpu_display_pop_cmd(&cmd));
    return 0;
}

int main(void)
{
    CHECK(test_cursor_moves_are_queued_in_order() == 0);
    CHECK(test_cursor_set_keeps_chronological_order_after_move() == 0);
    CHECK(test_cursor_moves_before_set_keep_fifo_order() == 0);
    return 0;
}
