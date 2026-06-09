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
#include "ram_access.h"
#include "virtio-gpu.h"
#include "virtio-irq.h"
#include "virtio.h"

void semu_wake_interruptible_harts(emu_state_t *emu UNUSED) {}

void virtio_gpu_sw_backend_init(virtio_gpu_state_t *vgpu UNUSED) {}

static _Atomic int deferred_handler_calls;

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

static void store_u16(uint32_t *ram, uint32_t addr, uint16_t value)
{
    memcpy((uint8_t *) ram + addr, &value, sizeof(value));
}

static uint16_t load_u16(const uint32_t *ram, uint32_t addr)
{
    uint16_t value;

    memcpy(&value, (const uint8_t *) ram + addr, sizeof(value));
    return value;
}

static void wait_for_deferred_handler(void)
{
    const struct timespec delay = {
        .tv_sec = 0,
        .tv_nsec = 1000000,
    };

    for (int i = 0; i < 1000; i++) {
        if (atomic_load_explicit(&deferred_handler_calls,
                                 memory_order_acquire) == 1)
            return;
        nanosleep(&delay, NULL);
    }

    fprintf(stderr, "deferred handler was not called\n");
    exit(1);
}

static void deferred_get_display_info_handler(virtio_gpu_state_t *vgpu UNUSED,
                                              struct virtq_desc *vq_desc UNUSED,
                                              uint32_t *plen)
{
    atomic_fetch_add_explicit(&deferred_handler_calls, 1, memory_order_release);
    *plen = VIRTIO_GPU_RESPONSE_DEFERRED;
}

const struct virtio_gpu_cmd_backend g_virtio_gpu_backend = {
    .get_display_info = deferred_get_display_info_handler,
    .resource_create_2d = VIRTIO_GPU_CMD_UNDEF,
    .resource_unref = VIRTIO_GPU_CMD_UNDEF,
    .set_scanout = VIRTIO_GPU_CMD_UNDEF,
    .resource_flush = VIRTIO_GPU_CMD_UNDEF,
    .transfer_to_host_2d = VIRTIO_GPU_CMD_UNDEF,
    .resource_attach_backing = VIRTIO_GPU_CMD_UNDEF,
    .resource_detach_backing = VIRTIO_GPU_CMD_UNDEF,
    .get_capset_info = VIRTIO_GPU_CMD_UNDEF,
    .get_capset = VIRTIO_GPU_CMD_UNDEF,
    .get_edid = VIRTIO_GPU_CMD_UNDEF,
    .resource_assign_uuid = VIRTIO_GPU_CMD_UNDEF,
    .resource_create_blob = VIRTIO_GPU_CMD_UNDEF,
    .set_scanout_blob = VIRTIO_GPU_CMD_UNDEF,
    .ctx_create = VIRTIO_GPU_CMD_UNDEF,
    .ctx_destroy = VIRTIO_GPU_CMD_UNDEF,
    .ctx_attach_resource = VIRTIO_GPU_CMD_UNDEF,
    .ctx_detach_resource = VIRTIO_GPU_CMD_UNDEF,
    .resource_create_3d = VIRTIO_GPU_CMD_UNDEF,
    .transfer_to_host_3d = VIRTIO_GPU_CMD_UNDEF,
    .transfer_from_host_3d = VIRTIO_GPU_CMD_UNDEF,
    .submit_3d = VIRTIO_GPU_CMD_UNDEF,
    .resource_map_blob = VIRTIO_GPU_CMD_UNDEF,
    .resource_unmap_blob = VIRTIO_GPU_CMD_UNDEF,
    .update_cursor = VIRTIO_GPU_CMD_UNDEF,
    .move_cursor = VIRTIO_GPU_CMD_UNDEF,
};

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

static void configure_test_queue(emu_state_t *emu, virtio_gpu_state_t *vgpu)
{
    const guest_paddr_t desc_addr = 0x100;
    const guest_paddr_t driver_addr = 0x200;
    const guest_paddr_t device_addr = 0x300;

    require_int("configure queue",
                virtq_configure(&vgpu->common.queues[VIRTIO_GPU_CONTROLQ],
                                &emu->ram_dma, 8, desc_addr, driver_addr,
                                device_addr, 0),
                0);
}

static void write_get_display_info_chain(uint32_t *ram)
{
    struct virtio_gpu_ctrl_hdr *request =
        (struct virtio_gpu_ctrl_hdr *) ((uint8_t *) ram + 0x40);
    struct virtq_desc desc0 = {
        .addr = 0x40,
        .len = sizeof(*request),
        .flags = VIRTIO_DESC_F_NEXT,
        .next = 1,
    };
    struct virtq_desc desc1 = {
        .addr = 0x80,
        .len = sizeof(struct virtio_gpu_resp_disp_info),
        .flags = VIRTIO_DESC_F_WRITE,
    };

    request->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    memcpy((uint8_t *) ram + 0x100, &desc0, sizeof(desc0));
    memcpy((uint8_t *) ram + 0x110, &desc1, sizeof(desc1));
    store_u16(ram, 0x200, 0);
    store_u16(ram, 0x202, 1);
    store_u16(ram, 0x204, 0);
    store_u16(ram, 0x302, 0);
}

static void test_deferred_handler_does_not_publish_used_ring(void)
{
    uint32_t ram[256] = {0};
    emu_state_t emu;
    virtio_gpu_state_t vgpu;

    atomic_store_explicit(&deferred_handler_calls, 0, memory_order_release);
    init_vgpu_test_state(&emu, &vgpu, ram, sizeof(ram));
    configure_test_queue(&emu, &vgpu);
    write_get_display_info_chain(ram);

    require_int("configure actor", virtio_actor_enter_configuring(&vgpu.actor),
                0);
    require_int("activate actor", virtio_actor_activate(&vgpu.actor), 0);
    require_int("start actor", virtio_actor_start(&vgpu.actor), 0);
    atomic_store_explicit(&vgpu.common.status, VIRTIO_STATUS__DRIVER_OK,
                          memory_order_release);

    require_int(
        "notify controlq",
        vgpu.common.ops->notify_queue(vgpu.common.opaque, VIRTIO_GPU_CONTROLQ,
                                      vgpu.common.generation),
        0);
    wait_for_deferred_handler();

    require_u16("used idx remains empty", load_u16(ram, 0x302), 0);
    require_u32(
        "used-ring irq remains clear",
        virtio_irq_read_status(&vgpu.common.irq) & VIRTIO_INT__USED_RING, 0);
    require_bool("queue descriptor consumed",
                 vgpu.common.queues[VIRTIO_GPU_CONTROLQ].last_avail == 1, true);

    destroy_vgpu_test_state(&emu, &vgpu);
}

int main(void)
{
    test_deferred_handler_does_not_publish_used_ring();
    return 0;
}
