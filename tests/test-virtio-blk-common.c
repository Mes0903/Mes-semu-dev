#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../ram_access.h"
#include "../riscv_private.h"

static bool test_ram_dma_read(const ram_dma_t *dma,
                              guest_paddr_t addr,
                              void *buf,
                              guest_size_t len);
static bool test_ram_dma_write(ram_dma_t *dma,
                               guest_paddr_t addr,
                               const void *buf,
                               guest_size_t len);

#define ram_dma_read test_ram_dma_read
#define ram_dma_write test_ram_dma_write
#include "../virtio-blk.c"
#undef ram_dma_write
#undef ram_dma_read

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
static struct virtio_device_common *reset_start_on_avail_read_common;
static guest_paddr_t reset_start_on_avail_read_addr;

struct dma_gate {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool enabled;
    bool entered;
    bool release;
    guest_paddr_t addr;
    guest_size_t len;
};

static struct dma_gate blk_dma_write_gate = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};
static struct dma_gate blk_dma_read_gate = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static void dma_gate_block_if_enabled(struct dma_gate *gate,
                                      guest_paddr_t addr,
                                      guest_size_t len)
{
    pthread_mutex_lock(&gate->lock);
    if (gate->enabled && addr == gate->addr && len == gate->len) {
        gate->entered = true;
        pthread_cond_broadcast(&gate->cond);
        while (!gate->release)
            pthread_cond_wait(&gate->cond, &gate->lock);
    }
    pthread_mutex_unlock(&gate->lock);
}

static bool test_ram_dma_read(const ram_dma_t *dma,
                              guest_paddr_t addr,
                              void *buf,
                              guest_size_t len)
{
    if (reset_start_on_avail_read_common &&
        addr == reset_start_on_avail_read_addr && len == sizeof(uint16_t)) {
        struct virtio_device_common *common = reset_start_on_avail_read_common;

        reset_start_on_avail_read_common = NULL;
        pthread_mutex_lock(&common->transport_lock);
        common->generation++;
        common->reset_in_progress = true;
        pthread_mutex_unlock(&common->transport_lock);
    }

    dma_gate_block_if_enabled(&blk_dma_read_gate, addr, len);
    return ram_dma_read(dma, addr, buf, len);
}

static bool test_ram_dma_write(ram_dma_t *dma,
                               guest_paddr_t addr,
                               const void *buf,
                               guest_size_t len)
{
    dma_gate_block_if_enabled(&blk_dma_write_gate, addr, len);

    return ram_dma_write(dma, addr, buf, len);
}

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

static struct timespec deadline_after_ms(unsigned timeout_ms)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

static void sleep_one_ms(void)
{
    const struct timespec ts = {
        .tv_sec = 0,
        .tv_nsec = 1000000L,
    };

    nanosleep(&ts, NULL);
}

static void dma_gate_enable(struct dma_gate *gate,
                            guest_paddr_t addr,
                            guest_size_t len)
{
    pthread_mutex_lock(&gate->lock);
    gate->enabled = true;
    gate->entered = false;
    gate->release = false;
    gate->addr = addr;
    gate->len = len;
    pthread_mutex_unlock(&gate->lock);
}

static bool dma_gate_wait_entered(struct dma_gate *gate, unsigned timeout_ms)
{
    struct timespec deadline = deadline_after_ms(timeout_ms);
    bool entered;

    pthread_mutex_lock(&gate->lock);
    while (!gate->entered) {
        int ret = pthread_cond_timedwait(&gate->cond, &gate->lock, &deadline);
        if (ret == ETIMEDOUT)
            break;
    }
    entered = gate->entered;
    pthread_mutex_unlock(&gate->lock);
    return entered;
}

static void dma_gate_release(struct dma_gate *gate)
{
    pthread_mutex_lock(&gate->lock);
    gate->enabled = false;
    gate->release = true;
    pthread_cond_broadcast(&gate->cond);
    pthread_mutex_unlock(&gate->lock);
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
    int ret = virtio_mmio_write(&emu.vblk.common, reg, sizeof(uint32_t), value);

    if (ret != 0) {
        fprintf(stderr, "mmio write reg 0x%x value 0x%x: got %d, want 0\n", reg,
                value, ret);
        exit(1);
    }
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

static bool source_asserted(emu_state_t *emu_arg, enum semu_irq_source source)
{
    return (emu_arg->plic.active & semu_irq_source_plic_bit(source)) != 0;
}

static bool wait_for_used_idx(uint16_t idx)
{
    for (unsigned i = 0; i < 1000; i++) {
        if (read16(USED_ADDR + 2) == idx)
            return true;
        sleep_one_ms();
    }
    return false;
}

static bool wait_for_blk_actor_state(virtio_blk_state_t *vblk,
                                     enum virtio_actor_state state)
{
    for (unsigned i = 0; i < 1000; i++) {
        if (virtio_actor_get_state(&vblk->actor) == state)
            return true;
        sleep_one_ms();
    }
    return false;
}

static bool wait_for_blk_actor_pending_mask(virtio_blk_state_t *vblk,
                                            uint32_t mask)
{
    for (unsigned i = 0; i < 1000; i++) {
        if (virtio_actor_pending_mask(&vblk->actor) == mask)
            return true;
        sleep_one_ms();
    }
    return false;
}

struct async_blk_call {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    virtio_blk_state_t *vblk;
    int ret;
    bool done;
};

static void async_blk_call_init(struct async_blk_call *call,
                                virtio_blk_state_t *vblk)
{
    memset(call, 0, sizeof(*call));
    call->vblk = vblk;
    require_int("async lock init", pthread_mutex_init(&call->lock, NULL), 0);
    require_int("async cond init", pthread_cond_init(&call->cond, NULL), 0);
}

static void async_blk_call_destroy(struct async_blk_call *call)
{
    pthread_cond_destroy(&call->cond);
    pthread_mutex_destroy(&call->lock);
}

static void async_blk_call_finish(struct async_blk_call *call, int ret)
{
    pthread_mutex_lock(&call->lock);
    call->ret = ret;
    call->done = true;
    pthread_cond_broadcast(&call->cond);
    pthread_mutex_unlock(&call->lock);
}

static bool async_blk_call_wait_done(struct async_blk_call *call,
                                     unsigned timeout_ms)
{
    struct timespec deadline = deadline_after_ms(timeout_ms);
    bool done;

    pthread_mutex_lock(&call->lock);
    while (!call->done) {
        int ret = pthread_cond_timedwait(&call->cond, &call->lock, &deadline);
        if (ret == ETIMEDOUT)
            break;
    }
    done = call->done;
    pthread_mutex_unlock(&call->lock);
    return done;
}

static void async_blk_call_join(struct async_blk_call *call)
{
    require_int("async join", pthread_join(call->thread, NULL), 0);
}

static void *notify_queue_thread(void *opaque)
{
    struct async_blk_call *call = opaque;
    int ret = virtio_mmio_write(&call->vblk->common, REG(QueueNotify), 4, 0);

    async_blk_call_finish(call, ret);
    return NULL;
}

static void *reset_blk_thread(void *opaque)
{
    struct async_blk_call *call = opaque;
    int ret = virtio_device_common_reset(&call->vblk->common);

    async_blk_call_finish(call, ret);
    return NULL;
}

static void *destroy_blk_thread(void *opaque)
{
    struct async_blk_call *call = opaque;

    virtio_blk_destroy(call->vblk);
    async_blk_call_finish(call, 0);
    return NULL;
}

static void async_blk_call_start_notify(struct async_blk_call *call)
{
    require_int("notify thread create",
                pthread_create(&call->thread, NULL, notify_queue_thread, call),
                0);
}

static void async_blk_call_start_reset(struct async_blk_call *call)
{
    require_int("reset thread create",
                pthread_create(&call->thread, NULL, reset_blk_thread, call), 0);
}

static void async_blk_call_start_destroy(struct async_blk_call *call)
{
    require_int("destroy thread create",
                pthread_create(&call->thread, NULL, destroy_blk_thread, call),
                0);
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

    require_bool("actor published used completion", wait_for_used_idx(1), true);
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

    require_bool("actor published ioerr completion", wait_for_used_idx(1),
                 true);
    require_u8("status ioerr", read8(STATUS_ADDR), VIRTIO_BLK_S_IOERR);
    require_u16("used idx", read16(USED_ADDR + 2), 1);
    require_u32("used id", read32(USED_ADDR + 4), 0);
    require_u32("used len", read32(USED_ADDR + 8), 0);
    require_u32("irq status", mmio_read(REG(InterruptStatus)),
                VIRTIO_INT__USED_RING);
}


static void test_queue_notify_enqueues_before_actor_status_write_returns(void)
{
    uint8_t disk_pattern[DISK_BLK_SIZE];
    struct async_blk_call notify;

    configure_blk();
    for (size_t i = 0; i < sizeof(disk_pattern); i++)
        disk_pattern[i] = (uint8_t) (0x20U + i);
    memcpy(disk_words, disk_pattern, sizeof(disk_pattern));
    dma_write(STATUS_ADDR, &(uint8_t) {0xff}, 1);

    publish_one_request(VIRTIO_BLK_T_IN, 0, sizeof(disk_pattern));
    dma_gate_enable(&blk_dma_write_gate, STATUS_ADDR, 1);
    async_blk_call_init(&notify, &emu.vblk);
    async_blk_call_start_notify(&notify);

    require_bool("actor entered status write",
                 dma_gate_wait_entered(&blk_dma_write_gate, 1000), true);
    require_bool("QueueNotify returned while actor status write is blocked",
                 async_blk_call_wait_done(&notify, 1000), true);
    require_int("QueueNotify return", notify.ret, 0);
    require_u16("no used completion before actor status returns",
                read16(USED_ADDR + 2), 0);
    require_u32("no interrupt before actor status returns",
                mmio_read(REG(InterruptStatus)), 0);

    dma_gate_release(&blk_dma_write_gate);
    async_blk_call_join(&notify);
    require_bool("actor published async used completion", wait_for_used_idx(1),
                 true);
    require_u8("async status ok", read8(STATUS_ADDR), VIRTIO_BLK_S_OK);
    require_u32("async interrupt", mmio_read(REG(InterruptStatus)),
                VIRTIO_INT__USED_RING);
    require_bool("async irq line", source_asserted(&emu, SEMU_IRQ_SOURCE_VBLK),
                 true);

    async_blk_call_destroy(&notify);
}

static void test_reset_cancels_stale_pending_actor_completion(void)
{
    struct async_blk_call notify;
    struct async_blk_call reset;

    configure_blk();
    dma_write(STATUS_ADDR, &(uint8_t) {0xff}, 1);
    publish_one_request(VIRTIO_BLK_T_IN, 0, DISK_BLK_SIZE);
    dma_gate_enable(&blk_dma_write_gate, STATUS_ADDR, 1);
    async_blk_call_init(&notify, &emu.vblk);
    async_blk_call_start_notify(&notify);
    require_bool("actor entered status write before reset",
                 dma_gate_wait_entered(&blk_dma_write_gate, 1000), true);
    require_bool("QueueNotify returned before reset",
                 async_blk_call_wait_done(&notify, 1000), true);

    async_blk_call_init(&reset, &emu.vblk);
    async_blk_call_start_reset(&reset);
    require_bool("reset advanced actor generation",
                 wait_for_blk_actor_state(&emu.vblk, VIRTIO_ACTOR_RESETTING),
                 true);

    dma_gate_release(&blk_dma_write_gate);
    async_blk_call_join(&notify);
    async_blk_call_join(&reset);
    require_int("reset return", reset.ret, 0);
    require_u16("reset stale used idx remains clear", read16(USED_ADDR + 2), 0);
    require_u32("reset stale interrupt remains clear",
                mmio_read(REG(InterruptStatus)), 0);
    require_bool("reset stale irq line remains clear",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VBLK), false);

    async_blk_call_destroy(&reset);
    async_blk_call_destroy(&notify);
}

static void test_stop_cancels_stale_pending_actor_completion(void)
{
    struct async_blk_call notify;
    struct async_blk_call destroy;

    configure_blk();
    dma_write(STATUS_ADDR, &(uint8_t) {0xff}, 1);
    publish_one_request(VIRTIO_BLK_T_IN, 0, DISK_BLK_SIZE);
    dma_gate_enable(&blk_dma_write_gate, STATUS_ADDR, 1);
    async_blk_call_init(&notify, &emu.vblk);
    async_blk_call_start_notify(&notify);
    require_bool("actor entered status write before stop",
                 dma_gate_wait_entered(&blk_dma_write_gate, 1000), true);
    require_bool("QueueNotify returned before stop",
                 async_blk_call_wait_done(&notify, 1000), true);

    async_blk_call_init(&destroy, &emu.vblk);
    async_blk_call_start_destroy(&destroy);
    require_bool("stop advanced actor generation",
                 wait_for_blk_actor_state(&emu.vblk, VIRTIO_ACTOR_STOPPING),
                 true);

    dma_gate_release(&blk_dma_write_gate);
    async_blk_call_join(&notify);
    async_blk_call_join(&destroy);
    require_int("destroy return", destroy.ret, 0);
    require_u16("stop stale used idx remains clear", read16(USED_ADDR + 2), 0);
    require_bool("stop stale irq line remains clear",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VBLK), false);

    async_blk_call_destroy(&destroy);
    async_blk_call_destroy(&notify);
    pthread_mutex_destroy(&emu.plic_lock);
    semu_vm_lifecycle_destroy(&emu.lifecycle);
}

static void test_common_reset_start_cancels_stale_actor_completion(void)
{
    struct async_blk_call notify;

    configure_blk();
    dma_write(STATUS_ADDR, &(uint8_t) {0xff}, 1);
    publish_one_request(VIRTIO_BLK_T_IN, 0, DISK_BLK_SIZE);
    dma_gate_enable(&blk_dma_write_gate, STATUS_ADDR, 1);
    async_blk_call_init(&notify, &emu.vblk);
    async_blk_call_start_notify(&notify);
    require_bool("actor entered status write before common reset",
                 dma_gate_wait_entered(&blk_dma_write_gate, 1000), true);
    require_bool("QueueNotify returned before common reset",
                 async_blk_call_wait_done(&notify, 1000), true);

    pthread_mutex_lock(&emu.vblk.common.transport_lock);
    emu.vblk.common.generation++;
    emu.vblk.common.reset_in_progress = true;
    pthread_mutex_unlock(&emu.vblk.common.transport_lock);

    dma_gate_release(&blk_dma_write_gate);
    async_blk_call_join(&notify);
    require_bool("common reset stale pending clears",
                 wait_for_blk_actor_pending_mask(&emu.vblk, 0), true);
    require_u16("common reset stale used idx remains clear",
                read16(USED_ADDR + 2), 0);
    require_u32("common reset stale interrupt remains clear",
                mmio_read(REG(InterruptStatus)), 0);
    require_bool("common reset stale irq line remains clear",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VBLK), false);

    async_blk_call_destroy(&notify);
}

static void test_reset_drains_pending_out_disk_write_without_stale_completion(
    void)
{
    uint8_t guest_pattern[DISK_BLK_SIZE];
    uint8_t disk_before[DISK_BLK_SIZE];
    struct async_blk_call notify;
    struct async_blk_call reset;

    configure_blk();
    for (size_t i = 0; i < sizeof(guest_pattern); i++) {
        guest_pattern[i] = (uint8_t) (0x70U + i);
        disk_before[i] = (uint8_t) (0xa0U + i);
    }
    memcpy(disk_words, disk_before, sizeof(disk_before));
    dma_write(DATA_ADDR, guest_pattern, sizeof(guest_pattern));
    dma_write(STATUS_ADDR, &(uint8_t) {0xff}, 1);

    publish_one_request(VIRTIO_BLK_T_OUT, 0, sizeof(guest_pattern));
    dma_gate_enable(&blk_dma_read_gate, DATA_ADDR, sizeof(guest_pattern));
    async_blk_call_init(&notify, &emu.vblk);
    async_blk_call_start_notify(&notify);
    require_bool("actor entered disk write read before reset",
                 dma_gate_wait_entered(&blk_dma_read_gate, 1000), true);
    require_bool("QueueNotify returned before out reset",
                 async_blk_call_wait_done(&notify, 1000), true);

    async_blk_call_init(&reset, &emu.vblk);
    async_blk_call_start_reset(&reset);
    require_bool("out reset advanced actor generation",
                 wait_for_blk_actor_state(&emu.vblk, VIRTIO_ACTOR_RESETTING),
                 true);
    require_bool("reset waits while out disk write is blocked",
                 async_blk_call_wait_done(&reset, 25), false);
    require_mem("disk unchanged before blocked out write is released",
                disk_words, disk_before, sizeof(disk_before));

    dma_gate_release(&blk_dma_read_gate);
    async_blk_call_join(&notify);
    async_blk_call_join(&reset);
    require_int("out reset return", reset.ret, 0);
    require_mem("out disk write drained before reset returned", disk_words,
                guest_pattern, sizeof(guest_pattern));
    require_u16("out reset stale used idx remains clear", read16(USED_ADDR + 2),
                0);
    require_u32("out reset stale interrupt remains clear",
                mmio_read(REG(InterruptStatus)), 0);
    require_bool("out reset stale irq line remains clear",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VBLK), false);

    async_blk_call_destroy(&reset);
    async_blk_call_destroy(&notify);
}

static void test_common_reset_start_cancels_stale_avail_failure(void)
{
    int ret;

    configure_blk();
    write16(AVAIL_ADDR + 2, 9);

    reset_start_on_avail_read_common = &emu.vblk.common;
    reset_start_on_avail_read_addr = AVAIL_ADDR + 2;
    ret =
        virtio_blk_actor_drain_queue(&emu.vblk, &emu.vblk.actor, VBLK_QUEUE,
                                     virtio_actor_generation(&emu.vblk.actor));

    require_int("common reset stale avail drain return", ret, 0);
    require_bool("common reset stale avail hook consumed",
                 reset_start_on_avail_read_common == NULL, true);
    require_u32("common reset stale avail interrupt remains clear",
                virtio_irq_read_status(&emu.vblk.common.irq), 0);
    require_bool("common reset stale avail irq line remains clear",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VBLK), false);
    require_u32(
        "common reset stale avail needs-reset remains clear",
        virtio_blk_status_load(&emu.vblk) & VIRTIO_STATUS__DEVICE_NEEDS_RESET,
        0);
}

static void test_stale_common_generation_queue_has_work_is_side_effect_free(
    void)
{
    bool has_work;

    configure_blk();
    write16(AVAIL_ADDR + 2, 9);

    pthread_mutex_lock(&emu.vblk.common.transport_lock);
    emu.vblk.common.generation++;
    emu.vblk.common.reset_in_progress = true;
    pthread_mutex_unlock(&emu.vblk.common.transport_lock);

    has_work = virtio_blk_actor_queue_has_work(
        &emu.vblk, &emu.vblk.actor, VBLK_QUEUE,
        virtio_actor_generation(&emu.vblk.actor));

    require_bool("stale queue_has_work false", has_work, false);
    require_u32("stale queue_has_work interrupt remains clear",
                virtio_irq_read_status(&emu.vblk.common.irq), 0);
    require_u32(
        "stale queue_has_work needs-reset remains clear",
        virtio_blk_status_load(&emu.vblk) & VIRTIO_STATUS__DEVICE_NEEDS_RESET,
        0);
}

int main(void)
{
    test_successful_read_uses_common_transport_and_irq_ack();
    destroy_blk_fixture();

    test_out_of_range_sets_ioerr_with_zero_length_completion();
    destroy_blk_fixture();

    test_queue_notify_enqueues_before_actor_status_write_returns();
    destroy_blk_fixture();

    test_reset_cancels_stale_pending_actor_completion();
    destroy_blk_fixture();

    test_reset_drains_pending_out_disk_write_without_stale_completion();
    destroy_blk_fixture();

    test_stop_cancels_stale_pending_actor_completion();

    test_common_reset_start_cancels_stale_actor_completion();
    destroy_blk_fixture();

    test_common_reset_start_cancels_stale_avail_failure();
    destroy_blk_fixture();

    test_stale_common_generation_queue_has_work_is_side_effect_free();
    destroy_blk_fixture();
    return 0;
}
