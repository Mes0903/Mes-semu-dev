#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common.h"
#include "device.h"
#include "ram_access.h"
#include "riscv.h"
#include "riscv_private.h"
#include "virtio-irq.h"
#include "virtio-mmio.h"
#include "virtio.h"
#include "virtq.h"

#define DISK_BLK_SIZE 512

#define VBLK_DEV_CNT_MAX 1
#define VIRTIO_BLK_F_VERSION_1 (UINT64_C(1) << 32)
#define VBLK_QUEUE_NUM_MAX 1024
#define VBLK_QUEUE 0

#define PRIV(x) ((struct virtio_blk_config *) (x)->priv)

PACKED(struct virtio_blk_config {
    uint64_t capacity;
    uint32_t size_max;
    uint32_t seg_max;

    struct virtio_blk_geometry {
        uint16_t cylinders;
        uint8_t heads;
        uint8_t sectors;
    } geometry;

    uint32_t blk_size;

    struct virtio_blk_topology {
        uint8_t physical_block_exp;
        uint8_t alignment_offset;
        uint16_t min_io_size;
        uint32_t opt_io_size;
    } topology;

    uint8_t writeback;
    uint8_t unused0[3];
    uint32_t max_discard_sectors;
    uint32_t max_discard_seg;
    uint32_t discard_sector_alignment;
    uint32_t max_write_zeroes_sectors;
    uint32_t max_write_zeroes_seg;
    uint8_t write_zeroes_may_unmap;
    uint8_t unused1[3];
});

PACKED(struct vblk_req_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
});

static struct virtio_blk_config vblk_configs[VBLK_DEV_CNT_MAX];
static int vblk_dev_cnt = 0;

/* Track each MAP_SHARED disk mapping so we can msync(MS_SYNC) on graceful
 * exit. Without this, dirty pages live in the host page cache and rely on
 * the kernel's writeback to land on disk. The guest cannot trigger a sync
 * via VIRTIO_BLK_T_FLUSH today because we do not advertise
 * VIRTIO_BLK_F_FLUSH; this hook is the best-effort substitute for that.
 */
static struct {
    void *addr;
    size_t size;
} vblk_disks[VBLK_DEV_CNT_MAX];
static int vblk_disks_cnt = 0;

static void virtio_blk_sync_all(void)
{
    for (int i = 0; i < vblk_disks_cnt; i++) {
        if (!vblk_disks[i].addr)
            continue;
        if (msync(vblk_disks[i].addr, vblk_disks[i].size, MS_SYNC) < 0)
            perror("virtio-blk: msync");
    }
}

static inline unsigned virtio_blk_status_load(virtio_blk_state_t *vblk)
{
    return atomic_load_explicit(&vblk->common.status, memory_order_acquire);
}

static void virtio_blk_set_fail(virtio_blk_state_t *vblk)
{
    unsigned status = virtio_blk_status_load(vblk);

    virtio_device_common_set_needs_reset(&vblk->common);
    if (status & VIRTIO_STATUS__DRIVER_OK)
        virtio_irq_trigger(&vblk->common.irq, VIRTIO_INT__CONF_CHANGE);
}

static bool virtio_blk_config_range_valid(uint32_t offset, uint32_t size)
{
    return size != 0 && offset < sizeof(struct virtio_blk_config) &&
           size <= sizeof(struct virtio_blk_config) - offset;
}

static uint32_t virtio_blk_read_config(void *opaque,
                                       uint32_t offset,
                                       uint32_t size)
{
    virtio_blk_state_t *vblk = opaque;
    uint32_t value = 0;

    if (!vblk || !virtio_blk_config_range_valid(offset, size))
        return 0;

    memcpy(&value, (uint8_t *) PRIV(vblk) + offset, size);
    return value;
}

static void virtio_blk_write_config(void *opaque,
                                    uint32_t offset,
                                    uint32_t size,
                                    uint32_t value)
{
    virtio_blk_state_t *vblk = opaque;

    if (!vblk || !virtio_blk_config_range_valid(offset, size))
        return;

    memcpy((uint8_t *) PRIV(vblk) + offset, &value, size);
}

static bool virtio_blk_queue_available(virtio_blk_state_t *vblk,
                                       const struct virtq *queue,
                                       uint16_t *available)
{
    uint16_t avail_idx;
    uint16_t delta;

    if (!queue || !queue->ready || !available)
        return false;

    if (!ram_dma_read(vblk->common.dma, queue->driver_addr + 2, &avail_idx,
                      sizeof(avail_idx))) {
        virtio_blk_set_fail(vblk);
        return false;
    }

    delta = (uint16_t) (avail_idx - queue->last_avail);
    if (delta > queue->queue_size) {
        virtio_blk_set_fail(vblk);
        return false;
    }

    *available = delta;
    return true;
}

static guest_size_t virtio_blk_iov_bytes(const struct virtq_iov *iov,
                                         size_t count)
{
    guest_size_t total = 0;

    for (size_t i = 0; i < count; i++)
        total += iov[i].len;
    return total;
}

static bool virtio_blk_write_status(virtio_blk_state_t *vblk,
                                    const struct virtq_iov *status_iov,
                                    uint8_t status)
{
    if (!status_iov || status_iov->len < 1)
        return false;
    return ram_dma_write(vblk->common.dma, status_iov->addr, &status,
                         sizeof(status));
}

static bool virtio_blk_disk_range_valid(virtio_blk_state_t *vblk,
                                        uint64_t sector,
                                        guest_size_t len,
                                        uint64_t *offset)
{
    uint64_t capacity = PRIV(vblk)->capacity;
    uint64_t disk_size;

    if (sector > UINT64_MAX / DISK_BLK_SIZE)
        return false;
    *offset = sector * DISK_BLK_SIZE;
    if (capacity > UINT64_MAX / DISK_BLK_SIZE)
        return false;
    disk_size = capacity * DISK_BLK_SIZE;
    if (sector >= capacity)
        return false;
    return len <= disk_size - *offset;
}

static bool virtio_blk_read_disk_to_iovs(virtio_blk_state_t *vblk,
                                         const struct virtq_iov *iov,
                                         size_t count,
                                         uint64_t disk_offset)
{
    const uint8_t *disk = (const uint8_t *) vblk->disk + disk_offset;

    for (size_t i = 0; i < count; i++) {
        if (!ram_dma_write(vblk->common.dma, iov[i].addr, disk, iov[i].len))
            return false;
        disk += iov[i].len;
    }
    return true;
}

static bool virtio_blk_write_iovs_to_disk(virtio_blk_state_t *vblk,
                                          const struct virtq_iov *iov,
                                          size_t count,
                                          uint64_t disk_offset)
{
    uint8_t *disk = (uint8_t *) vblk->disk + disk_offset;

    for (size_t i = 0; i < count; i++) {
        if (!ram_dma_read(vblk->common.dma, iov[i].addr, disk, iov[i].len))
            return false;
        disk += iov[i].len;
    }
    return true;
}

static int virtio_blk_process_chain(virtio_blk_state_t *vblk,
                                    const struct virtq_chain *chain,
                                    uint32_t *used_len)
{
    struct vblk_req_header header;
    const struct virtq_iov *data_iov = NULL;
    const struct virtq_iov *status_iov = NULL;
    size_t data_count = 0;
    guest_size_t data_len;
    uint64_t disk_offset = 0;
    uint8_t status = VIRTIO_BLK_S_OK;

    if (!vblk || !chain || !used_len)
        return -EINVAL;
    *used_len = 0;

    if (chain->readable_count == 0 || chain->readable[0].len < sizeof(header))
        return -EINVAL;
    if (!ram_dma_read(vblk->common.dma, chain->readable[0].addr, &header,
                      sizeof(header)))
        return -EFAULT;

    switch (header.type) {
    case VIRTIO_BLK_T_IN:
        if (chain->writable_count < 2)
            return -EINVAL;
        data_iov = chain->writable;
        data_count = chain->writable_count - 1;
        status_iov = &chain->writable[chain->writable_count - 1];
        break;
    case VIRTIO_BLK_T_OUT:
        if (chain->readable_count < 2 || chain->writable_count != 1)
            return -EINVAL;
        data_iov = &chain->readable[1];
        data_count = chain->readable_count - 1;
        status_iov = &chain->writable[0];
        break;
    default:
        if (chain->writable_count < 1)
            return -EINVAL;
        status_iov = &chain->writable[chain->writable_count - 1];
        status = VIRTIO_BLK_S_UNSUPP;
        if (!virtio_blk_write_status(vblk, status_iov, status))
            return -EFAULT;
        return 0;
    }

    data_len = virtio_blk_iov_bytes(data_iov, data_count);
    if (!virtio_blk_disk_range_valid(vblk, header.sector, data_len,
                                     &disk_offset) ||
        (data_len != 0 && !vblk->disk)) {
        status = VIRTIO_BLK_S_IOERR;
        if (!virtio_blk_write_status(vblk, status_iov, status))
            return -EFAULT;
        return 0;
    }

    if (header.type == VIRTIO_BLK_T_IN) {
        if (!virtio_blk_read_disk_to_iovs(vblk, data_iov, data_count,
                                          disk_offset))
            return -EFAULT;
    } else {
        if (!virtio_blk_write_iovs_to_disk(vblk, data_iov, data_count,
                                           disk_offset))
            return -EFAULT;
    }

    if (!virtio_blk_write_status(vblk, status_iov, VIRTIO_BLK_S_OK))
        return -EFAULT;
    *used_len = (uint32_t) data_len;
    return 0;
}

static void virtio_blk_drain_queue(virtio_blk_state_t *vblk,
                                   struct virtq *queue)
{
    struct virtq_iov readable[VBLK_QUEUE_NUM_MAX];
    struct virtq_iov writable[VBLK_QUEUE_NUM_MAX];
    bool consumed = false;

    for (;;) {
        struct virtq_chain chain = {
            .readable = readable,
            .readable_capacity = ARRAY_SIZE(readable),
            .writable = writable,
            .writable_capacity = ARRAY_SIZE(writable),
        };
        uint16_t available = 0;
        uint32_t used_len = 0;
        int ret;

        if (!virtio_blk_queue_available(vblk, queue, &available))
            return;
        if (available == 0)
            break;

        ret = virtq_pop(vblk->common.dma, queue, &chain);
        if (ret < 0) {
            virtio_blk_set_fail(vblk);
            return;
        }
        if (ret == 0)
            break;

        ret = virtio_blk_process_chain(vblk, &chain, &used_len);
        if (ret < 0) {
            virtio_blk_set_fail(vblk);
            return;
        }

        if (virtq_add_used(vblk->common.dma, queue, chain.head, used_len) < 0) {
            virtio_blk_set_fail(vblk);
            return;
        }
        consumed = true;
    }

    if (consumed && !virtq_interrupt_suppressed(vblk->common.dma, queue))
        virtio_irq_trigger(&vblk->common.irq, VIRTIO_INT__USED_RING);
}

static int virtio_blk_activate(void *opaque,
                               const struct virtio_activation_context *ctx)
{
    (void) opaque;
    (void) ctx;
    return 0;
}

static int virtio_blk_reset(void *opaque,
                            uint64_t old_generation,
                            uint64_t new_generation)
{
    (void) opaque;
    (void) old_generation;
    (void) new_generation;
    return 0;
}

static int virtio_blk_notify_queue(void *opaque,
                                   uint16_t queue_index,
                                   uint64_t generation)
{
    virtio_blk_state_t *vblk = opaque;
    struct virtq *queue;
    unsigned status;

    (void) generation;

    if (!vblk || queue_index != VBLK_QUEUE) {
        if (vblk)
            virtio_blk_set_fail(vblk);
        return -EINVAL;
    }

    status = virtio_blk_status_load(vblk);
    queue = &vblk->common.queues[VBLK_QUEUE];
    if (status & VIRTIO_STATUS__DEVICE_NEEDS_RESET)
        return 0;
    if (!(status & VIRTIO_STATUS__DRIVER_OK) || !queue->ready) {
        virtio_blk_set_fail(vblk);
        return -EINVAL;
    }

    virtio_blk_drain_queue(vblk, queue);
    return 0;
}

static const struct virtio_device_ops virtio_blk_ops = {
    .activate = virtio_blk_activate,
    .reset = virtio_blk_reset,
    .notify_queue = virtio_blk_notify_queue,
    .read_config = virtio_blk_read_config,
    .write_config = virtio_blk_write_config,
};

static bool virtio_blk_load_width_bytes(uint8_t width, size_t *access_size)
{
    switch (width) {
    case RV_MEM_LW:
        *access_size = 4;
        return true;
    case RV_MEM_LBU:
    case RV_MEM_LB:
        *access_size = 1;
        return true;
    case RV_MEM_LHU:
    case RV_MEM_LH:
        *access_size = 2;
        return true;
    default:
        return false;
    }
}

static bool virtio_blk_store_width_bytes(uint8_t width, size_t *access_size)
{
    switch (width) {
    case RV_MEM_SW:
        *access_size = 4;
        return true;
    case RV_MEM_SB:
        *access_size = 1;
        return true;
    case RV_MEM_SH:
        *access_size = 2;
        return true;
    default:
        return false;
    }
}

static bool virtio_blk_is_config_access(uint32_t addr, size_t access_size)
{
    const uint32_t base = VIRTIO_Config << 2;
    const uint32_t end = base + (uint32_t) sizeof(struct virtio_blk_config);

    if (access_size == 0 || addr < base || addr >= end)
        return false;
    return access_size <= end - addr;
}

void virtio_blk_read(hart_t *vm,
                     virtio_blk_state_t *vblk,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value)
{
    size_t access_size = 0;
    bool is_cfg;
    int ret;

    if (!virtio_blk_load_width_bytes(width, &access_size)) {
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }

    is_cfg = virtio_blk_is_config_access(addr, access_size);
    if (addr >= (VIRTIO_Config << 2) && !is_cfg) {
        vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
        return;
    }

    if (!is_cfg) {
        if (access_size != 4 || (addr & 0x3)) {
            vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
            return;
        }
    } else if (addr & (access_size - 1)) {
        vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
        return;
    }

    ret = virtio_mmio_read(&vblk->common, addr, (uint8_t) access_size, value);
    if (ret < 0)
        vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
}

void virtio_blk_write(hart_t *vm,
                      virtio_blk_state_t *vblk,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value)
{
    size_t access_size = 0;
    bool is_cfg;
    int ret;

    if (!virtio_blk_store_width_bytes(width, &access_size)) {
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }

    is_cfg = virtio_blk_is_config_access(addr, access_size);
    if (addr >= (VIRTIO_Config << 2) && !is_cfg) {
        vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
        return;
    }

    if (!is_cfg) {
        if (access_size != 4 || (addr & 0x3)) {
            vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
            return;
        }
    } else if (addr & (access_size - 1)) {
        vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
        return;
    }

    ret = virtio_mmio_write(&vblk->common, addr, (uint8_t) access_size, value);
    if (ret < 0)
        vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
}

bool virtio_blk_irq_pending(virtio_blk_state_t *vblk)
{
    return virtio_irq_read_status(&vblk->common.irq) != 0;
}

uint32_t *virtio_blk_init(virtio_blk_state_t *vblk,
                          emu_state_t *emu,
                          char *disk_file)
{
    static const uint16_t queue_max_sizes[] = {
        [VBLK_QUEUE] = VBLK_QUEUE_NUM_MAX,
    };
    struct virtio_device_common_config common_config;
    uint32_t *disk_mem = NULL;

    if (!vblk || !emu) {
        fprintf(stderr, "Failed to initialize virtio-blk common device.\n");
        exit(2);
    }

    if (vblk_dev_cnt >= VBLK_DEV_CNT_MAX) {
        fprintf(stderr,
                "Exceeded the number of virtio-blk devices that can be "
                "allocated.\n");
        exit(2);
    }

    memset(vblk, 0, sizeof(*vblk));
    vblk->ram = emu->ram;
    vblk->priv = &vblk_configs[vblk_dev_cnt++];
    memset(PRIV(vblk), 0, sizeof(*PRIV(vblk)));
    PRIV(vblk)->blk_size = DISK_BLK_SIZE;

    if (disk_file) {
        int disk_fd;
        struct stat st;
        size_t disk_size;

        disk_fd = open(disk_file, O_RDWR);
        if (disk_fd < 0) {
            fprintf(stderr, "could not open %s\n", disk_file);
            exit(2);
        }

        if (fstat(disk_fd, &st) < 0) {
            fprintf(stderr, "fstat(%s): %s\n", disk_file, strerror(errno));
            close(disk_fd);
            exit(2);
        }
        if (st.st_size <= 0) {
            fprintf(stderr, "%s is empty or has invalid size\n", disk_file);
            close(disk_fd);
            exit(2);
        }
        disk_size = st.st_size;

        disk_mem = mmap(NULL, disk_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        disk_fd, 0);
        if (disk_mem == MAP_FAILED) {
            fprintf(stderr, "Could not map disk\n");
            close(disk_fd);
            return NULL;
        }
        assert(!(((uintptr_t) disk_mem) & 0b11));
        close(disk_fd);

        vblk->disk = disk_mem;
        PRIV(vblk)->capacity = (disk_size - 1) / DISK_BLK_SIZE + 1;

        if (vblk_disks_cnt == 0)
            atexit(virtio_blk_sync_all);
        vblk_disks[vblk_disks_cnt].addr = disk_mem;
        vblk_disks[vblk_disks_cnt].size = disk_size;
        vblk_disks_cnt++;
    }

    common_config = (struct virtio_device_common_config) {
        .emu = emu,
        .dma = &emu->ram_dma,
        .irq_source = SEMU_IRQ_SOURCE_VBLK,
        .device_id = 2,
        .vendor_id = VIRTIO_VENDOR_ID,
        .device_features = VIRTIO_BLK_F_VERSION_1,
        .required_features = VIRTIO_BLK_F_VERSION_1,
        .queue_max_sizes = queue_max_sizes,
        .num_queues = ARRAY_SIZE(queue_max_sizes),
        .ops = &virtio_blk_ops,
        .opaque = vblk,
    };

    if (virtio_device_common_init(&vblk->common, &common_config) < 0) {
        fprintf(stderr, "Failed to initialize virtio-blk common device.\n");
        exit(2);
    }

    return disk_mem;
}

void virtio_blk_destroy(virtio_blk_state_t *vblk)
{
    if (!vblk)
        return;

    virtio_device_common_destroy(&vblk->common);
    if (vblk->priv && vblk_dev_cnt > 0)
        vblk_dev_cnt--;
    vblk->priv = NULL;
}
