#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "device.h"
#include "virtio-gpu.h"
#if SEMU_HAS(VIRGL)
#include "vgpu-renderer.h"
#endif
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

#if SEMU_HAS(VIRGL)
static void require_ptr(const char *name, const void *got, const void *want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %p, want %p\n", name, got, want);
    exit(1);
}

static uint32_t renderer_release_response_count;

static void renderer_release_response(void *response)
{
    renderer_release_response_count++;
    free(response);
}


#endif

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

#if SEMU_HAS(VIRGL)
static void store_u16(uint32_t *ram, uint32_t addr, uint16_t value)
{
    memcpy((uint8_t *) ram + addr, &value, sizeof(value));
}

static void wait_for_used_idx(const uint32_t *ram,
                              uint32_t used_idx_addr,
                              uint16_t want)
{
    const struct timespec delay = {
        .tv_sec = 0,
        .tv_nsec = 1000000,
    };

    for (int i = 0; i < 1000; i++) {
        if (load_u16(ram, used_idx_addr) == want)
            return;
        nanosleep(&delay, NULL);
    }

    fprintf(stderr, "used idx at 0x%x did not reach %u\n", used_idx_addr, want);
    exit(1);
}

static uint64_t load_u64(const uint32_t *ram, uint32_t addr)
{
    uint64_t value;

    memcpy(&value, (const uint8_t *) ram + addr, sizeof(value));
    return value;
}
#endif

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

#if SEMU_HAS(VIRGL)
static void test_virgl_capset_info_handler_submits_host_owned_ctrl_payload(void)
{
    uint32_t ram[512] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_get_capset_info *request =
        (struct virtio_gpu_get_capset_info *) ((uint8_t *) ram + 0x40);
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x71;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    require_int("configure actor", virtio_actor_enter_configuring(&vgpu.actor),
                0);
    require_int("activate actor", virtio_actor_activate(&vgpu.actor), 0);
    vgpu.common.generation = renderer_generation;
    vgpu.actor_drain_generation = virtio_actor_generation(&vgpu.actor);
    vgpu.ctrl_dispatch = (struct virtio_gpu_ctrl_dispatch_context) {
        .active = true,
        .queue_index = VIRTIO_GPU_CONTROLQ,
        .desc_head = 7,
        .actor_generation = vgpu.actor_drain_generation,
        .common_generation = vgpu.common.generation,
        .trigger_irq = true,
    };
    vgpu_renderer_reset_queues(renderer_generation);

    request->hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
    request->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    request->hdr.fence_id = UINT64_C(0x1234);
    request->capset_index = 3;
    desc[0].addr = 0x40;
    desc[0].len = sizeof(*request);
    desc[1].addr = 0x100;
    desc[1].len = sizeof(struct virtio_gpu_resp_capset_info);
    desc[1].flags = VIRTIO_DESC_F_WRITE;

    g_virtio_gpu_backend.get_capset_info(&vgpu, desc, &len);
    request->capset_index = 99;
    request->hdr.fence_id = UINT64_C(0x9999);

    require_u32("capset info is deferred", len, VIRTIO_GPU_RESPONSE_DEFERRED);

    struct vgpu_renderer_request queued = {0};
    require_int("renderer ctrl request queued",
                vgpu_renderer_pop_request(&queued), true);
    require_u32("renderer ctrl request type", queued.type,
                VGPU_RENDERER_REQ_CTRL);
    require_u32("renderer command type", queued.command_type,
                VIRTIO_GPU_CMD_GET_CAPSET_INFO);
    require_u64("renderer generation", queued.token.generation,
                renderer_generation);
    require_u32("renderer token id remains zero", queued.token.id, 0);
    require_u32("payload size", queued.payload_size,
                sizeof(struct vgpu_renderer_ctrl_payload));
    require_ptr("payload release hook", (const void *) queued.release_payload,
                (const void *) free);

    struct vgpu_renderer_ctrl_payload *payload = queued.payload;
    require_u32("snapshot command type", payload->hdr.type,
                VIRTIO_GPU_CMD_GET_CAPSET_INFO);
    require_u64("snapshot fence id", payload->hdr.fence_id, UINT64_C(0x1234));
    require_u32("snapshot capset index",
                payload->cmd.get_capset_info.capset_index, 3);
    require_u32("response capacity", payload->response_capacity,
                sizeof(struct virtio_gpu_resp_capset_info));
    require_u32("completion queue", payload->ctrl_completion.queue_index,
                VIRTIO_GPU_CONTROLQ);
    require_u32("completion desc head", payload->ctrl_completion.desc_head, 7);
    require_u64("completion actor generation",
                payload->ctrl_completion.actor_generation,
                vgpu.actor_drain_generation);
    require_u64("completion common generation",
                payload->ctrl_completion.common_generation,
                renderer_generation);
    require_u32("completion triggers irq", payload->ctrl_completion.trigger_irq,
                true);
    require_u32("response desc addr", payload->response_desc.addr, 0x100);
    require_u32("response desc len", payload->response_desc.len,
                sizeof(struct virtio_gpu_resp_capset_info));
    queued.release_payload(queued.payload);

    struct virtio_gpu_get_capset *capset_request =
        (struct virtio_gpu_get_capset *) ((uint8_t *) ram + 0x140);
    vgpu.ctrl_dispatch.desc_head = 8;
    capset_request->hdr.type = VIRTIO_GPU_CMD_GET_CAPSET;
    capset_request->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    capset_request->hdr.fence_id = UINT64_C(0x5678);
    capset_request->capset_id = VIRTIO_GPU_CAPSET_VIRGL;
    capset_request->capset_version = 2;
    desc[0].addr = 0x140;
    desc[0].len = sizeof(*capset_request);
    desc[1].addr = 0x180;
    desc[1].len = sizeof(struct virtio_gpu_resp_capset) + 16;
    desc[1].flags = VIRTIO_DESC_F_WRITE;
    len = 0;

    g_virtio_gpu_backend.get_capset(&vgpu, desc, &len);
    capset_request->capset_id = 99;
    capset_request->capset_version = 99;
    capset_request->hdr.fence_id = UINT64_C(0x9999);

    require_u32("capset is deferred", len, VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("capset request queued", vgpu_renderer_pop_request(&queued),
                true);
    require_u32("capset command type", queued.command_type,
                VIRTIO_GPU_CMD_GET_CAPSET);
    payload = queued.payload;
    require_u32("snapshot capset id", payload->cmd.get_capset.capset_id,
                VIRTIO_GPU_CAPSET_VIRGL);
    require_u32("snapshot capset version",
                payload->cmd.get_capset.capset_version, 2);
    require_u32("capset desc head", payload->ctrl_completion.desc_head, 8);
    require_u32("capset response capacity", payload->response_capacity,
                sizeof(struct virtio_gpu_resp_capset) + 16);
    require_u32("capset response desc addr", payload->response_desc.addr,
                0x180);
    queued.release_payload(queued.payload);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_hidden_virgl_command_returns_undefined_without_renderer_work(
    void)
{
    uint32_t ram[512] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtio_gpu_get_capset_info *request =
        (struct virtio_gpu_get_capset_info *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x80);
    struct virtq_desc desc0 = {
        .addr = 0x40,
        .len = sizeof(*request),
        .flags = VIRTIO_DESC_F_NEXT,
        .next = 1,
    };
    struct virtq_desc desc1 = {
        .addr = 0x80,
        .len = sizeof(*response),
        .flags = VIRTIO_DESC_F_WRITE,
    };
    struct vgpu_renderer_request queued = {0};

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    configure_test_queue(&emu, &vgpu, VIRTIO_GPU_CONTROLQ);
    vgpu_renderer_reset_queues(vgpu.common.generation);

    request->hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
    request->capset_index = 0;
    memcpy((uint8_t *) ram + 0x100, &desc0, sizeof(desc0));
    memcpy((uint8_t *) ram + 0x110, &desc1, sizeof(desc1));
    store_u16(ram, 0x200, 0);
    store_u16(ram, 0x202, 1);
    store_u16(ram, 0x204, 0);
    store_u16(ram, 0x302, 0);

    require_int("configure actor", virtio_actor_enter_configuring(&vgpu.actor),
                0);
    require_int("activate actor", virtio_actor_activate(&vgpu.actor), 0);
    require_int("start actor", virtio_actor_start(&vgpu.actor), 0);
    atomic_store_explicit(&vgpu.common.status, VIRTIO_STATUS__DRIVER_OK,
                          memory_order_release);

    require_int(
        "notify hidden capset info",
        vgpu.common.ops->notify_queue(vgpu.common.opaque, VIRTIO_GPU_CONTROLQ,
                                      vgpu.common.generation),
        0);
    wait_for_used_idx(ram, 0x302, 1);

    require_u32("hidden capset response", response->type,
                VIRTIO_GPU_RESP_ERR_UNSPEC);
    require_u32("hidden capset used id", load_u32(ram, 0x304), 0);
    require_u32("hidden capset used len", load_u32(ram, 0x308),
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_int("hidden command queues no renderer work",
                vgpu_renderer_pop_request(&queued), false);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_virgl_context_handlers_submit_ctrl_skeletons(void)
{
    uint32_t ram[512] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_ctx_create *create =
        (struct virtio_gpu_ctx_create *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_ctx_destroy *destroy =
        (struct virtio_gpu_ctx_destroy *) ((uint8_t *) ram + 0x140);
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x72;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    require_int("configure actor", virtio_actor_enter_configuring(&vgpu.actor),
                0);
    require_int("activate actor", virtio_actor_activate(&vgpu.actor), 0);
    vgpu.common.generation = renderer_generation;
    vgpu.actor_drain_generation = virtio_actor_generation(&vgpu.actor);
    vgpu.ctrl_dispatch = (struct virtio_gpu_ctrl_dispatch_context) {
        .active = true,
        .queue_index = VIRTIO_GPU_CONTROLQ,
        .desc_head = 8,
        .actor_generation = vgpu.actor_drain_generation,
        .common_generation = vgpu.common.generation,
        .trigger_irq = true,
    };
    vgpu_renderer_reset_queues(renderer_generation);

    create->hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    create->hdr.ctx_id = 44;
    create->nlen = 4;
    memcpy(create->debug_name, "ctxA", 4);
    desc[0].addr = 0x40;
    desc[0].len = sizeof(*create);
    desc[1].addr = 0x100;
    desc[1].len = sizeof(struct virtio_gpu_ctrl_hdr);
    desc[1].flags = VIRTIO_DESC_F_WRITE;

    g_virtio_gpu_backend.ctx_create(&vgpu, desc, &len);
    require_u32("ctx create is deferred", len, VIRTIO_GPU_RESPONSE_DEFERRED);

    struct vgpu_renderer_request queued = {0};
    require_int("ctx create queued", vgpu_renderer_pop_request(&queued), true);
    require_u32("ctx create command type", queued.command_type,
                VIRTIO_GPU_CMD_CTX_CREATE);
    struct vgpu_renderer_ctrl_payload *payload = queued.payload;
    require_u32("ctx create id snapshot", payload->cmd.ctx_create.hdr.ctx_id,
                44);
    require_u32("ctx create nlen snapshot", payload->cmd.ctx_create.nlen, 4);
    require_u32("ctx create response capacity", payload->response_capacity,
                sizeof(struct virtio_gpu_ctrl_hdr));
    queued.release_payload(queued.payload);

    vgpu.ctrl_dispatch.desc_head = 9;
    destroy->hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    destroy->hdr.ctx_id = 44;
    desc[0].addr = 0x140;
    desc[0].len = sizeof(*destroy);
    len = 0;

    g_virtio_gpu_backend.ctx_destroy(&vgpu, desc, &len);
    require_u32("ctx destroy is deferred", len, VIRTIO_GPU_RESPONSE_DEFERRED);

    require_int("ctx destroy queued", vgpu_renderer_pop_request(&queued), true);
    require_u32("ctx destroy command type", queued.command_type,
                VIRTIO_GPU_CMD_CTX_DESTROY);
    payload = queued.payload;
    require_u32("ctx destroy id snapshot", payload->cmd.ctx_destroy.hdr.ctx_id,
                44);
    require_u32("ctx destroy desc head", payload->ctrl_completion.desc_head, 9);
    queued.release_payload(queued.payload);

    destroy_vgpu_test_state(&emu, &vgpu);
}
#endif

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

#if SEMU_HAS(VIRGL)
static void test_renderer_completion_drain_writes_response_and_used_ring(void)
{
    uint32_t ram[512] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    const uint64_t renderer_generation = 0x55;
    struct virtio_gpu_ctrl_hdr *response = calloc(1, sizeof(*response));

    if (!response) {
        fprintf(stderr, "failed to allocate renderer response\n");
        exit(1);
    }

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    require_int("configure actor", virtio_actor_enter_configuring(&vgpu.actor),
                0);
    require_int("activate actor", virtio_actor_activate(&vgpu.actor), 0);
    atomic_store_explicit(&vgpu.common.status, VIRTIO_STATUS__DRIVER_OK,
                          memory_order_release);
    configure_test_queue(&emu, &vgpu, VIRTIO_GPU_CONTROLQ);

    response->type = VIRTIO_GPU_RESP_OK_NODATA;
    response->flags = VIRTIO_GPU_FLAG_FENCE;
    response->fence_id = UINT64_C(0x1234567887654321);

    renderer_release_response_count = 0;
    vgpu_renderer_reset_queues(renderer_generation);
    struct vgpu_renderer_completion completion = {
        .type = VGPU_RENDERER_DONE_CTRL,
        .token = {.generation = renderer_generation},
        .response = response,
        .response_size = sizeof(*response),
        .release_response = renderer_release_response,
        .has_ctrl_completion = true,
        .ctrl_completion =
            {
                .queue_index = VIRTIO_GPU_CONTROLQ,
                .desc_head = 5,
                .actor_generation = virtio_actor_generation(&vgpu.actor),
                .common_generation = vgpu.common.generation,
                .trigger_irq = true,
            },
        .has_response_desc = true,
        .response_desc =
            {
                .addr = 0x80,
                .len = sizeof(*response),
                .flags = VIRTIO_DESC_F_WRITE,
            },
    };

    require_int("queue renderer completion",
                vgpu_renderer_complete(&completion), true);
    virtio_gpu_drain_renderer_completions(&vgpu);

    require_u32("renderer response type", load_u32(ram, 0x80),
                VIRTIO_GPU_RESP_OK_NODATA);
    require_u32("renderer response flags", load_u32(ram, 0x84),
                VIRTIO_GPU_FLAG_FENCE);
    require_u64("renderer response fence", load_u64(ram, 0x88),
                UINT64_C(0x1234567887654321));
    require_u16("used idx after renderer completion", load_u16(ram, 0x302), 1);
    require_u32("renderer used elem id", load_u32(ram, 0x304), 5);
    require_u32("renderer used elem len", load_u32(ram, 0x308),
                sizeof(*response));
    require_u32(
        "renderer used-ring irq",
        virtio_irq_read_status(&vgpu.common.irq) & VIRTIO_INT__USED_RING,
        VIRTIO_INT__USED_RING);
    require_u32("renderer response released", renderer_release_response_count,
                1);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_renderer_completion_drops_stale_common_generation(void)
{
    uint32_t ram[512] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    const uint64_t renderer_generation = 0x57;
    struct virtio_gpu_ctrl_hdr *response = calloc(1, sizeof(*response));

    if (!response) {
        fprintf(stderr, "failed to allocate stale renderer response\n");
        exit(1);
    }

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    require_int("configure actor", virtio_actor_enter_configuring(&vgpu.actor),
                0);
    require_int("activate actor", virtio_actor_activate(&vgpu.actor), 0);
    atomic_store_explicit(&vgpu.common.status, VIRTIO_STATUS__DRIVER_OK,
                          memory_order_release);
    configure_test_queue(&emu, &vgpu, VIRTIO_GPU_CONTROLQ);

    response->type = VIRTIO_GPU_RESP_OK_NODATA;

    renderer_release_response_count = 0;
    vgpu_renderer_reset_queues(renderer_generation);
    struct vgpu_renderer_completion completion = {
        .type = VGPU_RENDERER_DONE_CTRL,
        .token = {.generation = renderer_generation},
        .response = response,
        .response_size = sizeof(*response),
        .release_response = renderer_release_response,
        .has_ctrl_completion = true,
        .ctrl_completion =
            {
                .queue_index = VIRTIO_GPU_CONTROLQ,
                .desc_head = 6,
                .actor_generation = virtio_actor_generation(&vgpu.actor),
                .common_generation = vgpu.common.generation - 1,
                .trigger_irq = true,
            },
        .has_response_desc = true,
        .response_desc =
            {
                .addr = 0x80,
                .len = sizeof(*response),
                .flags = VIRTIO_DESC_F_WRITE,
            },
    };

    require_int("queue stale renderer completion",
                vgpu_renderer_complete(&completion), true);
    virtio_gpu_drain_renderer_completions(&vgpu);

    require_u32("stale renderer response not written", load_u32(ram, 0x80), 0);
    require_u16("used idx unchanged by stale renderer completion",
                load_u16(ram, 0x302), 0);
    require_u32(
        "stale renderer completion does not trigger used irq",
        virtio_irq_read_status(&vgpu.common.irq) & VIRTIO_INT__USED_RING, 0);
    require_u32("stale renderer response released",
                renderer_release_response_count, 1);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_renderer_ctrl_completion_without_metadata_fails(void)
{
    uint32_t ram[64] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    const uint64_t renderer_generation = 0x56;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    atomic_store_explicit(&vgpu.common.status, VIRTIO_STATUS__DRIVER_OK,
                          memory_order_release);

    vgpu_renderer_reset_queues(renderer_generation);
    struct vgpu_renderer_completion completion = {
        .type = VGPU_RENDERER_DONE_CTRL,
        .token = {.generation = renderer_generation},
    };

    require_int("queue renderer ctrl without metadata",
                vgpu_renderer_complete(&completion), true);
    virtio_gpu_drain_renderer_completions(&vgpu);

    require_u32(
        "renderer ctrl without metadata sets reset-needed",
        atomic_load(&vgpu.common.status) & VIRTIO_STATUS__DEVICE_NEEDS_RESET,
        VIRTIO_STATUS__DEVICE_NEEDS_RESET);

    destroy_vgpu_test_state(&emu, &vgpu);
}
#endif

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
#if SEMU_HAS(VIRGL)
    test_hidden_virgl_command_returns_undefined_without_renderer_work();
    test_virgl_capset_info_handler_submits_host_owned_ctrl_payload();
    test_virgl_context_handlers_submit_ctrl_skeletons();
    test_renderer_completion_drain_writes_response_and_used_ring();
    test_renderer_completion_drops_stale_common_generation();
    test_renderer_ctrl_completion_without_metadata_fails();
#endif
    test_vgpu_destroy_releases_common_without_actor();
    test_vgpu_destroy_stops_started_actor_and_is_idempotent();
    return 0;
}
