#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "vgpu-display.h"
#include "vgpu-rect.h"
#include "vgpu-scanout.h"

static void require_true(const char *name, bool got)
{
    if (got)
        return;

    fprintf(stderr, "%s: got false, want true\n", name);
    exit(1);
}

static void require_false(const char *name, bool got)
{
    if (!got)
        return;

    fprintf(stderr, "%s: got true, want false\n", name);
    exit(1);
}

static void require_u32(const char *name, uint32_t got, uint32_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %u, want %u\n", name, got, want);
    exit(1);
}

static void require_rect(const char *name,
                         struct vgpu_dirty_rect got,
                         uint32_t x,
                         uint32_t y,
                         uint32_t width,
                         uint32_t height)
{
    if (got.x == x && got.y == y && got.width == width && got.height == height)
        return;

    fprintf(stderr, "%s: got %u,%u %ux%u, want %u,%u %ux%u\n", name, got.x,
            got.y, got.width, got.height, x, y, width, height);
    exit(1);
}

static void require_no_oob(const char *name,
                           struct vgpu_dirty_rect rect,
                           uint32_t width,
                           uint32_t height)
{
    require_true(name, rect.x < width && rect.y < height &&
                           rect.width <= width - rect.x &&
                           rect.height <= height - rect.y);
}

static void test_full_scanout_maps_to_full_payload_and_dst(void)
{
    struct vgpu_dirty_rect scanout = {0, 0, 1024, 768};
    struct vgpu_dirty_rect flush = {0, 0, 1024, 768};
    struct vgpu_rect_update update;

    require_true("full scanout intersection",
                 vgpu_rect_compute_update(&scanout, &flush, &update));

    require_rect("full source", update.src, 0, 0, 1024, 768);
    require_rect("full dst", update.dst, 0, 0, 1024, 768);
    require_u32("full payload width", update.src.width, update.dst.width);
    require_u32("full payload height", update.src.height, update.dst.height);
    require_no_oob("full source no OOB", update.src, 1024, 768);
}

static void test_flush_outside_scanout_is_ignored(void)
{
    struct vgpu_dirty_rect scanout = {100, 100, 400, 300};
    struct vgpu_dirty_rect flush = {0, 0, 50, 50};
    struct vgpu_rect_update update = {{1, 2, 3, 4}, {5, 6, 7, 8}};

    require_false("outside scanout intersection",
                  vgpu_rect_compute_update(&scanout, &flush, &update));
    require_rect("outside update unchanged src", update.src, 1, 2, 3, 4);
    require_rect("outside update unchanged dst", update.dst, 5, 6, 7, 8);
}

static void test_left_top_partial_overlap_is_trimmed(void)
{
    struct vgpu_dirty_rect scanout = {100, 100, 400, 300};
    struct vgpu_dirty_rect flush = {80, 90, 60, 50};
    struct vgpu_rect_update update;

    require_true("left/top partial intersection",
                 vgpu_rect_compute_update(&scanout, &flush, &update));

    require_rect("left/top source", update.src, 100, 100, 40, 40);
    require_rect("left/top dst", update.dst, 0, 0, 40, 40);
    require_no_oob("left/top source no OOB", update.src, 500, 400);
    require_no_oob("left/top dst no OOB", update.dst, 400, 300);
}

static void test_right_bottom_partial_overlap_is_trimmed(void)
{
    struct vgpu_dirty_rect scanout = {100, 100, 400, 300};
    struct vgpu_dirty_rect flush = {450, 350, 120, 80};
    struct vgpu_rect_update update;

    require_true("right/bottom partial intersection",
                 vgpu_rect_compute_update(&scanout, &flush, &update));

    require_rect("right/bottom source", update.src, 450, 350, 50, 50);
    require_rect("right/bottom dst", update.dst, 350, 250, 50, 50);
    require_no_oob("right/bottom source no OOB", update.src, 500, 400);
    require_no_oob("right/bottom dst no OOB", update.dst, 400, 300);
}

static void test_scanout_crop_inside_resource_maps_subrect_to_dst(void)
{
    struct vgpu_dirty_rect scanout = {200, 150, 320, 240};
    struct vgpu_dirty_rect flush = {250, 190, 60, 70};
    struct vgpu_rect_update update;

    require_true("cropped scanout intersection",
                 vgpu_rect_compute_update(&scanout, &flush, &update));

    require_rect("cropped source", update.src, 250, 190, 60, 70);
    require_rect("cropped dst", update.dst, 50, 40, 60, 70);
    require_no_oob("cropped source no OOB", update.src, 800, 600);
    require_no_oob("cropped dst no OOB", update.dst, 320, 240);
}

static void test_zero_sized_rectangles_are_invalid_and_ignored(void)
{
    struct vgpu_dirty_rect scanout = {0, 0, 1024, 768};
    struct vgpu_dirty_rect zero_width = {20, 20, 0, 5};
    struct vgpu_dirty_rect zero_height = {20, 20, 5, 0};
    struct vgpu_rect_update update;

    require_false("zero-width flush ignored",
                  vgpu_rect_compute_update(&scanout, &zero_width, &update));
    require_false("zero-height flush ignored",
                  vgpu_rect_compute_update(&scanout, &zero_height, &update));
    require_false("zero-width resource fit",
                  vgpu_dirty_rect_fits(1024, 768, &zero_width));
    require_false("zero-height resource fit",
                  vgpu_dirty_rect_fits(1024, 768, &zero_height));
}

static void test_uint32_max_overflow_rectangles_are_invalid(void)
{
    struct vgpu_dirty_rect scanout = {0, 0, UINT32_MAX, UINT32_MAX};
    struct vgpu_dirty_rect overflow_x = {UINT32_MAX - 1U, 10, 2, 20};
    struct vgpu_dirty_rect overflow_y = {10, UINT32_MAX - 1U, 20, 2};
    struct vgpu_dirty_rect oob_x = {1024, 0, 1, 1};
    struct vgpu_dirty_rect oob_y = {0, 768, 1, 1};
    struct vgpu_rect_update update;

    require_false("overflow-x rect fit",
                  vgpu_dirty_rect_fits(UINT32_MAX, UINT32_MAX, &overflow_x));
    require_false("overflow-y rect fit",
                  vgpu_dirty_rect_fits(UINT32_MAX, UINT32_MAX, &overflow_y));
    require_false("overflow-x intersection ignored",
                  vgpu_rect_compute_update(&scanout, &overflow_x, &update));
    require_false("overflow-y intersection ignored",
                  vgpu_rect_compute_update(&scanout, &overflow_y, &update));
    require_false("oob-x resource fit",
                  vgpu_dirty_rect_fits(1024, 768, &oob_x));
    require_false("oob-y resource fit",
                  vgpu_dirty_rect_fits(1024, 768, &oob_y));
}

static void test_top_left_partial_payload_is_not_full_texture_update(void)
{
    struct vgpu_display_cpu_payload partial = {
        .width = 100,
        .height = 100,
        .stride = 400,
        .bits_per_pixel = 32,
        .texture_width = 1024,
        .texture_height = 768,
        .dst_x = 0,
        .dst_y = 0,
        .dst_width = 100,
        .dst_height = 100,
    };
    struct vgpu_display_cpu_payload full = {
        .width = 1024,
        .height = 768,
        .stride = 4096,
        .bits_per_pixel = 32,
        .texture_width = 1024,
        .texture_height = 768,
        .dst_x = 0,
        .dst_y = 0,
        .dst_width = 1024,
        .dst_height = 768,
    };

    require_false("top-left partial is not full texture update",
                  vgpu_display_cpu_payload_is_full_texture_update(&partial));
    require_true("full scanout payload is full texture update",
                 vgpu_display_cpu_payload_is_full_texture_update(&full));
}

static void require_primary_binding(const char *name,
                                    const struct virtio_gpu_scanout_info *s,
                                    uint32_t resource_id,
                                    uint32_t x,
                                    uint32_t y,
                                    uint32_t width,
                                    uint32_t height,
                                    uint32_t dirty_generation)
{
    if (s->primary_resource_id == resource_id && s->src_x == x &&
        s->src_y == y && s->src_w == width && s->src_h == height &&
        !s->primary_dirty.dirty &&
        s->primary_dirty.primary_clear_generation == dirty_generation)
        return;

    fprintf(stderr,
            "%s: got resource=%u src=%u,%u %ux%u dirty=%d gen=%u, want "
            "resource=%u src=%u,%u %ux%u dirty=0 gen=%u\n",
            name, s->primary_resource_id, s->src_x, s->src_y, s->src_w,
            s->src_h, s->primary_dirty.dirty,
            s->primary_dirty.primary_clear_generation, resource_id, x, y, width,
            height, dirty_generation);
    exit(1);
}

static void test_primary_bind_requires_generation_advance(void)
{
    struct virtio_gpu_scanout_info scanout = {
        .primary_resource_id = 7,
        .src_x = 1,
        .src_y = 2,
        .src_w = 300,
        .src_h = 200,
        .primary_dirty =
            {
                .dirty = false,
                .primary_clear_generation = 4,
            },
    };
    struct vgpu_dirty_rect next_src = {10, 20, 640, 480};

    require_false("bind rejected without generation advance",
                  vgpu_scanout_primary_bind_if_generation_advanced(
                      &scanout, false, 9, &next_src, 5));
    require_primary_binding("binding unchanged after failed generation",
                            &scanout, 7, 1, 2, 300, 200, 4);

    require_true("bind accepted after generation advance",
                 vgpu_scanout_primary_bind_if_generation_advanced(
                     &scanout, true, 9, &next_src, 5));
    require_primary_binding("binding changed after generation advance",
                            &scanout, 9, 10, 20, 640, 480, 5);
}

static void test_primary_clear_requires_published_clear(void)
{
    struct virtio_gpu_scanout_info scanout = {
        .primary_resource_id = 7,
        .src_x = 1,
        .src_y = 2,
        .src_w = 300,
        .src_h = 200,
        .primary_dirty =
            {
                .dirty = false,
                .primary_clear_generation = 4,
            },
    };

    require_false("clear rejected on queue-full result",
                  vgpu_scanout_primary_clear_if_published(
                      &scanout, VGPU_DISPLAY_PUBLISH_QUEUE_FULL, 5));
    require_primary_binding("binding unchanged after failed clear", &scanout, 7,
                            1, 2, 300, 200, 4);

    require_false("clear rejected on backpressure result",
                  vgpu_scanout_primary_clear_if_published(
                      &scanout, VGPU_DISPLAY_PUBLISH_BACKPRESSURE, 5));
    require_primary_binding("binding unchanged after backpressure clear",
                            &scanout, 7, 1, 2, 300, 200, 4);

    require_true("clear accepted when display unavailable",
                 vgpu_scanout_primary_clear_if_published(
                     &scanout, VGPU_DISPLAY_PUBLISH_UNAVAILABLE, 5));
    require_primary_binding("binding cleared after published clear", &scanout,
                            0, 0, 0, 0, 0, 5);
}

static void test_lifecycle_publish_result_classification(void)
{
    require_true(
        "OK lifecycle publish accepted",
        vgpu_display_lifecycle_publish_succeeded(VGPU_DISPLAY_PUBLISH_OK));
    require_true("UNAVAILABLE lifecycle publish accepted",
                 vgpu_display_lifecycle_publish_succeeded(
                     VGPU_DISPLAY_PUBLISH_UNAVAILABLE));
    require_false("queue-full lifecycle publish rejected",
                  vgpu_display_lifecycle_publish_succeeded(
                      VGPU_DISPLAY_PUBLISH_QUEUE_FULL));
    require_false("backpressure lifecycle publish rejected",
                  vgpu_display_lifecycle_publish_succeeded(
                      VGPU_DISPLAY_PUBLISH_BACKPRESSURE));
}

int main(void)
{
    test_full_scanout_maps_to_full_payload_and_dst();
    test_flush_outside_scanout_is_ignored();
    test_left_top_partial_overlap_is_trimmed();
    test_right_bottom_partial_overlap_is_trimmed();
    test_scanout_crop_inside_resource_maps_subrect_to_dst();
    test_zero_sized_rectangles_are_invalid_and_ignored();
    test_uint32_max_overflow_rectangles_are_invalid();
    test_top_left_partial_payload_is_not_full_texture_update();
    test_primary_bind_requires_generation_advance();
    test_primary_clear_requires_published_clear();
    test_lifecycle_publish_result_classification();
    return 0;
}
