#define _XOPEN_SOURCE 700

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../ram_access.h"
#include "../riscv_private.h"

static bool test_ram_dma_write(ram_dma_t *dma,
                               guest_paddr_t addr,
                               const void *buf,
                               guest_size_t len);

#define ram_dma_write test_ram_dma_write
#include "../virtio-fs.c"
#undef ram_dma_write

#define REG(reg) ((uint32_t) VIRTIO_##reg << 2)
#define TEST_RAM_SIZE 16384
#define QUEUE_SIZE 8
#define QUEUE_STRIDE 0x300
#define DESC_ADDR(q) (0x100 + (guest_paddr_t) (q) * QUEUE_STRIDE)
#define AVAIL_ADDR(q) (0x180 + (guest_paddr_t) (q) * QUEUE_STRIDE)
#define USED_ADDR(q) (0x200 + (guest_paddr_t) (q) * QUEUE_STRIDE)
#define REQ_HDR_ADDR 0x0c00
#define INIT_IN_ADDR 0x0d00
#define RESP_HDR_ADDR 0x0e00
#define INIT_OUT_ADDR 0x0f00
#define ATTR_OUT_ADDR 0x1000
#define NAME_ADDR 0x1100
#define ENTRY_OUT_ADDR 0x1200
#define OPEN_IN_ADDR 0x1300
#define OPEN_OUT_ADDR 0x1400
#define READ_IN_ADDR 0x1500
#define READ_OUT_ADDR 0x1600
#define RELEASE_IN_ADDR 0x1900
#define TEST_FILE_CONTENT_ADDR 0x1a00
#define TEST_FILE_CONTENT "virtio-fs-common-read"

static uint32_t ram_words[TEST_RAM_SIZE / 4];
static emu_state_t emu;
static unsigned wake_count;

struct dma_gate {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool enabled;
    bool entered;
    bool release;
    guest_paddr_t addr;
    guest_size_t len;
};

static struct dma_gate fs_dma_write_gate = {
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

static bool test_ram_dma_write(ram_dma_t *dma,
                               guest_paddr_t addr,
                               const void *buf,
                               guest_size_t len)
{
    dma_gate_block_if_enabled(&fs_dma_write_gate, addr, len);
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

static void write_desc(uint16_t queue,
                       uint16_t index,
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

    dma_write(DESC_ADDR(queue) + (guest_paddr_t) index * sizeof(desc), &desc,
              sizeof(desc));
}

static int mmio_write_width_result(uint32_t reg, uint8_t width, uint32_t value)
{
    return virtio_mmio_write(&emu.vfs.common, reg, width, value);
}

static int mmio_write_result(uint32_t reg, uint32_t value)
{
    return mmio_write_width_result(reg, sizeof(uint32_t), value);
}

static void mmio_write_width(uint32_t reg, uint8_t width, uint32_t value)
{
    int ret = mmio_write_width_result(reg, width, value);

    if (ret != 0) {
        fprintf(stderr,
                "mmio write reg 0x%x width %u value 0x%x: got %d, want 0\n",
                reg, width, value, ret);
        exit(1);
    }
}

static void mmio_write(uint32_t reg, uint32_t value)
{
    int ret = mmio_write_result(reg, value);

    if (ret != 0) {
        fprintf(stderr, "mmio write reg 0x%x value 0x%x: got %d, want 0\n", reg,
                value, ret);
        exit(1);
    }
}

static uint32_t mmio_read_width(uint32_t reg, uint8_t width)
{
    uint32_t value = 0;

    require_int("mmio read",
                virtio_mmio_read(&emu.vfs.common, reg, width, &value), 0);
    return value;
}

static uint32_t mmio_read(uint32_t reg)
{
    return mmio_read_width(reg, sizeof(uint32_t));
}

static void setup_fixture_with_dir(const char *shared_dir)
{
    memset(&emu, 0, sizeof(emu));
    memset(ram_words, 0, sizeof(ram_words));
    ram_dma_init(&emu.ram_dma, ram_words, TEST_RAM_SIZE, NULL);
    emu.ram = ram_words;
    require_int("lifecycle init", semu_vm_lifecycle_init(&emu.lifecycle), 0);
    require_int("lifecycle running",
                semu_vm_lifecycle_enter_running(&emu.lifecycle), 0);
    require_int("plic lock init", pthread_mutex_init(&emu.plic_lock, NULL), 0);
    wake_count = 0;

    require_bool("virtio fs init",
                 virtio_fs_init(&emu.vfs, &emu, "myfs", (char *) shared_dir),
                 true);

    require_u32("device id", mmio_read(REG(DeviceID)), 26);
    require_u32("queue max", mmio_read(REG(QueueNumMax)), VFS_QUEUE_NUM_MAX);
    mmio_write(REG(DeviceFeaturesSel), 1);
    require_u32("VERSION_1 advertised", mmio_read(REG(DeviceFeatures)), 1);
    mmio_write(REG(DriverFeaturesSel), 1);
    mmio_write(REG(DriverFeatures), 1);
    mmio_write(REG(Status), VIRTIO_STATUS__ACKNOWLEDGE);
    mmio_write(REG(Status), VIRTIO_STATUS__DRIVER);
    mmio_write(REG(Status), VIRTIO_STATUS__FEATURES_OK);

    for (uint16_t q = 0; q < VFS_NUM_QUEUES; q++) {
        mmio_write(REG(QueueSel), q);
        mmio_write(REG(QueueNum), QUEUE_SIZE);
        mmio_write(REG(QueueDescLow), DESC_ADDR(q));
        mmio_write(REG(QueueDriverLow), AVAIL_ADDR(q));
        mmio_write(REG(QueueDeviceLow), USED_ADDR(q));
        mmio_write(REG(QueueReady), 1);
    }

    mmio_write(REG(Status), VIRTIO_STATUS__DRIVER_OK);
}

static void setup_fixture(void)
{
    setup_fixture_with_dir("/tmp");
}

static void teardown_fixture(void)
{
    virtio_fs_destroy(&emu.vfs);
    pthread_mutex_destroy(&emu.plic_lock);
    semu_vm_lifecycle_destroy(&emu.lifecycle);
}

static bool source_asserted(emu_state_t *emu_arg, enum semu_irq_source source)
{
    return (emu_arg->plic.active & semu_irq_source_plic_bit(source)) != 0;
}

static bool wait_for_used_idx(uint16_t queue, uint16_t idx)
{
    for (unsigned i = 0; i < 1000; i++) {
        if (read16(USED_ADDR(queue) + 2) == idx)
            return true;
        sleep_one_ms();
    }
    return false;
}

static bool wait_for_fs_actor_state(virtio_fs_state_t *vfs,
                                    enum virtio_actor_state state)
{
    for (unsigned i = 0; i < 1000; i++) {
        if (virtio_actor_get_state(&vfs->actor) == state)
            return true;
        sleep_one_ms();
    }
    return false;
}

static bool wait_for_status_bits(uint32_t bits)
{
    for (unsigned i = 0; i < 1000; i++) {
        if ((mmio_read(REG(Status)) & bits) == bits)
            return true;
        sleep_one_ms();
    }
    return false;
}

static bool wait_for_interrupt_status(uint32_t status)
{
    for (unsigned i = 0; i < 1000; i++) {
        if (mmio_read(REG(InterruptStatus)) == status)
            return true;
        sleep_one_ms();
    }
    return false;
}

struct async_fs_call {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    virtio_fs_state_t *vfs;
    uint16_t queue;
    int ret;
    bool done;
};

static void async_fs_call_init(struct async_fs_call *call,
                               virtio_fs_state_t *vfs,
                               uint16_t queue)
{
    memset(call, 0, sizeof(*call));
    call->vfs = vfs;
    call->queue = queue;
    require_int("async lock init", pthread_mutex_init(&call->lock, NULL), 0);
    require_int("async cond init", pthread_cond_init(&call->cond, NULL), 0);
}

static void async_fs_call_destroy(struct async_fs_call *call)
{
    pthread_cond_destroy(&call->cond);
    pthread_mutex_destroy(&call->lock);
}

static void async_fs_call_finish(struct async_fs_call *call, int ret)
{
    pthread_mutex_lock(&call->lock);
    call->ret = ret;
    call->done = true;
    pthread_cond_broadcast(&call->cond);
    pthread_mutex_unlock(&call->lock);
}

static bool async_fs_call_wait_done(struct async_fs_call *call,
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

static void async_fs_call_join(struct async_fs_call *call)
{
    require_int("async join", pthread_join(call->thread, NULL), 0);
}

static void *notify_queue_thread(void *opaque)
{
    struct async_fs_call *call = opaque;
    int ret =
        virtio_mmio_write(&call->vfs->common, REG(QueueNotify), 4, call->queue);

    async_fs_call_finish(call, ret);
    return NULL;
}

static void *reset_fs_thread(void *opaque)
{
    struct async_fs_call *call = opaque;
    int ret = virtio_device_common_reset(&call->vfs->common);

    async_fs_call_finish(call, ret);
    return NULL;
}

static void *destroy_fs_thread(void *opaque)
{
    struct async_fs_call *call = opaque;

    virtio_fs_destroy(call->vfs);
    async_fs_call_finish(call, 0);
    return NULL;
}

static void async_fs_call_start_notify(struct async_fs_call *call)
{
    require_int("notify thread create",
                pthread_create(&call->thread, NULL, notify_queue_thread, call),
                0);
}

static void async_fs_call_start_reset(struct async_fs_call *call)
{
    require_int("reset thread create",
                pthread_create(&call->thread, NULL, reset_fs_thread, call), 0);
}

static void async_fs_call_start_destroy(struct async_fs_call *call)
{
    require_int("destroy thread create",
                pthread_create(&call->thread, NULL, destroy_fs_thread, call),
                0);
}

static void test_config_reads_tag_and_request_queue_count(void)
{
    setup_fixture();

    require_u8("tag byte 0", (uint8_t) mmio_read_width(REG(Config), 1), 'm');
    require_u8("tag byte 1", (uint8_t) mmio_read_width(REG(Config) + 1, 1),
               'y');
    require_u8("tag terminator", (uint8_t) mmio_read_width(REG(Config) + 4, 1),
               '\0');
    require_u32("num request queues", mmio_read(REG(Config) + 36), 2);

    teardown_fixture();
}

static void publish_fuse_init_request(uint16_t queue)
{
    struct vfs_req_header req = {
        .in =
            {
                .len =
                    sizeof(struct vfs_req_header) + sizeof(struct fuse_init_in),
                .opcode = FUSE_INIT,
                .unique = 0x12345678,
            },
    };
    struct fuse_init_in init_in = {
        .major = 7,
        .minor = 31,
        .max_readahead = 0x10000,
        .flags = FUSE_ASYNC_READ,
    };

    dma_write(REQ_HDR_ADDR, &req, sizeof(req));
    dma_write(INIT_IN_ADDR, &init_in, sizeof(init_in));
    write_desc(queue, 0, REQ_HDR_ADDR, sizeof(req), VIRTIO_DESC_F_NEXT, 1);
    write_desc(queue, 1, INIT_IN_ADDR, sizeof(init_in), VIRTIO_DESC_F_NEXT, 2);
    write_desc(queue, 2, RESP_HDR_ADDR, sizeof(struct vfs_resp_header),
               VIRTIO_DESC_F_NEXT | VIRTIO_DESC_F_WRITE, 3);
    write_desc(queue, 3, INIT_OUT_ADDR, sizeof(struct fuse_init_out),
               VIRTIO_DESC_F_WRITE, 0);
    write16(AVAIL_ADDR(queue) + 4, 0);
    write16(AVAIL_ADDR(queue) + 2, 1);
}

static void publish_getattr_root_request(uint16_t queue)
{
    struct vfs_req_header req = {
        .in =
            {
                .len = sizeof(struct vfs_req_header),
                .opcode = FUSE_GETATTR,
                .unique = 0x87654321,
                .nodeid = 1,
            },
    };

    dma_write(REQ_HDR_ADDR, &req, sizeof(req));
    write_desc(queue, 0, REQ_HDR_ADDR, sizeof(req), VIRTIO_DESC_F_NEXT, 1);
    write_desc(queue, 1, RESP_HDR_ADDR, sizeof(struct vfs_resp_header),
               VIRTIO_DESC_F_NEXT | VIRTIO_DESC_F_WRITE, 2);
    write_desc(queue, 2, ATTR_OUT_ADDR, sizeof(struct fuse_attr_out),
               VIRTIO_DESC_F_WRITE, 0);
    write16(AVAIL_ADDR(queue) + 4, 0);
    write16(AVAIL_ADDR(queue) + 2, 1);
}

static void submit_queue_head(uint16_t queue, uint16_t avail_idx, uint16_t head)
{
    write16(AVAIL_ADDR(queue) + 4 +
                (guest_paddr_t) (avail_idx % QUEUE_SIZE) * sizeof(uint16_t),
            head);
    write16(AVAIL_ADDR(queue) + 2, (uint16_t) (avail_idx + 1));
    mmio_write(REG(QueueNotify), queue);
    require_bool("actor published submitted queue head",
                 wait_for_used_idx(queue, (uint16_t) (avail_idx + 1)), true);
}

static void ack_used_irq(void)
{
    require_u32("used irq status", mmio_read(REG(InterruptStatus)),
                VIRTIO_INT__USED_RING);
    mmio_write(REG(InterruptACK), VIRTIO_INT__USED_RING);
}

static void publish_lookup_request(uint16_t queue,
                                   uint64_t unique,
                                   uint64_t nodeid,
                                   const void *name,
                                   uint32_t name_len)
{
    struct vfs_req_header req = {
        .in =
            {
                .len = sizeof(struct vfs_req_header) + name_len,
                .opcode = FUSE_LOOKUP,
                .unique = unique,
                .nodeid = nodeid,
            },
    };

    dma_write(REQ_HDR_ADDR, &req, sizeof(req));
    dma_write(NAME_ADDR, name, name_len);
    write_desc(queue, 0, REQ_HDR_ADDR, sizeof(req), VIRTIO_DESC_F_NEXT, 1);
    write_desc(queue, 1, NAME_ADDR, name_len, VIRTIO_DESC_F_NEXT, 2);
    write_desc(queue, 2, RESP_HDR_ADDR, sizeof(struct vfs_resp_header),
               VIRTIO_DESC_F_NEXT | VIRTIO_DESC_F_WRITE, 3);
    write_desc(queue, 3, ENTRY_OUT_ADDR, sizeof(struct fuse_entry_out),
               VIRTIO_DESC_F_WRITE, 0);
}

static void publish_open_request(uint16_t queue,
                                 uint64_t unique,
                                 uint64_t nodeid)
{
    struct vfs_req_header req = {
        .in =
            {
                .len =
                    sizeof(struct vfs_req_header) + sizeof(struct fuse_open_in),
                .opcode = FUSE_OPEN,
                .unique = unique,
                .nodeid = nodeid,
            },
    };
    struct fuse_open_in open_in = {0};

    dma_write(REQ_HDR_ADDR, &req, sizeof(req));
    dma_write(OPEN_IN_ADDR, &open_in, sizeof(open_in));
    write_desc(queue, 0, REQ_HDR_ADDR, sizeof(req), VIRTIO_DESC_F_NEXT, 1);
    write_desc(queue, 1, OPEN_IN_ADDR, sizeof(open_in), VIRTIO_DESC_F_NEXT, 2);
    write_desc(queue, 2, RESP_HDR_ADDR, sizeof(struct vfs_resp_header),
               VIRTIO_DESC_F_NEXT | VIRTIO_DESC_F_WRITE, 3);
    write_desc(queue, 3, OPEN_OUT_ADDR, sizeof(struct fuse_open_out),
               VIRTIO_DESC_F_WRITE, 0);
}

static void publish_read_request(uint16_t queue,
                                 uint64_t unique,
                                 uint64_t nodeid,
                                 uint64_t fh,
                                 uint32_t size)
{
    struct vfs_req_header req = {
        .in =
            {
                .len =
                    sizeof(struct vfs_req_header) + sizeof(struct fuse_read_in),
                .opcode = FUSE_READ,
                .unique = unique,
                .nodeid = nodeid,
            },
    };
    struct fuse_read_in read_in = {
        .fh = fh,
        .size = size,
    };

    dma_write(REQ_HDR_ADDR, &req, sizeof(req));
    dma_write(READ_IN_ADDR, &read_in, sizeof(read_in));
    write_desc(queue, 0, REQ_HDR_ADDR, sizeof(req), VIRTIO_DESC_F_NEXT, 1);
    write_desc(queue, 1, READ_IN_ADDR, sizeof(read_in), VIRTIO_DESC_F_NEXT, 2);
    write_desc(queue, 2, RESP_HDR_ADDR, sizeof(struct vfs_resp_header),
               VIRTIO_DESC_F_NEXT | VIRTIO_DESC_F_WRITE, 3);
    write_desc(queue, 3, READ_OUT_ADDR, size, VIRTIO_DESC_F_WRITE, 0);
}

static void publish_release_request(uint16_t queue,
                                    uint64_t unique,
                                    uint64_t nodeid,
                                    uint64_t fh)
{
    struct vfs_req_header req = {
        .in =
            {
                .len = sizeof(struct vfs_req_header) +
                       sizeof(struct fuse_release_in),
                .opcode = FUSE_RELEASE,
                .unique = unique,
                .nodeid = nodeid,
            },
    };
    struct fuse_release_in release_in = {.fh = fh};

    dma_write(REQ_HDR_ADDR, &req, sizeof(req));
    dma_write(RELEASE_IN_ADDR, &release_in, sizeof(release_in));
    write_desc(queue, 0, REQ_HDR_ADDR, sizeof(req), VIRTIO_DESC_F_NEXT, 1);
    write_desc(queue, 1, RELEASE_IN_ADDR, sizeof(release_in),
               VIRTIO_DESC_F_NEXT, 2);
    write_desc(queue, 2, RESP_HDR_ADDR, sizeof(struct vfs_resp_header),
               VIRTIO_DESC_F_WRITE, 0);
}

static void publish_releasedir_request(uint16_t queue,
                                       uint64_t unique,
                                       uint64_t fh)
{
    struct vfs_req_header req = {
        .in =
            {
                .len = sizeof(struct vfs_req_header) +
                       sizeof(struct fuse_release_in),
                .opcode = FUSE_RELEASEDIR,
                .unique = unique,
                .nodeid = 1,
            },
    };
    struct fuse_release_in release_in = {.fh = fh};

    dma_write(REQ_HDR_ADDR, &req, sizeof(req));
    dma_write(RELEASE_IN_ADDR, &release_in, sizeof(release_in));
    write_desc(queue, 0, REQ_HDR_ADDR, sizeof(req), VIRTIO_DESC_F_NEXT, 1);
    write_desc(queue, 1, RELEASE_IN_ADDR, sizeof(release_in),
               VIRTIO_DESC_F_NEXT, 2);
    write_desc(queue, 2, RESP_HDR_ADDR, sizeof(struct vfs_resp_header),
               VIRTIO_DESC_F_WRITE, 0);
}

static void create_shared_file(char *dir_template,
                               const char *name,
                               const char *contents,
                               char **created_dir)
{
    char *dir = mkdtemp(dir_template);
    char path[256];
    FILE *file;

    require_bool("mkdtemp", dir != NULL, true);
    require_bool(
        "test path fits",
        snprintf(path, sizeof(path), "%s/%s", dir, name) < (int) sizeof(path),
        true);
    file = fopen(path, "wb");
    require_bool("create shared file", file != NULL, true);
    require_bool(
        "write shared file",
        fwrite(contents, 1, strlen(contents), file) == strlen(contents), true);
    require_int("close shared file", fclose(file), 0);
    *created_dir = dir;
}

static void remove_shared_file_tree(const char *dir, const char *name)
{
    char path[256];

    require_bool(
        "cleanup path fits",
        snprintf(path, sizeof(path), "%s/%s", dir, name) < (int) sizeof(path),
        true);
    require_int("unlink shared file", unlink(path), 0);
    require_int("rmdir shared dir", rmdir(dir), 0);
}

static void test_config_writes_do_not_mutate_device_config(void)
{
    setup_fixture();

    require_u8("tag byte before write",
               (uint8_t) mmio_read_width(REG(Config), 1), 'm');
    require_u32("queue count before write", mmio_read(REG(Config) + 36), 2);

    mmio_write_width(REG(Config), 1, 'x');
    mmio_write_width(REG(Config) + 36, sizeof(uint32_t), 0xdeadbeef);

    require_u8("tag byte after write",
               (uint8_t) mmio_read_width(REG(Config), 1), 'm');
    require_u32("queue count after write", mmio_read(REG(Config) + 36), 2);

    teardown_fixture();
}

static void test_fuse_init_completes_asynchronously_and_acks_irq(void)
{
    struct vfs_resp_header header;
    struct fuse_init_out init_out;
    uint32_t expected_len =
        sizeof(struct fuse_out_header) + sizeof(struct fuse_init_out);
    const uint16_t queue = 1;

    setup_fixture();
    publish_fuse_init_request(queue);

    mmio_write(REG(QueueNotify), queue);
    require_bool("actor published fuse init completion",
                 wait_for_used_idx(queue, 1), true);

    dma_read(RESP_HDR_ADDR, &header, sizeof(header));
    dma_read(INIT_OUT_ADDR, &init_out, sizeof(init_out));
    require_u16("used idx", read16(USED_ADDR(queue) + 2), 1);
    require_u32("used id", read32(USED_ADDR(queue) + 4), 0);
    require_u32("used len", read32(USED_ADDR(queue) + 8), expected_len);
    require_u32("response len", header.out.len, expected_len);
    require_u32("response error", (uint32_t) header.out.error, 0);
    require_u32("init major", init_out.major, 7);
    require_u32("init minor", init_out.minor, 41);
    require_u32("init flags", init_out.flags,
                FUSE_ASYNC_READ | FUSE_BIG_WRITES | FUSE_DO_READDIRPLUS);
    require_u32("irq status", mmio_read(REG(InterruptStatus)),
                VIRTIO_INT__USED_RING);
    require_bool("irq pending helper", virtio_fs_irq_pending(&emu.vfs), true);
    require_bool("irq line", source_asserted(&emu, SEMU_IRQ_SOURCE_VFS), true);
    require_u32("wake count", wake_count, 1);

    mmio_write(REG(InterruptACK), VIRTIO_INT__USED_RING);
    require_u32("irq acked", mmio_read(REG(InterruptStatus)), 0);
    require_bool("irq pending after ack", virtio_fs_irq_pending(&emu.vfs),
                 false);

    teardown_fixture();
}

static void test_queue_notify_returns_before_used_ring_publication(void)
{
    struct async_fs_call notify;
    struct vfs_resp_header header = {0};
    const uint16_t queue = 1;

    setup_fixture();
    publish_fuse_init_request(queue);
    dma_gate_enable(&fs_dma_write_gate, RESP_HDR_ADDR,
                    sizeof(struct vfs_resp_header));
    async_fs_call_init(&notify, &emu.vfs, queue);
    async_fs_call_start_notify(&notify);

    require_bool("actor entered response header write",
                 dma_gate_wait_entered(&fs_dma_write_gate, 1000), true);
    require_bool("QueueNotify returned while actor response write is blocked",
                 async_fs_call_wait_done(&notify, 1000), true);
    require_int("QueueNotify return", notify.ret, 0);
    require_u16("no used completion before actor write returns",
                read16(USED_ADDR(queue) + 2), 0);
    require_u32("no interrupt before actor write returns",
                mmio_read(REG(InterruptStatus)), 0);

    dma_gate_release(&fs_dma_write_gate);
    async_fs_call_join(&notify);
    require_bool("actor published async fs completion",
                 wait_for_used_idx(queue, 1), true);
    dma_read(RESP_HDR_ADDR, &header, sizeof(header));
    require_u32("async response len", header.out.len,
                sizeof(struct fuse_out_header) + sizeof(struct fuse_init_out));
    require_u32("async interrupt", mmio_read(REG(InterruptStatus)),
                VIRTIO_INT__USED_RING);
    require_bool("async irq line", source_asserted(&emu, SEMU_IRQ_SOURCE_VFS),
                 true);

    async_fs_call_destroy(&notify);
    teardown_fixture();
}

static void test_getattr_root_uses_common_dma_path(void)
{
    struct vfs_resp_header header;
    struct fuse_attr_out outattr;
    uint32_t expected_len =
        sizeof(struct fuse_out_header) + sizeof(struct fuse_attr_out);
    const uint16_t queue = 1;

    setup_fixture();
    publish_getattr_root_request(queue);

    mmio_write(REG(QueueNotify), queue);
    require_bool("actor published getattr completion",
                 wait_for_used_idx(queue, 1), true);

    dma_read(RESP_HDR_ADDR, &header, sizeof(header));
    dma_read(ATTR_OUT_ADDR, &outattr, sizeof(outattr));
    require_u16("used idx", read16(USED_ADDR(queue) + 2), 1);
    require_u32("used id", read32(USED_ADDR(queue) + 4), 0);
    require_u32("used len", read32(USED_ADDR(queue) + 8), expected_len);
    require_u32("getattr response len", header.out.len, expected_len);
    require_u32("getattr response error", (uint32_t) header.out.error, 0);
    require_bool("getattr ino populated", outattr.attr.ino != 0, true);
    require_bool("getattr mode populated", outattr.attr.mode != 0, true);
    require_u32("getattr irq status", mmio_read(REG(InterruptStatus)),
                VIRTIO_INT__USED_RING);

    mmio_write(REG(InterruptACK), VIRTIO_INT__USED_RING);

    teardown_fixture();
}

static void test_lookup_rejects_path_escape_name(void)
{
    struct vfs_resp_header header;
    const uint16_t queue = 1;
    const char name[] = "../escape";

    setup_fixture();
    publish_lookup_request(queue, 0x1111, 1, name, sizeof(name) - 1);

    submit_queue_head(queue, 0, 0);

    dma_read(RESP_HDR_ADDR, &header, sizeof(header));
    require_u16("lookup escape used idx", read16(USED_ADDR(queue) + 2), 1);
    require_u32("lookup escape used len", read32(USED_ADDR(queue) + 8),
                sizeof(struct fuse_out_header));
    require_u32("lookup escape response len", header.out.len,
                sizeof(struct fuse_out_header));
    require_int("lookup escape error", header.out.error, -EINVAL);
    ack_used_irq();

    teardown_fixture();
}

static void test_lookup_rejects_embedded_nul_name(void)
{
    struct vfs_resp_header header;
    const uint16_t queue = 1;
    const char name[] = {'f', 'i', '\0', 'l', 'e'};

    setup_fixture();
    publish_lookup_request(queue, 0x1112, 1, name, sizeof(name));

    submit_queue_head(queue, 0, 0);

    dma_read(RESP_HDR_ADDR, &header, sizeof(header));
    require_u32("lookup nul response len", header.out.len,
                sizeof(struct fuse_out_header));
    require_int("lookup nul error", header.out.error, -EINVAL);
    ack_used_irq();

    teardown_fixture();
}

static void test_invalid_read_handle_returns_ebadf(void)
{
    struct vfs_resp_header header;
    const uint16_t queue = 1;

    setup_fixture();
    publish_read_request(queue, 0x3002, 1, 0, 8);

    submit_queue_head(queue, 0, 0);

    dma_read(RESP_HDR_ADDR, &header, sizeof(header));
    require_u32("invalid read len", header.out.len,
                sizeof(struct fuse_out_header));
    require_int("invalid read error", header.out.error, -EBADF);
    ack_used_irq();

    teardown_fixture();
}

static void test_lookup_open_read_release_valid_file(void)
{
    struct vfs_resp_header header;
    struct fuse_entry_out entry_out;
    struct fuse_open_out open_out;
    char contents[sizeof(TEST_FILE_CONTENT)] = {0};
    uint32_t expected_read_len =
        sizeof(struct fuse_out_header) + strlen(TEST_FILE_CONTENT);
    char dir_template[] = "/tmp/semu-vfs-test-XXXXXX";
    char *shared_dir = NULL;
    const uint16_t queue = 1;

    create_shared_file(dir_template, "file.txt", TEST_FILE_CONTENT,
                       &shared_dir);
    setup_fixture_with_dir(shared_dir);

    publish_lookup_request(queue, 0x2001, 1, "file.txt", strlen("file.txt"));
    submit_queue_head(queue, 0, 0);
    dma_read(RESP_HDR_ADDR, &header, sizeof(header));
    dma_read(ENTRY_OUT_ADDR, &entry_out, sizeof(entry_out));
    require_int("lookup file error", header.out.error, 0);
    require_bool("lookup file nodeid", entry_out.nodeid != 0, true);
    ack_used_irq();

    publish_open_request(queue, 0x2002, entry_out.nodeid);
    submit_queue_head(queue, 1, 0);
    dma_read(RESP_HDR_ADDR, &header, sizeof(header));
    dma_read(OPEN_OUT_ADDR, &open_out, sizeof(open_out));
    require_int("open file error", header.out.error, 0);
    require_bool("open handle id", open_out.fh != 0, true);
    ack_used_irq();

    publish_read_request(queue, 0x2003, entry_out.nodeid, open_out.fh,
                         strlen(TEST_FILE_CONTENT));
    submit_queue_head(queue, 2, 0);
    dma_read(RESP_HDR_ADDR, &header, sizeof(header));
    dma_read(READ_OUT_ADDR, contents, strlen(TEST_FILE_CONTENT));
    require_int("read file error", header.out.error, 0);
    require_u32("read file len", header.out.len, expected_read_len);
    require_bool(
        "read file contents",
        memcmp(contents, TEST_FILE_CONTENT, strlen(TEST_FILE_CONTENT)) == 0,
        true);
    ack_used_irq();

    publish_release_request(queue, 0x2004, entry_out.nodeid, open_out.fh);
    submit_queue_head(queue, 3, 0);
    dma_read(RESP_HDR_ADDR, &header, sizeof(header));
    require_int("release file error", header.out.error, 0);
    require_u32("release file len", header.out.len,
                sizeof(struct fuse_out_header));
    ack_used_irq();

    teardown_fixture();
    remove_shared_file_tree(shared_dir, "file.txt");
}

static void test_invalid_releasedir_handle_returns_ebadf(void)
{
    struct vfs_resp_header header;
    const uint16_t queue = 1;

    setup_fixture();
    publish_releasedir_request(queue, 0x3001, 0);

    submit_queue_head(queue, 0, 0);

    dma_read(RESP_HDR_ADDR, &header, sizeof(header));
    require_u32("invalid releasedir len", header.out.len,
                sizeof(struct fuse_out_header));
    require_int("invalid releasedir error", header.out.error, -EBADF);
    ack_used_irq();

    teardown_fixture();
}

static void test_invalid_queue_notify_sets_needs_reset(void)
{
    int ret;

    setup_fixture();

    ret = virtio_fs_notify_queue(&emu.vfs, VFS_NUM_QUEUES,
                                 emu.vfs.common.generation);

    require_int("invalid notify return", ret, -EINVAL);
    require_u32("invalid notify needs reset",
                mmio_read(REG(Status)) & VIRTIO_STATUS__DEVICE_NEEDS_RESET,
                VIRTIO_STATUS__DEVICE_NEEDS_RESET);
    require_u32("invalid notify conf change irq",
                mmio_read(REG(InterruptStatus)), VIRTIO_INT__CONF_CHANGE);

    teardown_fixture();
}

static void test_reset_cancels_stale_pending_actor_completion(void)
{
    struct async_fs_call notify;
    struct async_fs_call reset;
    const uint16_t queue = 1;

    setup_fixture();
    publish_fuse_init_request(queue);
    dma_gate_enable(&fs_dma_write_gate, RESP_HDR_ADDR,
                    sizeof(struct vfs_resp_header));
    async_fs_call_init(&notify, &emu.vfs, queue);
    async_fs_call_start_notify(&notify);
    require_bool("actor entered response write before reset",
                 dma_gate_wait_entered(&fs_dma_write_gate, 1000), true);
    require_bool("QueueNotify returned before reset",
                 async_fs_call_wait_done(&notify, 1000), true);

    async_fs_call_init(&reset, &emu.vfs, 0);
    async_fs_call_start_reset(&reset);
    require_bool("reset advanced actor generation",
                 wait_for_fs_actor_state(&emu.vfs, VIRTIO_ACTOR_RESETTING),
                 true);

    dma_gate_release(&fs_dma_write_gate);
    async_fs_call_join(&notify);
    async_fs_call_join(&reset);
    require_int("reset return", reset.ret, 0);
    require_u16("reset stale used idx remains clear",
                read16(USED_ADDR(queue) + 2), 0);
    require_u32("reset stale interrupt remains clear",
                mmio_read(REG(InterruptStatus)), 0);
    require_bool("reset stale irq line remains clear",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VFS), false);

    async_fs_call_destroy(&reset);
    async_fs_call_destroy(&notify);
    teardown_fixture();
}

static void test_stop_cancels_stale_pending_actor_completion(void)
{
    struct async_fs_call notify;
    struct async_fs_call destroy;
    const uint16_t queue = 1;

    setup_fixture();
    publish_fuse_init_request(queue);
    dma_gate_enable(&fs_dma_write_gate, RESP_HDR_ADDR,
                    sizeof(struct vfs_resp_header));
    async_fs_call_init(&notify, &emu.vfs, queue);
    async_fs_call_start_notify(&notify);
    require_bool("actor entered response write before stop",
                 dma_gate_wait_entered(&fs_dma_write_gate, 1000), true);
    require_bool("QueueNotify returned before stop",
                 async_fs_call_wait_done(&notify, 1000), true);

    async_fs_call_init(&destroy, &emu.vfs, 0);
    async_fs_call_start_destroy(&destroy);
    require_bool("stop advanced actor generation",
                 wait_for_fs_actor_state(&emu.vfs, VIRTIO_ACTOR_STOPPING),
                 true);

    dma_gate_release(&fs_dma_write_gate);
    async_fs_call_join(&notify);
    async_fs_call_join(&destroy);
    require_int("destroy return", destroy.ret, 0);
    require_u16("stop stale used idx remains clear",
                read16(USED_ADDR(queue) + 2), 0);
    require_bool("stop stale irq line remains clear",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VFS), false);

    async_fs_call_destroy(&destroy);
    async_fs_call_destroy(&notify);
    pthread_mutex_destroy(&emu.plic_lock);
    semu_vm_lifecycle_destroy(&emu.lifecycle);
}

static void test_malformed_avail_sets_needs_reset_and_conf_change_irq(void)
{
    int ret;
    const uint16_t queue = 1;

    setup_fixture();
    write16(AVAIL_ADDR(queue) + 2, QUEUE_SIZE + 1);

    ret = mmio_write_result(REG(QueueNotify), queue);

    require_int("malformed notify return", ret, 0);
    require_bool("malformed avail actor sets needs reset",
                 wait_for_status_bits(VIRTIO_STATUS__DEVICE_NEEDS_RESET), true);
    require_u32("needs reset",
                mmio_read(REG(Status)) & VIRTIO_STATUS__DEVICE_NEEDS_RESET,
                VIRTIO_STATUS__DEVICE_NEEDS_RESET);
    require_bool("conf change irq",
                 wait_for_interrupt_status(VIRTIO_INT__CONF_CHANGE), true);

    mmio_write(REG(InterruptACK), VIRTIO_INT__CONF_CHANGE);
    require_u32("conf change acked", mmio_read(REG(InterruptStatus)), 0);

    teardown_fixture();
}

int main(void)
{
    test_config_reads_tag_and_request_queue_count();
    test_config_writes_do_not_mutate_device_config();
    test_fuse_init_completes_asynchronously_and_acks_irq();
    test_queue_notify_returns_before_used_ring_publication();
    test_getattr_root_uses_common_dma_path();
    test_lookup_rejects_path_escape_name();
    test_lookup_rejects_embedded_nul_name();
    test_lookup_open_read_release_valid_file();
    test_invalid_read_handle_returns_ebadf();
    test_invalid_releasedir_handle_returns_ebadf();
    test_invalid_queue_notify_sets_needs_reset();
    test_reset_cancels_stale_pending_actor_completion();
    test_stop_cancels_stale_pending_actor_completion();
    test_malformed_avail_sets_needs_reset_and_conf_change_irq();
    return 0;
}
