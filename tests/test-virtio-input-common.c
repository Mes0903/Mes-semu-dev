#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../ram_access.h"
#include "../riscv_private.h"

static bool test_ram_dma_read(ram_dma_t *dma,
                              guest_paddr_t addr,
                              void *buf,
                              guest_size_t len);
static bool test_ram_dma_write(ram_dma_t *dma,
                               guest_paddr_t addr,
                               const void *buf,
                               guest_size_t len);

#define ram_dma_read test_ram_dma_read
#define ram_dma_write test_ram_dma_write
#include "../virtio-input-event.c"
#include "../virtio-input.c"
#undef ram_dma_write
#undef ram_dma_read

#define REG(reg) ((uint32_t) VIRTIO_##reg << 2)
#define TEST_RAM_SIZE 16384
#define KBD_DESC_ADDR 0x100
#define KBD_AVAIL_ADDR 0x300
#define KBD_USED_ADDR 0x500
#define MOUSE_DESC_ADDR 0x900
#define MOUSE_AVAIL_ADDR 0xb00
#define MOUSE_USED_ADDR 0xd00
#define KBD_EVENT_BUF0 0x1100
#define KBD_EVENT_BUF1 0x1120
#define KBD_EVENT_BUF2 0x1140
#define KBD_EVENT_BUF3 0x1160
#define KBD_STATUS_BUF 0x1200
#define MOUSE_EVENT_BUF0 0x2100
#define MOUSE_EVENT_BUF1 0x2120
#define MOUSE_EVENT_BUF2 0x2140

static uint32_t ram_words[TEST_RAM_SIZE / 4];
static emu_state_t emu;
static unsigned wake_count;
static unsigned window_wake_count;
static struct virtio_device_common *reset_start_on_avail_read_common;
static guest_paddr_t reset_start_on_avail_read_addr;

struct dma_gate {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool enabled;
    bool entered;
    bool release;
    guest_paddr_t addr;
    guest_size_t len;
};

static struct dma_gate input_dma_write_gate = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};
static struct dma_gate input_dma_read_gate = {
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

static bool test_ram_dma_read(ram_dma_t *dma,
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

    dma_gate_block_if_enabled(&input_dma_read_gate, addr, len);
    return ram_dma_read(dma, addr, buf, len);
}

static bool test_ram_dma_write(ram_dma_t *dma,
                               guest_paddr_t addr,
                               const void *buf,
                               guest_size_t len)
{
    dma_gate_block_if_enabled(&input_dma_write_gate, addr, len);
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

static void test_window_wake_backend(void)
{
    window_wake_count++;
}

int SDL_WaitEventTimeout(SDL_Event *event, int timeout)
{
    (void) event;
    (void) timeout;
    return 0;
}

int SDL_PollEvent(SDL_Event *event)
{
    (void) event;
    return 0;
}

const struct window_backend g_window = {
    .window_wake_backend = test_window_wake_backend,
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

static int input_mmio_write_result(virtio_input_state_t *vinput,
                                   uint32_t reg,
                                   uint32_t value)
{
    return virtio_mmio_write(&vinput->common, reg, sizeof(uint32_t), value);
}

static void input_mmio_write(virtio_input_state_t *vinput,
                             uint32_t reg,
                             uint32_t value)
{
    int ret = input_mmio_write_result(vinput, reg, value);

    if (ret != 0) {
        fprintf(stderr, "mmio write reg 0x%x value 0x%x: got %d, want 0\n", reg,
                value, ret);
        exit(1);
    }
}

static uint32_t input_mmio_read(virtio_input_state_t *vinput, uint32_t reg)
{
    uint32_t value = 0;

    require_int("mmio read",
                virtio_mmio_read(&vinput->common, reg, sizeof(uint32_t),
                                  &value),
                0);
    return value;
}

static bool wait_for_input_actor_state(virtio_input_state_t *vinput,
                                       enum virtio_actor_state state);

static void configure_queue(virtio_input_state_t *vinput,
                            uint16_t queue,
                            guest_paddr_t desc,
                            guest_paddr_t avail,
                            guest_paddr_t used)
{
    input_mmio_write(vinput, REG(QueueSel), queue);
    input_mmio_write(vinput, REG(QueueNum), 8);
    input_mmio_write(vinput, REG(QueueDescLow), desc);
    input_mmio_write(vinput, REG(QueueDriverLow), avail);
    input_mmio_write(vinput, REG(QueueDeviceLow), used);
    input_mmio_write(vinput, REG(QueueReady), 1);
}

static void configure_device(virtio_input_state_t *vinput)
{
    input_mmio_write(vinput, REG(DeviceFeaturesSel), 1);
    require_u32("VERSION_1 advertised", input_mmio_read(vinput, REG(DeviceFeatures)),
                1);
    input_mmio_write(vinput, REG(DriverFeaturesSel), 1);
    input_mmio_write(vinput, REG(DriverFeatures), 1);
    input_mmio_write(vinput, REG(Status), VIRTIO_STATUS__ACKNOWLEDGE);
    input_mmio_write(vinput, REG(Status), VIRTIO_STATUS__DRIVER);
    input_mmio_write(vinput, REG(Status), VIRTIO_STATUS__FEATURES_OK);
}

static void configure_input_fixture(void)
{
    memset(&emu, 0, sizeof(emu));
    memset(ram_words, 0, sizeof(ram_words));
    memset(vinput_cmd_queues, 0, sizeof(vinput_cmd_queues));
    memset(&vinput_cmd_wake_pending, 0, sizeof(vinput_cmd_wake_pending));
    memset(vinput_dev, 0, sizeof(vinput_dev));
    ram_dma_init(&emu.ram_dma, ram_words, TEST_RAM_SIZE, NULL);
    emu.ram = ram_words;
    require_int("lifecycle init", semu_vm_lifecycle_init(&emu.lifecycle), 0);
    require_int("lifecycle running",
                semu_vm_lifecycle_enter_running(&emu.lifecycle), 0);
    require_int("plic lock init", pthread_mutex_init(&emu.plic_lock, NULL), 0);
    wake_count = 0;
    window_wake_count = 0;
    reset_start_on_avail_read_common = NULL;
    reset_start_on_avail_read_addr = 0;

    virtio_input_init(&emu.vkeyboard, &emu, SEMU_IRQ_SOURCE_VINPUT_KEYBOARD);
    virtio_input_init(&emu.vmouse, &emu, SEMU_IRQ_SOURCE_VINPUT_MOUSE);

    configure_device(&emu.vkeyboard);
    configure_queue(&emu.vkeyboard, VIRTIO_INPUT_EVENTQ, KBD_DESC_ADDR,
                    KBD_AVAIL_ADDR, KBD_USED_ADDR);
    configure_queue(&emu.vkeyboard, VIRTIO_INPUT_STATUSQ, KBD_DESC_ADDR,
                    KBD_AVAIL_ADDR, KBD_USED_ADDR);
    input_mmio_write(&emu.vkeyboard, REG(Status), VIRTIO_STATUS__DRIVER_OK);
    require_bool("keyboard actor active",
                 wait_for_input_actor_state(&emu.vkeyboard,
                                            VIRTIO_ACTOR_ACTIVE),
                 true);

    configure_device(&emu.vmouse);
    configure_queue(&emu.vmouse, VIRTIO_INPUT_EVENTQ, MOUSE_DESC_ADDR,
                    MOUSE_AVAIL_ADDR, MOUSE_USED_ADDR);
    configure_queue(&emu.vmouse, VIRTIO_INPUT_STATUSQ, MOUSE_DESC_ADDR,
                    MOUSE_AVAIL_ADDR, MOUSE_USED_ADDR);
    input_mmio_write(&emu.vmouse, REG(Status), VIRTIO_STATUS__DRIVER_OK);
    require_bool("mouse actor active",
                 wait_for_input_actor_state(&emu.vmouse, VIRTIO_ACTOR_ACTIVE),
                 true);
}

static void destroy_input_fixture(void)
{
    virtio_input_destroy(&emu.vkeyboard);
    virtio_input_destroy(&emu.vmouse);
    pthread_mutex_destroy(&emu.plic_lock);
    semu_vm_lifecycle_destroy(&emu.lifecycle);
}

static bool source_asserted(emu_state_t *emu_arg, enum semu_irq_source source)
{
    return (emu_arg->plic.active & semu_irq_source_plic_bit(source)) != 0;
}

static bool wait_for_used_idx(guest_paddr_t used_addr, uint16_t idx)
{
    for (unsigned i = 0; i < 1000; i++) {
        if (read16(used_addr + 2) == idx)
            return true;
        sleep_one_ms();
    }
    return false;
}

static bool wait_for_input_actor_state(virtio_input_state_t *vinput,
                                       enum virtio_actor_state state)
{
    for (unsigned i = 0; i < 1000; i++) {
        if (virtio_actor_get_state(&vinput->actor) == state)
            return true;
        sleep_one_ms();
    }
    return false;
}

static bool wait_for_input_actor_pending_mask(virtio_input_state_t *vinput,
                                              uint32_t mask)
{
    for (unsigned i = 0; i < 1000; i++) {
        if (virtio_actor_pending_mask(&vinput->actor) == mask)
            return true;
        sleep_one_ms();
    }
    return false;
}

struct async_input_call {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    virtio_input_state_t *vinput;
    uint16_t queue;
    int ret;
    bool done;
};

static void async_input_call_init(struct async_input_call *call,
                                  virtio_input_state_t *vinput,
                                  uint16_t queue)
{
    memset(call, 0, sizeof(*call));
    call->vinput = vinput;
    call->queue = queue;
    require_int("async lock init", pthread_mutex_init(&call->lock, NULL), 0);
    require_int("async cond init", pthread_cond_init(&call->cond, NULL), 0);
}

static void async_input_call_destroy(struct async_input_call *call)
{
    pthread_cond_destroy(&call->cond);
    pthread_mutex_destroy(&call->lock);
}

static void async_input_call_finish(struct async_input_call *call, int ret)
{
    pthread_mutex_lock(&call->lock);
    call->ret = ret;
    call->done = true;
    pthread_cond_broadcast(&call->cond);
    pthread_mutex_unlock(&call->lock);
}

static bool async_input_call_wait_done(struct async_input_call *call,
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

static void async_input_call_join(struct async_input_call *call)
{
    require_int("async join", pthread_join(call->thread, NULL), 0);
}

static void *notify_input_thread(void *opaque)
{
    struct async_input_call *call = opaque;
    int ret = virtio_mmio_write(&call->vinput->common, REG(QueueNotify), 4,
                                call->queue);

    async_input_call_finish(call, ret);
    return NULL;
}

static void *reset_input_thread(void *opaque)
{
    struct async_input_call *call = opaque;
    int ret = virtio_device_common_reset(&call->vinput->common);

    async_input_call_finish(call, ret);
    return NULL;
}

static void async_input_call_start_notify(struct async_input_call *call)
{
    require_int("notify thread create",
                pthread_create(&call->thread, NULL, notify_input_thread, call),
                0);
}

static void async_input_call_start_reset(struct async_input_call *call)
{
    require_int("reset thread create",
                pthread_create(&call->thread, NULL, reset_input_thread, call),
                0);
}

static void publish_statusq_request(void)
{
    struct virtio_input_event event = {
        .type = SEMU_EV_LED,
        .code = SEMU_LED_CAPSL,
        .value = 1,
    };

    dma_write(KBD_STATUS_BUF, &event, sizeof(event));
    write_desc(KBD_DESC_ADDR, 0, KBD_STATUS_BUF, sizeof(event), 0, 0);
    write16(KBD_AVAIL_ADDR + 4, 0);
    write16(KBD_AVAIL_ADDR + 2, 1);
}

static void publish_kbd_eventq_buffers(uint16_t count)
{
    const guest_paddr_t bufs[] = {
        KBD_EVENT_BUF0,
        KBD_EVENT_BUF1,
        KBD_EVENT_BUF2,
        KBD_EVENT_BUF3,
    };

    for (uint16_t i = 0; i < count; i++) {
        write_desc(KBD_DESC_ADDR, i, bufs[i], sizeof(struct virtio_input_event),
                   VIRTIO_DESC_F_WRITE, 0);
        write16(KBD_AVAIL_ADDR + 4 + (guest_paddr_t) i * sizeof(uint16_t), i);
    }
    write16(KBD_AVAIL_ADDR + 2, count);
}

static void publish_mouse_eventq_buffers(uint16_t count)
{
    const guest_paddr_t bufs[] = {
        MOUSE_EVENT_BUF0,
        MOUSE_EVENT_BUF1,
        MOUSE_EVENT_BUF2,
    };

    for (uint16_t i = 0; i < count; i++) {
        write_desc(MOUSE_DESC_ADDR, i, bufs[i],
                   sizeof(struct virtio_input_event), VIRTIO_DESC_F_WRITE, 0);
        write16(MOUSE_AVAIL_ADDR + 4 + (guest_paddr_t) i * sizeof(uint16_t),
                i);
    }
    write16(MOUSE_AVAIL_ADDR + 2, count);
}

static void test_statusq_notify_returns_before_actor_publishes_completion(void)
{
    struct async_input_call notify;

    configure_input_fixture();
    publish_statusq_request();
    dma_gate_enable(&input_dma_read_gate, KBD_AVAIL_ADDR + 2,
                    sizeof(uint16_t));
    async_input_call_init(&notify, &emu.vkeyboard, VIRTIO_INPUT_STATUSQ);
    async_input_call_start_notify(&notify);

    require_bool("actor entered statusq avail read",
                 dma_gate_wait_entered(&input_dma_read_gate, 1000), true);
    require_bool("QueueNotify returned while actor used write is blocked",
                 async_input_call_wait_done(&notify, 1000), true);
    require_int("QueueNotify return", notify.ret, 0);
    require_u16("no used idx before actor write returns", read16(KBD_USED_ADDR + 2),
                0);
    require_u32("no interrupt before actor write returns",
                input_mmio_read(&emu.vkeyboard, REG(InterruptStatus)), 0);

    dma_gate_release(&input_dma_read_gate);
    async_input_call_join(&notify);
    require_bool("actor published statusq completion",
                 wait_for_used_idx(KBD_USED_ADDR, 1), true);
    require_u32("statusq irq", input_mmio_read(&emu.vkeyboard, REG(InterruptStatus)),
                VIRTIO_INT__USED_RING);
    require_bool("statusq irq line",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VINPUT_KEYBOARD), true);

    async_input_call_destroy(&notify);
}

static void test_host_event_wake_notifies_actor_and_io_path_does_not_publish(
    void)
{
    struct vinput_cmd key = {
        .type = VINPUT_CMD_KEYBOARD_KEY,
        .u.keyboard_key = {
            .key = SEMU_KEY_A,
            .value = 1,
        },
    };

    configure_input_fixture();
    publish_kbd_eventq_buffers(2);
    require_bool("push key", vinput_push_cmd(VINPUT_KEYBOARD_ID, &key), true);
    require_bool("host pending after push", vinput_may_have_pending_cmds(),
                 true);

    dma_gate_enable(&input_dma_write_gate, KBD_EVENT_BUF0,
                    sizeof(struct virtio_input_event));
    virtio_input_drain_host_events();
    require_bool("actor entered eventq write",
                 dma_gate_wait_entered(&input_dma_write_gate, 1000), true);
    require_u16("io path did not publish used idx", read16(KBD_USED_ADDR + 2),
                0);
    require_u32("io path did not publish interrupt",
                input_mmio_read(&emu.vkeyboard, REG(InterruptStatus)), 0);

    dma_gate_release(&input_dma_write_gate);
    require_bool("actor published eventq events",
                 wait_for_used_idx(KBD_USED_ADDR, 2), true);
    require_u32("eventq irq", input_mmio_read(&emu.vkeyboard, REG(InterruptStatus)),
                VIRTIO_INT__USED_RING);
}

static void test_eventq_drain_waits_for_driver_ok(void)
{
    struct vinput_cmd key = {
        .type = VINPUT_CMD_KEYBOARD_KEY,
        .u.keyboard_key = {
            .key = SEMU_KEY_A,
            .value = 1,
        },
    };
    int ret;

    configure_input_fixture();
    publish_kbd_eventq_buffers(2);
    require_bool("push key before driver-ok gate test",
                 vinput_push_cmd(VINPUT_KEYBOARD_ID, &key), true);

    atomic_fetch_and_explicit(&emu.vkeyboard.common.status,
                              ~(unsigned) VIRTIO_STATUS__DRIVER_OK,
                              memory_order_release);
    require_bool(
        "eventq has-work waits for DRIVER_OK",
        virtio_input_actor_queue_has_work(
            &emu.vkeyboard, &emu.vkeyboard.actor, VIRTIO_INPUT_EVENTQ,
            virtio_actor_generation(&emu.vkeyboard.actor)),
        false);
    ret = virtio_input_actor_drain_queue(
        &emu.vkeyboard, &emu.vkeyboard.actor, VIRTIO_INPUT_EVENTQ,
        virtio_actor_generation(&emu.vkeyboard.actor));
    require_int("eventq drain before DRIVER_OK", ret, 0);
    require_bool("host event remains pending before DRIVER_OK",
                 vinput_may_have_pending_cmds_for_dev(VINPUT_KEYBOARD_ID),
                 true);
    require_u16("no eventq completion before DRIVER_OK",
                read16(KBD_USED_ADDR + 2), 0);

    atomic_fetch_or_explicit(&emu.vkeyboard.common.status,
                             VIRTIO_STATUS__DRIVER_OK, memory_order_release);
    ret = virtio_input_actor_drain_queue(
        &emu.vkeyboard, &emu.vkeyboard.actor, VIRTIO_INPUT_EVENTQ,
        virtio_actor_generation(&emu.vkeyboard.actor));
    require_int("eventq drain after DRIVER_OK", ret, 0);
    require_bool("eventq completion after DRIVER_OK",
                 wait_for_used_idx(KBD_USED_ADDR, 2), true);
    require_bool("host event rearmed after DRIVER_OK drain",
                 vinput_may_have_pending_cmds_for_dev(VINPUT_KEYBOARD_ID),
                 false);
}

static void test_reset_cancels_stale_host_event_completion(void)
{
    struct vinput_cmd key = {
        .type = VINPUT_CMD_KEYBOARD_KEY,
        .u.keyboard_key = {
            .key = SEMU_KEY_A,
            .value = 1,
        },
    };
    struct async_input_call reset;

    configure_input_fixture();
    publish_kbd_eventq_buffers(2);
    require_bool("push key before reset", vinput_push_cmd(VINPUT_KEYBOARD_ID, &key),
                 true);
    dma_gate_enable(&input_dma_write_gate, KBD_EVENT_BUF0,
                    sizeof(struct virtio_input_event));
    virtio_input_drain_host_events();
    require_bool("actor entered eventq write before reset",
                 dma_gate_wait_entered(&input_dma_write_gate, 1000), true);

    async_input_call_init(&reset, &emu.vkeyboard, VIRTIO_INPUT_EVENTQ);
    async_input_call_start_reset(&reset);
    require_bool("reset advanced actor generation",
                 wait_for_input_actor_state(&emu.vkeyboard,
                                            VIRTIO_ACTOR_RESETTING),
                 true);

    dma_gate_release(&input_dma_write_gate);
    async_input_call_join(&reset);
    require_int("reset return", reset.ret, 0);
    require_bool("stale pending clears",
                 wait_for_input_actor_pending_mask(&emu.vkeyboard, 0), true);
    require_u16("stale used idx remains clear", read16(KBD_USED_ADDR + 2), 0);
    require_u32("stale interrupt remains clear",
                input_mmio_read(&emu.vkeyboard, REG(InterruptStatus)), 0);
    require_bool("stale irq line remains clear",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VINPUT_KEYBOARD), false);

    async_input_call_destroy(&reset);
}

static void test_per_device_wake_bookkeeping_survives_reset_race(void)
{
    struct vinput_cmd key = {
        .type = VINPUT_CMD_KEYBOARD_KEY,
        .u.keyboard_key = {
            .key = SEMU_KEY_A,
            .value = 1,
        },
    };
    struct vinput_cmd motion = {
        .type = VINPUT_CMD_MOUSE_MOTION,
        .u.mouse_motion = {
            .dx = 7,
            .dy = -3,
        },
    };

    configure_input_fixture();
    publish_mouse_eventq_buffers(3);

    require_bool("push keyboard work", vinput_push_cmd(VINPUT_KEYBOARD_ID, &key),
                 true);
    require_bool("push mouse work", vinput_push_cmd(VINPUT_MOUSE_ID, &motion),
                 true);
    require_bool("pending after both pushes", vinput_may_have_pending_cmds(),
                 true);

    require_int("keyboard reset", virtio_device_common_reset(&emu.vkeyboard.common),
                0);
    require_bool("mouse pending survives keyboard reset",
                 vinput_may_have_pending_cmds(), true);

    dma_gate_enable(&input_dma_write_gate, MOUSE_EVENT_BUF0,
                    sizeof(struct virtio_input_event));
    virtio_input_drain_host_events();
    require_bool("mouse actor entered eventq write",
                 dma_gate_wait_entered(&input_dma_write_gate, 1000), true);
    dma_gate_release(&input_dma_write_gate);
    require_bool("mouse actor published grouped motion",
                 wait_for_used_idx(MOUSE_USED_ADDR, 3), true);

    require_u16("keyboard queue reset dropped stale work",
                read16(KBD_USED_ADDR + 2), 0);
    require_u32("mouse irq", input_mmio_read(&emu.vmouse, REG(InterruptStatus)),
                VIRTIO_INT__USED_RING);
}

static void test_common_reset_start_cancels_stale_avail_failure(void)
{
    int ret;

    configure_input_fixture();
    write16(KBD_AVAIL_ADDR + 2, 9);

    reset_start_on_avail_read_common = &emu.vkeyboard.common;
    reset_start_on_avail_read_addr = KBD_AVAIL_ADDR + 2;
    ret = virtio_input_actor_drain_queue(
        &emu.vkeyboard, &emu.vkeyboard.actor, VIRTIO_INPUT_STATUSQ,
        virtio_actor_generation(&emu.vkeyboard.actor));

    require_int("common reset stale avail drain return", ret, 0);
    require_bool("common reset stale avail hook consumed",
                 reset_start_on_avail_read_common == NULL, true);
    require_u32("common reset stale interrupt remains clear",
                virtio_irq_read_status(&emu.vkeyboard.common.irq), 0);
    require_bool("common reset stale irq line remains clear",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VINPUT_KEYBOARD), false);
    require_u32("common reset stale needs-reset remains clear",
                virtio_input_status_load(&emu.vkeyboard) &
                    VIRTIO_STATUS__DEVICE_NEEDS_RESET,
                0);
}

int main(void)
{
    test_statusq_notify_returns_before_actor_publishes_completion();
    destroy_input_fixture();

    test_host_event_wake_notifies_actor_and_io_path_does_not_publish();
    destroy_input_fixture();

    test_eventq_drain_waits_for_driver_ok();
    destroy_input_fixture();

    test_reset_cancels_stale_host_event_completion();
    destroy_input_fixture();

    test_per_device_wake_bookkeeping_survives_reset_race();
    destroy_input_fixture();

    test_common_reset_start_cancels_stale_avail_failure();
    destroy_input_fixture();
    return 0;
}
