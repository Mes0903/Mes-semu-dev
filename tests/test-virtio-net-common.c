#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "../ram_access.h"
#include "../riscv_private.h"
#include "../semu-event.h"

static bool test_ram_dma_read(const ram_dma_t *dma,
                              guest_paddr_t addr,
                              void *buf,
                              guest_size_t len);
static bool test_ram_dma_write(ram_dma_t *dma,
                               guest_paddr_t addr,
                               const void *buf,
                               guest_size_t len);
static ssize_t test_writev(int fd, const struct iovec *iov, int iovcnt);

#define ram_dma_read test_ram_dma_read
#define ram_dma_write test_ram_dma_write
#define writev test_writev
#include "../virtio-net.c"
#undef writev
#undef ram_dma_write
#undef ram_dma_read

#define REG(reg) ((uint32_t) VIRTIO_##reg << 2)
#define TEST_RAM_SIZE 8192
#define RX_DESC_ADDR 0x100
#define RX_AVAIL_ADDR 0x200
#define RX_USED_ADDR 0x300
#define TX_DESC_ADDR 0x400
#define TX_AVAIL_ADDR 0x500
#define TX_USED_ADDR 0x600
#define HEADER_ADDR 0x700
#define DATA_ADDR 0x740

static uint32_t ram_words[TEST_RAM_SIZE / 4];
static emu_state_t emu;
static net_user_options_t user_net;
static unsigned wake_count;
static struct virtio_device_common *reset_start_on_avail_read_common;
static guest_paddr_t reset_start_on_avail_read_addr;

struct writev_gate {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool enabled;
    bool entered;
    bool release;
};

static struct writev_gate net_writev_gate = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

struct async_net_call {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    virtio_net_state_t *vnet;
    bool done;
    int ret;
};

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

    return ram_dma_read(dma, addr, buf, len);
}

static bool test_ram_dma_write(ram_dma_t *dma,
                               guest_paddr_t addr,
                               const void *buf,
                               guest_size_t len)
{
    return ram_dma_write(dma, addr, buf, len);
}

static ssize_t test_writev(int fd, const struct iovec *iov, int iovcnt)
{
    pthread_mutex_lock(&net_writev_gate.lock);
    if (net_writev_gate.enabled) {
        net_writev_gate.entered = true;
        pthread_cond_broadcast(&net_writev_gate.cond);
        while (!net_writev_gate.release)
            pthread_cond_wait(&net_writev_gate.cond, &net_writev_gate.lock);
    }
    pthread_mutex_unlock(&net_writev_gate.lock);

    return writev(fd, iov, iovcnt);
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

bool netdev_init(netdev_t *netdev, const char *net_type)
{
    (void) netdev;
    (void) net_type;
    return false;
}

int net_slirp_read(net_user_options_t *usr)
{
    (void) usr;
    return 0;
}

void slirp_pollfds_fill_socket(Slirp *slirp,
                               uint32_t *timeout,
                               SlirpAddPollSocketCb add_poll,
                               void *opaque)
{
    (void) slirp;
    (void) timeout;
    (void) add_poll;
    (void) opaque;
}

void slirp_pollfds_poll(Slirp *slirp,
                        int select_error,
                        SlirpGetREventsCb get_revents,
                        void *opaque)
{
    (void) slirp;
    (void) select_error;
    (void) get_revents;
    (void) opaque;
}

static void dma_write(guest_paddr_t addr, const void *src, guest_size_t len)
{
    require_bool("dma write", ram_dma_write(&emu.ram_dma, addr, src, len),
                 true);
}

static uint16_t read16(guest_paddr_t addr)
{
    uint16_t value = 0;

    require_bool("dma read16", ram_dma_read(&emu.ram_dma, addr, &value,
                                            sizeof(value)),
                 true);
    return value;
}

static uint32_t read32(guest_paddr_t addr)
{
    uint32_t value = 0;

    require_bool("dma read32", ram_dma_read(&emu.ram_dma, addr, &value,
                                            sizeof(value)),
                 true);
    return value;
}

static void write16(guest_paddr_t addr, uint16_t value)
{
    dma_write(addr, &value, sizeof(value));
}

static void write_desc(guest_paddr_t desc_base,
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

    dma_write(desc_base + (guest_paddr_t) index * sizeof(desc), &desc,
              sizeof(desc));
}

static void mmio_write(uint32_t reg, uint32_t value)
{
    int ret = virtio_mmio_write(&emu.vnet.common, reg, sizeof(uint32_t), value);

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
        virtio_mmio_read(&emu.vnet.common, reg, sizeof(uint32_t), &value), 0);
    return value;
}

static void configure_queue(uint16_t queue,
                            guest_paddr_t desc,
                            guest_paddr_t avail,
                            guest_paddr_t used)
{
    mmio_write(REG(QueueSel), queue);
    mmio_write(REG(QueueNum), 8);
    mmio_write(REG(QueueDescLow), desc);
    mmio_write(REG(QueueDriverLow), avail);
    mmio_write(REG(QueueDeviceLow), used);
    mmio_write(REG(QueueReady), 1);
}

static void configure_net_base(void)
{
    memset(&emu, 0, sizeof(emu));
    memset(&user_net, 0, sizeof(user_net));
    user_net.host_to_guest_channel[0] = -1;
    user_net.host_to_guest_channel[1] = -1;
    user_net.guest_to_host_channel[0] = -1;
    user_net.guest_to_host_channel[1] = -1;
    memset(ram_words, 0, sizeof(ram_words));
    ram_dma_init(&emu.ram_dma, ram_words, TEST_RAM_SIZE, NULL);
    emu.ram = ram_words;
    require_int("lifecycle init", semu_vm_lifecycle_init(&emu.lifecycle), 0);
    require_int("lifecycle running",
                semu_vm_lifecycle_enter_running(&emu.lifecycle), 0);
    require_int("plic lock init", pthread_mutex_init(&emu.plic_lock, NULL), 0);
    wake_count = 0;

    require_bool("net init", virtio_net_init(&emu.vnet, &emu, NULL), true);
    require_u32("device id", mmio_read(REG(DeviceID)), 1);
    require_u32("queue max", mmio_read(REG(QueueNumMax)), 1024);
}

static void configure_net_transport(void)
{
    configure_net_base();

    mmio_write(REG(DeviceFeaturesSel), 1);
    require_u32("VERSION_1 advertised", mmio_read(REG(DeviceFeatures)), 1);
    mmio_write(REG(DriverFeaturesSel), 1);
    mmio_write(REG(DriverFeatures), 1);
    mmio_write(REG(Status), VIRTIO_STATUS__ACKNOWLEDGE);
    mmio_write(REG(Status), VIRTIO_STATUS__DRIVER);
    mmio_write(REG(Status), VIRTIO_STATUS__FEATURES_OK);

    configure_queue(VNET_QUEUE_RX, RX_DESC_ADDR, RX_AVAIL_ADDR, RX_USED_ADDR);
    configure_queue(VNET_QUEUE_TX, TX_DESC_ADDR, TX_AVAIL_ADDR, TX_USED_ADDR);
    mmio_write(REG(Status), VIRTIO_STATUS__DRIVER_OK);
}

static void configure_net(void)
{
    int flags;

    configure_net_base();
    require_int("host pipe", pipe(user_net.host_to_guest_channel), 0);
    require_int("guest pipe", pipe(user_net.guest_to_host_channel), 0);
    flags = fcntl(user_net.host_to_guest_channel[SLIRP_WRITE_SIDE], F_GETFL, 0);
    require_int("host pipe nonblock",
                fcntl(user_net.host_to_guest_channel[SLIRP_WRITE_SIDE],
                      F_SETFL, flags | O_NONBLOCK),
                0);
    emu.vnet.peer.type = NETDEV_IMPL_user;
    emu.vnet.peer.op = &user_net;
    virtio_net_set_queue_fd_ready(&emu.vnet, VNET_QUEUE_TX, true);

    mmio_write(REG(DeviceFeaturesSel), 1);
    require_u32("VERSION_1 advertised", mmio_read(REG(DeviceFeatures)), 1);
    mmio_write(REG(DriverFeaturesSel), 1);
    mmio_write(REG(DriverFeatures), 1);
    mmio_write(REG(Status), VIRTIO_STATUS__ACKNOWLEDGE);
    mmio_write(REG(Status), VIRTIO_STATUS__DRIVER);
    mmio_write(REG(Status), VIRTIO_STATUS__FEATURES_OK);

    configure_queue(VNET_QUEUE_RX, RX_DESC_ADDR, RX_AVAIL_ADDR, RX_USED_ADDR);
    configure_queue(VNET_QUEUE_TX, TX_DESC_ADDR, TX_AVAIL_ADDR, TX_USED_ADDR);
    mmio_write(REG(Status), VIRTIO_STATUS__DRIVER_OK);
}

static void destroy_net_fixture(void)
{
    if (user_net.host_to_guest_channel[0] >= 0)
        close(user_net.host_to_guest_channel[0]);
    if (user_net.host_to_guest_channel[1] >= 0)
        close(user_net.host_to_guest_channel[1]);
    if (user_net.guest_to_host_channel[0] >= 0)
        close(user_net.guest_to_host_channel[0]);
    if (user_net.guest_to_host_channel[1] >= 0)
        close(user_net.guest_to_host_channel[1]);
    virtio_net_destroy(&emu.vnet);
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
        if (read16(TX_USED_ADDR + 2) == idx)
            return true;
        sleep_one_ms();
    }
    return false;
}

static bool wait_for_rx_used_idx(uint16_t idx)
{
    for (unsigned i = 0; i < 1000; i++) {
        if (read16(RX_USED_ADDR + 2) == idx)
            return true;
        sleep_one_ms();
    }
    return false;
}

static ssize_t event_loop_find_fd(const struct semu_event_loop *loop, int fd)
{
    for (size_t i = 0; i < loop->count; i++) {
        if (loop->fds[i] == fd)
            return (ssize_t) i;
    }
    return -1;
}

static void publish_tx_packet(const char *payload)
{
    uint8_t header[VNET_HEADER_LEN] = {0};

    dma_write(HEADER_ADDR, header, sizeof(header));
    dma_write(DATA_ADDR, payload, strlen(payload));
    write_desc(TX_DESC_ADDR, 0, HEADER_ADDR, sizeof(header),
               VIRTIO_DESC_F_NEXT, 1);
    write_desc(TX_DESC_ADDR, 1, DATA_ADDR, (uint32_t) strlen(payload), 0, 0);
    write16(TX_AVAIL_ADDR + 4, 0);
    write16(TX_AVAIL_ADDR + 2, 1);
}

static void publish_rx_buffer(uint32_t len)
{
    write_desc(RX_DESC_ADDR, 0, HEADER_ADDR, len, VIRTIO_DESC_F_WRITE, 0);
    write16(RX_AVAIL_ADDR + 4, 0);
    write16(RX_AVAIL_ADDR + 2, 1);
}

static void fill_tx_pipe_until_eagain(void)
{
    uint8_t bytes[512] = {0};
    int fd = user_net.host_to_guest_channel[SLIRP_WRITE_SIDE];

    for (;;) {
        ssize_t n = write(fd, bytes, sizeof(bytes));

        if (n > 0)
            continue;
        require_bool("fill tx pipe hit EAGAIN",
                     n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK),
                     true);
        return;
    }
}

static void writev_gate_enable(void)
{
    pthread_mutex_lock(&net_writev_gate.lock);
    net_writev_gate.enabled = true;
    net_writev_gate.entered = false;
    net_writev_gate.release = false;
    pthread_mutex_unlock(&net_writev_gate.lock);
}

static bool writev_gate_wait_entered(unsigned timeout_ms)
{
    struct timespec deadline = deadline_after_ms(timeout_ms);
    bool entered;

    pthread_mutex_lock(&net_writev_gate.lock);
    while (!net_writev_gate.entered) {
        int ret = pthread_cond_timedwait(&net_writev_gate.cond,
                                         &net_writev_gate.lock, &deadline);
        if (ret == ETIMEDOUT)
            break;
    }
    entered = net_writev_gate.entered;
    pthread_mutex_unlock(&net_writev_gate.lock);
    return entered;
}

static void writev_gate_release(void)
{
    pthread_mutex_lock(&net_writev_gate.lock);
    net_writev_gate.enabled = false;
    net_writev_gate.release = true;
    pthread_cond_broadcast(&net_writev_gate.cond);
    pthread_mutex_unlock(&net_writev_gate.lock);
}

static void async_net_call_init(struct async_net_call *call,
                                virtio_net_state_t *vnet)
{
    memset(call, 0, sizeof(*call));
    call->vnet = vnet;
    require_int("async lock init", pthread_mutex_init(&call->lock, NULL), 0);
    require_int("async cond init", pthread_cond_init(&call->cond, NULL), 0);
}

static void async_net_call_finish(struct async_net_call *call, int ret)
{
    pthread_mutex_lock(&call->lock);
    call->ret = ret;
    call->done = true;
    pthread_cond_broadcast(&call->cond);
    pthread_mutex_unlock(&call->lock);
}

static bool async_net_call_wait_done(struct async_net_call *call,
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

static void async_net_call_destroy(struct async_net_call *call)
{
    pthread_mutex_destroy(&call->lock);
    pthread_cond_destroy(&call->cond);
}

static void *notify_queue_thread(void *opaque)
{
    struct async_net_call *call = opaque;
    int ret = virtio_mmio_write(&call->vnet->common, REG(QueueNotify), 4,
                                VNET_QUEUE_TX);

    async_net_call_finish(call, ret);
    return NULL;
}

static void *reset_net_thread(void *opaque)
{
    struct async_net_call *call = opaque;
    int ret = virtio_device_common_reset(&call->vnet->common);

    async_net_call_finish(call, ret);
    return NULL;
}

static void async_net_call_start_notify(struct async_net_call *call)
{
    require_int("notify thread create",
                pthread_create(&call->thread, NULL, notify_queue_thread, call),
                0);
}

static void async_net_call_start_reset(struct async_net_call *call)
{
    require_int("reset thread create",
                pthread_create(&call->thread, NULL, reset_net_thread, call), 0);
}

static void async_net_call_join(struct async_net_call *call)
{
    require_int("async join", pthread_join(call->thread, NULL), 0);
}

static void test_init_without_peer_is_transport_safe(void)
{
    configure_net_transport();

    mmio_write(REG(QueueNotify), VNET_QUEUE_RX);
    mmio_write(REG(QueueNotify), VNET_QUEUE_TX);
    virtio_net_refresh_queue(&emu.vnet);

    require_u32("no-peer interrupt remains clear",
                virtio_irq_read_status(&emu.vnet.common.irq), 0);
    require_bool("no-peer irq line remains clear",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VNET), false);
    require_u32("no-peer notify does not request reset",
                virtio_net_status_load(&emu.vnet) &
                    VIRTIO_STATUS__DEVICE_NEEDS_RESET,
                0);
}

static void test_queue_notify_returns_before_net_backend_work(void)
{
    struct async_net_call notify;

    configure_net();
    publish_tx_packet("abc");
    writev_gate_enable();
    async_net_call_init(&notify, &emu.vnet);
    async_net_call_start_notify(&notify);

    require_bool("QueueNotify returned before backend write completed",
                 async_net_call_wait_done(&notify, 100), true);
    require_int("QueueNotify return", notify.ret, 0);
    require_bool("actor eventually reached backend write",
                 writev_gate_wait_entered(1000), true);
    require_u16("used idx not published while backend blocked",
                read16(TX_USED_ADDR + 2), 0);

    writev_gate_release();
    async_net_call_join(&notify);
    require_bool("actor published used completion", wait_for_used_idx(1),
                 true);
    require_u32("used id", read32(TX_USED_ADDR + 4), 0);
    require_u32("used len", read32(TX_USED_ADDR + 8), 0);
    require_u32("irq status", mmio_read(REG(InterruptStatus)),
                VIRTIO_INT__USED_RING);
    require_bool("irq pending helper", virtio_net_irq_pending(&emu.vnet),
                 true);

    async_net_call_destroy(&notify);
}

static void test_reset_cancels_stale_net_completion(void)
{
    struct async_net_call notify;
    struct async_net_call reset;

    configure_net();
    publish_tx_packet("reset");
    writev_gate_enable();
    async_net_call_init(&notify, &emu.vnet);
    async_net_call_init(&reset, &emu.vnet);
    async_net_call_start_notify(&notify);

    require_bool("actor entered backend write", writev_gate_wait_entered(1000),
                 true);
    async_net_call_start_reset(&reset);
    require_bool("reset waits for in-flight actor backend mutation",
                 async_net_call_wait_done(&reset, 50), false);

    writev_gate_release();
    require_bool("reset completed after backend release",
                 async_net_call_wait_done(&reset, 1000), true);
    require_int("reset return", reset.ret, 0);
    require_u16("reset stale used idx remains clear", read16(TX_USED_ADDR + 2),
                0);
    require_u32("reset stale interrupt remains clear",
                virtio_irq_read_status(&emu.vnet.common.irq), 0);
    require_bool("reset stale irq line remains clear",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VNET), false);

    async_net_call_join(&notify);
    async_net_call_join(&reset);
    async_net_call_destroy(&notify);
    async_net_call_destroy(&reset);
}

static void test_common_reset_start_cancels_stale_net_avail_failure(void)
{
    int ret;

    configure_net();
    write16(TX_AVAIL_ADDR + 2, 9);

    reset_start_on_avail_read_common = &emu.vnet.common;
    reset_start_on_avail_read_addr = TX_AVAIL_ADDR + 2;
    ret =
        virtio_net_actor_drain_queue(&emu.vnet, &emu.vnet.actor, VNET_QUEUE_TX,
                                     virtio_actor_generation(&emu.vnet.actor));

    require_int("common reset stale avail drain return", ret, 0);
    require_bool("common reset stale avail hook consumed",
                 reset_start_on_avail_read_common == NULL, true);
    require_u32("common reset stale avail interrupt remains clear",
                virtio_irq_read_status(&emu.vnet.common.irq), 0);
    require_bool("common reset stale avail irq line remains clear",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VNET), false);
    require_u32(
        "common reset stale avail needs-reset remains clear",
        virtio_net_status_load(&emu.vnet) & VIRTIO_STATUS__DEVICE_NEEDS_RESET,
        0);
}

static void test_net_event_sync_avoids_ready_tx_writable_subscription(void)
{
    struct semu_event_loop loop;

    configure_net();
    require_int("event loop init", semu_event_loop_init(&loop, "net-test"), 0);
    require_int("net event sync",
                virtio_net_event_sync(&emu.vnet, &loop,
                                      SEMU_EVENT_TOKEN_FIRST_DEVICE),
                0);

    require_bool("rx internal read fd registered",
                 event_loop_find_fd(
                     &loop,
                     user_net.guest_to_host_channel[SLIRP_READ_SIDE]) >= 0,
                 true);
    require_bool("slirp input read fd registered",
                 event_loop_find_fd(
                     &loop,
                     user_net.host_to_guest_channel[SLIRP_READ_SIDE]) >= 0,
                 true);
    require_bool("ready tx write fd not registered",
                 event_loop_find_fd(
                     &loop,
                     user_net.host_to_guest_channel[SLIRP_WRITE_SIDE]) < 0,
                 true);

    semu_event_loop_destroy(&loop);
}

static void test_user_internal_rx_event_drives_rx_actor_work(void)
{
    struct semu_event_loop loop;
    struct semu_event event = {0};
    const char payload[] = "rxpkt";

    configure_net();
    publish_rx_buffer(VNET_HEADER_LEN + sizeof(payload) - 1);
    require_int("event loop init", semu_event_loop_init(&loop, "net-test"), 0);
    require_int("net event sync",
                virtio_net_event_sync(&emu.vnet, &loop,
                                      SEMU_EVENT_TOKEN_FIRST_DEVICE),
                0);
    require_int("write rx packet",
                (int) write(user_net.guest_to_host_channel[SLIRP_WRITE_SIDE],
                            payload, sizeof(payload) - 1),
                (int) sizeof(payload) - 1);
    require_int("wait rx event", semu_event_wait(&loop, &event, 1, 100), 1);

    require_bool("net rx event handled",
                 virtio_net_event_handle(&emu.vnet, &event,
                                         SEMU_EVENT_TOKEN_FIRST_DEVICE),
                 true);
    require_bool("rx actor published used completion", wait_for_rx_used_idx(1),
                 true);
    require_u32("rx used id", read32(RX_USED_ADDR + 4), 0);
    require_u32("rx used len", read32(RX_USED_ADDR + 8),
                VNET_HEADER_LEN + sizeof(payload) - 1);

    semu_event_loop_destroy(&loop);
}

static void test_tx_eagain_event_restores_tx_readiness(void)
{
    struct semu_event_loop loop;
    struct semu_event event = {0};
    struct iovec iov = {
        .iov_base = (void *) "x",
        .iov_len = 1,
    };
    ssize_t written = 0;
    ssize_t tx_index;

    configure_net();
    fill_tx_pipe_until_eagain();
    require_int("host write EAGAIN",
                virtio_net_host_write(&emu.vnet, &iov, 1, &written), -EAGAIN);
    require_bool("tx readiness cleared after EAGAIN",
                 virtio_net_queue_fd_ready(&emu.vnet, VNET_QUEUE_TX), false);

    require_int("event loop init", semu_event_loop_init(&loop, "net-test"), 0);
    require_int("net event sync",
                virtio_net_event_sync(&emu.vnet, &loop,
                                      SEMU_EVENT_TOKEN_FIRST_DEVICE),
                0);
    tx_index =
        event_loop_find_fd(&loop,
                           user_net.host_to_guest_channel[SLIRP_WRITE_SIDE]);
    require_bool("tx writable fd registered after EAGAIN", tx_index >= 0,
                 true);

    event.token = loop.tokens[tx_index];
    event.events = SEMU_EVENT_WRITABLE;
    require_bool("tx writable event handled",
                 virtio_net_event_handle(&emu.vnet, &event,
                                         SEMU_EVENT_TOKEN_FIRST_DEVICE),
                 true);
    require_bool("tx readiness restored",
                 virtio_net_queue_fd_ready(&emu.vnet, VNET_QUEUE_TX), true);

    semu_event_loop_destroy(&loop);
}

static void test_net_reset_resync_does_not_restore_fd_readiness(void)
{
    struct semu_event_loop loop;
    struct semu_event event = {0};

    configure_net();
    require_int("event loop init", semu_event_loop_init(&loop, "net-test"), 0);
    require_int("net event sync",
                virtio_net_event_sync(&emu.vnet, &loop,
                                      SEMU_EVENT_TOKEN_FIRST_DEVICE),
                0);
    virtio_net_set_queue_fd_ready(&emu.vnet, VNET_QUEUE_RX, true);
    virtio_net_set_queue_fd_ready(&emu.vnet, VNET_QUEUE_TX, true);

    require_int("net reset", virtio_net_reset(&emu.vnet, 1, 2), 0);
    require_bool("reset clears rx readiness",
                 virtio_net_queue_fd_ready(&emu.vnet, VNET_QUEUE_RX), false);
    require_bool("reset clears tx readiness",
                 virtio_net_queue_fd_ready(&emu.vnet, VNET_QUEUE_TX), false);

    require_int("net event resync",
                virtio_net_event_sync(&emu.vnet, &loop,
                                      SEMU_EVENT_TOKEN_FIRST_DEVICE),
                0);
    require_bool("resync does not restore rx readiness",
                 virtio_net_queue_fd_ready(&emu.vnet, VNET_QUEUE_RX), false);
    require_bool("resync does not restore tx readiness",
                 virtio_net_queue_fd_ready(&emu.vnet, VNET_QUEUE_TX), false);

    ssize_t rx_index =
        event_loop_find_fd(&loop,
                           user_net.guest_to_host_channel[SLIRP_READ_SIDE]);
    require_bool("rx fd still registered", rx_index >= 0, true);
    event.token = loop.tokens[rx_index];
    event.events = SEMU_EVENT_READABLE;
    require_bool("rx readiness event handled",
                 virtio_net_event_handle(&emu.vnet, &event,
                                         SEMU_EVENT_TOKEN_FIRST_DEVICE),
                 true);
    require_bool("event restores rx readiness",
                 virtio_net_queue_fd_ready(&emu.vnet, VNET_QUEUE_RX), true);
    require_bool("rx event does not restore tx readiness",
                 virtio_net_queue_fd_ready(&emu.vnet, VNET_QUEUE_TX), false);

    semu_event_loop_destroy(&loop);
}

int main(void)
{
    test_init_without_peer_is_transport_safe();
    destroy_net_fixture();

    test_queue_notify_returns_before_net_backend_work();
    destroy_net_fixture();

    test_reset_cancels_stale_net_completion();
    destroy_net_fixture();

    test_common_reset_start_cancels_stale_net_avail_failure();
    destroy_net_fixture();

    test_net_event_sync_avoids_ready_tx_writable_subscription();
    destroy_net_fixture();

    test_user_internal_rx_event_drives_rx_actor_work();
    destroy_net_fixture();

    test_tx_eagain_event_restores_tx_readiness();
    destroy_net_fixture();

    test_net_reset_resync_does_not_restore_fd_readiness();
    destroy_net_fixture();

    return 0;
}
