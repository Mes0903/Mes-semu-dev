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
#include "platform.h"
#include "vgpu-display.h"
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
static uint32_t renderer_release_payload_count;

static void renderer_release_payload(void *payload)
{
    renderer_release_payload_count++;
    free(payload);
}

static void renderer_release_response(void *response)
{
    renderer_release_response_count++;
    free(response);
}

static struct vgpu_display_gl_payload test_gl_payload(uint32_t texture_id)
{
    return (struct vgpu_display_gl_payload) {
        .texture_id = texture_id,
        .width = 320,
        .height = 240,
        .src_x = 0,
        .src_y = 0,
        .src_width = 160,
        .src_height = 120,
        .y_0_top = false,
    };
}


static void drain_display_queue(void)
{
    struct vgpu_display_cmd cmd;

    while (vgpu_display_pop_cmd(&cmd))
        vgpu_display_release_cmd(&cmd);
}

static struct vgpu_display_payload *alloc_test_gl_display_payload(
    uint32_t texture_id)
{
    struct vgpu_display_payload *payload = calloc(1, sizeof(*payload));

    if (!payload) {
        fprintf(stderr, "failed to allocate test GL display payload\n");
        exit(1);
    }

    payload->type = VGPU_DISPLAY_PAYLOAD_GL;
    payload->gl = test_gl_payload(texture_id);
    return payload;
}

static void fill_display_primary_queue(void)
{
    for (uint32_t i = 0; i < VGPU_RENDERER_QUEUE_CAPACITY; i++) {
        struct vgpu_display_payload *payload =
            alloc_test_gl_display_payload(0x9000u + i);
        enum vgpu_display_publish_result result =
            vgpu_display_publish_primary_set(0, payload);

        if (result == VGPU_DISPLAY_PUBLISH_QUEUE_FULL) {
            free(payload);
            return;
        }
        if (result != VGPU_DISPLAY_PUBLISH_OK) {
            fprintf(stderr,
                    "fill display queue publish result: got 0x%x, want 0x%x\n",
                    result, VGPU_DISPLAY_PUBLISH_OK);
            exit(1);
        }
    }

    fprintf(stderr, "display queue did not reach full state\n");
    exit(1);
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
    require_false("payload release hook installed",
                  queued.release_payload == NULL);

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

static void test_hidden_blob_command_returns_undefined_without_renderer_work(
    void)
{
    uint32_t ram[512] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtio_gpu_resource_create_blob *blob =
        (struct virtio_gpu_resource_create_blob *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x80);
    struct virtq_desc desc0 = {
        .addr = 0x40,
        .len = sizeof(*blob),
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

    blob->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    blob->resource_id = 77;
    blob->blob_mem = VIRTIO_GPU_BLOB_MEM_HOST3D;
    blob->size = 4096;
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
        "notify hidden blob create",
        vgpu.common.ops->notify_queue(vgpu.common.opaque, VIRTIO_GPU_CONTROLQ,
                                      vgpu.common.generation),
        0);
    wait_for_used_idx(ram, 0x302, 1);

    require_u32("hidden blob response", response->type,
                VIRTIO_GPU_RESP_ERR_UNSPEC);
    require_u32("hidden blob used id", load_u32(ram, 0x304), 0);
    require_u32("hidden blob used len", load_u32(ram, 0x308),
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_int("hidden blob queues no renderer work",
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


static void test_virgl_resource_create_blob_handler_tracks_pending_resource(
    void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc blob_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc create_2d_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_blob *blob =
        (struct virtio_gpu_resource_create_blob *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_mem_entry *entries =
        (struct virtio_gpu_mem_entry *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_res_create_2d *create_2d =
        (struct virtio_gpu_res_create_2d *) ((uint8_t *) ram + 0x180);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x240);
    struct virtio_gpu_ctrl_hdr *response_2d =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x280);
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x74;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 35);

    blob->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    blob->hdr.ctx_id = 9;
    blob->resource_id = 58;
    blob->blob_mem = VIRTIO_GPU_BLOB_MEM_GUEST;
    blob->blob_flags = VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE;
    blob->nr_entries = 2;
    blob->blob_id = UINT64_C(0x1122334455667788);
    blob->size = 48;
    entries[0].addr = 0x300;
    entries[0].length = 16;
    entries[1].addr = 0x340;
    entries[1].length = 32;
    blob_desc[0].addr = 0x40;
    blob_desc[0].len = sizeof(*blob);
    blob_desc[1].addr = 0x100;
    blob_desc[1].len = 2 * sizeof(*entries);
    blob_desc[2].addr = 0x240;
    blob_desc[2].len = sizeof(*response);
    blob_desc[2].flags = VIRTIO_DESC_F_WRITE;

    struct vgpu_renderer_request empty = {0};

    blob->blob_mem = 0;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.resource_create_blob(&vgpu, blob_desc, &len);
    require_u32("blob invalid mem response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("blob invalid mem response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    require_int("blob invalid mem queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    blob->blob_mem = VIRTIO_GPU_BLOB_MEM_GUEST;
    blob->blob_flags = UINT32_C(0x80000000);
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.resource_create_blob(&vgpu, blob_desc, &len);
    require_u32("blob invalid flags response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("blob invalid flags response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    require_int("blob invalid flags queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    blob->blob_flags = VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE;
    blob->nr_entries = UINT32_MAX;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.resource_create_blob(&vgpu, blob_desc, &len);
    require_u32("blob too many entries response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("blob too many entries response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    require_int("blob too many entries queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    blob->nr_entries = 2;
    blob_desc[1].len = sizeof(*entries);
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.resource_create_blob(&vgpu, blob_desc, &len);
    require_u32("blob short backing list response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("blob short backing list response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    require_int("blob short backing list queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    blob_desc[1].len = 2 * sizeof(*entries);
    entries[1].addr = UINT64_C(0x100000000);
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.resource_create_blob(&vgpu, blob_desc, &len);
    require_u32("blob invalid backing address response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("blob invalid backing address response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    require_int("blob invalid backing address queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    entries[1].addr = 0x340;
    g_virtio_gpu_backend.resource_create_blob(&vgpu, blob_desc, &len);
    require_u32("blob create is deferred", len, VIRTIO_GPU_RESPONSE_DEFERRED);

    struct vgpu_renderer_request queued = {0};
    require_int("blob create queued", vgpu_renderer_pop_request(&queued), true);
    require_u32("blob create command type", queued.command_type,
                VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB);
    struct vgpu_renderer_ctrl_payload *payload = queued.payload;
    uint64_t first_generation = payload->resource_generation;
    require_false("blob resource generation assigned", first_generation == 0);
    require_u32("blob snapshot resource id",
                payload->cmd.resource_create_blob.resource_id, 58);
    require_u32("blob snapshot ctx id",
                payload->cmd.resource_create_blob.hdr.ctx_id, 9);
    require_u32("blob snapshot mem", payload->cmd.resource_create_blob.blob_mem,
                VIRTIO_GPU_BLOB_MEM_GUEST);
    require_u32("blob snapshot flags",
                payload->cmd.resource_create_blob.blob_flags,
                VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE);
    require_u32("blob snapshot nr entries",
                payload->cmd.resource_create_blob.nr_entries, 2);
    require_u64("blob snapshot id", payload->cmd.resource_create_blob.blob_id,
                UINT64_C(0x1122334455667788));
    require_u64("blob snapshot size", payload->cmd.resource_create_blob.size,
                48);
    require_u32("blob iov count", payload->iov_count, 2);
    require_ptr("blob iov0 base", payload->iov[0].iov_base,
                (uint8_t *) ram + 0x300);
    require_u64("blob iov0 len", payload->iov[0].iov_len, 16);
    require_ptr("blob iov1 base", payload->iov[1].iov_base,
                (uint8_t *) ram + 0x340);
    require_u64("blob iov1 len", payload->iov[1].iov_len, 32);

    blob->resource_id = 999;
    blob->hdr.ctx_id = 99;
    blob->blob_mem = VIRTIO_GPU_BLOB_MEM_HOST3D;
    blob->nr_entries = 0;
    entries[0].addr = 0x380;
    require_u32("blob payload is host-owned resource id",
                payload->cmd.resource_create_blob.resource_id, 58);
    require_u32("blob payload is host-owned ctx id",
                payload->cmd.resource_create_blob.hdr.ctx_id, 9);
    require_u32("blob payload is host-owned mem",
                payload->cmd.resource_create_blob.blob_mem,
                VIRTIO_GPU_BLOB_MEM_GUEST);
    queued.release_payload(queued.payload);

    blob->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    blob->resource_id = 58;
    blob->hdr.ctx_id = 9;
    blob->blob_mem = VIRTIO_GPU_BLOB_MEM_GUEST;
    blob->blob_flags = VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE;
    blob->nr_entries = 2;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.resource_create_blob(&vgpu, blob_desc, &len);
    require_u32("duplicate pending blob response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("duplicate pending blob response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    require_int("duplicate pending blob queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    create_2d->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create_2d->resource_id = 58;
    create_2d->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    create_2d->width = 8;
    create_2d->height = 8;
    create_2d_desc[0].addr = 0x180;
    create_2d_desc[0].len = sizeof(*create_2d);
    create_2d_desc[1].addr = 0x280;
    create_2d_desc[1].len = sizeof(*response_2d);
    create_2d_desc[1].flags = VIRTIO_DESC_F_WRITE;
    len = 0;
    g_virtio_gpu_backend.resource_create_2d(&vgpu, create_2d_desc, &len);
    require_u32("2d duplicate of pending blob response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("2d duplicate of pending blob response", response_2d->type,
                VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);

    struct vgpu_renderer_completion cleanup = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_CREATE_3D_ROLLBACK,
                .resource_id = 58,
                .resource_generation = first_generation,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &cleanup);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_virgl_resource_map_unmap_blob_frontend_policy(void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_blob_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc create_3d_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc map_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc unmap_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_blob *create_blob =
        (struct virtio_gpu_resource_create_blob *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_resource_create_3d *create_3d =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0xc0);
    struct virtio_gpu_resource_map_blob *map =
        (struct virtio_gpu_resource_map_blob *) ((uint8_t *) ram + 0x140);
    struct virtio_gpu_resource_unmap_blob *unmap =
        (struct virtio_gpu_resource_unmap_blob *) ((uint8_t *) ram + 0x1c0);
    struct virtio_gpu_resp_map_info *map_response =
        (struct virtio_gpu_resp_map_info *) ((uint8_t *) ram + 0x240);
    struct virtio_gpu_ctrl_hdr *ctrl_response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x2c0);
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x7b;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 61);

    create_blob->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    create_blob->resource_id = 101;
    create_blob->blob_mem = VIRTIO_GPU_BLOB_MEM_HOST3D;
    create_blob->blob_flags = VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE;
    create_blob->size = 4096;
    create_blob_desc[0].addr = 0x40;
    create_blob_desc[0].len = sizeof(*create_blob);
    create_blob_desc[1].addr = 0x2c0;
    create_blob_desc[1].len = sizeof(*ctrl_response);
    create_blob_desc[1].flags = VIRTIO_DESC_F_WRITE;
    g_virtio_gpu_backend.resource_create_blob(&vgpu, create_blob_desc, &len);
    require_u32("blob create before map is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_renderer_request queued = {0};
    require_int("blob create before map queued",
                vgpu_renderer_pop_request(&queued), true);
    queued.release_payload(queued.payload);

    map->hdr.type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    map->hdr.flags = VIRTIO_GPU_FLAG_FENCE | VIRTIO_GPU_FLAG_INFO_RING_IDX;
    map->hdr.fence_id = UINT64_C(0x1111222233334444);
    map->hdr.ctx_id = 12;
    map->hdr.ring_idx = 5;
    map->resource_id = 101;
    map_desc[0].addr = 0x140;
    map_desc[0].len = sizeof(*map);
    map_desc[1].addr = 0x240;
    map_desc[1].len = sizeof(*map_response);
    map_desc[1].flags = VIRTIO_DESC_F_WRITE;
    vgpu.ctrl_dispatch.desc_head = 62;
    len = 0;
    g_virtio_gpu_backend.resource_map_blob(&vgpu, map_desc, &len);
    require_u32("blob map is deferred", len, VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("blob map queued", vgpu_renderer_pop_request(&queued), true);
    require_u32("blob map command type", queued.command_type,
                VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB);
    struct vgpu_renderer_ctrl_payload *map_payload = queued.payload;
    require_u32("blob map snapshot resource id",
                map_payload->cmd.resource_map_blob.resource_id, 101);
    require_u32("blob map response capacity", map_payload->response_capacity,
                sizeof(*map_response));
    require_u32("blob map response type", map_payload->response_type,
                VIRTIO_GPU_RESP_OK_MAP_INFO);
    require_u32("blob map response desc len", map_payload->response_desc.len,
                sizeof(*map_response));
    map->resource_id = 999;
    map->hdr.ctx_id = 99;
    require_u32("blob map payload is host-owned resource id",
                map_payload->cmd.resource_map_blob.resource_id, 101);
    require_u32("blob map payload is host-owned ctx id",
                map_payload->cmd.resource_map_blob.hdr.ctx_id, 12);
    queued.release_payload(queued.payload);

    unmap->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB;
    unmap->resource_id = 101;
    unmap_desc[0].addr = 0x1c0;
    unmap_desc[0].len = sizeof(*unmap);
    unmap_desc[1].addr = 0x2c0;
    unmap_desc[1].len = sizeof(*ctrl_response);
    unmap_desc[1].flags = VIRTIO_DESC_F_WRITE;
    vgpu.ctrl_dispatch.desc_head = 63;
    len = 0;
    g_virtio_gpu_backend.resource_unmap_blob(&vgpu, unmap_desc, &len);
    require_u32("blob unmap is deferred", len, VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("blob unmap queued", vgpu_renderer_pop_request(&queued), true);
    require_u32("blob unmap command type", queued.command_type,
                VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB);
    struct vgpu_renderer_ctrl_payload *unmap_payload = queued.payload;
    require_u32("blob unmap snapshot resource id",
                unmap_payload->cmd.resource_unmap_blob.resource_id, 101);
    require_u32("blob unmap response capacity",
                unmap_payload->response_capacity, sizeof(*ctrl_response));
    require_u32("blob unmap response type", unmap_payload->response_type,
                VIRTIO_GPU_RESP_OK_NODATA);
    unmap->resource_id = 999;
    require_u32("blob unmap payload is host-owned resource id",
                unmap_payload->cmd.resource_unmap_blob.resource_id, 101);
    queued.release_payload(queued.payload);

    map->resource_id = 404;
    map_response->hdr.type = 0;
    len = 0;
    g_virtio_gpu_backend.resource_map_blob(&vgpu, map_desc, &len);
    require_u32("missing blob map response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("missing blob map response", map_response->hdr.type,
                VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    require_int("missing blob map queues no renderer work",
                vgpu_renderer_pop_request(&queued), false);

    unmap->resource_id = 404;
    ctrl_response->type = 0;
    len = 0;
    g_virtio_gpu_backend.resource_unmap_blob(&vgpu, unmap_desc, &len);
    require_u32("missing blob unmap response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("missing blob unmap response", ctrl_response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    require_int("missing blob unmap queues no renderer work",
                vgpu_renderer_pop_request(&queued), false);

    create_3d->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create_3d->resource_id = 102;
    create_3d->target = 2;
    create_3d->format = 3;
    create_3d->bind = 4;
    create_3d->width = 32;
    create_3d->height = 32;
    create_3d->depth = 1;
    create_3d->array_size = 1;
    create_3d->nr_samples = 1;
    create_3d_desc[0].addr = 0xc0;
    create_3d_desc[0].len = sizeof(*create_3d);
    create_3d_desc[1].addr = 0x2c0;
    create_3d_desc[1].len = sizeof(*ctrl_response);
    create_3d_desc[1].flags = VIRTIO_DESC_F_WRITE;
    len = 0;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_3d_desc, &len);
    require_u32("3d create before non-blob map is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d create before non-blob map queued",
                vgpu_renderer_pop_request(&queued), true);
    queued.release_payload(queued.payload);

    map->resource_id = 102;
    map_response->hdr.type = 0;
    len = 0;
    g_virtio_gpu_backend.resource_map_blob(&vgpu, map_desc, &len);
    require_u32("non-blob map response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("non-blob map response", map_response->hdr.type,
                VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    require_int("non-blob map queues no renderer work",
                vgpu_renderer_pop_request(&queued), false);

    unmap->resource_id = 102;
    ctrl_response->type = 0;
    len = 0;
    g_virtio_gpu_backend.resource_unmap_blob(&vgpu, unmap_desc, &len);
    require_u32("non-blob unmap response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("non-blob unmap response", ctrl_response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    require_int("non-blob unmap queues no renderer work",
                vgpu_renderer_pop_request(&queued), false);

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

static void test_virgl_set_scanout_3d_defers_and_commits_by_generation(void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc scanout_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_set_scanout *scanout =
        (struct virtio_gpu_set_scanout *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x180);
    virtio_gpu_data_t *data;
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x76;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    virtio_gpu_register_scanout(&vgpu, 1024, 768);
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 39);
    data = vgpu.priv;

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = 62;
    create->target = 2;
    create->format = 3;
    create->bind = 4;
    create->width = 320;
    create->height = 240;
    create->depth = 1;
    create->array_size = 1;
    create->nr_samples = 1;
    create_desc[0].addr = 0x40;
    create_desc[0].len = sizeof(*create);
    create_desc[1].addr = 0x180;
    create_desc[1].len = sizeof(*response);
    create_desc[1].flags = VIRTIO_DESC_F_WRITE;

    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("3d create for scanout is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_renderer_request queued = {0};
    require_int("3d create for scanout queued",
                vgpu_renderer_pop_request(&queued), true);
    queued.release_payload(queued.payload);

    scanout->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout->r.x = 4;
    scanout->r.y = 8;
    scanout->r.width = 160;
    scanout->r.height = 120;
    scanout->scanout_id = 0;
    scanout->resource_id = 62;
    scanout_desc[0].addr = 0x100;
    scanout_desc[0].len = sizeof(*scanout);
    scanout_desc[1].addr = 0x180;
    scanout_desc[1].len = sizeof(*response);
    scanout_desc[1].flags = VIRTIO_DESC_F_WRITE;
    vgpu.ctrl_dispatch.desc_head = 40;
    len = 0;
    response->type = 0;

    g_virtio_gpu_backend.set_scanout(&vgpu, scanout_desc, &len);
    require_u32("3d set scanout is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_u32("3d set scanout does not commit before renderer",
                data->scanouts[0].primary_resource_id, 0);

    require_int("3d set scanout queued", vgpu_renderer_pop_request(&queued),
                true);
    require_u32("3d set scanout command type", queued.command_type,
                VIRTIO_GPU_CMD_SET_SCANOUT);
    struct vgpu_renderer_ctrl_payload *payload = queued.payload;
    require_u32("3d set scanout snapshots scanout id",
                payload->cmd.set_scanout.scanout_id, 0);
    require_u32("3d set scanout snapshots resource id",
                payload->cmd.set_scanout.resource_id, 62);
    require_u32("3d set scanout snapshots rect x", payload->cmd.set_scanout.r.x,
                4);
    require_u32("3d set scanout snapshots rect width",
                payload->cmd.set_scanout.r.width, 160);
    require_false("3d set scanout generation assigned",
                  payload->scanout_generation == 0);

    scanout->resource_id = 999;
    scanout->r.width = 1;
    require_u32("3d set scanout payload is host-owned resource id",
                payload->cmd.set_scanout.resource_id, 62);
    require_u32("3d set scanout payload is host-owned width",
                payload->cmd.set_scanout.r.width, 160);

    struct vgpu_renderer_completion stale = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT,
                .scanouts =
                    {
                        {
                            .scanout_id = 0,
                            .scanout_generation =
                                payload->scanout_generation + 1,
                            .has_gl_payload = true,
                            .gl_payload =
                                {
                                    .texture_id = 0x6200,
                                    .width = 320,
                                    .height = 240,
                                    .src_x = 4,
                                    .src_y = 8,
                                    .src_width = 160,
                                    .src_height = 120,
                                },
                            .scanout =
                                {
                                    .enabled = 1,
                                    .width = data->scanouts[0].width,
                                    .height = data->scanouts[0].height,
                                    .primary_resource_id = 62,
                                    .src_x = 4,
                                    .src_y = 8,
                                    .src_w = 160,
                                    .src_h = 120,
                                },
                        },
                    },
                .scanout_count = 1,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &stale);
    require_u32("stale 3d set scanout side effect does not commit",
                data->scanouts[0].primary_resource_id, 0);

    stale.virgl_resource.scanouts[0].scanout_generation =
        payload->scanout_generation;
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &stale);
    require_u32("3d set scanout commit resource",
                data->scanouts[0].primary_resource_id, 62);
    require_u32("3d set scanout commit src x", data->scanouts[0].src_x, 4);
    require_u32("3d set scanout commit src y", data->scanouts[0].src_y, 8);
    require_u32("3d set scanout commit src width", data->scanouts[0].src_w,
                160);
    require_u32("3d set scanout commit src height", data->scanouts[0].src_h,
                120);

    queued.release_payload(queued.payload);
    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_virgl_set_scanout_3d_enqueue_failure_cancels_generation(void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc scanout_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_set_scanout *scanout =
        (struct virtio_gpu_set_scanout *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x180);
    virtio_gpu_data_t *data;
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x76;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    virtio_gpu_register_scanout(&vgpu, 1024, 768);
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 40);
    data = vgpu.priv;

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = 63;
    create->target = 2;
    create->format = 3;
    create->bind = 4;
    create->width = 320;
    create->height = 240;
    create->depth = 1;
    create->array_size = 1;
    create->nr_samples = 1;
    create_desc[0].addr = 0x40;
    create_desc[0].len = sizeof(*create);
    create_desc[1].addr = 0x180;
    create_desc[1].len = sizeof(*response);
    create_desc[1].flags = VIRTIO_DESC_F_WRITE;

    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("3d create for failed scanout is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_renderer_request queued = {0};
    require_int("3d create for failed scanout queued",
                vgpu_renderer_pop_request(&queued), true);
    queued.release_payload(queued.payload);

    scanout->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout->r.x = 0;
    scanout->r.y = 0;
    scanout->r.width = 160;
    scanout->r.height = 120;
    scanout->scanout_id = 0;
    scanout->resource_id = 63;
    scanout_desc[0].addr = 0x100;
    scanout_desc[0].len = sizeof(*scanout);
    scanout_desc[1].addr = 0x180;
    scanout_desc[1].len = sizeof(*response);
    scanout_desc[1].flags = VIRTIO_DESC_F_WRITE;
    vgpu.ctrl_dispatch.desc_head = 41;
    len = 0;

    g_virtio_gpu_backend.set_scanout(&vgpu, scanout_desc, &len);
    require_u32("first 3d set scanout is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("first 3d set scanout queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *payload = queued.payload;
    uint64_t first_generation = payload->scanout_generation;
    require_false("first 3d set scanout generation assigned",
                  first_generation == 0);
    queued.release_payload(queued.payload);

    for (uint32_t i = 0; i < VGPU_RENDERER_QUEUE_CAPACITY; i++) {
        struct vgpu_renderer_request filler = {
            .type = VGPU_RENDERER_REQ_RESET,
            .token = {.generation = renderer_generation},
        };
        require_int("fill renderer queue before failed scanout",
                    vgpu_renderer_submit(&filler), true);
    }

    vgpu.ctrl_dispatch.desc_head = 42;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.set_scanout(&vgpu, scanout_desc, &len);
    require_u32("failed 3d set scanout response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("failed 3d set scanout response", response->type,
                VIRTIO_GPU_RESP_ERR_UNSPEC);

    struct vgpu_renderer_completion stale = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT,
                .scanouts =
                    {
                        {
                            .scanout_id = 0,
                            .scanout_generation = first_generation + 1,
                            .scanout =
                                {
                                    .enabled = 1,
                                    .width = data->scanouts[0].width,
                                    .height = data->scanouts[0].height,
                                    .primary_resource_id = 63,
                                    .src_w = 160,
                                    .src_h = 120,
                                },
                        },
                    },
                .scanout_count = 1,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &stale);
    require_u32("failed 3d set scanout generation is cancelled",
                data->scanouts[0].primary_resource_id, 0);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_virgl_set_scanout_3d_stale_after_2d_bind_is_ignored(void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_3d_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc create_2d_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc scanout_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create_3d =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_res_create_2d *create_2d =
        (struct virtio_gpu_res_create_2d *) ((uint8_t *) ram + 0x200);
    struct virtio_gpu_set_scanout *scanout =
        (struct virtio_gpu_set_scanout *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x180);
    virtio_gpu_data_t *data;
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x76;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    virtio_gpu_register_scanout(&vgpu, 1024, 768);
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 43);
    data = vgpu.priv;

    create_3d->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create_3d->resource_id = 64;
    create_3d->target = 2;
    create_3d->format = 3;
    create_3d->bind = 4;
    create_3d->width = 320;
    create_3d->height = 240;
    create_3d->depth = 1;
    create_3d->array_size = 1;
    create_3d->nr_samples = 1;
    create_3d_desc[0].addr = 0x40;
    create_3d_desc[0].len = sizeof(*create_3d);
    create_3d_desc[1].addr = 0x180;
    create_3d_desc[1].len = sizeof(*response);
    create_3d_desc[1].flags = VIRTIO_DESC_F_WRITE;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_3d_desc, &len);
    require_u32("3d create before 2d bind race is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_renderer_request queued = {0};
    require_int("3d create before 2d bind race queued",
                vgpu_renderer_pop_request(&queued), true);
    queued.release_payload(queued.payload);

    scanout->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout->r.x = 4;
    scanout->r.y = 8;
    scanout->r.width = 160;
    scanout->r.height = 120;
    scanout->scanout_id = 0;
    scanout->resource_id = 64;
    scanout_desc[0].addr = 0x100;
    scanout_desc[0].len = sizeof(*scanout);
    scanout_desc[1].addr = 0x180;
    scanout_desc[1].len = sizeof(*response);
    scanout_desc[1].flags = VIRTIO_DESC_F_WRITE;
    vgpu.ctrl_dispatch.desc_head = 43;
    len = 0;
    g_virtio_gpu_backend.set_scanout(&vgpu, scanout_desc, &len);
    require_u32("3d set scanout before 2d bind race is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d set scanout before 2d bind race queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *payload = queued.payload;
    uint64_t old_generation = payload->scanout_generation;
    queued.release_payload(queued.payload);

    create_2d->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create_2d->resource_id = 65;
    create_2d->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    create_2d->width = 320;
    create_2d->height = 240;
    create_2d_desc[0].addr = 0x200;
    create_2d_desc[0].len = sizeof(*create_2d);
    create_2d_desc[1].addr = 0x180;
    create_2d_desc[1].len = sizeof(*response);
    create_2d_desc[1].flags = VIRTIO_DESC_F_WRITE;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.resource_create_2d(&vgpu, create_2d_desc, &len);
    require_u32("2d create for bind race response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("2d create for bind race response", response->type,
                VIRTIO_GPU_RESP_OK_NODATA);

    scanout->resource_id = 65;
    scanout->r.x = 0;
    scanout->r.y = 0;
    scanout->r.width = 320;
    scanout->r.height = 240;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.set_scanout(&vgpu, scanout_desc, &len);
    require_u32("2d bind after pending 3d response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("2d bind after pending 3d response", response->type,
                VIRTIO_GPU_RESP_OK_NODATA);
    require_u32("2d bind wins before stale 3d side effect",
                data->scanouts[0].primary_resource_id, 65);

    struct vgpu_renderer_completion stale = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT,
                .scanouts =
                    {
                        {
                            .scanout_id = 0,
                            .scanout_generation = old_generation,
                            .scanout =
                                {
                                    .enabled = 1,
                                    .width = data->scanouts[0].width,
                                    .height = data->scanouts[0].height,
                                    .primary_resource_id = 64,
                                    .src_x = 4,
                                    .src_y = 8,
                                    .src_w = 160,
                                    .src_h = 120,
                                },
                        },
                    },
                .scanout_count = 1,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &stale);
    require_u32("stale 3d side effect cannot replace newer 2d bind",
                data->scanouts[0].primary_resource_id, 65);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_virgl_unref_clears_committed_3d_scanout(void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc scanout_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc unref_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_set_scanout *scanout =
        (struct virtio_gpu_set_scanout *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_res_unref *unref =
        (struct virtio_gpu_res_unref *) ((uint8_t *) ram + 0x140);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x180);
    virtio_gpu_data_t *data;
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x76;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    virtio_gpu_register_scanout(&vgpu, 1024, 768);
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 44);
    data = vgpu.priv;

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = 66;
    create->target = 2;
    create->format = 3;
    create->bind = 4;
    create->width = 320;
    create->height = 240;
    create->depth = 1;
    create->array_size = 1;
    create->nr_samples = 1;
    create_desc[0].addr = 0x40;
    create_desc[0].len = sizeof(*create);
    create_desc[1].addr = 0x180;
    create_desc[1].len = sizeof(*response);
    create_desc[1].flags = VIRTIO_DESC_F_WRITE;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("3d create before unref scanout is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_renderer_request queued = {0};
    require_int("3d create before unref scanout queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *create_payload = queued.payload;
    uint64_t resource_generation = create_payload->resource_generation;
    queued.release_payload(queued.payload);

    scanout->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout->r.width = 160;
    scanout->r.height = 120;
    scanout->scanout_id = 0;
    scanout->resource_id = 66;
    scanout_desc[0].addr = 0x100;
    scanout_desc[0].len = sizeof(*scanout);
    scanout_desc[1].addr = 0x180;
    scanout_desc[1].len = sizeof(*response);
    scanout_desc[1].flags = VIRTIO_DESC_F_WRITE;
    vgpu.ctrl_dispatch.desc_head = 44;
    len = 0;
    g_virtio_gpu_backend.set_scanout(&vgpu, scanout_desc, &len);
    require_u32("3d set scanout before unref is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d set scanout before unref queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *scanout_payload = queued.payload;
    uint64_t scanout_generation = scanout_payload->scanout_generation;
    queued.release_payload(queued.payload);

    struct vgpu_renderer_completion set_scanout = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT,
                .scanouts =
                    {
                        {
                            .scanout_id = 0,
                            .scanout_generation = scanout_generation,
                            .resource_generation = resource_generation,
                            .has_gl_payload = true,
                            .gl_payload =
                                {
                                    .texture_id = 0x6600,
                                    .width = 320,
                                    .height = 240,
                                    .src_width = 160,
                                    .src_height = 120,
                                },
                            .scanout =
                                {
                                    .enabled = 1,
                                    .width = data->scanouts[0].width,
                                    .height = data->scanouts[0].height,
                                    .primary_resource_id = 66,
                                    .src_w = 160,
                                    .src_h = 120,
                                },
                        },
                    },
                .scanout_count = 1,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &set_scanout);
    require_u32("3d scanout committed before unref",
                data->scanouts[0].primary_resource_id, 66);

    unref->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    unref->resource_id = 66;
    unref_desc[0].addr = 0x140;
    unref_desc[0].len = sizeof(*unref);
    unref_desc[1].addr = 0x180;
    unref_desc[1].len = sizeof(*response);
    unref_desc[1].flags = VIRTIO_DESC_F_WRITE;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.resource_unref(&vgpu, unref_desc, &len);
    require_u32("3d unref scanout resource is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d unref scanout resource queued",
                vgpu_renderer_pop_request(&queued), true);
    queued.release_payload(queued.payload);

    struct vgpu_renderer_completion unref_done = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_UNREF,
                .resource_id = 66,
                .resource_generation = resource_generation,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &unref_done);
    require_u32("3d unref clears committed scanout resource",
                data->scanouts[0].primary_resource_id, 0);
    require_u32("3d unref clears committed scanout width",
                data->scanouts[0].src_w, 0);
    require_u32("3d unref clears committed scanout height",
                data->scanouts[0].src_h, 0);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_virgl_unref_completion_preserves_reused_2d_scanout(void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_3d_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc create_2d_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc scanout_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc unref_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create_3d =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_set_scanout *scanout =
        (struct virtio_gpu_set_scanout *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_res_unref *unref =
        (struct virtio_gpu_res_unref *) ((uint8_t *) ram + 0x140);
    struct virtio_gpu_res_create_2d *create_2d =
        (struct virtio_gpu_res_create_2d *) ((uint8_t *) ram + 0x1c0);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x220);
    virtio_gpu_data_t *data;
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x76;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    virtio_gpu_register_scanout(&vgpu, 1024, 768);
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 45);
    data = vgpu.priv;

    create_3d->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create_3d->resource_id = 67;
    create_3d->target = 2;
    create_3d->format = 3;
    create_3d->bind = 4;
    create_3d->width = 320;
    create_3d->height = 240;
    create_3d->depth = 1;
    create_3d->array_size = 1;
    create_3d->nr_samples = 1;
    create_3d_desc[0].addr = 0x40;
    create_3d_desc[0].len = sizeof(*create_3d);
    create_3d_desc[1].addr = 0x220;
    create_3d_desc[1].len = sizeof(*response);
    create_3d_desc[1].flags = VIRTIO_DESC_F_WRITE;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_3d_desc, &len);
    require_u32("3d create before 2d scanout reuse is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_renderer_request queued = {0};
    require_int("3d create before 2d scanout reuse queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *create_payload = queued.payload;
    uint64_t resource_generation = create_payload->resource_generation;
    queued.release_payload(queued.payload);

    scanout->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout->r.width = 160;
    scanout->r.height = 120;
    scanout->scanout_id = 0;
    scanout->resource_id = 67;
    scanout_desc[0].addr = 0x100;
    scanout_desc[0].len = sizeof(*scanout);
    scanout_desc[1].addr = 0x220;
    scanout_desc[1].len = sizeof(*response);
    scanout_desc[1].flags = VIRTIO_DESC_F_WRITE;
    vgpu.ctrl_dispatch.desc_head = 45;
    len = 0;
    g_virtio_gpu_backend.set_scanout(&vgpu, scanout_desc, &len);
    require_u32("3d set scanout before 2d reuse is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d set scanout before 2d reuse queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *scanout_payload = queued.payload;
    uint64_t scanout_generation = scanout_payload->scanout_generation;
    queued.release_payload(queued.payload);

    struct vgpu_renderer_completion set_scanout = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT,
                .scanouts =
                    {
                        {
                            .scanout_id = 0,
                            .scanout_generation = scanout_generation,
                            .resource_generation = resource_generation,
                            .has_gl_payload = true,
                            .gl_payload =
                                {
                                    .texture_id = 0x6700,
                                    .width = 320,
                                    .height = 240,
                                    .src_width = 160,
                                    .src_height = 120,
                                },
                            .scanout =
                                {
                                    .enabled = 1,
                                    .width = data->scanouts[0].width,
                                    .height = data->scanouts[0].height,
                                    .primary_resource_id = 67,
                                    .src_w = 160,
                                    .src_h = 120,
                                },
                        },
                    },
                .scanout_count = 1,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &set_scanout);
    require_u32("3d scanout committed before 2d reuse",
                data->scanouts[0].primary_resource_id, 67);

    unref->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    unref->resource_id = 67;
    unref_desc[0].addr = 0x140;
    unref_desc[0].len = sizeof(*unref);
    unref_desc[1].addr = 0x220;
    unref_desc[1].len = sizeof(*response);
    unref_desc[1].flags = VIRTIO_DESC_F_WRITE;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.resource_unref(&vgpu, unref_desc, &len);
    require_u32("3d unref before 2d scanout reuse is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d unref before 2d scanout reuse queued",
                vgpu_renderer_pop_request(&queued), true);
    queued.release_payload(queued.payload);

    create_2d->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create_2d->resource_id = 67;
    create_2d->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    create_2d->width = 64;
    create_2d->height = 64;
    create_2d_desc[0].addr = 0x1c0;
    create_2d_desc[0].len = sizeof(*create_2d);
    create_2d_desc[1].addr = 0x220;
    create_2d_desc[1].len = sizeof(*response);
    create_2d_desc[1].flags = VIRTIO_DESC_F_WRITE;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.resource_create_2d(&vgpu, create_2d_desc, &len);
    require_u32("2d reuse after 3d scanout unref response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("2d reuse after 3d scanout unref response", response->type,
                VIRTIO_GPU_RESP_OK_NODATA);

    scanout->resource_id = 67;
    scanout->r.x = 0;
    scanout->r.y = 0;
    scanout->r.width = 64;
    scanout->r.height = 64;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.set_scanout(&vgpu, scanout_desc, &len);
    require_u32("2d scanout bind after 3d unref response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("2d scanout bind after 3d unref response", response->type,
                VIRTIO_GPU_RESP_OK_NODATA);
    require_u32("2d scanout bind wins before stale 3d unref",
                data->scanouts[0].primary_resource_id, 67);
    require_u32("2d scanout bind width before stale 3d unref",
                data->scanouts[0].src_w, 64);

    struct vgpu_renderer_completion unref_done = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_UNREF,
                .resource_id = 67,
                .resource_generation = resource_generation,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &unref_done);
    require_u32("stale 3d unref cannot clear reused 2d scanout resource",
                data->scanouts[0].primary_resource_id, 67);
    require_u32("stale 3d unref cannot clear reused 2d scanout width",
                data->scanouts[0].src_w, 64);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_virgl_failed_pending_scanout_preserves_committed_3d_owner(void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc scanout_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc unref_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_set_scanout *scanout =
        (struct virtio_gpu_set_scanout *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_res_unref *unref =
        (struct virtio_gpu_res_unref *) ((uint8_t *) ram + 0x140);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x180);
    virtio_gpu_data_t *data;
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x76;

    drain_display_queue();
    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    virtio_gpu_register_scanout(&vgpu, 1024, 768);
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 48);
    data = vgpu.priv;

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = 71;
    create->target = 2;
    create->format = 3;
    create->bind = 4;
    create->width = 320;
    create->height = 240;
    create->depth = 1;
    create->array_size = 1;
    create->nr_samples = 1;
    create_desc[0].addr = 0x40;
    create_desc[0].len = sizeof(*create);
    create_desc[1].addr = 0x180;
    create_desc[1].len = sizeof(*response);
    create_desc[1].flags = VIRTIO_DESC_F_WRITE;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("old 3d create before pending failure is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_renderer_request queued = {0};
    require_int("old 3d create before pending failure queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *create_payload = queued.payload;
    uint64_t old_resource_generation = create_payload->resource_generation;
    queued.release_payload(queued.payload);

    scanout->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout->r.width = 160;
    scanout->r.height = 120;
    scanout->scanout_id = 0;
    scanout->resource_id = 71;
    scanout_desc[0].addr = 0x100;
    scanout_desc[0].len = sizeof(*scanout);
    scanout_desc[1].addr = 0x180;
    scanout_desc[1].len = sizeof(*response);
    scanout_desc[1].flags = VIRTIO_DESC_F_WRITE;
    vgpu.ctrl_dispatch.desc_head = 48;
    len = 0;
    g_virtio_gpu_backend.set_scanout(&vgpu, scanout_desc, &len);
    require_u32("old 3d set scanout before pending failure is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("old 3d set scanout before pending failure queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *old_scanout_payload = queued.payload;
    uint64_t old_scanout_generation = old_scanout_payload->scanout_generation;
    queued.release_payload(queued.payload);

    struct vgpu_renderer_completion old_set_scanout = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT,
                .scanouts =
                    {
                        {
                            .scanout_id = 0,
                            .scanout_generation = old_scanout_generation,
                            .resource_generation = old_resource_generation,
                            .has_gl_payload = true,
                            .gl_payload = test_gl_payload(0x7100),
                            .scanout =
                                {
                                    .enabled = 1,
                                    .width = data->scanouts[0].width,
                                    .height = data->scanouts[0].height,
                                    .primary_resource_id = 71,
                                    .src_w = 160,
                                    .src_h = 120,
                                },
                        },
                    },
                .scanout_count = 1,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &old_set_scanout);
    require_u32("old 3d scanout committed before pending failure",
                data->scanouts[0].primary_resource_id, 71);
    drain_display_queue();

    create->resource_id = 72;
    vgpu.ctrl_dispatch.desc_head = 49;
    len = 0;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("new 3d create before pending failure is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("new 3d create before pending failure queued",
                vgpu_renderer_pop_request(&queued), true);
    queued.release_payload(queued.payload);

    scanout->resource_id = 72;
    vgpu.ctrl_dispatch.desc_head = 50;
    len = 0;
    g_virtio_gpu_backend.set_scanout(&vgpu, scanout_desc, &len);
    require_u32("new 3d set scanout before failure is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("new 3d set scanout before failure queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *new_scanout_payload = queued.payload;
    uint64_t failed_scanout_generation =
        new_scanout_payload->scanout_generation;
    queued.release_payload(queued.payload);

    struct vgpu_renderer_completion failed_set_scanout = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT_ROLLBACK,
                .scanouts =
                    {
                        {
                            .scanout_id = 0,
                            .scanout_generation = failed_scanout_generation,
                        },
                    },
                .scanout_count = 1,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &failed_set_scanout);
    require_u32("failed pending 3d scanout keeps old frontend resource",
                data->scanouts[0].primary_resource_id, 71);

    unref->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    unref->resource_id = 71;
    unref_desc[0].addr = 0x140;
    unref_desc[0].len = sizeof(*unref);
    unref_desc[1].addr = 0x180;
    unref_desc[1].len = sizeof(*response);
    unref_desc[1].flags = VIRTIO_DESC_F_WRITE;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.resource_unref(&vgpu, unref_desc, &len);
    require_u32("old 3d unref after pending failure is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("old 3d unref after pending failure queued",
                vgpu_renderer_pop_request(&queued), true);
    queued.release_payload(queued.payload);

    struct vgpu_renderer_completion unref_done = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_UNREF,
                .resource_id = 71,
                .resource_generation = old_resource_generation,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &unref_done);
    require_u32("old 3d unref clears scanout after pending failure",
                data->scanouts[0].primary_resource_id, 0);
    require_u32("old 3d unref clears width after pending failure",
                data->scanouts[0].src_w, 0);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_virgl_set_scanout_3d_side_effect_publishes_gl_payload(void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc scanout_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_set_scanout *scanout =
        (struct virtio_gpu_set_scanout *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x180);
    virtio_gpu_data_t *data;
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x76;

    drain_display_queue();
    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    virtio_gpu_register_scanout(&vgpu, 1024, 768);
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 46);
    data = vgpu.priv;

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = 68;
    create->target = 2;
    create->format = 3;
    create->bind = 4;
    create->width = 320;
    create->height = 240;
    create->depth = 1;
    create->array_size = 1;
    create->nr_samples = 1;
    create_desc[0].addr = 0x40;
    create_desc[0].len = sizeof(*create);
    create_desc[1].addr = 0x180;
    create_desc[1].len = sizeof(*response);
    create_desc[1].flags = VIRTIO_DESC_F_WRITE;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("3d create before gl payload scanout is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_renderer_request queued = {0};
    require_int("3d create before gl payload scanout queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *create_payload = queued.payload;
    uint64_t resource_generation = create_payload->resource_generation;
    queued.release_payload(queued.payload);

    scanout->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout->r.x = 4;
    scanout->r.y = 8;
    scanout->r.width = 160;
    scanout->r.height = 120;
    scanout->scanout_id = 0;
    scanout->resource_id = 68;
    scanout_desc[0].addr = 0x100;
    scanout_desc[0].len = sizeof(*scanout);
    scanout_desc[1].addr = 0x180;
    scanout_desc[1].len = sizeof(*response);
    scanout_desc[1].flags = VIRTIO_DESC_F_WRITE;
    vgpu.ctrl_dispatch.desc_head = 46;
    len = 0;
    g_virtio_gpu_backend.set_scanout(&vgpu, scanout_desc, &len);
    require_u32("3d set scanout before gl payload commit is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d set scanout before gl payload commit queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *scanout_payload = queued.payload;
    uint64_t scanout_generation = scanout_payload->scanout_generation;
    queued.release_payload(queued.payload);

    struct vgpu_renderer_completion set_scanout = {
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT,
                .scanouts =
                    {
                        {
                            .scanout_id = 0,
                            .scanout_generation = scanout_generation,
                            .resource_generation = resource_generation,
                            .has_gl_payload = true,
                            .gl_payload =
                                {
                                    .texture_id = 0x6800,
                                    .width = 320,
                                    .height = 240,
                                    .src_x = 4,
                                    .src_y = 8,
                                    .src_width = 160,
                                    .src_height = 120,
                                    .y_0_top = false,
                                },
                            .scanout =
                                {
                                    .enabled = 1,
                                    .width = data->scanouts[0].width,
                                    .height = data->scanouts[0].height,
                                    .primary_resource_id = 68,
                                    .src_x = 4,
                                    .src_y = 8,
                                    .src_w = 160,
                                    .src_h = 120,
                                },
                        },
                    },
                .scanout_count = 1,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &set_scanout);
    require_u32("3d gl scanout side effect keeps success response",
                set_scanout.response_type, VIRTIO_GPU_RESP_OK_NODATA);
    require_u32("3d gl scanout committed resource",
                data->scanouts[0].primary_resource_id, 68);

    struct vgpu_display_cmd cmd = {0};
    require_int("3d gl scanout publishes primary display command",
                vgpu_display_pop_cmd(&cmd), true);
    require_u32("3d gl scanout display command type", cmd.type,
                VGPU_DISPLAY_CMD_PRIMARY_SET);
    require_u32("3d gl scanout display payload type",
                cmd.u.primary_set.payload->type, VGPU_DISPLAY_PAYLOAD_GL);
    require_u32("3d gl scanout texture id",
                cmd.u.primary_set.payload->gl.texture_id, 0x6800);
    require_u32("3d gl scanout texture width",
                cmd.u.primary_set.payload->gl.width, 320);
    require_u32("3d gl scanout texture height",
                cmd.u.primary_set.payload->gl.height, 240);
    require_u32("3d gl scanout source x", cmd.u.primary_set.payload->gl.src_x,
                4);
    require_u32("3d gl scanout source y", cmd.u.primary_set.payload->gl.src_y,
                8);
    require_u32("3d gl scanout source width",
                cmd.u.primary_set.payload->gl.src_width, 160);
    require_u32("3d gl scanout source height",
                cmd.u.primary_set.payload->gl.src_height, 120);
    require_false("3d gl scanout y_0_top",
                  cmd.u.primary_set.payload->gl.y_0_top);
    vgpu_display_release_cmd(&cmd);
    require_int("3d gl scanout publishes one display command",
                vgpu_display_pop_cmd(&cmd), false);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_virgl_set_scanout_3d_publish_failure_does_not_commit(void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc scanout_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_set_scanout *scanout =
        (struct virtio_gpu_set_scanout *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x180);
    virtio_gpu_data_t *data;
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x76;

    drain_display_queue();
    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    virtio_gpu_register_scanout(&vgpu, 1024, 768);
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 47);
    data = vgpu.priv;

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = 69;
    create->target = 2;
    create->format = 3;
    create->bind = 4;
    create->width = 320;
    create->height = 240;
    create->depth = 1;
    create->array_size = 1;
    create->nr_samples = 1;
    create_desc[0].addr = 0x40;
    create_desc[0].len = sizeof(*create);
    create_desc[1].addr = 0x180;
    create_desc[1].len = sizeof(*response);
    create_desc[1].flags = VIRTIO_DESC_F_WRITE;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("3d create before gl publish failure is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_renderer_request queued = {0};
    require_int("3d create before gl publish failure queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *create_payload = queued.payload;
    uint64_t resource_generation = create_payload->resource_generation;
    queued.release_payload(queued.payload);

    scanout->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout->r.width = 160;
    scanout->r.height = 120;
    scanout->scanout_id = 0;
    scanout->resource_id = 69;
    scanout_desc[0].addr = 0x100;
    scanout_desc[0].len = sizeof(*scanout);
    scanout_desc[1].addr = 0x180;
    scanout_desc[1].len = sizeof(*response);
    scanout_desc[1].flags = VIRTIO_DESC_F_WRITE;
    vgpu.ctrl_dispatch.desc_head = 47;
    len = 0;
    g_virtio_gpu_backend.set_scanout(&vgpu, scanout_desc, &len);
    require_u32("3d set scanout before gl publish failure is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d set scanout before gl publish failure queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *scanout_payload = queued.payload;
    uint64_t scanout_generation = scanout_payload->scanout_generation;
    queued.release_payload(queued.payload);

    fill_display_primary_queue();

    struct vgpu_renderer_completion set_scanout = {
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT,
                .scanouts =
                    {
                        {
                            .scanout_id = 0,
                            .scanout_generation = scanout_generation,
                            .resource_generation = resource_generation,
                            .has_gl_payload = true,
                            .gl_payload =
                                {
                                    .texture_id = 0x6900,
                                    .width = 320,
                                    .height = 240,
                                    .src_width = 160,
                                    .src_height = 120,
                                },
                            .scanout =
                                {
                                    .enabled = 1,
                                    .width = data->scanouts[0].width,
                                    .height = data->scanouts[0].height,
                                    .primary_resource_id = 69,
                                    .src_w = 160,
                                    .src_h = 120,
                                },
                        },
                    },
                .scanout_count = 1,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &set_scanout);
    require_u32("3d gl publish failure maps response to error",
                set_scanout.response_type, VIRTIO_GPU_RESP_ERR_UNSPEC);
    require_u32("3d gl publish failure does not commit scanout",
                data->scanouts[0].primary_resource_id, 0);

    struct vgpu_display_cmd queued_cmd = {0};
    require_int("3d gl publish failure preserves queued primary payloads",
                vgpu_display_pop_cmd(&queued_cmd), true);
    vgpu_display_release_cmd(&queued_cmd);

    drain_display_queue();
    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_virgl_set_scanout_3d_rejects_invalid_inputs(void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc scanout_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_set_scanout *scanout =
        (struct virtio_gpu_set_scanout *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x180);
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x76;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    virtio_gpu_register_scanout(&vgpu, 1024, 768);
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 41);

    scanout->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout->r.width = 16;
    scanout->r.height = 16;
    scanout->scanout_id = 0;
    scanout->resource_id = 999;
    scanout_desc[0].addr = 0x100;
    scanout_desc[0].len = sizeof(*scanout);
    scanout_desc[1].addr = 0x180;
    scanout_desc[1].len = sizeof(*response);
    scanout_desc[1].flags = VIRTIO_DESC_F_WRITE;

    g_virtio_gpu_backend.set_scanout(&vgpu, scanout_desc, &len);
    require_u32("missing 3d set scanout response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("missing 3d set scanout response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    struct vgpu_renderer_request queued = {0};
    require_int("missing 3d set scanout queues no renderer work",
                vgpu_renderer_pop_request(&queued), false);

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = 62;
    create->target = 2;
    create->format = 3;
    create->bind = 4;
    create->width = 320;
    create->height = 240;
    create->depth = 1;
    create->array_size = 1;
    create->nr_samples = 1;
    create_desc[0].addr = 0x40;
    create_desc[0].len = sizeof(*create);
    create_desc[1].addr = 0x180;
    create_desc[1].len = sizeof(*response);
    create_desc[1].flags = VIRTIO_DESC_F_WRITE;
    len = 0;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("3d create for invalid scanout is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d create for invalid scanout queued",
                vgpu_renderer_pop_request(&queued), true);
    queued.release_payload(queued.payload);

    scanout->resource_id = 62;
    scanout->scanout_id = VIRTIO_GPU_MAX_SCANOUTS;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.set_scanout(&vgpu, scanout_desc, &len);
    require_u32("invalid 3d scanout response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("invalid 3d scanout response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID);
    require_int("invalid 3d scanout queues no renderer work",
                vgpu_renderer_pop_request(&queued), false);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_virgl_set_scanout_blob_snapshots_and_defers(void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc scanout_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_set_scanout_blob *scanout =
        (struct virtio_gpu_set_scanout_blob *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x200);
    virtio_gpu_data_t *data;
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x77;

    drain_display_queue();
    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    virtio_gpu_register_scanout(&vgpu, 1024, 768);
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 51);
    data = vgpu.priv;

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = 81;
    create->target = 2;
    create->format = 3;
    create->bind = 4;
    create->width = 320;
    create->height = 240;
    create->depth = 1;
    create->array_size = 1;
    create->nr_samples = 1;
    create_desc[0].addr = 0x40;
    create_desc[0].len = sizeof(*create);
    create_desc[1].addr = 0x200;
    create_desc[1].len = sizeof(*response);
    create_desc[1].flags = VIRTIO_DESC_F_WRITE;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("3d create for blob scanout is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_renderer_request queued = {0};
    require_int("3d create for blob scanout queued",
                vgpu_renderer_pop_request(&queued), true);
    queued.release_payload(queued.payload);

    scanout->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT_BLOB;
    scanout->r.x = 4;
    scanout->r.y = 8;
    scanout->r.width = 160;
    scanout->r.height = 120;
    scanout->scanout_id = 0;
    scanout->resource_id = 81;
    scanout->width = 320;
    scanout->height = 240;
    scanout->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    scanout->strides[0] = 1280;
    scanout->offsets[0] = 16;
    scanout_desc[0].addr = 0x100;
    scanout_desc[0].len = sizeof(*scanout);
    scanout_desc[1].addr = 0x200;
    scanout_desc[1].len = sizeof(*response);
    scanout_desc[1].flags = VIRTIO_DESC_F_WRITE;
    vgpu.ctrl_dispatch.desc_head = 52;
    len = 0;
    response->type = 0;

    g_virtio_gpu_backend.set_scanout_blob(&vgpu, scanout_desc, &len);
    require_u32("blob set scanout is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_u32("blob set scanout does not commit before renderer",
                data->scanouts[0].primary_resource_id, 0);
    struct vgpu_display_cmd display_cmd = {0};
    require_int("blob set scanout publishes nothing before renderer",
                vgpu_display_pop_cmd(&display_cmd), false);

    require_int("blob set scanout queued", vgpu_renderer_pop_request(&queued),
                true);
    require_u32("blob set scanout command type", queued.command_type,
                VIRTIO_GPU_CMD_SET_SCANOUT_BLOB);
    struct vgpu_renderer_ctrl_payload *payload = queued.payload;
    require_u32("blob set scanout snapshots scanout id",
                payload->cmd.set_scanout_blob.scanout_id, 0);
    require_u32("blob set scanout snapshots resource id",
                payload->cmd.set_scanout_blob.resource_id, 81);
    require_u32("blob set scanout snapshots rect x",
                payload->cmd.set_scanout_blob.r.x, 4);
    require_u32("blob set scanout snapshots rect width",
                payload->cmd.set_scanout_blob.r.width, 160);
    require_u32("blob set scanout snapshots width",
                payload->cmd.set_scanout_blob.width, 320);
    require_u32("blob set scanout snapshots height",
                payload->cmd.set_scanout_blob.height, 240);
    require_u32("blob set scanout snapshots format",
                payload->cmd.set_scanout_blob.format,
                VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM);
    require_u32("blob set scanout snapshots stride",
                payload->cmd.set_scanout_blob.strides[0], 1280);
    require_u32("blob set scanout snapshots offset",
                payload->cmd.set_scanout_blob.offsets[0], 16);
    require_false("blob set scanout generation assigned",
                  payload->scanout_generation == 0);

    scanout->resource_id = 999;
    scanout->r.width = 1;
    scanout->strides[0] = 4;
    require_u32("blob set scanout payload is host-owned resource id",
                payload->cmd.set_scanout_blob.resource_id, 81);
    require_u32("blob set scanout payload is host-owned width",
                payload->cmd.set_scanout_blob.r.width, 160);
    require_u32("blob set scanout payload is host-owned stride",
                payload->cmd.set_scanout_blob.strides[0], 1280);

    queued.release_payload(queued.payload);
    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_virgl_set_scanout_blob_enqueue_failure_cancels_generation(void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc scanout_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_set_scanout_blob *scanout =
        (struct virtio_gpu_set_scanout_blob *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x200);
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x7a;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    virtio_gpu_register_scanout(&vgpu, 1024, 768);
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 57);

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = 84;
    create->target = 2;
    create->format = 3;
    create->bind = 4;
    create->width = 320;
    create->height = 240;
    create->depth = 1;
    create->array_size = 1;
    create->nr_samples = 1;
    create_desc[0].addr = 0x40;
    create_desc[0].len = sizeof(*create);
    create_desc[1].addr = 0x200;
    create_desc[1].len = sizeof(*response);
    create_desc[1].flags = VIRTIO_DESC_F_WRITE;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("3d create before blob enqueue failure is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_renderer_request queued = {0};
    require_int("3d create before blob enqueue failure queued",
                vgpu_renderer_pop_request(&queued), true);
    queued.release_payload(queued.payload);

    for (uint32_t i = 0; i < VGPU_RENDERER_QUEUE_CAPACITY; i++) {
        struct vgpu_renderer_request filler = {
            .type = VGPU_RENDERER_REQ_POLL,
            .token = {.generation = renderer_generation},
        };
        require_int("fill renderer queue before failed blob scanout",
                    vgpu_renderer_submit(&filler), true);
    }

    scanout->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT_BLOB;
    scanout->r.width = 160;
    scanout->r.height = 120;
    scanout->scanout_id = 0;
    scanout->resource_id = 84;
    scanout->width = 320;
    scanout->height = 240;
    scanout->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    scanout->strides[0] = 1280;
    scanout_desc[0].addr = 0x100;
    scanout_desc[0].len = sizeof(*scanout);
    scanout_desc[1].addr = 0x200;
    scanout_desc[1].len = sizeof(*response);
    scanout_desc[1].flags = VIRTIO_DESC_F_WRITE;
    vgpu.ctrl_dispatch.desc_head = 58;
    len = 0;
    response->type = 0;
    g_virtio_gpu_backend.set_scanout_blob(&vgpu, scanout_desc, &len);
    require_u32("blob set scanout enqueue failure response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("blob set scanout enqueue failure response", response->type,
                VIRTIO_GPU_RESP_ERR_UNSPEC);

    vgpu_renderer_reset_queues(renderer_generation);
    scanout->resource_id = 999;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.set_scanout_blob(&vgpu, scanout_desc, &len);
    require_u32("blob enqueue failure cancel preserves missing resource len",
                len, sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("blob enqueue failure cancel preserves missing resource",
                response->type, VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    require_int("missing blob after enqueue failure queues no renderer work",
                vgpu_renderer_pop_request(&queued), false);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_virgl_set_scanout_blob_rejects_invalid_inputs(void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc scanout_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_set_scanout_blob *scanout =
        (struct virtio_gpu_set_scanout_blob *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x200);
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x78;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    virtio_gpu_register_scanout(&vgpu, 1024, 768);
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 53);

    scanout->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT_BLOB;
    scanout->r.width = 160;
    scanout->r.height = 120;
    scanout->scanout_id = 0;
    scanout->resource_id = 999;
    scanout->width = 320;
    scanout->height = 240;
    scanout->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    scanout->strides[0] = 1280;
    scanout_desc[0].addr = 0x100;
    scanout_desc[0].len = sizeof(*scanout);
    scanout_desc[1].addr = 0x200;
    scanout_desc[1].len = sizeof(*response);
    scanout_desc[1].flags = VIRTIO_DESC_F_WRITE;

    g_virtio_gpu_backend.set_scanout_blob(&vgpu, scanout_desc, &len);
    require_u32("missing blob scanout response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("missing blob scanout response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    struct vgpu_renderer_request queued = {0};
    require_int("missing blob scanout queues no renderer work",
                vgpu_renderer_pop_request(&queued), false);

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = 82;
    create->target = 2;
    create->format = 3;
    create->bind = 4;
    create->width = 320;
    create->height = 240;
    create->depth = 1;
    create->array_size = 1;
    create->nr_samples = 1;
    create_desc[0].addr = 0x40;
    create_desc[0].len = sizeof(*create);
    create_desc[1].addr = 0x200;
    create_desc[1].len = sizeof(*response);
    create_desc[1].flags = VIRTIO_DESC_F_WRITE;
    len = 0;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("3d create for invalid blob scanout is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d create for invalid blob scanout queued",
                vgpu_renderer_pop_request(&queued), true);
    queued.release_payload(queued.payload);

    scanout->resource_id = 82;
    scanout->scanout_id = VIRTIO_GPU_MAX_SCANOUTS;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.set_scanout_blob(&vgpu, scanout_desc, &len);
    require_u32("invalid blob scanout id response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("invalid blob scanout id response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID);
    require_int("invalid blob scanout id queues no renderer work",
                vgpu_renderer_pop_request(&queued), false);

    scanout->scanout_id = 0;
    scanout->r.x = 300;
    scanout->r.y = 0;
    scanout->r.width = 32;
    scanout->r.height = 120;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.set_scanout_blob(&vgpu, scanout_desc, &len);
    require_u32("invalid blob rect response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("invalid blob rect response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    require_int("invalid blob rect queues no renderer work",
                vgpu_renderer_pop_request(&queued), false);

    scanout->r.x = 0;
    scanout->r.width = 160;
    scanout->width = 0;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.set_scanout_blob(&vgpu, scanout_desc, &len);
    require_u32("invalid blob width response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("invalid blob width response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    require_int("invalid blob width queues no renderer work",
                vgpu_renderer_pop_request(&queued), false);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_virgl_set_scanout_blob_completion_publishes_gl_payload(void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc scanout_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_set_scanout_blob *scanout =
        (struct virtio_gpu_set_scanout_blob *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x200);
    virtio_gpu_data_t *data;
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x79;

    drain_display_queue();
    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    virtio_gpu_register_scanout(&vgpu, 1024, 768);
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 54);
    data = vgpu.priv;

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = 83;
    create->target = 2;
    create->format = 3;
    create->bind = 4;
    create->width = 320;
    create->height = 240;
    create->depth = 1;
    create->array_size = 1;
    create->nr_samples = 1;
    create_desc[0].addr = 0x40;
    create_desc[0].len = sizeof(*create);
    create_desc[1].addr = 0x200;
    create_desc[1].len = sizeof(*response);
    create_desc[1].flags = VIRTIO_DESC_F_WRITE;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("3d create before blob gl payload is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_renderer_request queued = {0};
    require_int("3d create before blob gl payload queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *create_payload = queued.payload;
    uint64_t resource_generation = create_payload->resource_generation;
    queued.release_payload(queued.payload);

    scanout->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT_BLOB;
    scanout->r.x = 4;
    scanout->r.y = 8;
    scanout->r.width = 160;
    scanout->r.height = 120;
    scanout->scanout_id = 0;
    scanout->resource_id = 83;
    scanout->width = 320;
    scanout->height = 240;
    scanout->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    scanout->strides[0] = 1280;
    scanout_desc[0].addr = 0x100;
    scanout_desc[0].len = sizeof(*scanout);
    scanout_desc[1].addr = 0x200;
    scanout_desc[1].len = sizeof(*response);
    scanout_desc[1].flags = VIRTIO_DESC_F_WRITE;
    vgpu.ctrl_dispatch.desc_head = 55;
    len = 0;
    response->type = 0;
    g_virtio_gpu_backend.set_scanout_blob(&vgpu, scanout_desc, &len);
    require_u32("blob set scanout before gl payload commit is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("blob set scanout before gl payload commit queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *scanout_payload = queued.payload;
    uint64_t scanout_generation = scanout_payload->scanout_generation;
    queued.release_payload(queued.payload);

    struct vgpu_display_cmd cmd = {0};
    require_int("blob gl scanout publishes no payload before side effect",
                vgpu_display_pop_cmd(&cmd), false);
    require_u32("blob gl scanout does not write response before side effect",
                response->type, 0);
    require_u32("blob gl scanout does not commit before side effect",
                data->scanouts[0].primary_resource_id, 0);

    struct vgpu_renderer_completion set_scanout = {
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT,
                .scanouts =
                    {
                        {
                            .scanout_id = 0,
                            .scanout_generation = scanout_generation,
                            .resource_generation = resource_generation,
                            .has_gl_payload = true,
                            .gl_payload =
                                {
                                    .texture_id = 0x8300,
                                    .width = 320,
                                    .height = 240,
                                    .src_x = 4,
                                    .src_y = 8,
                                    .src_width = 160,
                                    .src_height = 120,
                                    .y_0_top = false,
                                },
                            .scanout =
                                {
                                    .enabled = 1,
                                    .width = data->scanouts[0].width,
                                    .height = data->scanouts[0].height,
                                    .primary_resource_id = 83,
                                    .src_x = 4,
                                    .src_y = 8,
                                    .src_w = 160,
                                    .src_h = 120,
                                },
                        },
                    },
                .scanout_count = 1,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &set_scanout);

    require_u32("blob gl scanout side effect keeps success response",
                set_scanout.response_type, VIRTIO_GPU_RESP_OK_NODATA);
    require_u32("blob gl scanout still leaves response to completion path",
                response->type, 0);
    require_u32("blob gl scanout commits resource",
                data->scanouts[0].primary_resource_id, 83);
    require_int("blob gl scanout publishes primary display command",
                vgpu_display_pop_cmd(&cmd), true);
    require_u32("blob gl scanout display command type", cmd.type,
                VGPU_DISPLAY_CMD_PRIMARY_SET);
    require_u32("blob gl scanout display payload type",
                cmd.u.primary_set.payload->type, VGPU_DISPLAY_PAYLOAD_GL);
    require_u32("blob gl scanout texture id",
                cmd.u.primary_set.payload->gl.texture_id, 0x8300);
    require_u32("blob gl scanout source x", cmd.u.primary_set.payload->gl.src_x,
                4);
    require_u32("blob gl scanout source y", cmd.u.primary_set.payload->gl.src_y,
                8);
    require_u32("blob gl scanout source width",
                cmd.u.primary_set.payload->gl.src_width, 160);
    require_u32("blob gl scanout source height",
                cmd.u.primary_set.payload->gl.src_height, 120);
    vgpu_display_release_cmd(&cmd);

    scanout->resource_id = 0;
    memset(&scanout->r, 0, sizeof(scanout->r));
    scanout->width = 0;
    scanout->height = 0;
    scanout->strides[0] = 0;
    vgpu.ctrl_dispatch.desc_head = 56;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.set_scanout_blob(&vgpu, scanout_desc, &len);
    require_u32("blob scanout disable response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("blob scanout disable response", response->type,
                VIRTIO_GPU_RESP_OK_NODATA);
    require_u32("blob scanout disable clears committed resource",
                data->scanouts[0].primary_resource_id, 0);
    require_u32("blob scanout disable clears src width",
                data->scanouts[0].src_w, 0);
    require_int("blob scanout disable queues no renderer work",
                vgpu_renderer_pop_request(&queued), false);
    require_int("blob scanout disable publishes primary clear",
                vgpu_display_pop_cmd(&cmd), true);
    require_u32("blob scanout disable display command type", cmd.type,
                VGPU_DISPLAY_CMD_PRIMARY_CLEAR);
    vgpu_display_release_cmd(&cmd);
    require_int("blob gl scanout publishes no extra display command",
                vgpu_display_pop_cmd(&cmd), false);

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

static void test_virgl_transfer_3d_requires_attached_resource_and_defers(void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc attach_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc transfer_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc detach_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc unref_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_transfer_host_3d *transfer =
        (struct virtio_gpu_transfer_host_3d *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_res_attach_backing *attach =
        (struct virtio_gpu_res_attach_backing *) ((uint8_t *) ram + 0x180);
    struct virtio_gpu_mem_entry *entries =
        (struct virtio_gpu_mem_entry *) ((uint8_t *) ram + 0x200);
    struct virtio_gpu_res_detach_backing *detach =
        (struct virtio_gpu_res_detach_backing *) ((uint8_t *) ram + 0x280);
    struct virtio_gpu_res_unref *unref =
        (struct virtio_gpu_res_unref *) ((uint8_t *) ram + 0x2c0);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x300);
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x7c;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 90);

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = 66;
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
    create_desc[1].addr = 0x300;
    create_desc[1].len = sizeof(*response);
    create_desc[1].flags = VIRTIO_DESC_F_WRITE;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("3d create before transfer is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);

    struct vgpu_renderer_request queued = {0};
    require_int("3d create before transfer queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *create_payload = queued.payload;
    uint64_t resource_generation = create_payload->resource_generation;
    queued.release_payload(queued.payload);

    transfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    transfer->hdr.ctx_id = 12;
    transfer->box = (struct virtio_gpu_box) {
        .x = 1,
        .y = 2,
        .z = 3,
        .w = 4,
        .h = 5,
        .d = 6,
    };
    transfer->offset = UINT64_C(0x123456789);
    transfer->resource_id = 66;
    transfer->level = 1;
    transfer->stride = 256;
    transfer->layer_stride = 512;
    transfer_desc[0].addr = 0x100;
    transfer_desc[0].len = sizeof(*transfer);
    transfer_desc[1].addr = 0x300;
    transfer_desc[1].len = sizeof(*response);
    transfer_desc[1].flags = VIRTIO_DESC_F_WRITE;

    g_virtio_gpu_backend.transfer_to_host_3d(&vgpu, transfer_desc, &len);
    require_u32("3d transfer before backing response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("3d transfer before backing response", response->type,
                VIRTIO_GPU_RESP_ERR_UNSPEC);
    struct vgpu_renderer_request empty = {0};
    require_int("3d transfer before backing queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    attach->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach->resource_id = 66;
    attach->nr_entries = 1;
    entries[0].addr = 0x340;
    entries[0].length = 16;
    attach_desc[0].addr = 0x180;
    attach_desc[0].len = sizeof(*attach);
    attach_desc[1].addr = 0x200;
    attach_desc[1].len = sizeof(*entries);
    attach_desc[2].addr = 0x300;
    attach_desc[2].len = sizeof(*response);
    attach_desc[2].flags = VIRTIO_DESC_F_WRITE;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.resource_attach_backing(&vgpu, attach_desc, &len);
    require_u32("3d attach before transfer is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d attach before transfer queued",
                vgpu_renderer_pop_request(&queued), true);

    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.transfer_to_host_3d(&vgpu, transfer_desc, &len);
    require_u32("3d transfer during attach pending response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("3d transfer during attach pending response", response->type,
                VIRTIO_GPU_RESP_ERR_UNSPEC);
    require_int("3d transfer during attach pending queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    struct vgpu_renderer_completion attach_success = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_ATTACH_BACKING,
                .resource_id = 66,
                .resource_generation = resource_generation,
                .backing_transition_success = true,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &attach_success);
    queued.release_payload(queued.payload);

    vgpu.ctrl_dispatch.desc_head = 91;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.transfer_to_host_3d(&vgpu, transfer_desc, &len);
    require_u32("3d transfer to host is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d transfer to host queued",
                vgpu_renderer_pop_request(&queued), true);
    require_u32("3d transfer command type", queued.command_type,
                VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D);
    struct vgpu_renderer_ctrl_payload *transfer_payload = queued.payload;
    void (*transfer_release_payload)(void *) = queued.release_payload;
    require_u32("3d transfer snapshots resource id",
                transfer_payload->cmd.transfer_3d.resource_id, 66);
    require_u32("3d transfer snapshots ctx id",
                transfer_payload->cmd.transfer_3d.hdr.ctx_id, 12);
    require_u32("3d transfer snapshots box x",
                transfer_payload->cmd.transfer_3d.box.x, 1);
    require_u32("3d transfer snapshots box d",
                transfer_payload->cmd.transfer_3d.box.d, 6);
    require_u64("3d transfer snapshots offset",
                transfer_payload->cmd.transfer_3d.offset,
                UINT64_C(0x123456789));
    require_u32("3d transfer snapshots stride",
                transfer_payload->cmd.transfer_3d.stride, 256);
    require_u32("3d transfer snapshots layer stride",
                transfer_payload->cmd.transfer_3d.layer_stride, 512);
    require_u64("3d transfer carries resource generation",
                transfer_payload->resource_generation, resource_generation);
    require_u32("3d transfer desc head",
                transfer_payload->ctrl_completion.desc_head, 91);
    require_u32("3d transfer response capacity",
                transfer_payload->response_capacity,
                sizeof(struct virtio_gpu_ctrl_hdr));

    transfer->resource_id = 999;
    transfer->box.x = 99;
    require_u32("3d transfer payload is host-owned resource id",
                transfer_payload->cmd.transfer_3d.resource_id, 66);
    require_u32("3d transfer payload is host-owned box",
                transfer_payload->cmd.transfer_3d.box.x, 1);
    transfer_release_payload(transfer_payload);

    transfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    transfer->resource_id = 66;
    transfer->box.x = 7;
    transfer->box.d = 8;
    transfer->offset = UINT64_C(0x987654321);
    transfer->stride = 1024;
    transfer->layer_stride = 2048;
    vgpu.ctrl_dispatch.desc_head = 92;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.transfer_from_host_3d(&vgpu, transfer_desc, &len);
    require_u32("3d transfer from host is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d transfer from host queued",
                vgpu_renderer_pop_request(&queued), true);
    require_u32("3d transfer from host command type", queued.command_type,
                VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D);
    transfer_payload = queued.payload;
    transfer_release_payload = queued.release_payload;
    require_u32("3d transfer from host snapshots resource id",
                transfer_payload->cmd.transfer_3d.resource_id, 66);
    require_u32("3d transfer from host snapshots hdr type",
                transfer_payload->cmd.transfer_3d.hdr.type,
                VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D);
    require_u32("3d transfer from host snapshots box x",
                transfer_payload->cmd.transfer_3d.box.x, 7);
    require_u32("3d transfer from host snapshots box d",
                transfer_payload->cmd.transfer_3d.box.d, 8);
    require_u64("3d transfer from host snapshots offset",
                transfer_payload->cmd.transfer_3d.offset,
                UINT64_C(0x987654321));
    require_u32("3d transfer from host snapshots stride",
                transfer_payload->cmd.transfer_3d.stride, 1024);
    require_u32("3d transfer from host snapshots layer stride",
                transfer_payload->cmd.transfer_3d.layer_stride, 2048);
    require_u64("3d transfer from host carries resource generation",
                transfer_payload->resource_generation, resource_generation);
    require_u32("3d transfer from host desc head",
                transfer_payload->ctrl_completion.desc_head, 92);
    transfer->resource_id = 998;
    transfer->box.x = 98;
    require_u32("3d transfer from host payload is host-owned resource id",
                transfer_payload->cmd.transfer_3d.resource_id, 66);
    require_u32("3d transfer from host payload is host-owned box",
                transfer_payload->cmd.transfer_3d.box.x, 7);
    transfer_release_payload(transfer_payload);

    transfer->resource_id = 777;
    transfer->level = 1;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.transfer_from_host_3d(&vgpu, transfer_desc, &len);
    require_u32("missing 3d transfer response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("missing 3d transfer response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    require_int("missing 3d transfer queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    transfer->resource_id = 66;
    transfer->level = UINT32_MAX;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.transfer_from_host_3d(&vgpu, transfer_desc, &len);
    require_u32("invalid-level 3d transfer response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("invalid-level 3d transfer response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    require_int("invalid-level 3d transfer queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    detach->hdr.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    detach->resource_id = 66;
    detach_desc[0].addr = 0x280;
    detach_desc[0].len = sizeof(*detach);
    detach_desc[1].addr = 0x300;
    detach_desc[1].len = sizeof(*response);
    detach_desc[1].flags = VIRTIO_DESC_F_WRITE;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.resource_detach_backing(&vgpu, detach_desc, &len);
    require_u32("3d detach before transfer is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d detach before transfer queued",
                vgpu_renderer_pop_request(&queued), true);
    queued.release_payload(queued.payload);

    transfer->level = 1;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.transfer_from_host_3d(&vgpu, transfer_desc, &len);
    require_u32("3d transfer during detach pending response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("3d transfer during detach pending response", response->type,
                VIRTIO_GPU_RESP_ERR_UNSPEC);
    require_int("3d transfer during detach pending queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    struct vgpu_renderer_completion detach_success = {
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_DETACH_BACKING,
                .resource_id = 66,
                .resource_generation = resource_generation,
                .backing_transition_success = true,
            },
    };
    g_virtio_gpu_backend.apply_renderer_side_effect(&vgpu, &detach_success);
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.transfer_from_host_3d(&vgpu, transfer_desc, &len);
    require_u32("3d transfer after detach response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("3d transfer after detach response", response->type,
                VIRTIO_GPU_RESP_ERR_UNSPEC);
    require_int("3d transfer after detach queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

    unref->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    unref->resource_id = 66;
    unref_desc[0].addr = 0x2c0;
    unref_desc[0].len = sizeof(*unref);
    unref_desc[1].addr = 0x300;
    unref_desc[1].len = sizeof(*response);
    unref_desc[1].flags = VIRTIO_DESC_F_WRITE;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.resource_unref(&vgpu, unref_desc, &len);
    require_u32("3d unref before transfer is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d unref before transfer queued",
                vgpu_renderer_pop_request(&queued), true);
    queued.release_payload(queued.payload);

    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.transfer_from_host_3d(&vgpu, transfer_desc, &len);
    require_u32("3d transfer during unref pending response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("3d transfer during unref pending response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
    require_int("3d transfer during unref pending queues no renderer work",
                vgpu_renderer_pop_request(&empty), false);

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


#if SEMU_HAS(VIRGL)
static void test_virgl_submit_3d_snapshots_command_buffer_and_defers(void)
{
    uint32_t ram[512] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_cmd_submit *submit =
        (struct virtio_gpu_cmd_submit *) ((uint8_t *) ram + 0x40);
    uint32_t *inline_word = (uint32_t *) ((uint8_t *) submit + sizeof(*submit));
    uint32_t *split_words = (uint32_t *) ((uint8_t *) ram + 0x140);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x1c0);
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x7a;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 70);

    submit->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    submit->hdr.ctx_id = 88;
    submit->size = 3 * sizeof(uint32_t);
    *inline_word = 0x11111111;
    split_words[0] = 0x22222222;
    split_words[1] = 0x33333333;
    desc[0].addr = 0x40;
    desc[0].len = sizeof(*submit) + sizeof(uint32_t);
    desc[1].addr = 0x140;
    desc[1].len = 2 * sizeof(uint32_t);
    desc[2].addr = 0x1c0;
    desc[2].len = sizeof(*response);
    desc[2].flags = VIRTIO_DESC_F_WRITE;

    g_virtio_gpu_backend.submit_3d(&vgpu, desc, &len);
    require_u32("submit 3d is deferred", len, VIRTIO_GPU_RESPONSE_DEFERRED);

    *inline_word = 0xaaaaaaaa;
    split_words[0] = 0xbbbbbbbb;
    split_words[1] = 0xcccccccc;

    struct vgpu_renderer_request queued = {0};
    require_int("submit 3d queued", vgpu_renderer_pop_request(&queued), true);
    require_u32("submit 3d command type", queued.command_type,
                VIRTIO_GPU_CMD_SUBMIT_3D);
    struct vgpu_renderer_ctrl_payload *payload = queued.payload;
    require_u32("submit 3d ctx snapshot", payload->cmd.submit_3d.hdr.ctx_id,
                88);
    require_u32("submit 3d size snapshot", payload->cmd.submit_3d.size,
                3 * sizeof(uint32_t));
    require_u64("submit 3d data size", payload->submit_data_size,
                3 * sizeof(uint32_t));
    require_false("submit 3d data is host-owned",
                  payload->submit_data == inline_word);
    uint32_t *snapshot_words = payload->submit_data;
    require_u32("submit 3d word0 snapshot", snapshot_words[0], 0x11111111);
    require_u32("submit 3d word1 snapshot", snapshot_words[1], 0x22222222);
    require_u32("submit 3d word2 snapshot", snapshot_words[2], 0x33333333);
    require_u32("submit 3d response capacity", payload->response_capacity,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("submit 3d desc head", payload->ctrl_completion.desc_head, 70);
    queued.release_payload(queued.payload);

    destroy_vgpu_test_state(&emu, &vgpu);
}


static void test_virgl_submit_3d_fenced_request_snapshots_and_defers(void)
{
    uint32_t ram[512] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_cmd_submit *submit =
        (struct virtio_gpu_cmd_submit *) ((uint8_t *) ram + 0x40);
    uint32_t *inline_word = (uint32_t *) ((uint8_t *) submit + sizeof(*submit));
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x120);
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x7c;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 71);

    submit->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    submit->hdr.flags = VIRTIO_GPU_FLAG_FENCE | VIRTIO_GPU_FLAG_INFO_RING_IDX;
    submit->hdr.fence_id = UINT64_C(0x1020304050607080);
    submit->hdr.ctx_id = 91;
    submit->hdr.ring_idx = 6;
    submit->size = sizeof(uint32_t);
    *inline_word = 0x51525354;
    desc[0].addr = 0x40;
    desc[0].len = sizeof(*submit) + sizeof(*inline_word);
    desc[1].addr = 0x120;
    desc[1].len = sizeof(*response);
    desc[1].flags = VIRTIO_DESC_F_WRITE;

    g_virtio_gpu_backend.submit_3d(&vgpu, desc, &len);
    require_u32("fenced submit 3d is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_u32("fenced submit 3d writes no immediate response", response->type,
                0);

    struct vgpu_renderer_request queued = {0};
    require_int("fenced submit 3d queued", vgpu_renderer_pop_request(&queued),
                true);
    require_u32("fenced submit 3d command type", queued.command_type,
                VIRTIO_GPU_CMD_SUBMIT_3D);
    struct vgpu_renderer_ctrl_payload *payload = queued.payload;
    require_u32("fenced submit 3d flags snapshot",
                payload->cmd.submit_3d.hdr.flags,
                VIRTIO_GPU_FLAG_FENCE | VIRTIO_GPU_FLAG_INFO_RING_IDX);
    require_u64("fenced submit 3d fence id snapshot",
                payload->cmd.submit_3d.hdr.fence_id,
                UINT64_C(0x1020304050607080));
    require_u32("fenced submit 3d ctx snapshot",
                payload->cmd.submit_3d.hdr.ctx_id, 91);
    require_u32("fenced submit 3d ring snapshot",
                payload->cmd.submit_3d.hdr.ring_idx, 6);
    require_u32("fenced submit 3d word snapshot",
                ((uint32_t *) payload->submit_data)[0], 0x51525354);
    require_u32("fenced submit 3d desc head",
                payload->ctrl_completion.desc_head, 71);
    queued.release_payload(queued.payload);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_virgl_submit_3d_rejects_invalid_or_late_payload(void)
{
    uint32_t ram[512] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_cmd_submit *submit =
        (struct virtio_gpu_cmd_submit *) ((uint8_t *) ram + 0x40);
    uint32_t *inline_word = (uint32_t *) ((uint8_t *) submit + sizeof(*submit));
    uint32_t *late_word = (uint32_t *) ((uint8_t *) ram + 0x160);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x120);
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x7b;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 80);

    submit->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    submit->hdr.ctx_id = 89;
    desc[0].addr = 0x40;
    desc[0].len = sizeof(*submit);
    desc[1].addr = 0x120;
    desc[1].len = sizeof(*response);
    desc[1].flags = VIRTIO_DESC_F_WRITE;

    submit->size = 0;
    g_virtio_gpu_backend.submit_3d(&vgpu, desc, &len);
    require_u32("zero submit 3d response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("zero submit 3d response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    struct vgpu_renderer_request queued = {0};
    require_int("zero submit 3d queues no renderer work",
                vgpu_renderer_pop_request(&queued), false);

    submit->size = 3;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.submit_3d(&vgpu, desc, &len);
    require_u32("unaligned submit 3d response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("unaligned submit 3d response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    require_int("unaligned submit 3d queues no renderer work",
                vgpu_renderer_pop_request(&queued), false);

    desc[0].len = sizeof(*submit) + sizeof(*inline_word);
    *inline_word = 0x55555555;
    submit->size = sizeof(uint32_t);
    submit->num_in_fences = 1;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.submit_3d(&vgpu, desc, &len);
    require_u32("in-fence submit 3d response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("in-fence submit 3d response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    require_int("in-fence submit 3d queues no renderer work",
                vgpu_renderer_pop_request(&queued), false);

    submit->num_in_fences = 0;
    desc[0].len = sizeof(*submit);
    submit->size = sizeof(uint32_t);
    *late_word = 0x44444444;
    desc[2].addr = 0x160;
    desc[2].len = sizeof(*late_word);
    desc[2].flags = 0;
    response->type = 0;
    len = 0;
    g_virtio_gpu_backend.submit_3d(&vgpu, desc, &len);
    require_u32("late submit 3d payload response len", len,
                sizeof(struct virtio_gpu_ctrl_hdr));
    require_u32("late submit 3d payload response", response->type,
                VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    require_int("late submit 3d payload queues no renderer work",
                vgpu_renderer_pop_request(&queued), false);

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

#if SEMU_HAS(VIRGL)
static void test_vgpu_virgl_demo_gate_exposes_classic_3d_features(void)
{
    uint32_t ram[64] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    uint32_t num_capsets;

    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));

    require_u64("VirGL feature visible",
                vgpu.common.device_features & VIRTIO_GPU_F_VIRGL,
                VIRTIO_GPU_F_VIRGL);
    require_u64("context-init feature visible",
                vgpu.common.device_features & VIRTIO_GPU_F_CONTEXT_INIT,
                VIRTIO_GPU_F_CONTEXT_INIT);
    require_u64("resource-blob feature hidden",
                vgpu.common.device_features & VIRTIO_GPU_F_RESOURCE_BLOB, 0);
    require_u32("host-visible SHM configured", vgpu.common.has_shm_region, 1);
    require_u32("host-visible SHM id", vgpu.common.shm_region.id,
                VIRTIO_GPU_SHM_ID_HOST_VISIBLE);
    require_u64("host-visible SHM base", vgpu.common.shm_region.base,
                SEMU_PLATFORM_MMIO_VGPU_HOSTMEM_BASE);
    require_u64("host-visible SHM length", vgpu.common.shm_region.length,
                SEMU_PLATFORM_VGPU_HOSTMEM_SIZE);
    virtio_gpu_set_num_capsets(&vgpu, 5);
    num_capsets = vgpu.common.ops->read_config(
        vgpu.common.opaque, offsetof(struct virtio_gpu_config, num_capsets),
        sizeof(num_capsets));
    require_u32("classic virgl capsets visible", num_capsets, 5);

    destroy_vgpu_test_state(&emu, &vgpu);
}
#endif

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

static void test_renderer_gl_scanout_response_fault_does_not_publish(void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc scanout_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_set_scanout *scanout =
        (struct virtio_gpu_set_scanout *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x180);
    virtio_gpu_data_t *data;
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x58;

    drain_display_queue();
    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    virtio_gpu_register_scanout(&vgpu, 1024, 768);
    configure_test_queue(&emu, &vgpu, VIRTIO_GPU_CONTROLQ);
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 60);
    atomic_store_explicit(&vgpu.common.status, VIRTIO_STATUS__DRIVER_OK,
                          memory_order_release);
    data = vgpu.priv;

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = 70;
    create->target = 2;
    create->format = 3;
    create->bind = 4;
    create->width = 320;
    create->height = 240;
    create->depth = 1;
    create->array_size = 1;
    create->nr_samples = 1;
    create_desc[0].addr = 0x40;
    create_desc[0].len = sizeof(*create);
    create_desc[1].addr = 0x180;
    create_desc[1].len = sizeof(*response);
    create_desc[1].flags = VIRTIO_DESC_F_WRITE;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("3d create before bad response gl scanout is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_renderer_request queued = {0};
    require_int("3d create before bad response gl scanout queued",
                vgpu_renderer_pop_request(&queued), true);
    queued.release_payload(queued.payload);

    scanout->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout->r.width = 160;
    scanout->r.height = 120;
    scanout->scanout_id = 0;
    scanout->resource_id = 70;
    scanout_desc[0].addr = 0x100;
    scanout_desc[0].len = sizeof(*scanout);
    scanout_desc[1].addr = 0x180;
    scanout_desc[1].len = sizeof(*response);
    scanout_desc[1].flags = VIRTIO_DESC_F_WRITE;
    vgpu.ctrl_dispatch.desc_head = 61;
    len = 0;
    g_virtio_gpu_backend.set_scanout(&vgpu, scanout_desc, &len);
    require_u32("3d set scanout with bad response is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d set scanout with bad response queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *payload = queued.payload;
    struct virtio_gpu_deferred_ctrl_completion ctrl = payload->ctrl_completion;
    struct virtq_desc response_desc = payload->response_desc;
    response_desc.len = sizeof(struct virtio_gpu_ctrl_hdr) - 1;
    struct virtio_gpu_ctrl_hdr request_hdr = payload->hdr;
    uint64_t resource_generation = payload->resource_generation;
    uint64_t scanout_generation = payload->scanout_generation;
    queued.release_payload(queued.payload);

    struct vgpu_renderer_completion completion = {
        .type = VGPU_RENDERER_DONE_CTRL,
        .token = {.generation = renderer_generation},
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
        .has_ctrl_completion = true,
        .ctrl_completion = ctrl,
        .has_response_desc = true,
        .request_hdr = request_hdr,
        .response_desc = response_desc,
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT,
                .scanouts =
                    {
                        {
                            .scanout_id = 0,
                            .scanout_generation = scanout_generation,
                            .resource_generation = resource_generation,
                            .has_gl_payload = true,
                            .gl_payload =
                                {
                                    .texture_id = 0x7000,
                                    .width = 320,
                                    .height = 240,
                                    .src_width = 160,
                                    .src_height = 120,
                                },
                            .scanout =
                                {
                                    .enabled = 1,
                                    .width = data->scanouts[0].width,
                                    .height = data->scanouts[0].height,
                                    .primary_resource_id = 70,
                                    .src_w = 160,
                                    .src_h = 120,
                                },
                        },
                    },
                .scanout_count = 1,
            },
    };

    require_int("queue renderer gl scanout completion with bad response",
                vgpu_renderer_complete(&completion), true);
    virtio_gpu_drain_renderer_completions(&vgpu);

    require_u32(
        "bad response gl scanout sets reset-needed",
        atomic_load(&vgpu.common.status) & VIRTIO_STATUS__DEVICE_NEEDS_RESET,
        VIRTIO_STATUS__DEVICE_NEEDS_RESET);
    require_u32("bad response gl scanout does not commit frontend scanout",
                data->scanouts[0].primary_resource_id, 0);
    struct vgpu_display_cmd cmd = {0};
    require_int("bad response gl scanout publishes no display payload",
                vgpu_display_pop_cmd(&cmd), false);
    require_u16("bad response gl scanout used idx unchanged",
                load_u16(ram, 0x302), 0);

    destroy_vgpu_test_state(&emu, &vgpu);
}

static void test_renderer_gl_scanout_add_used_fault_does_not_publish(void)
{
    uint32_t ram[1024] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;
    struct virtq_desc create_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtq_desc scanout_desc[VIRTIO_GPU_MAX_DESC] = {0};
    struct virtio_gpu_resource_create_3d *create =
        (struct virtio_gpu_resource_create_3d *) ((uint8_t *) ram + 0x40);
    struct virtio_gpu_set_scanout *scanout =
        (struct virtio_gpu_set_scanout *) ((uint8_t *) ram + 0x100);
    struct virtio_gpu_ctrl_hdr *response =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x180);
    virtio_gpu_data_t *data;
    uint32_t len = 0;
    const uint64_t renderer_generation = 0x59;

    drain_display_queue();
    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    virtio_gpu_register_scanout(&vgpu, 1024, 768);
    configure_test_queue(&emu, &vgpu, VIRTIO_GPU_CONTROLQ);
    activate_test_renderer_dispatch(&vgpu, renderer_generation, 62);
    atomic_store_explicit(&vgpu.common.status, VIRTIO_STATUS__DRIVER_OK,
                          memory_order_release);
    data = vgpu.priv;

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create->resource_id = 73;
    create->target = 2;
    create->format = 3;
    create->bind = 4;
    create->width = 320;
    create->height = 240;
    create->depth = 1;
    create->array_size = 1;
    create->nr_samples = 1;
    create_desc[0].addr = 0x40;
    create_desc[0].len = sizeof(*create);
    create_desc[1].addr = 0x180;
    create_desc[1].len = sizeof(*response);
    create_desc[1].flags = VIRTIO_DESC_F_WRITE;
    g_virtio_gpu_backend.resource_create_3d(&vgpu, create_desc, &len);
    require_u32("3d create before add-used fault scanout is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    struct vgpu_renderer_request queued = {0};
    require_int("3d create before add-used fault scanout queued",
                vgpu_renderer_pop_request(&queued), true);
    queued.release_payload(queued.payload);

    scanout->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout->r.width = 160;
    scanout->r.height = 120;
    scanout->scanout_id = 0;
    scanout->resource_id = 73;
    scanout_desc[0].addr = 0x100;
    scanout_desc[0].len = sizeof(*scanout);
    scanout_desc[1].addr = 0x180;
    scanout_desc[1].len = sizeof(*response);
    scanout_desc[1].flags = VIRTIO_DESC_F_WRITE;
    vgpu.ctrl_dispatch.desc_head = 62;
    len = 0;
    g_virtio_gpu_backend.set_scanout(&vgpu, scanout_desc, &len);
    require_u32("3d set scanout with bad used id is deferred", len,
                VIRTIO_GPU_RESPONSE_DEFERRED);
    require_int("3d set scanout with bad used id queued",
                vgpu_renderer_pop_request(&queued), true);
    struct vgpu_renderer_ctrl_payload *payload = queued.payload;
    struct virtio_gpu_deferred_ctrl_completion ctrl = payload->ctrl_completion;
    struct virtq_desc response_desc = payload->response_desc;
    struct virtio_gpu_ctrl_hdr request_hdr = payload->hdr;
    uint64_t resource_generation = payload->resource_generation;
    uint64_t scanout_generation = payload->scanout_generation;
    queued.release_payload(queued.payload);

    response->type = 0;
    struct vgpu_renderer_completion completion = {
        .type = VGPU_RENDERER_DONE_CTRL,
        .token = {.generation = renderer_generation},
        .response_type = VIRTIO_GPU_RESP_OK_NODATA,
        .has_ctrl_completion = true,
        .ctrl_completion = ctrl,
        .has_response_desc = true,
        .request_hdr = request_hdr,
        .response_desc = response_desc,
        .virgl_resource =
            {
                .type = VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT,
                .scanouts =
                    {
                        {
                            .scanout_id = 0,
                            .scanout_generation = scanout_generation,
                            .resource_generation = resource_generation,
                            .has_gl_payload = true,
                            .gl_payload = test_gl_payload(0x7300),
                            .scanout =
                                {
                                    .enabled = 1,
                                    .width = data->scanouts[0].width,
                                    .height = data->scanouts[0].height,
                                    .primary_resource_id = 73,
                                    .src_w = 160,
                                    .src_h = 120,
                                },
                        },
                    },
                .scanout_count = 1,
            },
    };

    require_int("queue renderer gl scanout completion with bad used id",
                vgpu_renderer_complete(&completion), true);
    virtio_gpu_drain_renderer_completions(&vgpu);

    require_u32("bad used id gl scanout writes response", response->type,
                VIRTIO_GPU_RESP_OK_NODATA);
    require_u32(
        "bad used id gl scanout sets reset-needed",
        atomic_load(&vgpu.common.status) & VIRTIO_STATUS__DEVICE_NEEDS_RESET,
        VIRTIO_STATUS__DEVICE_NEEDS_RESET);
    require_u32("bad used id gl scanout does not commit frontend scanout",
                data->scanouts[0].primary_resource_id, 0);
    struct vgpu_display_cmd cmd = {0};
    require_int("bad used id gl scanout publishes no display payload",
                vgpu_display_pop_cmd(&cmd), false);
    require_u16("bad used id gl scanout used idx unchanged",
                load_u16(ram, 0x302), 0);

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

#if SEMU_HAS(VIRGL)
static void test_vgpu_destroy_shutdowns_renderer_queue(void)
{
    uint32_t ram[64] = {0};
    emu_state_t emu = {0};
    virtio_gpu_state_t vgpu;
    uint64_t generation;
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .payload_size = 1,
        .release_payload = renderer_release_payload,
    };
    struct vgpu_renderer_completion completion = {
        .type = VGPU_RENDERER_DONE_CTRL,
        .response_size = sizeof(struct virtio_gpu_ctrl_hdr),
        .release_response = renderer_release_response,
    };

    emu.ram = ram;
    ram_dma_init(&emu.ram_dma, ram, sizeof(ram), NULL);
    require_int("plic lock init", pthread_mutex_init(&emu.plic_lock, NULL), 0);

    virtio_gpu_init(&vgpu, &emu);
    generation = vgpu.common.generation;

    renderer_release_payload_count = 0;
    renderer_release_response_count = 0;
    request.token.generation = generation;
    request.payload = malloc(1);
    completion.token.generation = generation;
    completion.response = calloc(1, sizeof(struct virtio_gpu_ctrl_hdr));
    require_false("request payload allocated", request.payload == NULL);
    require_false("completion response allocated", completion.response == NULL);

    require_int("queue pending renderer request",
                vgpu_renderer_submit(&request), true);
    require_int("queue pending renderer completion",
                vgpu_renderer_complete(&completion), true);

    virtio_gpu_destroy(&vgpu);

    require_u32("destroy released pending renderer request",
                renderer_release_payload_count, 1);
    require_u32("destroy released pending renderer completion",
                renderer_release_response_count, 1);

    struct vgpu_renderer_request late_request = {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = generation},
    };
    require_int("destroyed renderer rejects request",
                vgpu_renderer_submit(&late_request), false);
    require_int("destroyed renderer has no request to pop",
                vgpu_renderer_pop_request(&late_request), false);

    struct vgpu_renderer_completion late_completion = {
        .type = VGPU_RENDERER_DONE_CTRL,
        .token = {.generation = generation},
        .response = calloc(1, sizeof(struct virtio_gpu_ctrl_hdr)),
        .response_size = sizeof(struct virtio_gpu_ctrl_hdr),
        .release_response = renderer_release_response,
    };
    require_false("late completion response allocated",
                  late_completion.response == NULL);
    require_int("destroyed renderer rejects completion",
                vgpu_renderer_complete(&late_completion), false);
    require_int("destroyed renderer has no completion to pop",
                vgpu_renderer_pop_completion(&late_completion), false);
    require_u32("destroy released rejected completion response",
                renderer_release_response_count, 2);

    struct vgpu_renderer_debug_stats stats;
    vgpu_renderer_debug_snapshot(&stats);
    require_false("destroyed renderer unavailable", stats.available);
    require_u32("destroyed renderer request depth", stats.request_depth, 0);
    require_u32("destroyed renderer completion depth", stats.completion_depth,
                0);

    virtio_gpu_destroy(&vgpu);
    require_u32("idempotent destroy releases no extra request",
                renderer_release_payload_count, 1);
    require_u32("idempotent destroy releases no extra completion",
                renderer_release_response_count, 2);

    pthread_mutex_destroy(&emu.plic_lock);
}
#endif

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
#if SEMU_HAS(VIRGL)
    test_vgpu_virgl_demo_gate_exposes_classic_3d_features();
#endif
    test_deferred_ctrl_completion_revalidates_generations();
#if SEMU_HAS(VIRGL)
    test_hidden_blob_command_returns_undefined_without_renderer_work();
    test_virgl_capset_info_handler_submits_host_owned_ctrl_payload();
    test_virgl_resource_create_3d_handler_tracks_pending_resource();
    test_virgl_resource_create_blob_handler_tracks_pending_resource();
    test_virgl_resource_map_unmap_blob_frontend_policy();
    test_virgl_resource_create_3d_rejects_live_2d_resource_id();
    test_virgl_set_scanout_3d_defers_and_commits_by_generation();
    test_virgl_set_scanout_3d_enqueue_failure_cancels_generation();
    test_virgl_set_scanout_3d_stale_after_2d_bind_is_ignored();
    test_virgl_unref_clears_committed_3d_scanout();
    test_virgl_unref_completion_preserves_reused_2d_scanout();
    test_virgl_failed_pending_scanout_preserves_committed_3d_owner();
    test_virgl_set_scanout_3d_side_effect_publishes_gl_payload();
    test_virgl_set_scanout_3d_publish_failure_does_not_commit();
    test_virgl_set_scanout_3d_rejects_invalid_inputs();
    test_virgl_set_scanout_blob_snapshots_and_defers();
    test_virgl_set_scanout_blob_enqueue_failure_cancels_generation();
    test_virgl_set_scanout_blob_rejects_invalid_inputs();
    test_virgl_set_scanout_blob_completion_publishes_gl_payload();
    test_virgl_resource_unref_handler_defers_and_frees_namespace();
    test_virgl_resource_unref_sync_submit_failure_restores_namespace();
    test_virgl_resource_unref_stale_rollback_after_2d_reuse();
    test_virgl_resource_attach_backing_snapshots_iov_and_defers();
    test_virgl_transfer_3d_requires_attached_resource_and_defers();
    test_virgl_resource_attach_rejects_malformed_backing_list();
    test_virgl_context_handlers_submit_ctrl_skeletons();
    test_virgl_submit_3d_snapshots_command_buffer_and_defers();
    test_virgl_submit_3d_fenced_request_snapshots_and_defers();
    test_virgl_submit_3d_rejects_invalid_or_late_payload();
    test_renderer_completion_drain_writes_response_and_used_ring();
    test_renderer_gl_scanout_response_fault_does_not_publish();
    test_renderer_gl_scanout_add_used_fault_does_not_publish();
    test_renderer_completion_drops_stale_common_generation();
    test_renderer_ctrl_completion_without_metadata_fails();
#endif
    test_vgpu_destroy_releases_common_without_actor();
    test_vgpu_destroy_stops_started_actor_and_is_idempotent();
#if SEMU_HAS(VIRGL)
    test_vgpu_destroy_shutdowns_renderer_queue();
#endif
    return 0;
}
