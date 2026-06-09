#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../riscv_private.h"

static ssize_t test_read(int fd, void *buf, size_t count);

#define read test_read
#include "../virtio-rng.c"
#undef read

struct read_step {
    ssize_t result;
    int err;
    uint8_t fill;
};

static const struct read_step *read_steps;
static size_t read_step_count;
static size_t read_step_index;
static size_t read_call_count;
static unsigned wake_count;

void vm_set_exception(hart_t *hart, uint32_t cause, uint32_t val)
{
    hart->error = ERR_EXCEPTION;
    hart->exc_cause = cause;
    hart->exc_val = val;
}

void semu_wake_interruptible_harts(emu_state_t *emu)
{
    (void) emu;
    wake_count++;
}

static ssize_t test_read(int fd, void *buf, size_t count)
{
    const struct read_step *step;

    (void) fd;
    if (read_step_index >= read_step_count) {
        fprintf(stderr, "unexpected read call %zu\n", read_step_index);
        exit(1);
    }

    read_call_count++;
    step = &read_steps[read_step_index++];
    if (step->result < 0) {
        errno = step->err;
        return -1;
    }

    if ((size_t) step->result > count) {
        fprintf(stderr, "scripted read result exceeds request\n");
        exit(1);
    }
    memset(buf, step->fill, (size_t) step->result);
    return step->result;
}

static void set_read_script(const struct read_step *steps, size_t step_count)
{
    read_steps = steps;
    read_step_count = step_count;
    read_step_index = 0;
    read_call_count = 0;
}

static void require_bool(const char *name, bool got, bool want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %s, want %s\n", name, got ? "true" : "false",
            want ? "true" : "false");
    exit(1);
}

static void require_size(const char *name, size_t got, size_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %zu, want %zu\n", name, got, want);
    exit(1);
}

static void require_int(const char *name, int got, int want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %d, want %d\n", name, got, want);
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

static void require_u8(const char *name, uint8_t got, uint8_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got 0x%x, want 0x%x\n", name, got, want);
    exit(1);
}

static size_t read_entropy(void *buf, size_t len, bool *permanent_failure)
{
    return virtio_rng_read_entropy(buf, len, permanent_failure);
}

static void test_eintr_retries_before_success(void)
{
    const struct read_step steps[] = {
        {.result = -1, .err = EINTR},
        {.result = 4, .fill = 0xa5},
    };
    uint8_t buf[8] = {0};
    bool permanent_failure = true;
    size_t total;

    set_read_script(steps, ARRAY_SIZE(steps));
    total = read_entropy(buf, sizeof(buf), &permanent_failure);

    require_size("read call count", read_call_count, 2);
    require_size("used byte count", total, 4);
    require_bool("permanent failure", permanent_failure, false);
    require_u8("entropy byte 0", buf[0], 0xa5);
    require_u8("entropy byte 3", buf[3], 0xa5);
}

static void test_eagain_transient_zero_completion(void)
{
    const struct read_step steps[] = {
        {.result = -1, .err = EAGAIN},
    };
    uint8_t buf[8] = {0};
    bool permanent_failure = true;
    size_t total;

    set_read_script(steps, ARRAY_SIZE(steps));
    total = read_entropy(buf, sizeof(buf), &permanent_failure);

    require_size("read call count", read_call_count, 1);
    require_size("used byte count", total, 0);
    require_bool("permanent failure", permanent_failure, false);
}

static void test_ewouldblock_transient_zero_completion(void)
{
    const struct read_step steps[] = {
        {.result = -1, .err = EWOULDBLOCK},
    };
    uint8_t buf[8] = {0};
    bool permanent_failure = true;
    size_t total;

    set_read_script(steps, ARRAY_SIZE(steps));
    total = read_entropy(buf, sizeof(buf), &permanent_failure);

    require_size("read call count", read_call_count, 1);
    require_size("used byte count", total, 0);
    require_bool("permanent failure", permanent_failure, false);
}

static void test_eio_completes_zero_and_reports_permanent_failure(void)
{
    const struct read_step steps[] = {
        {.result = -1, .err = EIO},
    };
    uint8_t buf[8] = {0};
    bool permanent_failure = false;
    size_t total;

    set_read_script(steps, ARRAY_SIZE(steps));
    total = read_entropy(buf, sizeof(buf), &permanent_failure);

    require_size("read call count", read_call_count, 1);
    require_size("used byte count", total, 0);
    require_bool("permanent failure", permanent_failure, true);
}

static void test_short_read_completes_actual_length(void)
{
    const struct read_step steps[] = {
        {.result = 3, .fill = 0x5a},
    };
    uint8_t buf[8] = {0};
    bool permanent_failure = true;
    size_t total;

    set_read_script(steps, ARRAY_SIZE(steps));
    total = read_entropy(buf, sizeof(buf), &permanent_failure);

    require_size("read call count", read_call_count, 1);
    require_size("used byte count", total, 3);
    require_bool("permanent failure", permanent_failure, false);
    require_u8("entropy byte 0", buf[0], 0x5a);
    require_u8("entropy byte 2", buf[2], 0x5a);
}

static void init_test_emu(emu_state_t *emu, hart_t *hart, hart_t **harts);
static void destroy_test_emu(emu_state_t *emu);

static void test_init_opens_entropy_fd_nonblocking(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    int flags;

    rng_fd = -1;
    init_test_emu(&emu, &hart, harts);
    virtio_rng_init(&emu.vrng, &emu);

    flags = fcntl(rng_fd, F_GETFL);
    require_bool("rng fd flags readable", flags >= 0, true);
    require_bool("rng fd nonblocking flag", (flags & O_NONBLOCK) != 0, true);

    destroy_test_emu(&emu);
    close(rng_fd);
    rng_fd = -1;
}

#define TEST_RAM_WORDS 256
#define DESC_ADDR 0x80
#define AVAIL_ADDR 0x100
#define USED_ADDR 0x140
#define DATA_ADDR 0x180
#define DATA_LEN 8

static uint32_t queue_ram[TEST_RAM_WORDS];
static ram_dma_t queue_dma;

static uint16_t read_u16(guest_paddr_t addr)
{
    uint16_t value;

    require_bool("read u16",
                 ram_dma_read(&queue_dma, addr, &value, sizeof(value)), true);
    return value;
}

static uint32_t read_u32(guest_paddr_t addr)
{
    uint32_t value;

    require_bool("read u32",
                 ram_dma_read(&queue_dma, addr, &value, sizeof(value)), true);
    return value;
}

static void write_u16(guest_paddr_t addr, uint16_t value)
{
    require_bool("write u16",
                 ram_dma_write(&queue_dma, addr, &value, sizeof(value)), true);
}

static void write_desc(guest_paddr_t addr, const struct virtq_desc *desc)
{
    require_bool("write desc",
                 ram_dma_write(&queue_dma, addr, desc, sizeof(*desc)), true);
}

static void init_test_emu(emu_state_t *emu, hart_t *hart, hart_t **harts)
{
    memset(emu, 0, sizeof(*emu));
    memset(hart, 0, sizeof(*hart));
    hart->vm = &emu->vm;
    harts[0] = hart;
    emu->vm.n_hart = 1;
    emu->vm.hart = harts;
    emu->ram = queue_ram;
    require_int("lifecycle init", semu_vm_lifecycle_init(&emu->lifecycle), 0);
    require_int("lifecycle running",
                semu_vm_lifecycle_enter_running(&emu->lifecycle), 0);
    require_int("plic lock init", pthread_mutex_init(&emu->plic_lock, NULL), 0);
    ram_dma_init(&emu->ram_dma, queue_ram, sizeof(queue_ram), NULL);
    queue_dma = emu->ram_dma;
    wake_count = 0;
}

static void destroy_test_emu(emu_state_t *emu)
{
    virtio_device_common_destroy(&emu->vrng.common);
    pthread_mutex_destroy(&emu->plic_lock);
    semu_vm_lifecycle_destroy(&emu->lifecycle);
}

static bool source_asserted(emu_state_t *emu, enum semu_irq_source source)
{
    return (emu->plic.active & semu_irq_source_plic_bit(source)) != 0;
}

static void write_rng_reg(virtio_rng_state_t *vrng,
                          uint32_t reg,
                          uint32_t value)
{
    require_int("rng mmio write",
                virtio_mmio_write(&vrng->common, reg, 4, value), 0);
}

static uint32_t read_rng_reg(virtio_rng_state_t *vrng, uint32_t reg)
{
    uint32_t value;

    require_int("rng mmio read",
                virtio_mmio_read(&vrng->common, reg, 4, &value), 0);
    return value;
}

static void init_notify_queue(emu_state_t *emu,
                              hart_t *hart,
                              hart_t **harts,
                              virtio_rng_state_t **vrng)
{
    struct virtq_desc desc;

    memset(queue_ram, 0, sizeof(queue_ram));
    init_test_emu(emu, hart, harts);
    virtio_rng_init(&emu->vrng, emu);
    *vrng = &emu->vrng;

    desc = (struct virtq_desc) {
        .addr = DATA_ADDR,
        .len = DATA_LEN,
        .flags = VIRTIO_DESC_F_WRITE,
    };
    write_desc(DESC_ADDR, &desc);

    write_rng_reg(*vrng, VIRTIO_Status << 2, VIRTIO_STATUS__ACKNOWLEDGE);
    write_rng_reg(*vrng, VIRTIO_Status << 2, VIRTIO_STATUS__DRIVER);
    write_rng_reg(*vrng, VIRTIO_DriverFeaturesSel << 2, 1);
    write_rng_reg(*vrng, VIRTIO_DriverFeatures << 2, 1);
    write_rng_reg(*vrng, VIRTIO_Status << 2, VIRTIO_STATUS__FEATURES_OK);
    write_rng_reg(*vrng, VIRTIO_QueueSel << 2, 0);
    write_rng_reg(*vrng, VIRTIO_QueueNum << 2, 2);
    write_rng_reg(*vrng, VIRTIO_QueueDescLow << 2, DESC_ADDR);
    write_rng_reg(*vrng, VIRTIO_QueueDriverLow << 2, AVAIL_ADDR);
    write_rng_reg(*vrng, VIRTIO_QueueDeviceLow << 2, USED_ADDR);
    write_rng_reg(*vrng, VIRTIO_QueueReady << 2, 1);
    write_rng_reg(*vrng, VIRTIO_Status << 2, VIRTIO_STATUS__DRIVER_OK);

    write_u16(AVAIL_ADDR + 4, 0);
    write_u16(AVAIL_ADDR + 2, 1);
}

static void require_used_completion(const char *name, uint16_t id, uint32_t len)
{
    char label[128];

    snprintf(label, sizeof(label), "%s used idx", name);
    require_u16(label, read_u16(USED_ADDR + 2), 1);
    snprintf(label, sizeof(label), "%s used id", name);
    require_u32(label, read_u32(USED_ADDR + 4), id);
    snprintf(label, sizeof(label), "%s used len", name);
    require_u32(label, read_u32(USED_ADDR + 8), len);
}

static void test_queue_notify_transient_zero_completion_without_reset(int err)
{
    const struct read_step steps[] = {
        {.result = -1, .err = err},
    };
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    virtio_rng_state_t *vrng;

    init_notify_queue(&emu, &hart, harts, &vrng);
    set_read_script(steps, ARRAY_SIZE(steps));
    write_rng_reg(vrng, VIRTIO_QueueNotify << 2, 0);

    require_size("notify transient read call count", read_call_count, 1);
    require_used_completion("notify transient", 0, 0);
    require_u32("notify transient status needs-reset",
                read_rng_reg(vrng, VIRTIO_Status << 2) &
                    VIRTIO_STATUS__DEVICE_NEEDS_RESET,
                0);
    require_u32(
        "notify transient status driver-ok",
        read_rng_reg(vrng, VIRTIO_Status << 2) & VIRTIO_STATUS__DRIVER_OK,
        VIRTIO_STATUS__DRIVER_OK);
    require_u32("notify transient interrupt",
                read_rng_reg(vrng, VIRTIO_InterruptStatus << 2),
                VIRTIO_INT__USED_RING);
    require_bool("notify transient irq line",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VRNG), true);
    require_bool("notify transient pending helper",
                 virtio_rng_irq_pending(vrng), true);
    write_rng_reg(vrng, VIRTIO_InterruptACK << 2, VIRTIO_INT__USED_RING);
    require_bool("notify transient irq ack line",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VRNG), false);
    destroy_test_emu(&emu);
}

static void test_queue_notify_eio_marks_needs_reset_and_conf_change(void)
{
    const struct read_step steps[] = {
        {.result = -1, .err = EIO},
    };
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    virtio_rng_state_t *vrng;

    init_notify_queue(&emu, &hart, harts, &vrng);
    set_read_script(steps, ARRAY_SIZE(steps));
    write_rng_reg(vrng, VIRTIO_QueueNotify << 2, 0);

    require_size("notify eio read call count", read_call_count, 1);
    require_used_completion("notify eio", 0, 0);
    require_u32("notify eio status needs-reset",
                read_rng_reg(vrng, VIRTIO_Status << 2) &
                    VIRTIO_STATUS__DEVICE_NEEDS_RESET,
                VIRTIO_STATUS__DEVICE_NEEDS_RESET);
    require_u32(
        "notify eio status driver-ok",
        read_rng_reg(vrng, VIRTIO_Status << 2) & VIRTIO_STATUS__DRIVER_OK,
        VIRTIO_STATUS__DRIVER_OK);
    require_u32("notify eio interrupt",
                read_rng_reg(vrng, VIRTIO_InterruptStatus << 2),
                VIRTIO_INT__CONF_CHANGE | VIRTIO_INT__USED_RING);
    require_bool("notify eio irq line",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VRNG), true);
    destroy_test_emu(&emu);
}

int main(void)
{
    test_eintr_retries_before_success();
    test_eagain_transient_zero_completion();
    test_ewouldblock_transient_zero_completion();
    test_eio_completes_zero_and_reports_permanent_failure();
    test_short_read_completes_actual_length();
    test_init_opens_entropy_fd_nonblocking();
    test_queue_notify_transient_zero_completion_without_reset(EAGAIN);
    test_queue_notify_transient_zero_completion_without_reset(EWOULDBLOCK);
    test_queue_notify_eio_marks_needs_reset_and_conf_change();
    return 0;
}
