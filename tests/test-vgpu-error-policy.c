#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
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

static void test_vgpu_resource_unref_releases_2d_resource_synchronously(void)
{
    uint32_t ram[512] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_res_create_2d *create =
        (struct virtio_gpu_res_create_2d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_res_unref *unref =
        (struct virtio_gpu_res_unref *) ((uint8_t *) ram + 0xc0);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x100);
    uint32_t len = 0;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create->resource_id = 41;
    create->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    create->width = 8;
    create->height = 8;
    desc[0].addr = 0x40;
    desc[0].len = sizeof(*create);
    desc[1].addr = 0x100;
    desc[1].len = sizeof(*response);
    desc[1].flags = VIRTIO_DESC_F_WRITE;

    g_virtio_gpu_backend.resource_create_2d(&vgpu, desc, &len);
    require_u32("2d create response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("2d create response", response->type,
                VIRTIO_GPU_RESP_OK_NODATA);

    memset(response, 0, sizeof(*response));
    unref->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    unref->resource_id = 41;
    desc[0].addr = 0xc0;
    desc[0].len = sizeof(*unref);
    len = 0;

    g_virtio_gpu_backend.resource_unref(&vgpu, desc, &len);
    require_u32("2d unref response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("2d unref response", response->type, VIRTIO_GPU_RESP_OK_NODATA);

#if SEMU_HAS(VIRGL)
    struct vgpu_renderer_request queued = {0};
    require_int("2d unref queues no renderer work",
                vgpu_renderer_pop_request(&queued), false);
#endif

    memset(response, 0, sizeof(*response));
    desc[0].addr = 0x40;
    desc[0].len = sizeof(*create);
    len = 0;
    g_virtio_gpu_backend.resource_create_2d(&vgpu, desc, &len);
    require_u32("2d recreate after unref response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("2d recreate after unref response", response->type,
                VIRTIO_GPU_RESP_OK_NODATA);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_vgpu_resource_attach_detach_keeps_2d_path_synchronous(void)
{
    uint32_t ram[512] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc attach_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc detach_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_res_create_2d *create =
        (struct virtio_gpu_res_create_2d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_res_attach_backing *attach =
        (struct virtio_gpu_res_attach_backing *) ((uint8_t *) ram + 0xc0);
    struct virtio_gpu_res_detach_backing *detach =
        (struct virtio_gpu_res_detach_backing *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_mem_entry *entry =
        (struct virtio_gpu_mem_entry *) ((uint8_t *) ram + 0x140);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x180);
    uint32_t len = 0;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create->resource_id = 42;
    create->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    create->width = 4;
    create->height = 4;
    create_desc[0].addr = 0x40;
    create_desc[0].len = sizeof(*create);
    create_desc[1].addr = 0x180;
    create_desc[1].len = sizeof(*response);
    create_desc[1].flags = VIRTIO_DESC_F_WRITE;

    g_virtio_gpu_backend.resource_create_2d(&vgpu, create_desc, &len);
    require_u32("2d create before attach response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("2d create before attach response", response->type,
                VIRTIO_GPU_RESP_OK_NODATA);

    attach->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach->resource_id = 42;
    attach->nr_entries = 1;
    entry->addr = 0x1c0;
    entry->length = 16;
    attach_desc[0].addr = 0xc0;
    attach_desc[0].len = sizeof(*attach);
    attach_desc[1].addr = 0x140;
    attach_desc[1].len = sizeof(*entry);
    attach_desc[2].addr = 0x180;
    attach_desc[2].len = sizeof(*response);
    attach_desc[2].flags = VIRTIO_DESC_F_WRITE;
    len = 0;
    response->type = 0;

    g_virtio_gpu_backend.resource_attach_backing(&vgpu, attach_desc, &len);
    require_u32("2d attach response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("2d attach response", response->type,
                VIRTIO_GPU_RESP_OK_NODATA);

    detach->hdr.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    detach->resource_id = 42;
    detach_desc[0].addr = 0x100;
    detach_desc[0].len = sizeof(*detach);
    detach_desc[1].addr = 0x180;
    detach_desc[1].len = sizeof(*response);
    detach_desc[1].flags = VIRTIO_DESC_F_WRITE;
    len = 0;
    response->type = 0;

    g_virtio_gpu_backend.resource_detach_backing(&vgpu, detach_desc, &len);
    require_u32("2d detach response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("2d detach response", response->type,
                VIRTIO_GPU_RESP_OK_NODATA);

#if SEMU_HAS(VIRGL)
    struct vgpu_renderer_request queued = {0};
    require_int("2d attach/detach queues no renderer work",
                vgpu_renderer_pop_request(&queued), false);
#endif

    destroy_vgpu_test_state(&emu, &vgpu);
}


#if SEMU_HAS(VIRGL)

static void activate_test_renderer_dispatch(virtio_gpu_state_t *vgpu,
                                            uint64_t renderer_generation,
                                            uint32_t desc_head)
{
    require_int("configure actor", virtio_actor_enter_configuring(&vgpu->actor),
                0);
    require_int("activate actor", virtio_actor_activate(&vgpu->actor), 0);
    vgpu->common.generation = renderer_generation;
    vgpu->actor_drain_generation = virtio_actor_generation(&vgpu->actor);
    vgpu->ctrl_dispatch = (struct virtio_gpu_ctrl_dispatch_context) {
        .active = true,
        .queue_index = VIRTIO_GPU_CONTROLQ,
        .desc_head = desc_head,
        .actor_generation = vgpu->actor_drain_generation,
        .common_generation = vgpu->common.generation,
        .trigger_irq = true,
    };
    vgpu_renderer_reset_queues(renderer_generation);
}

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

static void test_virgl_resource_create_3d_handler_tracks_pending_resource(void)
{
    uint32_t ram[512] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc desc_2d[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *request =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_res_create_2d *request_2d =
        (struct virtio_gpu_res_create_2d *) ((uint8_t *) ram + 0x180);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_ctrl_hdr *response_2d =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x1e0);
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x73;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    require_int("configure actor", virtio_actor_enter_configuring(&vgpu.actor),
                0);
    require_int("activate actor", virtio_actor_activate(&vgpu.actor), 0);
    vgpu.common.generation = renderer_generation;
    vgpu.actor_drain_generation = virtio_actor_generation(&vgpu.actor);
    vgpu.ctrl_dispatch = (struct virtio_gpu_ctrl_dispatch_context) {
        .active = true,
        .queue_index = VIRTIO_GPU_CONTROLQ,
        .desc_head = 10,
        .actor_generation = vgpu.actor_drain_generation,
        .common_generation = vgpu.common.generation,
        .trigger_irq = true,
    };
    vgpu_renderer_reset_queues(renderer_generation);

    request->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    request->resource_id = 55;
    request->target = 2;
    request->format = 3;
    request->bind = 4;
    request->width = 640;
    request->height = 480;
    request->depth = 1;
    request->array_size = 1;
    request->last_level = 0;
    request->nr_samples = 1;
    request->flags = 0x5a;
    desc[0].addr = 0x40;
    desc[0].len = sizeof(*request);
    desc[1].addr = 0x100;
    desc[1].len = sizeof(*response);
    desc[1].flags = VIRTIO_DESC_F_WRITE;

    g_virtio_gpu_backend.resource_create_3d(&vgpu, desc, &len);
    request->resource_id = 99;
    request->width = 1;
    request->height = 1;

    require_u32("resource create is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);

    struct vgpu_renderer_request queued = {0};
    require_int("resource create queued", vgpu_renderer_pop_request(&queued),
                true);
    require_u32("resource create command type", queued.command_type,
                VIRTIO_GPU_CMD_RESOURCE_CREATE_3D);
    struct vgpu_renderer_ctrl_payload *payload = queued.payload;
    uint64_t first_generation = payload->resource_generation;
    require_u32("snapshot resource id",
                payload->cmd.resource_create_3d.resource_id, 55);
    require_u32("snapshot width", payload->cmd.resource_create_3d.width, 640);
    require_u32("snapshot height", payload->cmd.resource_create_3d.height, 480);
    require_false("resource generation assigned", first_generation == 0);

    len = 0;
    request->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    request->resource_id = 55;
    request->width = 320;
    request->height = 240;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, desc, &len);
    require_u32("duplicate resource response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("duplicate resource response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    struct vgpu_renderer_request empty = {0};
    require_int("duplicate resource queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    request_2d->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    request_2d->resource_id = 55;
    request_2d->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    request_2d->width = 8;
    request_2d->height = 8;
    desc_2d[0].addr = 0x180;
    desc_2d[0].len = sizeof(*request_2d);
    desc_2d[1].addr = 0x1e0;
    desc_2d[1].len = sizeof(*response_2d);
    desc_2d[1].flags = VIRTIO_DESC_F_WRITE;
    len = 0;
    g_virtio_gpu_backend.resource_create_2d(&vgpu, desc_2d, &len);
    require_u32("2d duplicate of pending 3d response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("2d duplicate of pending 3d response", response_2d->type,
                VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);

    struct vgpu_renderer_completion stale_rollback = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_CREATE_3D_ROLLBACK,
                .resource_id = 55,
                .resource_generation = first_generation + 1,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &stale_rollback);

    len = 0;
    response->type = 0;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, desc, &len);
    require_u32("stale rollback keeps resource response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("stale rollback keeps resource response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    require_int("stale rollback queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    struct vgpu_renderer_completion rollback = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_CREATE_3D_ROLLBACK,
                .resource_id = 55,
                .resource_generation = first_generation,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &rollback);
    queued.release_payload(payload);

    len = 0;
    response->type = 0;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, desc, &len);
    require_u32("resource recreate after rollback is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("resource recreate queued", vgpu_renderer_pop_request(&queued),
                true);
    struct vgpu_renderer_ctrl_payload *payload2 = queued.payload;
    uint64_t second_generation = payload2->resource_generation;
    require_false("resource generation advances",
                  second_generation == first_generation);

    stale_rollback.virgl_resource.resource_generation = first_generation;
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &stale_rollback);

    len = 0;
    response->type = 0;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, desc, &len);
    require_u32("old rollback keeps newer resource response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("old rollback keeps newer resource response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    require_int("old rollback queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    rollback.virgl_resource.resource_generation = second_generation;
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &rollback);
    queued.release_payload(payload2);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_virgl_resource_create_3d_rejects_live_2d_resource_id(void)
{
    uint32_t ram[512] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_res_create_2d *request_2d =
        (struct virtio_gpu_res_create_2d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_resource_create_3d *request_3d =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x100);
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x74;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));

    request_2d->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    request_2d->resource_id = 77;
    request_2d->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    request_2d->width = 16;
    request_2d->height = 16;
    desc[0].addr = 0x40;
    desc[0].len = sizeof(*request_2d);
    desc[1].addr = 0x100;
    desc[1].len = sizeof(*response);
    desc[1].flags = VIRTIO_DESC_F_WRITE;

    g_virtio_gpu_backend.resource_create_2d(&vgpu, desc, &len);
    require_u32("2d resource create response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("2d resource create response", response->type,
                VIRTIO_GPU_RESP_OK_NODATA);

    require_int("configure actor", virtio_actor_enter_configuring(&vgpu.actor),
                0);
    require_int("activate actor", virtio_actor_activate(&vgpu.actor), 0);
    vgpu.common.generation = renderer_generation;
    vgpu.actor_drain_generation = virtio_actor_generation(&vgpu.actor);
    vgpu.ctrl_dispatch = (struct virtio_gpu_ctrl_dispatch_context) {
        .active = true,
        .queue_index = VIRTIO_GPU_CONTROLQ,
        .desc_head = 11,
        .actor_generation = vgpu.actor_drain_generation,
        .common_generation = vgpu.common.generation,
        .trigger_irq = true,
    };
    vgpu_renderer_reset_queues(renderer_generation);

    memset(request_3d, 0, sizeof(*request_3d));
    request_3d->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    request_3d->resource_id = 77;
    request_3d->target = 2;
    request_3d->format = 3;
    request_3d->bind = 4;
    request_3d->width = 16;
    request_3d->height = 16;
    request_3d->depth = 1;
    request_3d->array_size = 1;
    request_3d->nr_samples = 1;
    response->type = 0;
    len = 0;
    desc[0].len = sizeof(*request_3d);

    g_virtio_gpu_backend.resource_create_3d(&vgpu, desc, &len);
    require_u32("3d duplicate of live 2d response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("3d duplicate of live 2d response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    struct vgpu_renderer_request queued = {0};
    require_int("3d duplicate queues no renderer work",
                vgpu_renderer_pop_request(&queued), false);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_virgl_resource_unref_handler_defers_and_frees_namespace(void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc unref_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_res_unref *unref =
        (struct virtio_gpu_res_unref *) ((uint8_t *) ram + 0x140);
    struct virtio_gpu_ctrl_hdr *create_response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_ctrl_hdr *unref_response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x1a0);
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x75;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 20);

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = 61;
    create->target = 2;
    create->format = 3;
    create->bind = 4;
    create->width = 64;
    create->height = 64;
    create->depth = 1;
    create->array_size = 1;
    create->nr_samples = 1;
    create_desc[0].addr = 0x40;
    create_desc[0].len = sizeof(*create);
    create_desc[1].addr = 0x100;
    create_desc[1].len = sizeof(*create_response);
    create_desc[1].flags = VIRTIO_DESC_F_WRITE;

    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("3d create before unref is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);

    struct vgpu_renderer_request queued = {0};
    require_int("3d create request queued", vgpu_renderer_pop_request(&queued),
                true);
    struct vgpu_renderer_ctrl_payload *create_payload = queued.payload;
    uint64_t first_generation = create_payload->resource_generation;
    require_false("3d create generation assigned", first_generation == 0);
    queued.release_payload(queued.payload);

    vgpu.ctrl_dispatch.desc_head = 21;
    unref->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    unref->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    unref->hdr.fence_id = UINT64_C(0xabcdef);
    unref->resource_id = 61;
    unref_desc[0].addr = 0x140;
    unref_desc[0].len = sizeof(*unref);
    unref_desc[1].addr = 0x1a0;
    unref_desc[1].len = sizeof(*unref_response);
    unref_desc[1].flags = VIRTIO_DESC_F_WRITE;
    len = 0;

    g_virtio_gpu_backend.resource_unref(&vgpu, unref_desc, &len);
    unref->resource_id = 999;
    unref->hdr.fence_id = UINT64_C(0x123456);

    require_u32("3d unref is deferred", len, VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d unref request queued", vgpu_renderer_pop_request(&queued),
                true);
    require_u32("3d unref command type", queued.command_type,
                VIRTIO_GPU_CMD_RESOURCE_UNREF);
    struct vgpu_renderer_ctrl_payload *unref_payload = queued.payload;
    require_u32("3d unref snapshots resource id",
                unref_payload->cmd.resource_unref.resource_id, 61);
    require_u64("3d unref carries original generation",
                unref_payload->resource_generation, first_generation);
    require_u64("3d unref snapshots fence", unref_payload->hdr.fence_id,
                UINT64_C(0xabcdef));
    require_u32("3d unref response desc addr",
                unref_payload->response_desc.addr, 0x1a0);
    queued.release_payload(queued.payload);

    unref->resource_id = 61;
    unref->hdr.fence_id = UINT64_C(0x555555);
    unref_response->type = 0;
    len = 0;
    g_virtio_gpu_backend.resource_unref(&vgpu, unref_desc, &len);
    require_u32("duplicate pending 3d unref response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("duplicate pending 3d unref response", unref_response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    require_u32("duplicate pending 3d unref response flags",
                unref_response->flags, VIRTIO_GPU_FLAG_FENCE);
    require_u64("duplicate pending 3d unref response fence",
                unref_response->fence_id, UINT64_C(0x555555));
    struct vgpu_renderer_request empty = {0};
    require_int("duplicate pending 3d unref queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    vgpu.ctrl_dispatch.desc_head = 22;
    create->resource_id = 61;
    create->width = 32;
    create->height = 32;
    len = 0;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("3d recreate after deferred unref is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d recreate request queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *recreate_payload = queued.payload;
    uint64_t second_generation = recreate_payload->resource_generation;
    require_false("3d recreate generation advances",
                  second_generation == first_generation);

    struct vgpu_renderer_completion stale_rollback = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_UNREF_ROLLBACK,
                .resource_id = 61,
                .resource_generation = first_generation,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &stale_rollback);

    len = 0;
    create_response->type = 0;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("stale unref rollback keeps newer resource response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("stale unref rollback keeps newer resource response",
                create_response->type, VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    require_int("stale unref rollback queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    struct vgpu_renderer_completion stale_commit = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_UNREF,
                .resource_id = 61,
                .resource_generation = first_generation,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &stale_commit);

    len = 0;
    create_response->type = 0;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("stale unref commit keeps newer resource response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("stale unref commit keeps newer resource response",
                create_response->type, VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    require_int("stale unref commit queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    struct vgpu_renderer_completion cleanup = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_CREATE_3D_ROLLBACK,
                .resource_id = 61,
                .resource_generation = second_generation,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &cleanup);
    queued.release_payload(recreate_payload);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_virgl_resource_unref_sync_submit_failure_restores_namespace(
    void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc unref_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_res_unref *unref =
        (struct virtio_gpu_res_unref *) ((uint8_t *) ram + 0x140);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x100);
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x76;
    uint64_t resource_generation;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 30);

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = 62;
    create->target = 2;
    create->format = 3;
    create->bind = 4;
    create->width = 64;
    create->height = 64;
    create->depth = 1;
    create->array_size = 1;
    create->nr_samples = 1;
    create_desc[0].addr = 0x40;
    create_desc[0].len = sizeof(*create);
    create_desc[1].addr = 0x100;
    create_desc[1].len = sizeof(*response);
    create_desc[1].flags = VIRTIO_DESC_F_WRITE;

    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("3d create before sync-fail unref is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_renderer_request queued = {0};
    require_int("3d create before sync-fail queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *create_payload = queued.payload;
    resource_generation = create_payload->resource_generation;
    queued.release_payload(queued.payload);

    for (uint32_t i = 0; i < VGPU_RENDERER_QUEUE_CAPACITY; i++) {
        struct vgpu_renderer_request filler = {
            .type = VGPU_RENDERER_REQ_POLL,
            .token = {.generation = renderer_generation},
        };
        require_int("fill renderer queue", vgpu_renderer_submit(&filler), true);
    }

    unref->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    unref->resource_id = 62;
    unref_desc[0].addr = 0x140;
    unref_desc[0].len = sizeof(*unref);
    unref_desc[1].addr = 0x100;
    unref_desc[1].len = sizeof(*response);
    unref_desc[1].flags = VIRTIO_DESC_F_WRITE;
    len = 0;
    response->type = 0;

    g_virtio_gpu_backend.resource_unref(&vgpu, unref_desc, &len);
    require_u32("3d unref submit failure response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("3d unref submit failure response", response->type,
                VIRTIO_GPU_RESP_ERR_UNSPEC);

    vgpu_renderer_reset_queues(renderer_generation);
    len = 0;
    response->type = 0;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("sync-failed unref preserves resource response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("sync-failed unref preserves resource", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);

    struct vgpu_renderer_completion cleanup = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_CREATE_3D_ROLLBACK,
                .resource_id = 62,
                .resource_generation = resource_generation,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &cleanup);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_virgl_resource_unref_stale_rollback_after_2d_reuse(void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create_3d =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_res_create_2d *create_2d =
        (struct virtio_gpu_res_create_2d *) ((uint8_t *) ram + 0x140);
    struct virtio_gpu_res_unref *unref =
        (struct virtio_gpu_res_unref *) ((uint8_t *) ram + 0x1c0);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x100);
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x77;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 40);

    create_3d->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create_3d->resource_id = 63;
    create_3d->target = 2;
    create_3d->format = 3;
    create_3d->bind = 4;
    create_3d->width = 64;
    create_3d->height = 64;
    create_3d->depth = 1;
    create_3d->array_size = 1;
    create_3d->nr_samples = 1;
    desc[0].addr = 0x40;
    desc[0].len = sizeof(*create_3d);
    desc[1].addr = 0x100;
    desc[1].len = sizeof(*response);
    desc[1].flags = VIRTIO_DESC_F_WRITE;

    g_virtio_gpu_backend.resource_create_3d(&vgpu, desc, &len);
    require_u32("3d create before 2d reuse is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_renderer_request queued = {0};
    require_int("3d create before 2d reuse queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *payload = queued.payload;
    uint64_t resource_generation = payload->resource_generation;
    queued.release_payload(queued.payload);

    vgpu.ctrl_dispatch.desc_head = 41;
    unref->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    unref->resource_id = 63;
    desc[0].addr = 0x1c0;
    desc[0].len = sizeof(*unref);
    len = 0;
    g_virtio_gpu_backend.resource_unref(&vgpu, desc, &len);
    require_u32("3d unref before 2d reuse is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d unref before 2d reuse queued",
                vgpu_renderer_pop_request(&queued), true);
    queued.release_payload(queued.payload);

    create_2d->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create_2d->resource_id = 63;
    create_2d->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    create_2d->width = 8;
    create_2d->height = 8;
    desc[0].addr = 0x140;
    desc[0].len = sizeof(*create_2d);
    len = 0;
    response->type = 0;
    g_virtio_gpu_backend.resource_create_2d(&vgpu, desc, &len);
    require_u32("2d reuse after deferred 3d unref response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("2d reuse after deferred 3d unref response", response->type,
                VIRTIO_GPU_RESP_OK_NODATA);

    struct vgpu_renderer_completion stale_commit = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_UNREF,
                .resource_id = 63,
                .resource_generation = resource_generation,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &stale_commit);

    desc[0].addr = 0x40;
    desc[0].len = sizeof(*create_3d);
    len = 0;
    response->type = 0;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, desc, &len);
    require_u32("stale unref commit keeps 2d reuse response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("stale unref commit keeps 2d reuse response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    struct vgpu_renderer_request empty = {0};
    require_int("stale unref commit after 2d reuse queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    struct vgpu_renderer_completion stale_rollback = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_UNREF_ROLLBACK,
                .resource_id = 63,
                .resource_generation = resource_generation,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &stale_rollback);

    desc[0].addr = 0x1c0;
    desc[0].len = sizeof(*unref);
    len = 0;
    response->type = 0;
    g_virtio_gpu_backend.resource_unref(&vgpu, desc, &len);
    require_u32("2d unref after stale 3d side effects response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("2d unref after stale 3d side effects response", response->type,
                VIRTIO_GPU_RESP_OK_NODATA);

    vgpu.ctrl_dispatch.desc_head = 42;
    desc[0].addr = 0x40;
    desc[0].len = sizeof(*create_3d);
    len = 0;
    response->type = 0;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, desc, &len);
    require_u32("3d create after 2d reuse cleanup is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d create after 2d reuse cleanup queued",
                vgpu_renderer_pop_request(&queued), true);
    payload = queued.payload;
    struct vgpu_renderer_completion cleanup = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_CREATE_3D_ROLLBACK,
                .resource_id = 63,
                .resource_generation = payload->resource_generation,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &cleanup);
    queued.release_payload(queued.payload);

    destroy_vgpu_test_state(&emu, &vgpu);
}


static void test_virgl_resource_attach_backing_snapshots_iov_and_defers(void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc attach_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc detach_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_res_attach_backing *attach =
        (struct virtio_gpu_res_attach_backing *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_res_detach_backing *detach =
        (struct virtio_gpu_res_detach_backing *) ((uint8_t *) ram + 0x160);
    struct virtio_gpu_mem_entry *entries =
        (struct virtio_gpu_mem_entry *) ((uint8_t *) ram + 0x1c0);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x240);
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x78;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 50);

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = 64;
    create->target = 2;
    create->format = 3;
    create->bind = 4;
    create->width = 64;
    create->height = 64;
    create->depth = 1;
    create->array_size = 1;
    create->nr_samples = 1;
    create_desc[0].addr = 0x40;
    create_desc[0].len = sizeof(*create);
    create_desc[1].addr = 0x240;
    create_desc[1].len = sizeof(*response);
    create_desc[1].flags = VIRTIO_DESC_F_WRITE;

    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("3d create before attach is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_renderer_request queued = {0};
    require_int("3d create before attach queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *create_payload = queued.payload;
    uint64_t resource_generation = create_payload->resource_generation;
    queued.release_payload(queued.payload);

    attach->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    attach->hdr.fence_id = UINT64_C(0x123456789abcdef0);
    attach->resource_id = 64;
    attach->nr_entries = 2;
    entries[0].addr = 0x300;
    entries[0].length = 16;
    entries[1].addr = 0x340;
    entries[1].length = 32;
    attach_desc[0].addr = 0x100;
    attach_desc[0].len = sizeof(*attach);
    attach_desc[1].addr = 0x1c0;
    attach_desc[1].len = 2 * sizeof(*entries);
    attach_desc[2].addr = 0x240;
    attach_desc[2].len = sizeof(*response);
    attach_desc[2].flags = VIRTIO_DESC_F_WRITE;
    len = 0;
    response->type = 0;

    g_virtio_gpu_backend.resource_attach_backing(&vgpu, attach_desc, &len);
    require_u32("3d attach is deferred", len, VIRTIO_GPU_RESPONSE_DEFERRED);

    attach->resource_id = 999;
    entries[0].addr = 0x380;
    entries[0].length = 4;

    require_int("3d attach queued", vgpu_renderer_pop_request(&queued), true);
    require_u32("3d attach command type", queued.command_type,
                VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    struct vgpu_renderer_ctrl_payload *attach_payload = queued.payload;
    void (*attach_release_payload)(void *) = queued.release_payload;
    require_u32("3d attach snapshots resource id",
                attach_payload->cmd.resource_attach_backing.resource_id, 64);
    require_u32("3d attach snapshots entry count",
                attach_payload->cmd.resource_attach_backing.nr_entries, 2);
    require_u64("3d attach carries resource generation",
                attach_payload->resource_generation, resource_generation);
    require_u64("3d attach snapshots fence", attach_payload->hdr.fence_id,
                UINT64_C(0x123456789abcdef0));
    require_u32("3d attach iov count", attach_payload->iov_count, 2);
    require_ptr("3d attach iov0 base", attach_payload->iov[0].iov_base,
                (uint8_t *) ram + 0x300);
    require_u64("3d attach iov0 len", attach_payload->iov[0].iov_len, 16);
    require_ptr("3d attach iov1 base", attach_payload->iov[1].iov_base,
                (uint8_t *) ram + 0x340);
    require_u64("3d attach iov1 len", attach_payload->iov[1].iov_len, 32);

    attach->resource_id = 64;
    attach->hdr.fence_id = UINT64_C(0x1111);
    len = 0;
    response->type = 0;
    g_virtio_gpu_backend.resource_attach_backing(&vgpu, attach_desc, &len);
    require_u32("duplicate pending 3d attach response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("duplicate pending 3d attach response", response->type,
                VIRTIO_GPU_RESP_ERR_UNSPEC);
    struct vgpu_renderer_request empty = {0};
    require_int("duplicate pending 3d attach queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    detach->hdr.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    detach->resource_id = 64;
    detach_desc[0].addr = 0x160;
    detach_desc[0].len = sizeof(*detach);
    detach_desc[1].addr = 0x240;
    detach_desc[1].len = sizeof(*response);
    detach_desc[1].flags = VIRTIO_DESC_F_WRITE;
    len = 0;
    response->type = 0;
    g_virtio_gpu_backend.resource_detach_backing(&vgpu, detach_desc, &len);
    require_u32("3d detach before attach completion response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("3d detach before attach completion response", response->type,
                VIRTIO_GPU_RESP_ERR_UNSPEC);
    require_int("3d detach before attach completion queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    struct vgpu_renderer_completion attach_success = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_ATTACH_BACKING,
                .resource_id = 64,
                .resource_generation = resource_generation,
                .backing_transition_success = true,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &attach_success);

    vgpu.ctrl_dispatch.desc_head = 51;
    len = 0;
    response->type = 0;
    g_virtio_gpu_backend.resource_detach_backing(&vgpu, detach_desc, &len);
    require_u32("3d detach is deferred", len, VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d detach queued", vgpu_renderer_pop_request(&queued), true);
    require_u32("3d detach command type", queued.command_type,
                VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING);
    struct vgpu_renderer_ctrl_payload *detach_payload = queued.payload;
    void (*detach_release_payload)(void *) = queued.release_payload;
    require_u32("3d detach snapshots resource id",
                detach_payload->cmd.resource_detach_backing.resource_id, 64);
    require_u64("3d detach carries resource generation",
                detach_payload->resource_generation, resource_generation);

    struct vgpu_renderer_completion stale_detach = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_DETACH_BACKING,
                .resource_id = 64,
                .resource_generation = resource_generation + 1,
                .backing_transition_success = true,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &stale_detach);

    len = 0;
    response->type = 0;
    g_virtio_gpu_backend.resource_detach_backing(&vgpu, detach_desc, &len);
    require_u32("stale 3d detach side effect keeps pending response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("stale 3d detach side effect keeps pending response",
                response->type, VIRTIO_GPU_RESP_ERR_UNSPEC);
    require_int("stale 3d detach side effect queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    stale_detach.virgl_resource.resource_generation = resource_generation;
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &stale_detach);

    len = 0;
    response->type = 0;
    g_virtio_gpu_backend.resource_detach_backing(&vgpu, detach_desc, &len);
    require_u32("3d detach after detach completion response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("3d detach after detach completion response", response->type,
                VIRTIO_GPU_RESP_ERR_UNSPEC);

    attach_release_payload(attach_payload);
    detach_release_payload(detach_payload);

    struct vgpu_renderer_completion cleanup = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_CREATE_3D_ROLLBACK,
                .resource_id = 64,
                .resource_generation = resource_generation,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &cleanup);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_virgl_resource_attach_rejects_malformed_backing_list(void)
{
    uint32_t ram[512] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc attach_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_res_attach_backing *attach =
        (struct virtio_gpu_res_attach_backing *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x180);
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x79;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 60);

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = 65;
    create->target = 2;
    create->format = 3;
    create->bind = 4;
    create->width = 64;
    create->height = 64;
    create->depth = 1;
    create->array_size = 1;
    create->nr_samples = 1;
    create_desc[0].addr = 0x40;
    create_desc[0].len = sizeof(*create);
    create_desc[1].addr = 0x180;
    create_desc[1].len = sizeof(*response);
    create_desc[1].flags = VIRTIO_DESC_F_WRITE;

    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("3d create before malformed attach is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_renderer_request queued = {0};
    require_int("3d create before malformed attach queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *payload = queued.payload;
    uint64_t resource_generation = payload->resource_generation;
    queued.release_payload(queued.payload);

    attach->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach->resource_id = 65;
    attach->nr_entries = 1;
    attach_desc[0].addr = 0x100;
    attach_desc[0].len = sizeof(*attach);
    attach_desc[1].addr = 0x140;
    attach_desc[1].len = sizeof(struct virtio_gpu_mem_entry) - 1;
    attach_desc[2].addr = 0x180;
    attach_desc[2].len = sizeof(*response);
    attach_desc[2].flags = VIRTIO_DESC_F_WRITE;
    len = 0;
    response->type = 0;

    g_virtio_gpu_backend.resource_attach_backing(&vgpu, attach_desc, &len);
    require_u32("malformed 3d attach response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("malformed 3d attach response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    struct vgpu_renderer_request empty = {0};
    require_int("malformed 3d attach queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    struct virtio_gpu_mem_entry *late_entry =
        (struct virtio_gpu_mem_entry *) ((uint8_t *) ram + 0x1c0);
    late_entry->addr = 0x200;
    late_entry->length = 16;
    attach_desc[1].len = 0;
    attach_desc[2].addr = 0x180;
    attach_desc[2].len = sizeof(*response);
    attach_desc[2].flags = VIRTIO_DESC_F_WRITE;
    attach_desc[3].addr = 0x1c0;
    attach_desc[3].len = sizeof(*late_entry);
    attach_desc[3].flags = 0;
    len = 0;
    response->type = 0;

    g_virtio_gpu_backend.resource_attach_backing(&vgpu, attach_desc, &len);
    require_u32("late-readable 3d attach response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("late-readable 3d attach response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    require_int("late-readable 3d attach queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    struct vgpu_renderer_completion cleanup = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_CREATE_3D_ROLLBACK,
                .resource_id = 65,
                .resource_generation = resource_generation,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &cleanup);

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

#if SEMU_HAS(VIRGL)
static void test_vgpu_common_reset_queues_renderer_reset_request(void)
{
    uint32_t ram[64] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    uint64_t old_generation;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    old_generation = vgpu.common.generation;
    vgpu_renderer_reset_queues(old_generation);

    require_int("common reset", virtio_device_common_reset(&vgpu.common), 0);
    require_false("common generation advanced",
                  vgpu.common.generation == old_generation);

    struct vgpu_renderer_request request = {0};
    require_int("renderer reset request queued",
                vgpu_renderer_pop_request(&request), true);
    require_u32("renderer reset request type", request.type,
                VGPU_RENDERER_REQ_RESET);
    require_u64("renderer reset request generation", request.token.generation,
                vgpu.common.generation);
    require_ptr("renderer reset request payload", request.payload, NULL);
    require_ptr("renderer reset release hook",
                (const void *) request.release_payload, NULL);
    require_int("only one renderer reset request",
                vgpu_renderer_pop_request(&request), false);

    destroy_vgpu_test_state(&emu, &vgpu);
}
#endif

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
#if SEMU_HAS(VIRGL)
    test_vgpu_common_reset_queues_renderer_reset_request();
#endif
    test_vgpu_display_counters_snapshot_reads_existing_counters();
    test_undefined_command_returns_device_error();
    test_vgpu_resource_unref_releases_2d_resource_synchronously();
    test_vgpu_resource_attach_detach_keeps_2d_path_synchronous();
    test_vgpu_actor_failure_marks_device_reset_needed();
    test_vgpu_failed_actor_notify_counts_eio();
    test_vgpu_invalid_actor_notify_counts_einval();
    test_vgpu_virgl_gate_keeps_unsupported_features_hidden();
    test_deferred_ctrl_completion_revalidates_generations();
#if SEMU_HAS(VIRGL)
    test_hidden_virgl_command_returns_undefined_without_renderer_work();
    test_virgl_capset_info_handler_submits_host_owned_ctrl_payload();
    test_virgl_resource_create_3d_handler_tracks_pending_resource();
    test_virgl_resource_create_3d_rejects_live_2d_resource_id();
    test_virgl_resource_unref_handler_defers_and_frees_namespace();
    test_virgl_resource_unref_sync_submit_failure_restores_namespace();
    test_virgl_resource_unref_stale_rollback_after_2d_reuse();
    test_virgl_resource_attach_backing_snapshots_iov_and_defers();
    test_virgl_resource_attach_rejects_malformed_backing_list();
    test_virgl_context_handlers_submit_ctrl_skeletons();
    test_renderer_completion_drain_writes_response_and_used_ring();
    test_renderer_completion_drops_stale_common_generation();
    test_renderer_ctrl_completion_without_metadata_fails();
#endif
    test_vgpu_destroy_releases_common_without_actor();
    test_vgpu_destroy_stops_started_actor_and_is_idempotent();
    return 0;
}
