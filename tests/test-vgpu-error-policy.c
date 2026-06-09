#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "virtio-gpu.h"
#include "virtio.h"

void semu_wake_interruptible_harts(emu_state_t *emu UNUSED) {}

#define REQUIRE_ATOMIC_U64_COUNTER(expr)                                 \
    _Static_assert(_Generic(&(expr), _Atomic uint64_t *: 1, default: 0), \
                   #expr " must be _Atomic uint64_t")
#define REQUIRE_PLAIN_U64_COUNTER(expr)                       \
    _Static_assert(_Generic((expr), uint64_t: 1, default: 0), \
                   #expr " must be uint64_t")

REQUIRE_ATOMIC_U64_COUNTER(
    ((virtio_gpu_state_t *) 0)->sw_backend.display_counters.full_frame_bytes);
REQUIRE_ATOMIC_U64_COUNTER(
    ((virtio_gpu_state_t *) 0)->sw_backend.display_counters.dirty_rect_bytes);
REQUIRE_ATOMIC_U64_COUNTER(
    ((virtio_gpu_state_t *) 0)->sw_backend.display_counters.queue_backpressure);
REQUIRE_ATOMIC_U64_COUNTER(
    ((virtio_gpu_state_t *) 0)->sw_backend.display_counters.publish_queue_full);
REQUIRE_ATOMIC_U64_COUNTER(
    ((virtio_gpu_state_t *) 0)
        ->sw_backend.display_counters.publish_backpressure);
REQUIRE_ATOMIC_U64_COUNTER(
    ((virtio_gpu_state_t *) 0)
        ->sw_backend.display_counters.publish_unavailable);
REQUIRE_ATOMIC_U64_COUNTER(
    ((virtio_gpu_state_t *) 0)
        ->sw_backend.display_counters.publish_can_publish_false);
REQUIRE_ATOMIC_U64_COUNTER(
    ((virtio_gpu_state_t *) 0)->sw_backend.display_counters.dirty_merges);
REQUIRE_ATOMIC_U64_COUNTER(
    ((virtio_gpu_state_t *) 0)
        ->sw_backend.display_counters.full_resync_escalations);
REQUIRE_ATOMIC_U64_COUNTER(
    ((virtio_gpu_state_t *) 0)->debug_counters.actor_notify_ok);
REQUIRE_ATOMIC_U64_COUNTER(
    ((virtio_gpu_state_t *) 0)->debug_counters.actor_notify_eagain);
REQUIRE_ATOMIC_U64_COUNTER(
    ((virtio_gpu_state_t *) 0)->debug_counters.actor_notify_eio);
REQUIRE_ATOMIC_U64_COUNTER(
    ((virtio_gpu_state_t *) 0)->debug_counters.actor_notify_einval);
REQUIRE_ATOMIC_U64_COUNTER(
    ((virtio_gpu_state_t *) 0)->debug_counters.actor_notify_other_error);
REQUIRE_ATOMIC_U64_COUNTER(
    ((virtio_gpu_state_t *) 0)->debug_counters.actor_drain_calls);
REQUIRE_ATOMIC_U64_COUNTER(
    ((virtio_gpu_state_t *) 0)->debug_counters.actor_queue_index_invalid);
REQUIRE_ATOMIC_U64_COUNTER(
    ((virtio_gpu_state_t *) 0)->debug_counters.actor_stale_generation);
REQUIRE_ATOMIC_U64_COUNTER(
    ((virtio_gpu_state_t *) 0)->debug_counters.actor_completion_rejected);
REQUIRE_ATOMIC_U64_COUNTER(
    ((virtio_gpu_state_t *) 0)->debug_counters.actor_failed_callbacks);
REQUIRE_PLAIN_U64_COUNTER(
    ((struct virtio_gpu_debug_counters *) 0)->display.full_frame_bytes);
REQUIRE_PLAIN_U64_COUNTER(
    ((struct virtio_gpu_debug_counters *) 0)->display.dirty_rect_bytes);
REQUIRE_PLAIN_U64_COUNTER(
    ((struct virtio_gpu_debug_counters *) 0)->display.queue_backpressure);
REQUIRE_PLAIN_U64_COUNTER(
    ((struct virtio_gpu_debug_counters *) 0)->display.publish_queue_full);
REQUIRE_PLAIN_U64_COUNTER(
    ((struct virtio_gpu_debug_counters *) 0)->display.publish_backpressure);
REQUIRE_PLAIN_U64_COUNTER(
    ((struct virtio_gpu_debug_counters *) 0)->display.publish_unavailable);
REQUIRE_PLAIN_U64_COUNTER(((struct virtio_gpu_debug_counters *) 0)
                              ->display.publish_can_publish_false);
REQUIRE_PLAIN_U64_COUNTER(
    ((struct virtio_gpu_debug_counters *) 0)->display.dirty_merges);
REQUIRE_PLAIN_U64_COUNTER(
    ((struct virtio_gpu_debug_counters *) 0)->display.full_resync_escalations);
REQUIRE_PLAIN_U64_COUNTER(
    ((struct virtio_gpu_debug_counters *) 0)->actor_notify_ok);
REQUIRE_PLAIN_U64_COUNTER(
    ((struct virtio_gpu_debug_counters *) 0)->actor_notify_eagain);
REQUIRE_PLAIN_U64_COUNTER(
    ((struct virtio_gpu_debug_counters *) 0)->actor_notify_eio);
REQUIRE_PLAIN_U64_COUNTER(
    ((struct virtio_gpu_debug_counters *) 0)->actor_notify_einval);
REQUIRE_PLAIN_U64_COUNTER(
    ((struct virtio_gpu_debug_counters *) 0)->actor_notify_other_error);
REQUIRE_PLAIN_U64_COUNTER(
    ((struct virtio_gpu_debug_counters *) 0)->actor_drain_calls);
REQUIRE_PLAIN_U64_COUNTER(
    ((struct virtio_gpu_debug_counters *) 0)->actor_queue_index_invalid);
REQUIRE_PLAIN_U64_COUNTER(
    ((struct virtio_gpu_debug_counters *) 0)->actor_stale_generation);
REQUIRE_PLAIN_U64_COUNTER(
    ((struct virtio_gpu_debug_counters *) 0)->actor_completion_rejected);
REQUIRE_PLAIN_U64_COUNTER(
    ((struct virtio_gpu_debug_counters *) 0)->actor_failed_callbacks);

static void require_int(const char *name, int got, int want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %d, want %d\n", name, got, want);
    exit(1);
}

static void require_u32(const char *name, uint32_t got, uint32_t want)
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

static void require_false(const char *name, bool got)
{
    if (!got)
        return;

    fprintf(stderr, "%s: got true, want false\n", name);
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

static void require_debug_counters_zero(
    const struct virtio_gpu_debug_counters *counters)
{
    require_u64("full-frame bytes", counters->display.full_frame_bytes, 0);
    require_u64("dirty-rect bytes", counters->display.dirty_rect_bytes, 0);
    require_u64("queue backpressure", counters->display.queue_backpressure, 0);
    require_u64("queue full", counters->display.publish_queue_full, 0);
    require_u64("publish backpressure", counters->display.publish_backpressure,
                0);
    require_u64("publish unavailable", counters->display.publish_unavailable,
                0);
    require_u64("can-publish false",
                counters->display.publish_can_publish_false, 0);
    require_u64("dirty merges", counters->display.dirty_merges, 0);
    require_u64("full resync escalations",
                counters->display.full_resync_escalations, 0);
    require_u64("notify ok", counters->actor_notify_ok, 0);
    require_u64("notify eagain", counters->actor_notify_eagain, 0);
    require_u64("notify eio", counters->actor_notify_eio, 0);
    require_u64("notify einval", counters->actor_notify_einval, 0);
    require_u64("actor drain calls", counters->actor_drain_calls, 0);
    require_u64("actor queue invalid", counters->actor_queue_index_invalid, 0);
    require_u64("actor stale generation", counters->actor_stale_generation, 0);
    require_u64("actor completion rejected",
                counters->actor_completion_rejected, 0);
    require_u64("actor failed callbacks", counters->actor_failed_callbacks, 0);
}

static void init_vgpu_test_state(emu_state_t *emu,
                                 virtio_gpu_state_t *vgpu,
                                 uint32_t *ram,
                                 size_t ram_size)
{
    memset(emu, 0, sizeof(*emu));
    emu->ram = ram;
    ram_dma_init(&emu->ram_dma, ram, ram_size, NULL);
    require_int("plic lock init", pthread_mutex_init(&emu->plic_lock, NULL), 0);
    virtio_gpu_init(vgpu, emu);
}

static void destroy_vgpu_test_state(emu_state_t *emu, virtio_gpu_state_t *vgpu)
{
    virtio_gpu_destroy(vgpu);
    pthread_mutex_destroy(&emu->plic_lock);
}

static uint16_t load_u16(const uint32_t *ram, uint32_t addr)
{
    uint16_t value;

    memcpy(&value, (const uint8_t *) ram + addr, sizeof(value));
    return value;
}

static uint32_t load_u32(const uint32_t *ram, uint32_t addr)
{
    uint32_t value;

    memcpy(&value, (const uint8_t *) ram + addr, sizeof(value));
    return value;
}

static void configure_test_queue(emu_state_t *emu,
                                 virtio_gpu_state_t *vgpu,
                                 uint16_t queue_index)
{
    const guest_paddr_t desc_addr = 0x100;
    const guest_paddr_t driver_addr = 0x200;
    const guest_paddr_t device_addr = 0x300;

    require_int(
        "configure queue",
        virtq_configure(&vgpu->common.queues[queue_index], &emu->ram_dma, 8,
                        desc_addr, driver_addr, device_addr, 0),
        0);
}

static void test_vgpu_debug_counters_init_and_reset_to_zero(void)
{
    uint32_t ram[64] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtio_gpu_debug_counters counters;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));

    counters = virtio_gpu_debug_counters(&vgpu);
    require_debug_counters_zero(&counters);

    atomic_store_explicit(&vgpu.sw_backend.display_counters.full_frame_bytes,
                          4096, memory_order_relaxed);
    atomic_store_explicit(&vgpu.sw_backend.display_counters.publish_queue_full,
                          1, memory_order_relaxed);
    atomic_store_explicit(&vgpu.debug_counters.actor_notify_ok, 1,
                          memory_order_relaxed);

    require_int("common reset", virtio_device_common_reset(&vgpu.common), 0);
    counters = virtio_gpu_debug_counters(&vgpu);
    require_debug_counters_zero(&counters);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_vgpu_display_counters_snapshot_reads_existing_counters(void)
{
    virtio_gpu_state_t vgpu = {0};
    struct virtio_gpu_debug_counters counters;

    atomic_store_explicit(&vgpu.sw_backend.display_counters.full_frame_bytes,
                          1024, memory_order_relaxed);
    atomic_store_explicit(&vgpu.sw_backend.display_counters.dirty_rect_bytes,
                          64, memory_order_relaxed);
    atomic_store_explicit(&vgpu.sw_backend.display_counters.queue_backpressure,
                          3, memory_order_relaxed);
    atomic_store_explicit(&vgpu.sw_backend.display_counters.publish_queue_full,
                          1, memory_order_relaxed);
    atomic_store_explicit(
        &vgpu.sw_backend.display_counters.publish_backpressure, 2,
        memory_order_relaxed);
    atomic_store_explicit(&vgpu.sw_backend.display_counters.publish_unavailable,
                          4, memory_order_relaxed);
    atomic_store_explicit(
        &vgpu.sw_backend.display_counters.publish_can_publish_false, 5,
        memory_order_relaxed);
    atomic_store_explicit(&vgpu.sw_backend.display_counters.dirty_merges, 6,
                          memory_order_relaxed);
    atomic_store_explicit(
        &vgpu.sw_backend.display_counters.full_resync_escalations, 7,
        memory_order_relaxed);

    counters = virtio_gpu_debug_counters(&vgpu);

    require_u64("snapshot full-frame bytes", counters.display.full_frame_bytes,
                1024);
    require_u64("snapshot dirty-rect bytes", counters.display.dirty_rect_bytes,
                64);
    require_u64("snapshot queue backpressure",
                counters.display.queue_backpressure, 3);
    require_u64("snapshot queue full", counters.display.publish_queue_full, 1);
    require_u64("snapshot publish backpressure",
                counters.display.publish_backpressure, 2);
    require_u64("snapshot publish unavailable",
                counters.display.publish_unavailable, 4);
    require_u64("snapshot can-publish false",
                counters.display.publish_can_publish_false, 5);
    require_u64("snapshot dirty merges", counters.display.dirty_merges, 6);
    require_u64("snapshot full resync escalations",
                counters.display.full_resync_escalations, 7);
}

static void test_undefined_command_returns_device_error(void)
{
    uint32_t ram[64] = {0};
    virtio_gpu_state_t vgpu = {0};
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_ctrl_hdr *request =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x20);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x80);
    uint32_t len = 0;

    vgpu.ram = ram;
    atomic_init(&vgpu.common.status, VIRTIO_STATUS__DRIVER_OK);

    request->type = 0xdeadbeefU;
    request->flags = VIRTIO_GPU_FLAG_FENCE;
    request->fence_id = 0x123456789abcdef0ULL;

    desc[0].addr = 0x20;
    desc[0].len = sizeof(*request);
    desc[1].addr = 0x80;
    desc[1].len = sizeof(*response);
    desc[1].flags = VIRTIO_DESC_F_WRITE;

    virtio_gpu_cmd_undefined_handler(&vgpu, desc, &len);

    require_u32("response len", len, sizeof(*response));
    require_u32("response type", response->type, VIRTIO_GPU_RESP_ERR_UNSPEC);
    require_u32("response flags", response->flags, VIRTIO_GPU_FLAG_FENCE);
    require_false(
        "device not reset-needed",
        atomic_load(&vgpu.common.status) & VIRTIO_STATUS__DEVICE_NEEDS_RESET);
}


static void test_vgpu_actor_failure_marks_device_reset_needed(void)
{
    uint32_t ram[64] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtio_gpu_debug_counters counters;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    require_int("actor fail", virtio_actor_fail(&vgpu.actor), 0);
    require_u32(
        "actor failure sets reset-needed",
        atomic_load(&vgpu.common.status) & VIRTIO_STATUS__DEVICE_NEEDS_RESET,
        VIRTIO_STATUS__DEVICE_NEEDS_RESET);
    counters = virtio_gpu_debug_counters(&vgpu);
    require_u64("actor failed counter", counters.actor_failed_callbacks, 1);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_vgpu_failed_actor_notify_counts_eio(void)
{
    uint32_t ram[64] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtio_gpu_debug_counters counters;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    require_int("actor fail", virtio_actor_fail(&vgpu.actor), 0);
    require_int(
        "notify failed actor",
        vgpu.common.ops->notify_queue(vgpu.common.opaque, VIRTIO_GPU_CONTROLQ,
                                      vgpu.common.generation),
        -EIO);

    counters = virtio_gpu_debug_counters(&vgpu);
    require_u64("notify eio counter", counters.actor_notify_eio, 1);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_vgpu_invalid_actor_notify_counts_einval(void)
{
    uint32_t ram[64] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtio_gpu_debug_counters counters;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    require_int("notify invalid queue",
                vgpu.common.ops->notify_queue(vgpu.common.opaque, 17,
                                              vgpu.common.generation),
                -EINVAL);

    counters = virtio_gpu_debug_counters(&vgpu);
    require_u64("notify einval counter", counters.actor_notify_einval, 1);
    require_u32(
        "invalid notify sets reset-needed",
        atomic_load(&vgpu.common.status) & VIRTIO_STATUS__DEVICE_NEEDS_RESET,
        VIRTIO_STATUS__DEVICE_NEEDS_RESET);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_vgpu_virgl_gate_keeps_unsupported_features_hidden(void)
{
    uint32_t ram[64] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    uint32_t num_capsets;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));

    require_u64("VirGL feature hidden",
                vgpu.common.device_features & VIRTIO_GPU_F_VIRGL, 0);
    require_u64("context-init feature hidden",
                vgpu.common.device_features & VIRTIO_GPU_F_CONTEXT_INIT, 0);
    virtio_gpu_set_num_capsets(&vgpu, 5);
    num_capsets = vgpu.common.ops->read_config(
        vgpu.common.opaque, offsetof(struct virtio_gpu_config, num_capsets),
        sizeof(num_capsets));
    require_u32("no capsets without renderer backend", num_capsets, 0);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_deferred_ctrl_completion_revalidates_generations(void)
{
    uint32_t ram[256] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtio_gpu_deferred_ctrl_completion completion;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    require_int("configure actor", virtio_actor_enter_configuring(&vgpu.actor),
                0);
    require_int("activate actor", virtio_actor_activate(&vgpu.actor), 0);
    atomic_store_explicit(&vgpu.common.status, VIRTIO_STATUS__DRIVER_OK,
                          memory_order_release);
    configure_test_queue(&emu, &vgpu, VIRTIO_GPU_CONTROLQ);

    completion = (struct virtio_gpu_deferred_ctrl_completion) {
        .queue_index = VIRTIO_GPU_CONTROLQ,
        .desc_head = 3,
        .len = 0x44,
        .actor_generation = virtio_actor_generation(&vgpu.actor),
        .common_generation = vgpu.common.generation,
        .trigger_irq = true,
    };
    require_int("current deferred completion",
                virtio_gpu_complete_deferred_ctrl(&vgpu, &completion), 0);
    require_u16("used idx after current completion", load_u16(ram, 0x302), 1);
    require_u32("used elem id", load_u32(ram, 0x304), 3);
    require_u32("used elem len", load_u32(ram, 0x308), 0x44);
    require_u32(
        "used-ring irq",
        virtio_irq_read_status(&vgpu.common.irq) & VIRTIO_INT__USED_RING,
        VIRTIO_INT__USED_RING);

    completion.common_generation--;
    completion.desc_head = 4;
    completion.len = 0x88;
    require_int("stale common generation completion",
                virtio_gpu_complete_deferred_ctrl(&vgpu, &completion),
                -ECANCELED);
    require_u16("used idx unchanged by stale common completion",
                load_u16(ram, 0x302), 1);

    completion.common_generation = vgpu.common.generation;
    completion.actor_generation++;
    require_int("stale actor generation completion",
                virtio_gpu_complete_deferred_ctrl(&vgpu, &completion),
                -ECANCELED);
    require_u16("used idx unchanged by stale actor completion",
                load_u16(ram, 0x302), 1);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_vgpu_destroy_releases_common_without_actor(void)
{
    static const uint16_t queue_max_sizes[] = {8, 8};
    uint32_t ram[64] = {0};
    emu_state_t emu = {0};
    virtio_gpu_state_t vgpu = {0};
    struct virtio_device_common_config config = {
        .emu = &emu,
        .dma = &emu.ram_dma,
        .irq_source = SEMU_IRQ_SOURCE_COUNT,
        .device_id = 16,
        .vendor_id = VIRTIO_VENDOR_ID,
        .queue_max_sizes = queue_max_sizes,
        .num_queues = ARRAY_SIZE(queue_max_sizes),
    };

    emu.ram = ram;
    ram_dma_init(&emu.ram_dma, ram, sizeof(ram), NULL);
    require_int("common init", virtio_device_common_init(&vgpu.common, &config),
                0);

    virtio_gpu_destroy(&vgpu);
    require_false("partial common no longer initialized",
                  vgpu.common.initialized);
    virtio_gpu_destroy(&vgpu);
}

static void test_vgpu_destroy_stops_started_actor_and_is_idempotent(void)
{
    uint32_t ram[64] = {0};
    emu_state_t emu = {0};
    virtio_gpu_state_t vgpu;

    emu.ram = ram;
    ram_dma_init(&emu.ram_dma, ram, sizeof(ram), NULL);
    require_int("plic lock init", pthread_mutex_init(&emu.plic_lock, NULL), 0);

    virtio_gpu_init(&vgpu, &emu);
    require_int("start actor", virtio_actor_start(&vgpu.actor), 0);

    virtio_gpu_destroy(&vgpu);
    require_false("actor no longer initialized", vgpu.actor_initialized);
    require_false("common no longer initialized", vgpu.common.initialized);
    require_false("actor thread no longer live",
                  vgpu.actor.thread_started && !vgpu.actor.thread_joined);

    virtio_gpu_destroy(&vgpu);
    virtio_gpu_destroy(NULL);
    pthread_mutex_destroy(&emu.plic_lock);
}

int main(void)
{
    test_vgpu_debug_counters_init_and_reset_to_zero();
    test_vgpu_display_counters_snapshot_reads_existing_counters();
    test_undefined_command_returns_device_error();
    test_vgpu_actor_failure_marks_device_reset_needed();
    test_vgpu_failed_actor_notify_counts_eio();
    test_vgpu_invalid_actor_notify_counts_einval();
    test_vgpu_virgl_gate_keeps_unsupported_features_hidden();
    test_deferred_ctrl_completion_revalidates_generations();
    test_vgpu_destroy_releases_common_without_actor();
    test_vgpu_destroy_stops_started_actor_and_is_idempotent();
    return 0;
}
