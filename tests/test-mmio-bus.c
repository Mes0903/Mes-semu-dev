#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mmio-bus.h"

struct callback_log {
    unsigned reads;
    unsigned writes;
    hart_t *hart;
    uint64_t off;
    uint8_t width;
    uint32_t value;
};

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

static void require_ptr(const char *name, const void *got, const void *want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %p, want %p\n", name, got, want);
    exit(1);
}

static void require_nonnull(const char *name, const void *got)
{
    if (got)
        return;

    fprintf(stderr, "%s: got NULL\n", name);
    exit(1);
}

static void require_null(const char *name, const void *got)
{
    if (!got)
        return;

    fprintf(stderr, "%s: got %p, want NULL\n", name, got);
    exit(1);
}

static void require_u32(const char *name, uint32_t got, uint32_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got 0x%x, want 0x%x\n", name, got, want);
    exit(1);
}

static void require_u64(const char *name, uint64_t got, uint64_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got 0x%llx, want 0x%llx\n", name,
            (unsigned long long) got, (unsigned long long) want);
    exit(1);
}

static bool record_read(hart_t *hart,
                        void *opaque,
                        uint64_t off,
                        uint8_t width,
                        uint32_t *value)
{
    struct callback_log *log = opaque;

    log->reads++;
    log->hart = hart;
    log->off = off;
    log->width = width;
    *value = 0xA5000000U | width;
    return true;
}

static bool record_write(hart_t *hart,
                         void *opaque,
                         uint64_t off,
                         uint8_t width,
                         uint32_t value)
{
    struct callback_log *log = opaque;

    log->writes++;
    log->hart = hart;
    log->off = off;
    log->width = width;
    log->value = value;
    return true;
}

static void test_init_sets_count_and_generation_zero(void)
{
    struct semu_mmio_bus bus = {
        .count = 123,
        .generation = 456,
    };

    semu_mmio_bus_init(&bus);

    require_u32("init count", bus.count, 0);
    require_u64("init generation", bus.generation, 0);
}

static void test_registration_rejects_invalid_ranges(void)
{
    struct semu_mmio_bus bus;
    struct semu_mmio_region zero_size = {
        .base = 0x1000,
        .size = 0,
        .name = "zero",
    };
    struct semu_mmio_region overflow = {
        .base = UINT64_MAX - 0x10,
        .size = 0x20,
        .name = "overflow",
    };

    semu_mmio_bus_init(&bus);

    require_false("NULL bus rejected",
                  semu_mmio_bus_register(NULL, &zero_size));
    require_false("NULL region rejected",
                  semu_mmio_bus_register(&bus, NULL));
    require_false("zero-size registration",
                  semu_mmio_bus_register(&bus, &zero_size));
    require_false("overflow registration",
                  semu_mmio_bus_register(&bus, &overflow));
    require_u32("invalid range count", bus.count, 0);
    require_u64("invalid range generation", bus.generation, 0);
}

static void test_overlap_rejected_and_adjacent_accepted(void)
{
    struct semu_mmio_bus bus;
    struct semu_mmio_region first = {
        .base = 0x1000,
        .size = 0x1000,
        .name = "first",
    };
    struct semu_mmio_region overlap = {
        .base = 0x1800,
        .size = 0x100,
        .name = "overlap",
    };
    struct semu_mmio_region adjacent = {
        .base = 0x2000,
        .size = 0x1000,
        .name = "adjacent",
    };

    semu_mmio_bus_init(&bus);

    require_true("first registration", semu_mmio_bus_register(&bus, &first));
    require_u32("first count", bus.count, 1);
    require_u64("first generation", bus.generation, 1);

    require_false("overlap registration",
                  semu_mmio_bus_register(&bus, &overlap));
    require_u32("overlap count unchanged", bus.count, 1);
    require_u64("overlap generation unchanged", bus.generation, 1);

    require_true("adjacent registration",
                 semu_mmio_bus_register(&bus, &adjacent));
    require_u32("adjacent count", bus.count, 2);
    require_u64("adjacent generation", bus.generation, 2);
}

static void test_generation_increments_only_on_success_until_capacity(void)
{
    struct semu_mmio_bus bus;

    semu_mmio_bus_init(&bus);

    for (uint32_t i = 0; i < SEMU_MMIO_BUS_MAX_REGIONS; i++) {
        struct semu_mmio_region region = {
            .base = UINT64_C(0x10000) + ((uint64_t) i * UINT64_C(0x1000)),
            .size = 0x1000,
            .name = "region",
        };

        require_true("capacity fill", semu_mmio_bus_register(&bus, &region));
        require_u32("capacity count", bus.count, i + 1);
        require_u64("capacity generation", bus.generation, i + 1);
    }

    {
        struct semu_mmio_region extra = {
            .base = UINT64_C(0x10000) +
                    ((uint64_t) SEMU_MMIO_BUS_MAX_REGIONS * UINT64_C(0x1000)),
            .size = 0x1000,
            .name = "extra",
        };

        require_false("capacity overflow", semu_mmio_bus_register(&bus, &extra));
        require_u32("capacity overflow count", bus.count,
                    SEMU_MMIO_BUS_MAX_REGIONS);
        require_u64("capacity overflow generation", bus.generation,
                    SEMU_MMIO_BUS_MAX_REGIONS);
    }
}

static void test_find_returns_expected_region_and_offset(void)
{
    struct semu_mmio_bus bus;
    uint64_t off = UINT64_MAX;
    struct semu_mmio_region first = {
        .base = 0x4000,
        .size = 0x100,
        .name = "first",
        .irq_source = 17,
    };
    struct semu_mmio_region second = {
        .base = 0x5000,
        .size = 0x100,
        .name = "second",
        .irq_source = 18,
    };
    const struct semu_mmio_region *found;

    semu_mmio_bus_init(&bus);
    require_true("first lookup registration",
                 semu_mmio_bus_register(&bus, &first));
    require_true("second lookup registration",
                 semu_mmio_bus_register(&bus, &second));

    found = semu_mmio_bus_find(&bus, 0x4044, &off);
    require_nonnull("lookup first found", found);
    require_u64("lookup first offset", off, 0x44);
    require_u32("lookup first irq", found->irq_source, 17);
    require_ptr("lookup first copied region", found, &bus.regions[0]);

    found = semu_mmio_bus_find(&bus, 0x5000, &off);
    require_nonnull("lookup second found", found);
    require_u64("lookup second offset", off, 0);
    require_u32("lookup second irq", found->irq_source, 18);
    require_ptr("lookup second copied region", found, &bus.regions[1]);

    require_null("lookup rejects region end",
                 semu_mmio_bus_find(&bus, 0x4100, &off));
}

static void test_read_write_dispatch_to_callbacks(void)
{
    struct semu_mmio_bus bus;
    struct callback_log log = {0};
    uintptr_t hart_cookie = 0;
    hart_t *hart = (hart_t *) &hart_cookie;
    uint32_t value = 0;
    struct semu_mmio_region region = {
        .base = 0x8000,
        .size = 0x100,
        .name = "dispatch",
        .opaque = &log,
        .read = record_read,
        .write = record_write,
    };

    semu_mmio_bus_init(&bus);
    require_true("dispatch registration",
                 semu_mmio_bus_register(&bus, &region));

    require_true("read dispatch",
                 semu_mmio_bus_read(&bus, hart, 0x8024, 4, &value));
    require_u32("read callback count", log.reads, 1);
    require_ptr("read callback hart", log.hart, hart);
    require_u64("read callback offset", log.off, 0x24);
    require_u32("read callback width", log.width, 4);
    require_u32("read callback value", value, 0xA5000004U);

    require_true("write dispatch",
                 semu_mmio_bus_write(&bus, hart, 0x8030, 2, 0x12345678));
    require_u32("write callback count", log.writes, 1);
    require_ptr("write callback hart", log.hart, hart);
    require_u64("write callback offset", log.off, 0x30);
    require_u32("write callback width", log.width, 2);
    require_u32("write callback value", log.value, 0x12345678);
}

static void test_missing_callback_returns_false(void)
{
    struct semu_mmio_bus bus;
    uintptr_t hart_cookie = 0;
    hart_t *hart = (hart_t *) &hart_cookie;
    uint32_t value = 0;
    struct semu_mmio_region no_callbacks = {
        .base = 0x9000,
        .size = 0x100,
        .name = "no-callbacks",
    };
    struct semu_mmio_region read_only = {
        .base = 0xA000,
        .size = 0x100,
        .name = "read-only",
        .read = record_read,
    };

    semu_mmio_bus_init(&bus);
    require_true("missing callback registration",
                 semu_mmio_bus_register(&bus, &no_callbacks));
    require_true("read-only registration",
                 semu_mmio_bus_register(&bus, &read_only));

    require_false("read missing callback",
                  semu_mmio_bus_read(&bus, hart, 0x9000, 4, &value));
    require_false("write missing callback",
                  semu_mmio_bus_write(&bus, hart, 0x9000, 4, 0));
    require_false("write read-only missing callback",
                  semu_mmio_bus_write(&bus, hart, 0xA000, 4, 0));
    require_false("read missing address",
                  semu_mmio_bus_read(&bus, hart, 0xB000, 4, &value));
    require_false("write missing address",
                  semu_mmio_bus_write(&bus, hart, 0xB000, 4, 0));
}

int main(void)
{
    test_init_sets_count_and_generation_zero();
    test_registration_rejects_invalid_ranges();
    test_overlap_rejected_and_adjacent_accepted();
    test_generation_increments_only_on_success_until_capacity();
    test_find_returns_expected_region_and_offset();
    test_read_write_dispatch_to_callbacks();
    test_missing_callback_returns_false();
    return 0;
}
