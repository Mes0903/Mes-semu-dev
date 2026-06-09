#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct copy_case {
    const char *name;
    uint32_t texture_width;
    uint32_t texture_height;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_pixel;
    uint64_t base_iterations;
};

static const struct copy_case cases[] = {
    {
        .name = "full-frame",
        .texture_width = 1024,
        .texture_height = 768,
        .x = 0,
        .y = 0,
        .width = 1024,
        .height = 768,
        .bytes_per_pixel = 4,
        .base_iterations = 128,
    },
    {
        .name = "small-rect",
        .texture_width = 1024,
        .texture_height = 768,
        .x = 17,
        .y = 23,
        .width = 160,
        .height = 90,
        .bytes_per_pixel = 4,
        .base_iterations = 8192,
    },
    {
        .name = "cursor",
        .texture_width = 64,
        .texture_height = 64,
        .x = 0,
        .y = 0,
        .width = 64,
        .height = 64,
        .bytes_per_pixel = 4,
        .base_iterations = 16384,
    },
};

static void *xmalloc(size_t size)
{
    void *ptr = malloc(size);

    if (!ptr) {
        fprintf(stderr, "malloc(%zu) failed\n", size);
        exit(1);
    }
    return ptr;
}

static uint64_t bench_scale(void)
{
    const char *env = getenv("VGPU_COPY_BENCH_SCALE");
    char *end = NULL;
    unsigned long value;

    if (!env || env[0] == '\0')
        return 1;

    errno = 0;
    value = strtoul(env, &end, 10);
    if (errno != 0 || end == env || *end != '\0' || value == 0) {
        fprintf(stderr, "invalid VGPU_COPY_BENCH_SCALE='%s'\n", env);
        exit(1);
    }
    return value;
}

static double seconds_between(struct timespec start, struct timespec end)
{
    return (double) (end.tv_sec - start.tv_sec) +
           (double) (end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

static void fill_source(uint8_t *src, size_t size)
{
    uint32_t state = 0x12345678u;

    for (size_t i = 0; i < size; i++) {
        state = state * 1664525u + 1013904223u;
        src[i] = (uint8_t) (state >> 24);
    }
}

static void copy_snapshot(uint8_t *dst,
                          const uint8_t *src,
                          size_t src_stride,
                          uint32_t x,
                          uint32_t y,
                          uint32_t width,
                          uint32_t height,
                          uint32_t bytes_per_pixel)
{
    size_t row_bytes = (size_t) width * bytes_per_pixel;
    const uint8_t *src_pixels = src + (size_t) y * src_stride +
                                (size_t) x * bytes_per_pixel;

    if (x == 0 && src_stride == row_bytes) {
        memcpy(dst, src_pixels, row_bytes * height);
        return;
    }

    for (uint32_t row = 0; row < height; row++) {
        memcpy(dst + (size_t) row * row_bytes,
               src_pixels + (size_t) row * src_stride, row_bytes);
    }
}

static uint64_t run_case(const struct copy_case *c, uint64_t scale)
{
    size_t src_stride = (size_t) c->texture_width * c->bytes_per_pixel;
    size_t src_size = src_stride * c->texture_height;
    size_t row_bytes = (size_t) c->width * c->bytes_per_pixel;
    size_t dst_size = row_bytes * c->height;
    uint64_t iterations = c->base_iterations * scale;
    uint8_t *src = xmalloc(src_size);
    uint8_t *dst = xmalloc(dst_size);
    struct timespec start;
    struct timespec end;
    uint64_t checksum = 0;

    fill_source(src, src_size);
    memset(dst, 0, dst_size);

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        perror("clock_gettime start");
        exit(1);
    }
    for (uint64_t i = 0; i < iterations; i++) {
        copy_snapshot(dst, src, src_stride, c->x, c->y, c->width, c->height,
                      c->bytes_per_pixel);
        checksum += dst[(i * 2654435761u) % dst_size];
    }
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        perror("clock_gettime end");
        exit(1);
    }

    double seconds = seconds_between(start, end);
    double mib = (double) dst_size * (double) iterations / (1024.0 * 1024.0);
    double mib_per_second = seconds > 0.0 ? mib / seconds : 0.0;

    printf("%-12s rect=%ux%u@%u,%u bytes=%zu iterations=%llu "
           "time=%.6f MiB/s=%.2f checksum=%llu\n",
           c->name, c->width, c->height, c->x, c->y, dst_size,
           (unsigned long long) iterations, seconds, mib_per_second,
           (unsigned long long) checksum);

    free(dst);
    free(src);
    return checksum;
}

int main(void)
{
    uint64_t scale = bench_scale();
    uint64_t checksum = 0;

    printf("vgpu copy benchmark scale=%llu\n", (unsigned long long) scale);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        checksum ^= run_case(&cases[i], scale) + i;

    printf("combined-checksum=%llu\n", (unsigned long long) checksum);
    return 0;
}
