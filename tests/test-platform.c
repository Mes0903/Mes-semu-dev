#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmio-bus.h"
#include "platform.h"

struct expected_device {
    const char *name;
    uint64_t base;
    uint64_t size;
    uint32_t irq_source;
};

static const struct expected_device expected_devices[] = {
    {
        .name = "plic-window-0",
        .base = SEMU_PLATFORM_MMIO_PLIC_WINDOW0_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_NONE,
    },
    {
        .name = "plic-window-2",
        .base = SEMU_PLATFORM_MMIO_PLIC_WINDOW2_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_NONE,
    },
    {
        .name = "uart",
        .base = SEMU_PLATFORM_MMIO_UART_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_UART,
    },
    {
        .name = "virtio-net",
        .base = SEMU_PLATFORM_MMIO_VNET_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_VNET,
    },
    {
        .name = "virtio-blk",
        .base = SEMU_PLATFORM_MMIO_VBLK_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_VBLK,
    },
    {
        .name = "mtimer",
        .base = SEMU_PLATFORM_MMIO_MTIMER_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_NONE,
    },
    {
        .name = "mswi",
        .base = SEMU_PLATFORM_MMIO_MSWI_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_NONE,
    },
    {
        .name = "sswi",
        .base = SEMU_PLATFORM_MMIO_SSWI_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_NONE,
    },
    {
        .name = "virtio-rng",
        .base = SEMU_PLATFORM_MMIO_VRNG_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_VRNG,
    },
    {
        .name = "virtio-snd",
        .base = SEMU_PLATFORM_MMIO_VSND_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_VSND,
    },
    {
        .name = "virtio-fs",
        .base = SEMU_PLATFORM_MMIO_VFS_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_VFS,
    },
    {
        .name = "virtio-input-keyboard",
        .base = SEMU_PLATFORM_MMIO_VINPUT_KEYBOARD_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_VINPUT_KEYBOARD,
    },
    {
        .name = "virtio-input-mouse",
        .base = SEMU_PLATFORM_MMIO_VINPUT_MOUSE_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_VINPUT_MOUSE,
    },
    {
        .name = "virtio-gpu",
        .base = SEMU_PLATFORM_MMIO_VGPU_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_VGPU,
    },
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

static void require_string(const char *name, const char *got, const char *want)
{
    if (got && want && strcmp(got, want) == 0)
        return;

    fprintf(stderr, "%s: got %s, want %s\n", name, got ? got : "(null)",
            want ? want : "(null)");
    exit(1);
}

static void require_size(const char *name, size_t got, size_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %zu, want %zu\n", name, got, want);
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

static void require_region_matches(const char *label,
                                   const struct semu_mmio_region *region,
                                   const struct expected_device *want)
{
    require_nonnull(label, region);
    require_string("region name", region->name, want->name);
    require_u64("region base", region->base, want->base);
    require_u64("region size", region->size, want->size);
    require_u32("region irq source", region->irq_source, want->irq_source);
    require_null("region opaque", region->opaque);
    require_null("region read callback", region->read);
    require_null("region write callback", region->write);
}

static void test_fixed_table_matches_current_mmio_map(void)
{
    size_t count = 0;
    const struct semu_platform_device *devices =
        semu_platform_devices(&count);

    require_nonnull("platform devices", devices);
    require_size("platform device count", count, ARRAY_SIZE(expected_devices));
    require_size("platform device count constant", SEMU_PLATFORM_DEVICE_COUNT,
                 ARRAY_SIZE(expected_devices));

    for (size_t i = 0; i < count; i++) {
        require_string("device name", devices[i].name,
                       expected_devices[i].name);
        require_u64("device base", devices[i].base, expected_devices[i].base);
        require_u64("device size", devices[i].size, expected_devices[i].size);
        require_u32("device irq source", devices[i].irq_source,
                    expected_devices[i].irq_source);
    }
}

static void test_register_fixed_mmio_populates_bus(void)
{
    struct semu_mmio_bus bus;

    semu_mmio_bus_init(&bus);

    require_true("register fixed mmio",
                 semu_platform_register_fixed_mmio(&bus));
    require_u32("bus count", bus.count, ARRAY_SIZE(expected_devices));
    require_u64("bus generation", bus.generation,
                ARRAY_SIZE(expected_devices));

    for (size_t i = 0; i < ARRAY_SIZE(expected_devices); i++)
        require_region_matches("registered region", &bus.regions[i],
                               &expected_devices[i]);
}

static void test_registered_bus_lookup_matches_slots(void)
{
    struct semu_mmio_bus bus;
    uint64_t off = UINT64_MAX;
    const struct semu_mmio_region *found;

    semu_mmio_bus_init(&bus);
    require_true("register fixed mmio lookup",
                 semu_platform_register_fixed_mmio(&bus));

    found = semu_mmio_bus_find(&bus, SEMU_PLATFORM_MMIO_UART_BASE + 0x24, &off);
    require_region_matches("lookup uart", found, &expected_devices[2]);
    require_u64("lookup uart offset", off, 0x24);

    found = semu_mmio_bus_find(&bus, SEMU_PLATFORM_MMIO_VGPU_BASE + 0x10, &off);
    require_region_matches("lookup vgpu", found, &expected_devices[13]);
    require_u64("lookup vgpu offset", off, 0x10);

    found = semu_mmio_bus_find(&bus, SEMU_PLATFORM_MMIO_PLIC_WINDOW0_BASE, &off);
    require_region_matches("lookup plic window 0", found, &expected_devices[0]);
    require_u64("lookup plic window 0 offset", off, 0);

    found =
        semu_mmio_bus_find(&bus, SEMU_PLATFORM_MMIO_PLIC_WINDOW2_BASE + 0xFF0,
                           &off);
    require_region_matches("lookup plic window 2", found, &expected_devices[1]);
    require_u64("lookup plic window 2 offset", off, 0xFF0);

    require_null("lookup rejects unused plic gap",
                 semu_mmio_bus_find(&bus, UINT64_C(0xF0100000), &off));
}

static void test_registered_regions_have_no_callbacks_yet(void)
{
    struct semu_mmio_bus bus;
    uintptr_t hart_cookie = 0;
    hart_t *hart = (hart_t *) &hart_cookie;
    uint32_t value = 0;

    semu_mmio_bus_init(&bus);
    require_true("register fixed mmio callbacks",
                 semu_platform_register_fixed_mmio(&bus));

    for (uint32_t i = 0; i < bus.count; i++) {
        require_null("callback table read", bus.regions[i].read);
        require_null("callback table write", bus.regions[i].write);
        require_false("bus read without callback",
                      semu_mmio_bus_read(&bus, hart, bus.regions[i].base, 4,
                                         &value));
        require_false("bus write without callback",
                      semu_mmio_bus_write(&bus, hart, bus.regions[i].base, 4,
                                          0));
    }
}

int main(void)
{
    test_fixed_table_matches_current_mmio_map();
    test_register_fixed_mmio_populates_bus();
    test_registered_bus_lookup_matches_slots();
    test_registered_regions_have_no_callbacks_yet();
    return 0;
}
