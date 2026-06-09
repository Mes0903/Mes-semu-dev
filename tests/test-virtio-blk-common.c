#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../riscv_private.h"

#include "../virtio-blk.c"

#define REG(reg) ((uint32_t) VIRTIO_##reg << 2)
#define TEST_RAM_SIZE 8192
#define DESC_ADDR 0x100
#define AVAIL_ADDR 0x200
#define USED_ADDR 0x300
#define HEADER_ADDR 0x500
#define DATA_ADDR 0x600
#define STATUS_ADDR 0x900
#define DISK_SECTORS 2

static uint32_t ram_words[TEST_RAM_SIZE / 4];
static uint32_t disk_words[(DISK_SECTORS * DISK_BLK_SIZE) / 4];
static emu_state_t emu;
static unsigned wake_count;

void vm_set_exception(hart_t *hart, uint32_t cause, uint32_t val)
{
    hart->error = ERR_EXCEPTION;
    hart->exc_cause = cause;
    hart->exc_val = val;
}

void semu_wake_interruptible_harts(emu_state_t *emu_arg)
{
    (void) emu_arg;
    wake_count++;
}

static void require_bool(const char *name, bool got, bool want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %s, want %s\n", name, got ? "true" : "false",
            want ? "true" : "false");
    exit(1);
}

static void require_int(const char *name, int got, int want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %d, want %d\n", name, got, want);
    exit(1);
}

static void require_u8(const char *name, uint8_t got, uint8_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got 0x%x, want 0x%x\n", name, got, want);
    exit(1);
}

static void require_u16(const char *name, uint16_t got, uint16_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got 0x%x, want 0x%x\n", name, got, want);
    exit(1);
}

static void require_u32(const char *name, uint32_t got, uint32_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got 0x%x, want 0x%x\n", name, got, want);
    exit(1);
}

static void require_mem(const char *name,
                        const void *got,
                        const void *want,
                        size_t len)
{
    if (memcmp(got, want, len) == 0)
        return;

    fprintf(stderr, "%s: memory mismatch\n", name);
    exit(1);
}

static void dma_write(guest_paddr_t addr, const void *src, guest_size_t len)
{
    require_bool("dma write", ram_dma_write(&emu.ram_dma, addr, src, len),
                 true);
}

static void dma_read(guest_paddr_t addr, void *dst, guest_size_t len)
{
    require_bool("dma read", ram_dma_read(&emu.ram_dma, addr, dst, len), true);
}

static void write16(guest_paddr_t addr, uint16_t value)
{
    dma_write(addr, &value, sizeof(value));
}

static uint16_t read16(guest_paddr_t addr)
{
    uint16_t value;

    dma_read(addr, &value, sizeof(value));
    return value;
}

static uint32_t read32(guest_paddr_t addr)
{
    uint32_t value;

    dma_read(addr, &value, sizeof(value));
    return value;
}

static uint8_t read8(guest_paddr_t addr)
{
    uint8_t value;

    dma_read(addr, &value, sizeof(value));
    return value;
}

static void write_desc(uint16_t index,
                       uint64_t addr,
                       uint32_t len,
                       uint16_t flags,
                       uint16_t next)
{
    struct virtq_desc desc = {
        .addr = addr,
        .len = len,
        .flags = flags,
        .next = next,
    };

    dma_write(DESC_ADDR + (guest_paddr_t) index * sizeof(desc), &desc,
              sizeof(desc));
}

static void mmio_write(uint32_t reg, uint32_t value)
{
    require_int(
        "mmio write",
        virtio_mmio_write(&emu.vblk.common, reg, sizeof(uint32_t), value), 0);
}

static uint32_t mmio_read(uint32_t reg)
{
    uint32_t value = 0;

    require_int(
        "mmio read",
        virtio_mmio_read(&emu.vblk.common, reg, sizeof(uint32_t), &value), 0);
    return value;
}

static void configure_blk(void)
{
    memset(&emu, 0, sizeof(emu));
    memset(ram_words, 0, sizeof(ram_words));
    memset(disk_words, 0, sizeof(disk_words));
    ram_dma_init(&emu.ram_dma, ram_words, TEST_RAM_SIZE, NULL);
    emu.ram = ram_words;
    require_int("lifecycle init", semu_vm_lifecycle_init(&emu.lifecycle), 0);
    require_int("lifecycle running",
                semu_vm_lifecycle_enter_running(&emu.lifecycle), 0);
    require_int("plic lock init", pthread_mutex_init(&emu.plic_lock, NULL), 0);
    wake_count = 0;

    virtio_blk_init(&emu.vblk, &emu, NULL);
    emu.vblk.disk = disk_words;
    PRIV(&emu.vblk)->capacity = DISK_SECTORS;

    require_u32("device id", mmio_read(REG(DeviceID)), 2);
    require_u32("queue max", mmio_read(REG(QueueNumMax)), 1024);
    require_u32("capacity lo", mmio_read(REG(Config)), DISK_SECTORS);
    require_u32("blk size", mmio_read(REG(Config) + 20), DISK_BLK_SIZE);

    mmio_write(REG(DeviceFeaturesSel), 1);
    require_u32("VERSION_1 advertised", mmio_read(REG(DeviceFeatures)), 1);
    mmio_write(REG(DriverFeaturesSel), 1);
    mmio_write(REG(DriverFeatures), 1);
    mmio_write(REG(Status), VIRTIO_STATUS__ACKNOWLEDGE);
    mmio_write(REG(Status), VIRTIO_STATUS__DRIVER);
    mmio_write(REG(Status), VIRTIO_STATUS__FEATURES_OK);

    mmio_write(REG(QueueSel), 0);
    mmio_write(REG(QueueNum), 8);
    mmio_write(REG(QueueDescLow), DESC_ADDR);
    mmio_write(REG(QueueDriverLow), AVAIL_ADDR);
    mmio_write(REG(QueueDeviceLow), USED_ADDR);
    mmio_write(REG(QueueReady), 1);
    mmio_write(REG(Status), VIRTIO_STATUS__DRIVER_OK);
}

static void destroy_blk_fixture(void)
{
    virtio_blk_destroy(&emu.vblk);
    pthread_mutex_destroy(&emu.plic_lock);
    semu_vm_lifecycle_destroy(&emu.lifecycle);
}

static void publish_one_request(uint32_t type, uint64_t sector, uint32_t len)
{
    struct vblk_req_header header = {
        .type = type,
        .reserved = 0,
        .sector = sector,
    };

    dma_write(HEADER_ADDR, &header, sizeof(header));
    write_desc(0, HEADER_ADDR, sizeof(header), VIRTIO_DESC_F_NEXT, 1);
    write_desc(1, DATA_ADDR, len,
               VIRTIO_DESC_F_NEXT |
                   (type == VIRTIO_BLK_T_IN ? VIRTIO_DESC_F_WRITE : 0),
               2);
    write_desc(2, STATUS_ADDR, 1, VIRTIO_DESC_F_WRITE, 0);
    write16(AVAIL_ADDR + 4, 0);
    write16(AVAIL_ADDR + 2, 1);
}

static void test_successful_read_uses_common_transport_and_irq_ack(void)
{
    uint8_t disk_pattern[DISK_BLK_SIZE];
    uint8_t data[DISK_BLK_SIZE];

    configure_blk();
    for (size_t i = 0; i < sizeof(disk_pattern); i++)
        disk_pattern[i] = (uint8_t) (0xa0U + i);
    memcpy(disk_words, disk_pattern, sizeof(disk_pattern));
    memset(data, 0, sizeof(data));
    dma_write(DATA_ADDR, data, sizeof(data));
    dma_write(STATUS_ADDR, &(uint8_t) {0xff}, 1);

    publish_one_request(VIRTIO_BLK_T_IN, 0, sizeof(data));
    mmio_write(REG(QueueNotify), 0);

    dma_read(DATA_ADDR, data, sizeof(data));
    require_mem("read data", data, disk_pattern, sizeof(data));
    require_u8("status ok", read8(STATUS_ADDR), VIRTIO_BLK_S_OK);
    require_u16("used idx", read16(USED_ADDR + 2), 1);
    require_u32("used id", read32(USED_ADDR + 4), 0);
    require_u32("used len", read32(USED_ADDR + 8), sizeof(data));
    require_u32("irq status", mmio_read(REG(InterruptStatus)),
                VIRTIO_INT__USED_RING);
    require_bool("irq pending helper", virtio_blk_irq_pending(&emu.vblk), true);
    require_u32("wake count", wake_count, 1);

    mmio_write(REG(InterruptACK), VIRTIO_INT__USED_RING);
    require_u32("irq acked", mmio_read(REG(InterruptStatus)), 0);
    require_bool("irq pending after ack", virtio_blk_irq_pending(&emu.vblk),
                 false);
}

static void test_out_of_range_sets_ioerr_with_zero_length_completion(void)
{
    configure_blk();
    dma_write(STATUS_ADDR, &(uint8_t) {0xff}, 1);

    publish_one_request(VIRTIO_BLK_T_IN, DISK_SECTORS, DISK_BLK_SIZE);
    mmio_write(REG(QueueNotify), 0);

    require_u8("status ioerr", read8(STATUS_ADDR), VIRTIO_BLK_S_IOERR);
    require_u16("used idx", read16(USED_ADDR + 2), 1);
    require_u32("used id", read32(USED_ADDR + 4), 0);
    require_u32("used len", read32(USED_ADDR + 8), 0);
    require_u32("irq status", mmio_read(REG(InterruptStatus)),
                VIRTIO_INT__USED_RING);
}

int main(void)
{
    test_successful_read_uses_common_transport_and_irq_ack();
    destroy_blk_fixture();

    test_out_of_range_sets_ioerr_with_zero_length_completion();
    destroy_blk_fixture();
    return 0;
}
