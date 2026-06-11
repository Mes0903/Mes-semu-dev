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

#define FAKE_SLIRP_MAX_DYNAMIC 8
#define TEST_VNET_DYNAMIC_TOKEN(base) ((base) + 4U)

struct fake_slirp_dynamic_socket {
    int fd;
    int events;
};

static struct fake_slirp_dynamic_socket
    fake_slirp_dynamic[FAKE_SLIRP_MAX_DYNAMIC];
static size_t fake_slirp_dynamic_count;
static int fake_slirp_fill_calls;
static int fake_slirp_poll_calls;
static int fake_slirp_last_select_error;
static int fake_slirp_last_revents[FAKE_SLIRP_MAX_DYNAMIC];
static size_t fake_slirp_last_revents_count;

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

struct dma_write_gate {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    guest_paddr_t addr;
    guest_size_t len;
    bool enabled;
    bool entered;
    bool release;
};

static struct dma_write_gate net_dma_write_gate = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

struct async_net_call {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    virtio_net_state_t *vnet;
    uint16_t queue_index;
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
    pthread_mutex_lock(&net_dma_write_gate.lock);
    if (net_dma_write_gate.enabled && addr == net_dma_write_gate.addr &&
        len == net_dma_write_gate.len) {
        net_dma_write_gate.entered = true;
        pthread_cond_broadcast(&net_dma_write_gate.cond);
        while (!net_dma_write_gate.release)
            pthread_cond_wait(&net_dma_write_gate.cond,
                              &net_dma_write_gate.lock);
    }
    pthread_mutex_unlock(&net_dma_write_gate.lock);

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

static short fake_slirp_to_poll_events(int events)
{
    short ret = 0;

    if (events & SLIRP_POLL_IN)
        ret |= POLLIN;
    if (events & SLIRP_POLL_OUT)
        ret |= POLLOUT;
    if (events & SLIRP_POLL_PRI)
        ret |= POLLPRI;
    if (events & SLIRP_POLL_ERR)
        ret |= POLLERR;
    if (events & SLIRP_POLL_HUP)
        ret |= POLLHUP;
    return ret;
}

static int fake_poll_to_slirp_events(short events)
{
    int ret = 0;

    if (events & POLLIN)
        ret |= SLIRP_POLL_IN;
    if (events & POLLOUT)
        ret |= SLIRP_POLL_OUT;
    if (events & POLLPRI)
        ret |= SLIRP_POLL_PRI;
    if (events & POLLERR)
        ret |= SLIRP_POLL_ERR;
    if (events & POLLHUP)
        ret |= SLIRP_POLL_HUP;
    return ret;
}

int semu_slirp_add_poll_socket(slirp_os_socket fd, int events, void *opaque)
{
    net_user_options_t *usr = opaque;

    if (usr->pfd_len >= usr->pfd_size)
        return -1;

    int idx = usr->pfd_len++;
    usr->pfd[idx].fd = (int) fd;
    usr->pfd[idx].events = fake_slirp_to_poll_events(events);
    usr->pfd[idx].revents = 0;
    return idx;
}

int semu_slirp_get_revents(int idx, void *opaque)
{
    net_user_options_t *usr = opaque;

    return fake_poll_to_slirp_events(usr->pfd[idx].revents);
}

void slirp_pollfds_fill_socket(Slirp *slirp,
                               uint32_t *timeout,
                               SlirpAddPollSocketCb add_poll,
                               void *opaque)
{
    (void) slirp;

    fake_slirp_fill_calls++;
    if (timeout)
        *timeout = 0;
    for (size_t i = 0; i < fake_slirp_dynamic_count; i++)
        (void) add_poll((slirp_os_socket) fake_slirp_dynamic[i].fd,
                        fake_slirp_dynamic[i].events, opaque);
}

void slirp_pollfds_poll(Slirp *slirp,
                        int select_error,
                        SlirpGetREventsCb get_revents,
                        void *opaque)
{
    net_user_options_t *usr = opaque;

    (void) slirp;

    fake_slirp_poll_calls++;
    fake_slirp_last_select_error = select_error;
    fake_slirp_last_revents_count = 0;
    if (select_error || !usr || !get_revents)
        return;

    for (int i = 2; i < usr->pfd_len &&
                    fake_slirp_last_revents_count < FAKE_SLIRP_MAX_DYNAMIC;
         i++) {
        fake_slirp_last_revents[fake_slirp_last_revents_count++] =
            get_revents(i, opaque);
    }
}

static void fake_slirp_reset(void)
{
    memset(fake_slirp_dynamic, 0, sizeof(fake_slirp_dynamic));
    fake_slirp_dynamic_count = 0;
    fake_slirp_fill_calls = 0;
    fake_slirp_poll_calls = 0;
    fake_slirp_last_select_error = 0;
    memset(fake_slirp_last_revents, 0, sizeof(fake_slirp_last_revents));
    fake_slirp_last_revents_count = 0;
}

static void fake_slirp_set_dynamic(size_t index, int fd, int events)
{
    require_bool("fake slirp dynamic index valid",
                 index < FAKE_SLIRP_MAX_DYNAMIC, true);
    fake_slirp_dynamic[index].fd = fd;
    fake_slirp_dynamic[index].events = events;
    if (fake_slirp_dynamic_count <= index)
        fake_slirp_dynamic_count = index + 1;
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
    fake_slirp_reset();
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

static void configure_fake_slirp(void)
{
    user_net.slirp = (Slirp *) (void *) &user_net;
    user_net.pfd_size = FAKE_SLIRP_MAX_DYNAMIC + 2;
    user_net.pfd = calloc((size_t) user_net.pfd_size, sizeof(*user_net.pfd));
    require_bool("fake slirp pfd allocation", user_net.pfd != NULL, true);
    user_net.pfd_len = 2;
    user_net.pfd[0].fd = user_net.guest_to_host_channel[SLIRP_READ_SIDE];
    user_net.pfd[0].events = POLLIN | POLLHUP;
    user_net.pfd[1].fd = user_net.host_to_guest_channel[SLIRP_READ_SIDE];
    user_net.pfd[1].events = POLLIN | POLLHUP;
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
    free(user_net.pfd);
    user_net.pfd = NULL;
    user_net.pfd_len = 0;
    user_net.pfd_size = 0;
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

static bool wait_for_net_queue_fd_ready(uint16_t queue, bool want)
{
    for (unsigned i = 0; i < 1000; i++) {
        if (virtio_net_queue_fd_ready(&emu.vnet, queue) == want)
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

static void publish_tx_bytes(const uint8_t *payload, size_t payload_len)
{
    uint8_t header[VNET_V1_HEADER_LEN] = {0};
    unsigned header_len = virtio_net_header_len(&emu.vnet);

    require_bool("test tx header len fits", header_len <= sizeof(header), true);
    dma_write(HEADER_ADDR, header, header_len);
    dma_write(DATA_ADDR, payload, payload_len);
    write_desc(TX_DESC_ADDR, 0, HEADER_ADDR, header_len, VIRTIO_DESC_F_NEXT,
               1);
    write_desc(TX_DESC_ADDR, 1, DATA_ADDR, (uint32_t) payload_len, 0, 0);
    write16(TX_AVAIL_ADDR + 4, 0);
    write16(TX_AVAIL_ADDR + 2, 1);
}

static void publish_tx_packet(const char *payload)
{
    publish_tx_bytes((const uint8_t *) payload, strlen(payload));
}

static void publish_rx_buffer(uint32_t len)
{
    write_desc(RX_DESC_ADDR, 0, HEADER_ADDR, len, VIRTIO_DESC_F_WRITE, 0);
    write16(RX_AVAIL_ADDR + 4, 0);
    write16(RX_AVAIL_ADDR + 2, 1);
}

static void publish_rx_packet_buffer(uint32_t packet_len)
{
    uint32_t header_len = virtio_net_header_len(&emu.vnet);

    write_desc(RX_DESC_ADDR, 0, HEADER_ADDR, header_len,
               VIRTIO_DESC_F_WRITE | VIRTIO_DESC_F_NEXT, 1);
    write_desc(RX_DESC_ADDR, 1, DATA_ADDR, packet_len, VIRTIO_DESC_F_WRITE, 0);
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

static void dma_write_gate_enable(guest_paddr_t addr, guest_size_t len)
{
    pthread_mutex_lock(&net_dma_write_gate.lock);
    net_dma_write_gate.addr = addr;
    net_dma_write_gate.len = len;
    net_dma_write_gate.enabled = true;
    net_dma_write_gate.entered = false;
    net_dma_write_gate.release = false;
    pthread_mutex_unlock(&net_dma_write_gate.lock);
}

static bool dma_write_gate_wait_entered(unsigned timeout_ms)
{
    struct timespec deadline = deadline_after_ms(timeout_ms);
    bool entered;

    pthread_mutex_lock(&net_dma_write_gate.lock);
    while (!net_dma_write_gate.entered) {
        int ret = pthread_cond_timedwait(&net_dma_write_gate.cond,
                                         &net_dma_write_gate.lock, &deadline);
        if (ret == ETIMEDOUT)
            break;
    }
    entered = net_dma_write_gate.entered;
    pthread_mutex_unlock(&net_dma_write_gate.lock);
    return entered;
}

static void dma_write_gate_release(void)
{
    pthread_mutex_lock(&net_dma_write_gate.lock);
    net_dma_write_gate.enabled = false;
    net_dma_write_gate.release = true;
    pthread_cond_broadcast(&net_dma_write_gate.cond);
    pthread_mutex_unlock(&net_dma_write_gate.lock);
}

static void async_net_call_init(struct async_net_call *call,
                                virtio_net_state_t *vnet)
{
    memset(call, 0, sizeof(*call));
    call->vnet = vnet;
    call->queue_index = VNET_QUEUE_TX;
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
                                call->queue_index);

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

static void *stop_net_thread(void *opaque)
{
    struct async_net_call *call = opaque;
    int ret = virtio_actor_stop(&call->vnet->actor);

    async_net_call_finish(call, ret);
    return NULL;
}

static void async_net_call_start_notify(struct async_net_call *call)
{
    require_int("notify thread create",
                pthread_create(&call->thread, NULL, notify_queue_thread, call),
                0);
}

static void async_net_call_start_notify_queue(struct async_net_call *call,
                                              uint16_t queue_index)
{
    call->queue_index = queue_index;
    async_net_call_start_notify(call);
}

static void async_net_call_start_reset(struct async_net_call *call)
{
    require_int("reset thread create",
                pthread_create(&call->thread, NULL, reset_net_thread, call), 0);
}

static void async_net_call_start_stop(struct async_net_call *call)
{
    require_int("stop thread create",
                pthread_create(&call->thread, NULL, stop_net_thread, call), 0);
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

static void test_v1_tx_header_preserves_ethernet_frame(void)
{
    static const uint8_t arp_request[] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x16, 0x55, 0xe3, 0x36, 0x31, 0x9b,
        0x08, 0x06,
        0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01,
        0x16, 0x55, 0xe3, 0x36, 0x31, 0x9b,
        0x0a, 0x00, 0x02, 0x0f,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0a, 0x00, 0x02, 0x02,
    };
    uint8_t observed[sizeof(arp_request)] = {0};
    ssize_t got;
    int ret;

    configure_net();
    require_int("v1 negotiated header len",
                (int) virtio_net_header_len(&emu.vnet), VNET_V1_HEADER_LEN);
    publish_tx_bytes(arp_request, sizeof(arp_request));

    ret = virtio_net_actor_drain_queue(&emu.vnet, &emu.vnet.actor, VNET_QUEUE_TX,
                                       virtio_actor_generation(&emu.vnet.actor));
    require_int("v1 tx drain return", ret, 0);
    require_bool("v1 tx used published", wait_for_used_idx(1), true);

    got = read(user_net.host_to_guest_channel[SLIRP_READ_SIDE], observed,
               sizeof(observed));
    require_int("v1 tx observed len", (int) got, (int) sizeof(arp_request));
    require_int("v1 tx ethernet frame preserved",
                memcmp(observed, arp_request, sizeof(arp_request)), 0);
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

static void test_stop_cancels_stale_net_completion(void)
{
    struct async_net_call notify;
    struct async_net_call stop;

    configure_net();
    publish_tx_packet("stop");
    writev_gate_enable();
    async_net_call_init(&notify, &emu.vnet);
    async_net_call_init(&stop, &emu.vnet);
    async_net_call_start_notify(&notify);

    require_bool("actor entered backend write before stop",
                 writev_gate_wait_entered(1000), true);
    async_net_call_start_stop(&stop);
    require_bool("stop waits for in-flight actor backend mutation",
                 async_net_call_wait_done(&stop, 50), false);

    writev_gate_release();
    require_bool("stop completed after backend release",
                 async_net_call_wait_done(&stop, 1000), true);
    require_int("stop return", stop.ret, 0);
    require_u16("stop stale used idx remains clear", read16(TX_USED_ADDR + 2),
                0);
    require_u32("stop stale interrupt remains clear",
                virtio_irq_read_status(&emu.vnet.common.irq), 0);
    require_bool("stop stale irq line remains clear",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VNET), false);

    async_net_call_join(&notify);
    async_net_call_join(&stop);
    async_net_call_destroy(&notify);
    async_net_call_destroy(&stop);
}

static void test_reset_cancels_stale_net_rx_completion(void)
{
    struct async_net_call notify;
    struct async_net_call reset;
    const char payload[] = "rxreset";
    uint32_t packet_len = sizeof(payload) - 1;

    configure_net();
    publish_rx_packet_buffer(packet_len);
    require_int("write rx reset packet",
                (int) write(user_net.guest_to_host_channel[SLIRP_WRITE_SIDE],
                            payload, packet_len),
                (int) packet_len);
    virtio_net_set_queue_fd_ready(&emu.vnet, VNET_QUEUE_RX, true);

    dma_write_gate_enable(DATA_ADDR, packet_len);
    async_net_call_init(&notify, &emu.vnet);
    async_net_call_init(&reset, &emu.vnet);
    async_net_call_start_notify_queue(&notify, VNET_QUEUE_RX);

    require_bool("actor entered rx guest packet write",
                 dma_write_gate_wait_entered(1000), true);
    async_net_call_start_reset(&reset);
    require_bool("reset waits for in-flight rx actor mutation",
                 async_net_call_wait_done(&reset, 50), false);

    dma_write_gate_release();
    require_bool("reset completed after rx guest write release",
                 async_net_call_wait_done(&reset, 1000), true);
    require_int("rx reset return", reset.ret, 0);
    require_u16("rx reset stale used idx remains clear",
                read16(RX_USED_ADDR + 2), 0);
    require_u32("rx reset stale interrupt remains clear",
                virtio_irq_read_status(&emu.vnet.common.irq), 0);
    require_bool("rx reset stale irq line remains clear",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VNET), false);

    async_net_call_join(&notify);
    async_net_call_join(&reset);
    async_net_call_destroy(&notify);
    async_net_call_destroy(&reset);
}

static void test_stop_cancels_stale_net_rx_completion(void)
{
    struct async_net_call notify;
    struct async_net_call stop;
    const char payload[] = "rxstop";
    uint32_t packet_len = sizeof(payload) - 1;

    configure_net();
    publish_rx_packet_buffer(packet_len);
    require_int("write rx stop packet",
                (int) write(user_net.guest_to_host_channel[SLIRP_WRITE_SIDE],
                            payload, packet_len),
                (int) packet_len);
    virtio_net_set_queue_fd_ready(&emu.vnet, VNET_QUEUE_RX, true);

    dma_write_gate_enable(DATA_ADDR, packet_len);
    async_net_call_init(&notify, &emu.vnet);
    async_net_call_init(&stop, &emu.vnet);
    async_net_call_start_notify_queue(&notify, VNET_QUEUE_RX);

    require_bool("actor entered rx guest packet write before stop",
                 dma_write_gate_wait_entered(1000), true);
    async_net_call_start_stop(&stop);
    require_bool("stop waits for in-flight rx actor mutation",
                 async_net_call_wait_done(&stop, 50), false);

    dma_write_gate_release();
    require_bool("stop completed after rx guest write release",
                 async_net_call_wait_done(&stop, 1000), true);
    require_int("rx stop return", stop.ret, 0);
    require_u16("rx stop stale used idx remains clear",
                read16(RX_USED_ADDR + 2), 0);
    require_u32("rx stop stale interrupt remains clear",
                virtio_irq_read_status(&emu.vnet.common.irq), 0);
    require_bool("rx stop stale irq line remains clear",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VNET), false);

    async_net_call_join(&notify);
    async_net_call_join(&stop);
    async_net_call_destroy(&notify);
    async_net_call_destroy(&stop);
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

static void test_user_dynamic_slirp_fds_register_with_event_masks(void)
{
    struct semu_event_loop loop;
    int dynamic_in[2] = {-1, -1};
    int dynamic_out[2] = {-1, -1};
    ssize_t in_index;
    ssize_t out_index;

    configure_net();
    configure_fake_slirp();
    require_int("dynamic in pipe", pipe(dynamic_in), 0);
    require_int("dynamic out pipe", pipe(dynamic_out), 0);
    fake_slirp_set_dynamic(
        0, dynamic_in[0],
        SLIRP_POLL_IN | SLIRP_POLL_HUP | SLIRP_POLL_ERR);
    fake_slirp_set_dynamic(1, dynamic_out[1],
                           SLIRP_POLL_OUT | SLIRP_POLL_ERR);

    require_int("event loop init", semu_event_loop_init(&loop, "net-test"), 0);
    require_int("net event sync",
                virtio_net_event_sync(&emu.vnet, &loop,
                                      SEMU_EVENT_TOKEN_FIRST_DEVICE),
                0);

    in_index = event_loop_find_fd(&loop, dynamic_in[0]);
    out_index = event_loop_find_fd(&loop, dynamic_out[1]);
    require_bool("dynamic readable fd registered", in_index >= 0, true);
    require_bool("dynamic writable fd registered", out_index >= 0, true);
    require_u32("dynamic readable token", loop.tokens[in_index],
                TEST_VNET_DYNAMIC_TOKEN(SEMU_EVENT_TOKEN_FIRST_DEVICE));
    require_u32("dynamic writable token", loop.tokens[out_index],
                TEST_VNET_DYNAMIC_TOKEN(SEMU_EVENT_TOKEN_FIRST_DEVICE));
    require_u32("dynamic readable mask", loop.events[in_index],
                SEMU_EVENT_READABLE | SEMU_EVENT_ERROR);
    require_u32("dynamic writable mask", loop.events[out_index],
                SEMU_EVENT_WRITABLE | SEMU_EVENT_ERROR);
    require_int("slirp fill called during sync", fake_slirp_fill_calls, 1);

    semu_event_loop_destroy(&loop);
    close(dynamic_in[0]);
    close(dynamic_in[1]);
    close(dynamic_out[0]);
    close(dynamic_out[1]);
}

static void test_user_dynamic_slirp_resync_removes_stale_and_unregisters(void)
{
    struct semu_event_loop loop;
    int stale[2] = {-1, -1};
    int kept[2] = {-1, -1};
    int stale_read_fd;

    configure_net();
    configure_fake_slirp();
    require_int("stale dynamic pipe", pipe(stale), 0);
    require_int("kept dynamic pipe", pipe(kept), 0);
    fake_slirp_set_dynamic(
        0, stale[0],
        SLIRP_POLL_IN | SLIRP_POLL_HUP | SLIRP_POLL_ERR);
    fake_slirp_set_dynamic(
        1, kept[0],
        SLIRP_POLL_IN | SLIRP_POLL_HUP | SLIRP_POLL_ERR);

    require_int("event loop init", semu_event_loop_init(&loop, "net-test"), 0);
    require_int("net event sync",
                virtio_net_event_sync(&emu.vnet, &loop,
                                      SEMU_EVENT_TOKEN_FIRST_DEVICE),
                0);
    require_bool("stale dynamic fd initially registered",
                 event_loop_find_fd(&loop, stale[0]) >= 0, true);
    require_bool("kept dynamic fd initially registered",
                 event_loop_find_fd(&loop, kept[0]) >= 0, true);

    stale_read_fd = stale[0];
    close(stale[0]);
    stale[0] = -1;
    fake_slirp_dynamic_count = 0;
    fake_slirp_set_dynamic(
        0, kept[0],
        SLIRP_POLL_IN | SLIRP_POLL_HUP | SLIRP_POLL_ERR);
    require_int("net event resync",
                virtio_net_event_sync(&emu.vnet, &loop,
                                      SEMU_EVENT_TOKEN_FIRST_DEVICE),
                0);
    require_bool("closed stale dynamic fd removed",
                 event_loop_find_fd(&loop, stale_read_fd) < 0, true);
    require_bool("kept dynamic fd remains registered",
                 event_loop_find_fd(&loop, kept[0]) >= 0, true);

    virtio_net_event_unregister(&emu.vnet, &loop,
                                SEMU_EVENT_TOKEN_FIRST_DEVICE);
    require_bool("user rx stable fd unregistered",
                 event_loop_find_fd(
                     &loop,
                     user_net.guest_to_host_channel[SLIRP_READ_SIDE]) < 0,
                 true);
    require_bool("slirp stable fd unregistered",
                 event_loop_find_fd(
                     &loop,
                     user_net.host_to_guest_channel[SLIRP_READ_SIDE]) < 0,
                 true);
    require_bool("dynamic fd unregistered",
                 event_loop_find_fd(&loop, kept[0]) < 0, true);

    semu_event_loop_destroy(&loop);
    close(stale[1]);
    close(kept[0]);
    close(kept[1]);
}

static void test_user_dynamic_slirp_resync_removes_closed_current_fd(void)
{
    struct semu_event_loop loop;
    int dynamic[2] = {-1, -1};
    int dynamic_read_fd;

    configure_net();
    configure_fake_slirp();
    require_int("dynamic pipe", pipe(dynamic), 0);
    fake_slirp_set_dynamic(
        0, dynamic[0],
        SLIRP_POLL_IN | SLIRP_POLL_HUP | SLIRP_POLL_ERR);

    require_int("event loop init", semu_event_loop_init(&loop, "net-test"), 0);
    require_int("net event sync",
                virtio_net_event_sync(&emu.vnet, &loop,
                                      SEMU_EVENT_TOKEN_FIRST_DEVICE),
                0);
    require_bool("dynamic fd initially registered",
                 event_loop_find_fd(&loop, dynamic[0]) >= 0, true);

    dynamic_read_fd = dynamic[0];
    close(dynamic[0]);
    dynamic[0] = -1;
    require_int("net event resync after current dynamic close",
                virtio_net_event_sync(&emu.vnet, &loop,
                                      SEMU_EVENT_TOKEN_FIRST_DEVICE),
                0);
    require_bool("closed current dynamic fd removed",
                 event_loop_find_fd(&loop, dynamic_read_fd) < 0, true);

    semu_event_loop_destroy(&loop);
    close(dynamic[1]);
}

static void test_user_dynamic_slirp_capacity_falls_back_without_failing(void)
{
    struct semu_event_loop loop;
    int dynamic[2] = {-1, -1};
    int dummy[SEMU_EVENT_LOOP_MAX_FDS][2];
    size_t dummy_count = 0;

    for (size_t i = 0; i < SEMU_EVENT_LOOP_MAX_FDS; i++) {
        dummy[i][0] = -1;
        dummy[i][1] = -1;
    }

    configure_net();
    configure_fake_slirp();
    require_int("event loop init", semu_event_loop_init(&loop, "net-test"), 0);
    require_int("initial net event sync",
                virtio_net_event_sync(&emu.vnet, &loop,
                                      SEMU_EVENT_TOKEN_FIRST_DEVICE),
                0);

    while (loop.count < SEMU_EVENT_LOOP_MAX_FDS) {
        require_bool("dummy capacity bound", dummy_count < SEMU_EVENT_LOOP_MAX_FDS,
                     true);
        require_int("dummy pipe", pipe(dummy[dummy_count]), 0);
        require_int("dummy add fd",
                    semu_event_add_fd(
                        &loop, dummy[dummy_count][0],
                        1000U + (semu_event_token_t) dummy_count,
                        SEMU_EVENT_READABLE),
                    0);
        dummy_count++;
    }

    require_int("dynamic pipe", pipe(dynamic), 0);
    fake_slirp_set_dynamic(
        0, dynamic[0],
        SLIRP_POLL_IN | SLIRP_POLL_HUP | SLIRP_POLL_ERR);
    require_int("net event sync with full loop",
                virtio_net_event_sync(&emu.vnet, &loop,
                                      SEMU_EVENT_TOKEN_FIRST_DEVICE),
                0);
    require_bool("dynamic fd not registered when loop full",
                 event_loop_find_fd(&loop, dynamic[0]) < 0, true);

    semu_event_loop_destroy(&loop);
    close(dynamic[0]);
    close(dynamic[1]);
    for (size_t i = 0; i < dummy_count; i++) {
        close(dummy[i][0]);
        close(dummy[i][1]);
    }
}

static void test_user_dynamic_slirp_event_pumps_revents(void)
{
    struct semu_event_loop loop;
    struct semu_event event = {0};
    int dynamic[2] = {-1, -1};

    configure_net();
    configure_fake_slirp();
    require_int("dynamic pipe", pipe(dynamic), 0);
    fake_slirp_set_dynamic(
        0, dynamic[0],
        SLIRP_POLL_IN | SLIRP_POLL_HUP | SLIRP_POLL_ERR);

    require_int("event loop init", semu_event_loop_init(&loop, "net-test"), 0);
    require_int("net event sync",
                virtio_net_event_sync(&emu.vnet, &loop,
                                      SEMU_EVENT_TOKEN_FIRST_DEVICE),
                0);
    require_int("write dynamic byte", (int) write(dynamic[1], "x", 1), 1);
    require_int("wait dynamic event", semu_event_wait(&loop, &event, 1, 100),
                1);
    require_u32("dynamic event token", event.token,
                TEST_VNET_DYNAMIC_TOKEN(SEMU_EVENT_TOKEN_FIRST_DEVICE));
    require_bool("dynamic event handled",
                 virtio_net_event_handle(&emu.vnet, &event,
                                         SEMU_EVENT_TOKEN_FIRST_DEVICE),
                 true);
    require_int("dynamic event pumped slirp", fake_slirp_poll_calls, 1);
    require_int("dynamic event select_error", fake_slirp_last_select_error, 0);
    require_int("dynamic event revents count",
                (int) fake_slirp_last_revents_count, 1);
    require_bool("dynamic event reported readable revents",
                 (fake_slirp_last_revents[0] & SLIRP_POLL_IN) != 0, true);

    semu_event_loop_destroy(&loop);
    close(dynamic[0]);
    close(dynamic[1]);
}

static void test_user_internal_rx_event_drives_rx_actor_work(void)
{
    struct semu_event_loop loop;
    struct semu_event event = {0};
    const char payload[] = "rxpkt";

    configure_net();
    unsigned header_len = virtio_net_header_len(&emu.vnet);

    publish_rx_buffer(header_len + sizeof(payload) - 1);
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
                header_len + sizeof(payload) - 1);

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

static void test_rx_eagain_event_restores_rx_readiness_and_preserves_avail(
    void)
{
    struct semu_event_loop loop;
    struct semu_event event = {0};
    const char payload[] = "rx-eagain";
    unsigned header_len;
    ssize_t rx_index;
    int flags;

    configure_net();
    header_len = virtio_net_header_len(&emu.vnet);
    flags = fcntl(user_net.guest_to_host_channel[SLIRP_READ_SIDE], F_GETFL, 0);
    require_bool("rx pipe get flags", flags >= 0, true);
    require_int("rx pipe nonblock",
                fcntl(user_net.guest_to_host_channel[SLIRP_READ_SIDE],
                      F_SETFL, flags | O_NONBLOCK),
                0);

    publish_rx_buffer(header_len + sizeof(payload) - 1);
    virtio_net_set_queue_fd_ready(&emu.vnet, VNET_QUEUE_RX, true);
    mmio_write(REG(QueueNotify), VNET_QUEUE_RX);

    require_bool("rx readiness cleared after EAGAIN",
                 wait_for_net_queue_fd_ready(VNET_QUEUE_RX, false), true);
    require_u16("rx EAGAIN used idx unchanged", read16(RX_USED_ADDR + 2), 0);
    require_u32("rx EAGAIN interrupt remains clear",
                mmio_read(REG(InterruptStatus)), 0);
    require_bool("rx EAGAIN irq line remains clear",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VNET), false);
    require_u32("rx EAGAIN needs-reset remains clear",
                mmio_read(REG(Status)) & VIRTIO_STATUS__DEVICE_NEEDS_RESET, 0);

    require_int("event loop init", semu_event_loop_init(&loop, "net-test"), 0);
    require_int("net event sync",
                virtio_net_event_sync(&emu.vnet, &loop,
                                      SEMU_EVENT_TOKEN_FIRST_DEVICE),
                0);
    rx_index =
        event_loop_find_fd(&loop,
                           user_net.guest_to_host_channel[SLIRP_READ_SIDE]);
    require_bool("rx readable fd registered after EAGAIN", rx_index >= 0,
                 true);

    require_int("write rx packet after EAGAIN",
                (int) write(user_net.guest_to_host_channel[SLIRP_WRITE_SIDE],
                            payload, sizeof(payload) - 1),
                (int) sizeof(payload) - 1);
    event.token = loop.tokens[rx_index];
    event.events = SEMU_EVENT_READABLE;
    require_bool("rx readable event handled",
                 virtio_net_event_handle(&emu.vnet, &event,
                                         SEMU_EVENT_TOKEN_FIRST_DEVICE),
                 true);
    require_bool("rx readiness restored after event",
                 virtio_net_queue_fd_ready(&emu.vnet, VNET_QUEUE_RX), true);
    require_bool("rx actor completed preserved avail",
                 wait_for_rx_used_idx(1), true);
    require_u32("rx preserved avail used id", read32(RX_USED_ADDR + 4), 0);
    require_u32("rx preserved avail used len", read32(RX_USED_ADDR + 8),
                header_len + sizeof(payload) - 1);
    require_u32("rx preserved avail irq status",
                mmio_read(REG(InterruptStatus)), VIRTIO_INT__USED_RING);

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

    test_v1_tx_header_preserves_ethernet_frame();
    destroy_net_fixture();

    test_queue_notify_returns_before_net_backend_work();
    destroy_net_fixture();

    test_reset_cancels_stale_net_completion();
    destroy_net_fixture();

    test_stop_cancels_stale_net_completion();
    destroy_net_fixture();

    test_reset_cancels_stale_net_rx_completion();
    destroy_net_fixture();

    test_stop_cancels_stale_net_rx_completion();
    destroy_net_fixture();

    test_common_reset_start_cancels_stale_net_avail_failure();
    destroy_net_fixture();

    test_net_event_sync_avoids_ready_tx_writable_subscription();
    destroy_net_fixture();

    test_user_dynamic_slirp_fds_register_with_event_masks();
    destroy_net_fixture();

    test_user_dynamic_slirp_resync_removes_stale_and_unregisters();
    destroy_net_fixture();

    test_user_dynamic_slirp_resync_removes_closed_current_fd();
    destroy_net_fixture();

    test_user_dynamic_slirp_capacity_falls_back_without_failing();
    destroy_net_fixture();

    test_user_dynamic_slirp_event_pumps_revents();
    destroy_net_fixture();

    test_user_internal_rx_event_drives_rx_actor_work();
    destroy_net_fixture();

    test_tx_eagain_event_restores_tx_readiness();
    destroy_net_fixture();

    test_rx_eagain_event_restores_rx_readiness_and_preserves_avail();
    destroy_net_fixture();

    test_net_reset_resync_does_not_restore_fd_readiness();
    destroy_net_fixture();

    return 0;
}
