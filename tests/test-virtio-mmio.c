#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "irq-source.h"
#include "lock-order.h"
#include "platform.h"
#include "ram_access.h"
#include "riscv_private.h"
#include "virtio-mmio.h"
#include "virtio.h"

#define REG(reg) ((uint32_t) VIRTIO_##reg << 2)
#define TEST_RAM_SIZE 4096
#define DESC_ADDR 0x100
#define AVAIL_ADDR 0x200
#define USED_ADDR 0x300

static uint32_t ram_words[TEST_RAM_SIZE / 4];
static ram_dma_t dma;
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

struct backend_state {
    int activate_count;
    int activate_ret;
    uint64_t activate_generation;
    struct virtio_device_common *activate_common;
    struct virtq *activate_queues;
    uint16_t activate_num_queues;
    struct virtio_irq *activate_irq;
    int prepare_reset_count;
    uint64_t prepare_reset_old_generation;
    uint64_t prepare_reset_new_generation;
    bool prepare_reset_queue_ready;
    uint16_t prepare_reset_last_avail;
    int reset_count;
    uint64_t reset_old_generation;
    uint64_t reset_new_generation;
    bool reset_queue_ready;
    uint16_t reset_last_avail;
    int notify_count;
    uint16_t notify_queue;
    uint64_t notify_generation;
    int config_read_count;
    int config_write_count;
    uint32_t last_config_read_offset;
    uint32_t last_config_read_size;
    uint32_t last_config_write_offset;
    uint32_t last_config_write_size;
    uint32_t last_config_write_value;
    int notify_ret;
    bool notify_saw_backend_lock_held;
};

static int backend_activate(void *opaque,
                            const struct virtio_activation_context *ctx)
{
    struct backend_state *state = opaque;

    state->activate_count++;
    state->activate_generation = ctx->generation;
    state->activate_common = ctx->common;
    state->activate_queues = ctx->queues;
    state->activate_num_queues = ctx->num_queues;
    state->activate_irq = ctx->irq;
    return state->activate_ret;
}

static int backend_prepare_reset(void *opaque,
                                 uint64_t old_generation,
                                 uint64_t new_generation)
{
    struct backend_state *state = opaque;
    struct virtio_device_common *common = state->activate_common;

    state->prepare_reset_count++;
    state->prepare_reset_old_generation = old_generation;
    state->prepare_reset_new_generation = new_generation;
    state->prepare_reset_queue_ready = common->queues[0].ready;
    state->prepare_reset_last_avail = common->queues[0].last_avail;
    return 0;
}

static int backend_reset(void *opaque,
                         uint64_t old_generation,
                         uint64_t new_generation)
{
    struct backend_state *state = opaque;

    state->reset_count++;
    state->reset_old_generation = old_generation;
    state->reset_new_generation = new_generation;
    state->reset_queue_ready = state->activate_common->queues[0].ready;
    state->reset_last_avail = state->activate_common->queues[0].last_avail;
    return 0;
}

static int backend_notify_queue(void *opaque,
                                uint16_t queue_index,
                                uint64_t generation)
{
    struct backend_state *state = opaque;
    int lock_ret;

    state->notify_count++;
    state->notify_queue = queue_index;
    state->notify_generation = generation;
    lock_ret = pthread_mutex_trylock(&state->activate_common->backend_lock);
    state->notify_saw_backend_lock_held = lock_ret == EBUSY;
    if (lock_ret == 0)
        pthread_mutex_unlock(&state->activate_common->backend_lock);
    return state->notify_ret;
}

static uint32_t backend_read_config(void *opaque,
                                    uint32_t offset,
                                    uint32_t size)
{
    struct backend_state *state = opaque;

    state->config_read_count++;
    state->last_config_read_offset = offset;
    state->last_config_read_size = size;
    return UINT32_C(0xa5000000) | (size << 16) | offset;
}

static void backend_write_config(void *opaque,
                                 uint32_t offset,
                                 uint32_t size,
                                 uint32_t value)
{
    struct backend_state *state = opaque;

    state->config_write_count++;
    state->last_config_write_offset = offset;
    state->last_config_write_size = size;
    state->last_config_write_value = value;
}

static const struct virtio_device_ops backend_ops = {
    .activate = backend_activate,
    .prepare_reset = backend_prepare_reset,
    .reset = backend_reset,
    .notify_queue = backend_notify_queue,
    .read_config = backend_read_config,
    .write_config = backend_write_config,
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

static void require_u64(const char *name, uint64_t got, uint64_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got 0x%llx, want 0x%llx\n", name,
            (unsigned long long) got, (unsigned long long) want);
    exit(1);
}

static void init_one_hart_vm(vm_t *vm, hart_t *hart, hart_t **harts)
{
    memset(hart, 0, sizeof(*hart));
    memset(vm, 0, sizeof(*vm));
    hart->vm = vm;
    harts[0] = hart;
    vm->n_hart = 1;
    vm->hart = harts;
}

static void init_test_emu(emu_state_t *emu, hart_t *hart, hart_t **harts)
{
    memset(emu, 0, sizeof(*emu));
    init_one_hart_vm(&emu->vm, hart, harts);
    require_int("lifecycle init", semu_vm_lifecycle_init(&emu->lifecycle), 0);
    require_int("lifecycle running",
                semu_vm_lifecycle_enter_running(&emu->lifecycle), 0);
    require_int("plic lock init", pthread_mutex_init(&emu->plic_lock, NULL), 0);
    wake_count = 0;
}

static void destroy_test_emu(emu_state_t *emu)
{
    pthread_mutex_destroy(&emu->plic_lock);
    semu_vm_lifecycle_destroy(&emu->lifecycle);
}

static void init_ram(void)
{
    memset(ram_words, 0, sizeof(ram_words));
    ram_dma_init(&dma, ram_words, TEST_RAM_SIZE, NULL);
}

static void init_common(struct virtio_device_common *common,
                        emu_state_t *emu,
                        struct backend_state *backend,
                        uint64_t device_features,
                        uint64_t required_features,
                        const uint16_t *queue_max_sizes,
                        uint16_t num_queues)
{
    struct virtio_device_common_config config = {
        .emu = emu,
        .dma = &dma,
        .irq_source = emu ? SEMU_IRQ_SOURCE_VGPU : SEMU_IRQ_SOURCE_COUNT,
        .device_id = 16,
        .vendor_id = VIRTIO_VENDOR_ID,
        .device_features = device_features,
        .required_features = required_features,
        .queue_max_sizes = queue_max_sizes,
        .num_queues = num_queues,
        .ops = &backend_ops,
        .opaque = backend,
    };

    memset(backend, 0, sizeof(*backend));
    require_int("common init", virtio_device_common_init(common, &config), 0);
    backend->activate_common = common;
}

static uint32_t read_reg(struct virtio_device_common *common, uint32_t reg)
{
    uint32_t value;

    require_int("mmio read", virtio_mmio_read(common, reg, 4, &value), 0);
    return value;
}

static void write_reg(struct virtio_device_common *common,
                      uint32_t reg,
                      uint32_t value)
{
    int ret = virtio_mmio_write(common, reg, 4, value);

    if (ret == 0)
        return;

    fprintf(stderr, "mmio write reg 0x%x value 0x%x: got %d, want 0\n", reg,
            value, ret);
    exit(1);
}

static void configure_queue_regs(struct virtio_device_common *common,
                                 uint16_t queue_size,
                                 guest_paddr_t desc,
                                 guest_paddr_t driver,
                                 guest_paddr_t device)
{
    write_reg(common, REG(QueueNum), queue_size);
    write_reg(common, REG(QueueDescLow), (uint32_t) desc);
    write_reg(common, REG(QueueDescHigh), (uint32_t) (desc >> 32));
    write_reg(common, REG(QueueDriverLow), (uint32_t) driver);
    write_reg(common, REG(QueueDriverHigh), (uint32_t) (driver >> 32));
    write_reg(common, REG(QueueDeviceLow), (uint32_t) device);
    write_reg(common, REG(QueueDeviceHigh), (uint32_t) (device >> 32));
}

static void make_queue_ready(struct virtio_device_common *common)
{
    configure_queue_regs(common, 8, DESC_ADDR, AVAIL_ADDR, USED_ADDR);
    write_reg(common, REG(QueueReady), 1);
}

struct notify_thread_args {
    struct virtio_device_common *common;
    int ret;
};

static void *notify_thread_main(void *opaque)
{
    struct notify_thread_args *args = opaque;

    args->ret = virtio_mmio_write(args->common, REG(QueueNotify), 4, 0);
    return NULL;
}

static void set_driver_features(struct virtio_device_common *common,
                                uint64_t features)
{
    write_reg(common, REG(DriverFeaturesSel), 0);
    write_reg(common, REG(DriverFeatures), (uint32_t) features);
    write_reg(common, REG(DriverFeaturesSel), 1);
    write_reg(common, REG(DriverFeatures), (uint32_t) (features >> 32));
    write_reg(common, REG(DriverFeaturesSel), 0);
}

static void test_identity_and_feature_selection(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_device_common common;
    struct backend_state backend;
    const uint16_t queue_max_sizes[] = {8};

    init_ram();
    init_test_emu(&emu, &hart, harts);
    init_common(&common, &emu, &backend, UINT64_C(0x0000000200000001), 0,
                queue_max_sizes, ARRAY_SIZE(queue_max_sizes));

    require_u32("magic", read_reg(&common, REG(MagicValue)),
                VIRTIO_MMIO_MAGIC_VALUE);
    require_u32("version", read_reg(&common, REG(Version)),
                VIRTIO_MMIO_VERSION_VALUE);
    require_u32("device id", read_reg(&common, REG(DeviceID)), 16);
    require_u32("vendor", read_reg(&common, REG(VendorID)), VIRTIO_VENDOR_ID);
    require_u32("features low", read_reg(&common, REG(DeviceFeatures)), 1);
    write_reg(&common, REG(DeviceFeaturesSel), 1);
    require_u32("features high", read_reg(&common, REG(DeviceFeatures)), 2);
    write_reg(&common, REG(DeviceFeaturesSel), 2);
    require_u32("features invalid sel", read_reg(&common, REG(DeviceFeatures)),
                0);

    set_driver_features(&common, UINT64_C(0x0000000200000001));
    require_u32("driver features low", read_reg(&common, REG(DriverFeatures)),
                1);
    write_reg(&common, REG(DriverFeaturesSel), 1);
    require_u32("driver features high", read_reg(&common, REG(DriverFeatures)),
                2);

    virtio_device_common_destroy(&common);
    destroy_test_emu(&emu);
}

static void test_features_ok_validation_and_immutability(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_device_common common;
    struct backend_state backend;
    const uint16_t queue_max_sizes[] = {8};

    init_ram();
    init_test_emu(&emu, &hart, harts);
    init_common(&common, &emu, &backend, 0x3, 0x2, queue_max_sizes,
                ARRAY_SIZE(queue_max_sizes));
    write_reg(&common, REG(Status),
              VIRTIO_STATUS__ACKNOWLEDGE | VIRTIO_STATUS__DRIVER);

    set_driver_features(&common, 0x1);
    require_int(
        "missing required feature",
        virtio_mmio_write(&common, REG(Status), 4, VIRTIO_STATUS__FEATURES_OK),
        -EINVAL);
    require_u32("missing required leaves FEATURES_OK clear",
                read_reg(&common, REG(Status)) & VIRTIO_STATUS__FEATURES_OK, 0);

    set_driver_features(&common, 0x7);
    require_int(
        "unsupported feature",
        virtio_mmio_write(&common, REG(Status), 4, VIRTIO_STATUS__FEATURES_OK),
        -EINVAL);
    require_u32("unsupported leaves FEATURES_OK clear",
                read_reg(&common, REG(Status)) & VIRTIO_STATUS__FEATURES_OK, 0);

    set_driver_features(&common, 0x3);
    write_reg(&common, REG(Status), VIRTIO_STATUS__FEATURES_OK);
    require_u32("FEATURES_OK set",
                read_reg(&common, REG(Status)) & VIRTIO_STATUS__FEATURES_OK,
                VIRTIO_STATUS__FEATURES_OK);
    require_int("driver features immutable after FEATURES_OK",
                virtio_mmio_write(&common, REG(DriverFeatures), 4, 0), -EPERM);
    require_int("driver feature selector immutable after FEATURES_OK",
                virtio_mmio_write(&common, REG(DriverFeaturesSel), 4, 1),
                -EPERM);
    require_u32("driver feature selector unchanged",
                read_reg(&common, REG(DriverFeaturesSel)), 0);
    require_u64("driver features unchanged", common.driver_features, 0x3);

    virtio_device_common_destroy(&common);
    destroy_test_emu(&emu);
}

static void test_queue_selection_and_ready_freezes_config(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_device_common common;
    struct backend_state backend;
    const uint16_t queue_max_sizes[] = {8};

    init_ram();
    init_test_emu(&emu, &hart, harts);
    init_common(&common, &emu, &backend, 0, 0, queue_max_sizes,
                ARRAY_SIZE(queue_max_sizes));

    write_reg(&common, REG(QueueSel), 9);
    require_u32("invalid QueueNumMax", read_reg(&common, REG(QueueNumMax)), 0);
    require_int("invalid QueueNum write",
                virtio_mmio_write(&common, REG(QueueNum), 4, 8), -EINVAL);

    write_reg(&common, REG(QueueSel), 0);
    require_u32("QueueNumMax", read_reg(&common, REG(QueueNumMax)), 8);
    require_int("zero QueueNum",
                virtio_mmio_write(&common, REG(QueueNum), 4, 0), -EINVAL);
    require_int("too large QueueNum",
                virtio_mmio_write(&common, REG(QueueNum), 4, 9), -EINVAL);

    write_reg(&common, REG(QueueNum), 8);
    write_reg(&common, REG(QueueDescLow), 0x12345678);
    write_reg(&common, REG(QueueDescHigh), 1);
    require_u32("QueueDescLow assembled", read_reg(&common, REG(QueueDescLow)),
                0x12345678);
    require_u32("QueueDescHigh assembled",
                read_reg(&common, REG(QueueDescHigh)), 1);
    require_int("out-of-range high GPA rejected on ready",
                virtio_mmio_write(&common, REG(QueueReady), 4, 1), -EFAULT);

    configure_queue_regs(&common, 8, DESC_ADDR, AVAIL_ADDR, USED_ADDR);
    write_reg(&common, REG(QueueReady), 1);
    require_u32("QueueReady set", read_reg(&common, REG(QueueReady)), 1);
    require_true("virtq ready", common.queues[0].ready);
    require_u16("virtq size", common.queues[0].queue_size, 8);
    require_u64("virtq desc", common.queues[0].desc_addr, DESC_ADDR);

    require_int("QueueNum frozen",
                virtio_mmio_write(&common, REG(QueueNum), 4, 4), -EPERM);
    require_int("QueueDesc frozen",
                virtio_mmio_write(&common, REG(QueueDescLow), 4, 0x400),
                -EPERM);
    require_int("QueueReady direct clear rejected",
                virtio_mmio_write(&common, REG(QueueReady), 4, 0), -EPERM);
    require_u32("QueueReady remains set", read_reg(&common, REG(QueueReady)),
                1);

    virtio_device_common_destroy(&common);
    destroy_test_emu(&emu);
}

static void test_queue_notify_does_not_drain_queue(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_device_common common;
    struct backend_state backend;
    const uint16_t queue_max_sizes[] = {8};

    init_ram();
    init_test_emu(&emu, &hart, harts);
    init_common(&common, &emu, &backend, 0, 0, queue_max_sizes,
                ARRAY_SIZE(queue_max_sizes));
    make_queue_ready(&common);

    write_reg(&common, REG(QueueNotify), 0);
    require_int("notify count", backend.notify_count, 1);
    require_u16("notify queue", backend.notify_queue, 0);
    require_u64("notify generation", backend.notify_generation,
                common.generation);
    require_false("notify does not run under backend lock",
                  backend.notify_saw_backend_lock_held);
    require_u16("notify did not drain", common.queues[0].last_avail, 0);
    require_int("notify invalid queue",
                virtio_mmio_write(&common, REG(QueueNotify), 4, 1), -EINVAL);

    virtio_device_common_destroy(&common);
    destroy_test_emu(&emu);
}

static void test_queue_notify_succeeds_while_lifecycle_accepting(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_device_common common;
    struct backend_state backend;
    const uint16_t queue_max_sizes[] = {8};

    init_ram();
    init_test_emu(&emu, &hart, harts);
    init_common(&common, &emu, &backend, 0, 0, queue_max_sizes,
                ARRAY_SIZE(queue_max_sizes));
    make_queue_ready(&common);

    require_true("lifecycle accepting",
                 semu_vm_accepting_device_work(&emu.lifecycle));
    require_int("notify while accepting",
                virtio_mmio_write(&common, REG(QueueNotify), 4, 0), 0);
    require_int("notify count", backend.notify_count, 1);
    require_u64("notify carries generation", backend.notify_generation,
                common.generation);

    virtio_device_common_destroy(&common);
    destroy_test_emu(&emu);
}

static void test_queue_notify_is_gated_when_lifecycle_not_accepting(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_device_common common;
    struct backend_state backend;
    const uint16_t queue_max_sizes[] = {8};

    init_ram();
    init_test_emu(&emu, &hart, harts);
    init_common(&common, &emu, &backend, 0, 0, queue_max_sizes,
                ARRAY_SIZE(queue_max_sizes));
    make_queue_ready(&common);

    require_int("pause request",
                semu_vm_lifecycle_request_pause(&emu.lifecycle), 0);
    require_false("pause request stops device work",
                  semu_vm_accepting_device_work(&emu.lifecycle));
    require_int("paused notify ignored",
                virtio_mmio_write(&common, REG(QueueNotify), 4, 0), 0);
    require_int("paused notify skipped backend", backend.notify_count, 0);

    require_int("paused", semu_vm_lifecycle_enter_paused(&emu.lifecycle), 0);
    require_int("running after pause",
                semu_vm_lifecycle_enter_running(&emu.lifecycle), 0);
    require_int("resetting", semu_vm_lifecycle_enter_resetting(&emu.lifecycle),
                0);
    require_false("resetting stops device work",
                  semu_vm_accepting_device_work(&emu.lifecycle));
    require_int("resetting notify ignored",
                virtio_mmio_write(&common, REG(QueueNotify), 4, 0), 0);
    require_int("resetting notify skipped backend", backend.notify_count, 0);

    require_int("running after reset",
                semu_vm_lifecycle_enter_running(&emu.lifecycle), 0);
    require_int("stopping", semu_vm_lifecycle_enter_stopping(&emu.lifecycle),
                0);
    require_false("stopping stops device work",
                  semu_vm_accepting_device_work(&emu.lifecycle));
    require_int("stopping notify ignored",
                virtio_mmio_write(&common, REG(QueueNotify), 4, 0), 0);
    require_int("stopping notify skipped backend", backend.notify_count, 0);

    virtio_device_common_destroy(&common);
    destroy_test_emu(&emu);
}

static void test_queue_notify_without_emu_has_no_lifecycle_gate(void)
{
    struct virtio_device_common common;
    struct backend_state backend;
    const uint16_t queue_max_sizes[] = {8};

    init_ram();
    init_common(&common, NULL, &backend, 0, 0, queue_max_sizes,
                ARRAY_SIZE(queue_max_sizes));
    make_queue_ready(&common);

    require_int("notify without emu",
                virtio_mmio_write(&common, REG(QueueNotify), 4, 0), 0);
    require_int("notify count", backend.notify_count, 1);
    require_u64("notify generation", backend.notify_generation,
                common.generation);

    virtio_device_common_destroy(&common);
}

static void test_queue_notify_stale_generation_is_canceled_at_completion(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_device_common common;
    struct backend_state backend;
    struct notify_thread_args args = {.common = &common, .ret = -EAGAIN};
    pthread_t thread;
    const uint16_t queue_max_sizes[] = {8};
    uint64_t old_generation;
    bool saw_lifecycle_gate = false;

    init_ram();
    init_test_emu(&emu, &hart, harts);
    init_common(&common, &emu, &backend, 0, 0, queue_max_sizes,
                ARRAY_SIZE(queue_max_sizes));
    make_queue_ready(&common);
    old_generation = common.generation;

    require_int("hold backend lock", pthread_mutex_lock(&common.backend_lock),
                0);
    require_int("hold transport lock",
                pthread_mutex_lock(&common.transport_lock), 0);
    require_int("start notify thread",
                pthread_create(&thread, NULL, notify_thread_main, &args), 0);

    for (int i = 0; i < 100000; i++) {
        int ret = pthread_mutex_trylock(&emu.lifecycle.lock);

        if (ret == EBUSY) {
            saw_lifecycle_gate = true;
            break;
        }
        require_int("try lifecycle lock while notify starts", ret, 0);
        require_int("unlock lifecycle probe",
                    pthread_mutex_unlock(&emu.lifecycle.lock), 0);
        sched_yield();
    }
    require_true("notify entered lifecycle gate", saw_lifecycle_gate);

    require_int("release transport for notify capture",
                pthread_mutex_unlock(&common.transport_lock), 0);
    require_int("wait for notify capture",
                pthread_mutex_lock(&emu.lifecycle.lock), 0);

    require_int("lock transport for reset generation",
                pthread_mutex_lock(&common.transport_lock), 0);
    require_u64("stale test starts from old generation", common.generation,
                old_generation);
    common.generation++;
    virtq_init(&common.queues[0]);
    require_int("unlock transport after reset generation",
                pthread_mutex_unlock(&common.transport_lock), 0);
    require_int("release lifecycle after reset generation",
                pthread_mutex_unlock(&emu.lifecycle.lock), 0);

    require_int("release backend lock",
                pthread_mutex_unlock(&common.backend_lock), 0);
    require_int("join notify thread", pthread_join(thread, NULL), 0);
    require_int("stale notify canceled", args.ret, -ECANCELED);
    require_int("stale notify skipped backend", backend.notify_count, 0);

    virtio_device_common_destroy(&common);
    destroy_test_emu(&emu);
}

static void test_queue_notify_lifecycle_stop_before_backend_is_ignored(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_device_common common;
    struct backend_state backend;
    struct notify_thread_args args = {.common = &common, .ret = -EAGAIN};
    pthread_t thread;
    const uint16_t queue_max_sizes[] = {8};
    bool saw_lifecycle_gate = false;

    init_ram();
    init_test_emu(&emu, &hart, harts);
    init_common(&common, &emu, &backend, 0, 0, queue_max_sizes,
                ARRAY_SIZE(queue_max_sizes));
    make_queue_ready(&common);

    require_int("hold backend lock", pthread_mutex_lock(&common.backend_lock),
                0);
    require_int("hold transport lock",
                pthread_mutex_lock(&common.transport_lock), 0);
    require_int("start notify thread",
                pthread_create(&thread, NULL, notify_thread_main, &args), 0);

    for (int i = 0; i < 100000; i++) {
        int ret = pthread_mutex_trylock(&emu.lifecycle.lock);

        if (ret == EBUSY) {
            saw_lifecycle_gate = true;
            break;
        }
        require_int("try lifecycle lock while notify starts", ret, 0);
        require_int("unlock lifecycle probe",
                    pthread_mutex_unlock(&emu.lifecycle.lock), 0);
        sched_yield();
    }
    require_true("notify entered lifecycle gate", saw_lifecycle_gate);

    require_int("release transport for notify capture",
                pthread_mutex_unlock(&common.transport_lock), 0);
    require_int("wait for notify capture",
                pthread_mutex_lock(&emu.lifecycle.lock), 0);
    require_int("release lifecycle after capture",
                pthread_mutex_unlock(&emu.lifecycle.lock), 0);

    require_int("pause request before backend scheduling",
                semu_vm_lifecycle_request_pause(&emu.lifecycle), 0);
    require_false("pause request stops device work",
                  semu_vm_accepting_device_work(&emu.lifecycle));

    require_int("release backend lock",
                pthread_mutex_unlock(&common.backend_lock), 0);
    require_int("join notify thread", pthread_join(thread, NULL), 0);
    require_int("lifecycle-stopped notify ignored", args.ret, 0);
    require_int("lifecycle-stopped notify skipped backend",
                backend.notify_count, 0);

    virtio_device_common_destroy(&common);
    destroy_test_emu(&emu);
}

static void test_queue_notify_propagates_async_enqueue_failure(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_device_common common;
    struct backend_state backend;
    const uint16_t queue_max_sizes[] = {8};

    init_ram();
    init_test_emu(&emu, &hart, harts);
    init_common(&common, &emu, &backend, 0, 0, queue_max_sizes,
                ARRAY_SIZE(queue_max_sizes));
    make_queue_ready(&common);

    backend.notify_ret = -EAGAIN;
    require_int("notify async enqueue failure",
                virtio_mmio_write(&common, REG(QueueNotify), 4, 0), -EAGAIN);
    require_int("notify attempted once", backend.notify_count, 1);

    virtio_device_common_destroy(&common);
    destroy_test_emu(&emu);
}

static void test_queue_notify_rejects_tracked_rank_inversion(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_device_common common;
    struct backend_state backend;
    struct semu_lock_order_guard backend_guard;
    const uint16_t queue_max_sizes[] = {8};

    init_ram();
    init_test_emu(&emu, &hart, harts);
    init_common(&common, &emu, &backend, 0, 0, queue_max_sizes,
                ARRAY_SIZE(queue_max_sizes));
    make_queue_ready(&common);

    require_int(
        "enter tracked backend rank",
        semu_lock_order_enter(SEMU_LOCK_RANK_BACKEND_LOCAL, &backend_guard), 0);
    require_int("QueueNotify rejects lifecycle below backend",
                virtio_mmio_write(&common, REG(QueueNotify), 4, 0), -EDEADLK);
    require_int("inverted notify skipped backend", backend.notify_count, 0);
    require_int("leave tracked backend rank",
                semu_lock_order_leave(&backend_guard), 0);

    virtio_device_common_destroy(&common);
    destroy_test_emu(&emu);
}

static void test_status_order_rejects_bare_driver_ok(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_device_common common;
    struct backend_state backend;
    const uint16_t queue_max_sizes[] = {8};

    init_ram();
    init_test_emu(&emu, &hart, harts);
    init_common(&common, &emu, &backend, 0, 0, queue_max_sizes,
                ARRAY_SIZE(queue_max_sizes));
    make_queue_ready(&common);

    require_int(
        "bare DRIVER_OK rejected",
        virtio_mmio_write(&common, REG(Status), 4, VIRTIO_STATUS__DRIVER_OK),
        -EINVAL);
    require_u32("bare DRIVER_OK not stored",
                read_reg(&common, REG(Status)) & VIRTIO_STATUS__DRIVER_OK, 0);
    require_int("bare DRIVER_OK does not activate", backend.activate_count, 0);

    virtio_device_common_destroy(&common);
    destroy_test_emu(&emu);
}

static void test_driver_ok_activation_edge(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_device_common common;
    struct backend_state backend;
    const uint16_t queue_max_sizes[] = {8};

    init_ram();
    init_test_emu(&emu, &hart, harts);
    init_common(&common, &emu, &backend, 0x1, 0x1, queue_max_sizes,
                ARRAY_SIZE(queue_max_sizes));
    set_driver_features(&common, 0x1);
    write_reg(&common, REG(Status),
              VIRTIO_STATUS__ACKNOWLEDGE | VIRTIO_STATUS__DRIVER);
    write_reg(&common, REG(Status), VIRTIO_STATUS__FEATURES_OK);
    require_int(
        "DRIVER_OK before queue ready",
        virtio_mmio_write(&common, REG(Status), 4, VIRTIO_STATUS__DRIVER_OK),
        -EINVAL);
    require_u32("DRIVER_OK not stored before queues ready",
                read_reg(&common, REG(Status)) & VIRTIO_STATUS__DRIVER_OK, 0);

    make_queue_ready(&common);
    write_reg(&common, REG(Status), VIRTIO_STATUS__DRIVER_OK);
    require_int("activate count", backend.activate_count, 1);
    require_u64("activate generation", backend.activate_generation, 1);
    require_true("activate common", backend.activate_common == &common);
    require_true("activate queues", backend.activate_queues == common.queues);
    require_u16("activate num queues", backend.activate_num_queues, 1);
    require_true("activate irq", backend.activate_irq == &common.irq);

    write_reg(&common, REG(Status), VIRTIO_STATUS__DRIVER_OK);
    require_int("activate only once per edge", backend.activate_count, 1);

    virtio_device_common_destroy(&common);
    destroy_test_emu(&emu);
}

static void test_activation_failure_marks_needs_reset(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_device_common common;
    struct backend_state backend;
    const uint16_t queue_max_sizes[] = {8};

    init_ram();
    init_test_emu(&emu, &hart, harts);
    init_common(&common, &emu, &backend, 0, 0, queue_max_sizes,
                ARRAY_SIZE(queue_max_sizes));
    backend.activate_ret = -EIO;
    make_queue_ready(&common);
    write_reg(&common, REG(Status),
              VIRTIO_STATUS__ACKNOWLEDGE | VIRTIO_STATUS__DRIVER);
    write_reg(&common, REG(Status), VIRTIO_STATUS__FEATURES_OK);

    require_int(
        "activation failure",
        virtio_mmio_write(&common, REG(Status), 4, VIRTIO_STATUS__DRIVER_OK),
        -EIO);
    require_u32(
        "needs reset after activation failure",
        read_reg(&common, REG(Status)) & VIRTIO_STATUS__DEVICE_NEEDS_RESET,
        VIRTIO_STATUS__DEVICE_NEEDS_RESET);

    virtio_device_common_destroy(&common);
    destroy_test_emu(&emu);
}

static void test_interrupt_status_and_ack(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_device_common common;
    struct backend_state backend;
    const uint16_t queue_max_sizes[] = {8};

    init_ram();
    init_test_emu(&emu, &hart, harts);
    init_common(&common, &emu, &backend, 0, 0, queue_max_sizes,
                ARRAY_SIZE(queue_max_sizes));

    virtio_irq_trigger(&common.irq,
                       VIRTIO_INT__USED_RING | VIRTIO_INT__CONF_CHANGE);
    require_u32("interrupt status", read_reg(&common, REG(InterruptStatus)),
                VIRTIO_INT__USED_RING | VIRTIO_INT__CONF_CHANGE);
    write_reg(&common, REG(InterruptACK), VIRTIO_INT__USED_RING);
    require_u32("interrupt ack selected",
                read_reg(&common, REG(InterruptStatus)),
                VIRTIO_INT__CONF_CHANGE);
    write_reg(&common, REG(InterruptACK), VIRTIO_INT__CONF_CHANGE);
    require_u32("interrupt ack all", read_reg(&common, REG(InterruptStatus)),
                0);

    virtio_device_common_destroy(&common);
    destroy_test_emu(&emu);
}

static void test_reset_clears_transport_without_decrementing_config_generation(
    void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_device_common common;
    struct backend_state backend;
    const uint16_t queue_max_sizes[] = {8};
    uint64_t old_generation;

    init_ram();
    init_test_emu(&emu, &hart, harts);
    init_common(&common, &emu, &backend, 0x1, 0, queue_max_sizes,
                ARRAY_SIZE(queue_max_sizes));
    set_driver_features(&common, 0x1);
    make_queue_ready(&common);
    write_reg(&common, REG(Status),
              VIRTIO_STATUS__ACKNOWLEDGE | VIRTIO_STATUS__DRIVER);
    write_reg(&common, REG(Status), VIRTIO_STATUS__FEATURES_OK);
    write_reg(&common, REG(Status), VIRTIO_STATUS__DRIVER_OK);
    virtio_irq_trigger(&common.irq, VIRTIO_INT__USED_RING);
    common.config_generation = 7;
    common.queues[0].last_avail = 5;
    old_generation = common.generation;

    write_reg(&common, REG(Status), 0);
    require_u32("reset status", read_reg(&common, REG(Status)), 0);
    require_u64("reset generation", common.generation, old_generation + 1);
    require_int("prepare reset callback count", backend.prepare_reset_count, 1);
    require_u64("prepare reset callback old",
                backend.prepare_reset_old_generation, old_generation);
    require_u64("prepare reset callback new",
                backend.prepare_reset_new_generation, old_generation + 1);
    require_true("prepare reset sees queue ready",
                 backend.prepare_reset_queue_ready);
    require_u16("prepare reset sees old last_avail",
                backend.prepare_reset_last_avail, 5);
    require_int("reset callback count", backend.reset_count, 1);
    require_u64("reset callback old", backend.reset_old_generation,
                old_generation);
    require_u64("reset callback new", backend.reset_new_generation,
                old_generation + 1);
    require_false("reset callback sees queue cleared",
                  backend.reset_queue_ready);
    require_u16("reset callback sees last_avail cleared",
                backend.reset_last_avail, 0);
    require_false("reset clears queue ready", common.queues[0].ready);
    require_u32("reset clears QueueReady", read_reg(&common, REG(QueueReady)),
                0);
    require_u32("reset clears QueueNum", read_reg(&common, REG(QueueNum)), 0);
    require_u64("reset clears driver features", common.driver_features, 0);
    require_u32("reset clears isr", read_reg(&common, REG(InterruptStatus)), 0);
    require_u32("config generation not decremented",
                read_reg(&common, REG(ConfigGeneration)), 7);

    virtio_device_common_destroy(&common);
    destroy_test_emu(&emu);
}

static void test_config_and_unimplemented_common_registers(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_device_common common;
    struct backend_state backend;
    const uint16_t queue_max_sizes[] = {8};
    uint32_t value;

    init_ram();
    init_test_emu(&emu, &hart, harts);
    init_common(&common, &emu, &backend, 0, 0, queue_max_sizes,
                ARRAY_SIZE(queue_max_sizes));

    require_int("config read",
                virtio_mmio_read(&common, REG(Config) + 4, 2, &value), 0);
    require_u32("config read value", value, UINT32_C(0xa5020004));
    require_int("config read count", backend.config_read_count, 1);
    require_u32("config read offset", backend.last_config_read_offset, 4);
    require_u32("config read size", backend.last_config_read_size, 2);

    require_int("config write",
                virtio_mmio_write(&common, REG(Config) + 8, 1, 0x55), 0);
    require_int("config write count", backend.config_write_count, 1);
    require_u32("config write offset", backend.last_config_write_offset, 8);
    require_u32("config write size", backend.last_config_write_size, 1);
    require_u32("config write value", backend.last_config_write_value, 0x55);

    require_u32("missing SHM len low", read_reg(&common, REG(SHMLenLow)),
                UINT32_MAX);
    require_u32("missing SHM len high", read_reg(&common, REG(SHMLenHigh)),
                UINT32_MAX);
    require_u32("SHM base high", read_reg(&common, REG(SHMBaseHigh)), 0);
    require_u32("QueueReset read", read_reg(&common, REG(QueueReset)), 0);
    write_reg(&common, REG(QueueReset), 1);
    require_int("read-only identity write",
                virtio_mmio_write(&common, REG(MagicValue), 4, 0), -EPERM);

    virtio_device_common_destroy(&common);
    destroy_test_emu(&emu);
}

static void test_requested_irq_init_failure_is_reported(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_device_common common;
    struct backend_state backend;
    const uint16_t queue_max_sizes[] = {8};
    struct virtio_device_common_config config = {
        .dma = &dma,
        .irq_source = SEMU_IRQ_SOURCE_VGPU,
        .device_id = 16,
        .vendor_id = VIRTIO_VENDOR_ID,
        .queue_max_sizes = queue_max_sizes,
        .num_queues = ARRAY_SIZE(queue_max_sizes),
        .ops = &backend_ops,
        .opaque = &backend,
    };

    init_ram();
    memset(&backend, 0, sizeof(backend));
    require_int("requested irq needs emu",
                virtio_device_common_init(&common, &config), -EINVAL);

    init_test_emu(&emu, &hart, harts);
    config.emu = &emu;
    config.irq_source = (enum semu_irq_source)(SEMU_IRQ_SOURCE_COUNT + 1);
    require_int("requested invalid irq source fails",
                virtio_device_common_init(&common, &config), -EINVAL);

    config.emu = NULL;
    config.irq_source = SEMU_IRQ_SOURCE_COUNT;
    require_int("explicit no-irq mode",
                virtio_device_common_init(&common, &config), 0);
    require_false("no-irq mode leaves irq uninitialized",
                  common.irq_initialized);
    virtio_device_common_destroy(&common);
    destroy_test_emu(&emu);
}

int main(void)
{
    test_identity_and_feature_selection();
    test_features_ok_validation_and_immutability();
    test_queue_selection_and_ready_freezes_config();
    test_queue_notify_does_not_drain_queue();
    test_queue_notify_succeeds_while_lifecycle_accepting();
    test_queue_notify_is_gated_when_lifecycle_not_accepting();
    test_queue_notify_without_emu_has_no_lifecycle_gate();
    test_queue_notify_stale_generation_is_canceled_at_completion();
    test_queue_notify_lifecycle_stop_before_backend_is_ignored();
    test_queue_notify_propagates_async_enqueue_failure();
    test_queue_notify_rejects_tracked_rank_inversion();
    test_status_order_rejects_bare_driver_ok();
    test_driver_ok_activation_edge();
    test_activation_failure_marks_needs_reset();
    test_interrupt_status_and_ack();
    test_reset_clears_transport_without_decrementing_config_generation();
    test_config_and_unimplemented_common_registers();
    test_requested_irq_init_failure_is_reported();

    return 0;
}
