#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../ram_access.h"
#include "../riscv_private.h"

static bool test_ram_dma_read(const ram_dma_t *dma,
                              guest_paddr_t addr,
                              void *buf,
                              guest_size_t len);

#define ram_dma_read test_ram_dma_read
#include "../virtio-snd.c"
#undef ram_dma_read

#define REG(reg) ((uint32_t) VIRTIO_##reg << 2)
#define TEST_RAM_SIZE 16384
#define QUEUE_SIZE 8
#define QUEUE_STRIDE 0x300
#define DESC_ADDR(q) (0x100 + (guest_paddr_t) (q) * QUEUE_STRIDE)
#define AVAIL_ADDR(q) (0x180 + (guest_paddr_t) (q) * QUEUE_STRIDE)
#define USED_ADDR(q) (0x200 + (guest_paddr_t) (q) * QUEUE_STRIDE)
#define CTRL_PREPARE_REQ_ADDR 0x1000
#define CTRL_PREPARE_RESP_ADDR 0x1040
#define CTRL_START_REQ_ADDR 0x1080
#define CTRL_START_RESP_ADDR 0x10c0
#define TX_RELEASE_XFER_ADDR 0x1100
#define TX_RELEASE_PAYLOAD_ADDR 0x1140
#define TX_RELEASE_RESP_ADDR 0x1180
#define CTRL_RELEASE_REQ_ADDR 0x11c0
#define CTRL_RELEASE_RESP_ADDR 0x1200
#define RX_XFER_ADDR 0x1240
#define RX_STATUS_ADDR 0x1280

static uint32_t ram_words[TEST_RAM_SIZE / 4];
static emu_state_t emu;
static unsigned wake_count;
static unsigned fake_initialize_count;
static unsigned fake_terminate_count;
static unsigned fake_open_count;
static unsigned fake_start_count;
static unsigned fake_stop_count;
static unsigned fake_close_count;
static PaError fake_start_error;
static struct virtio_device_common *reset_start_on_avail_read_common;
static guest_paddr_t reset_start_on_avail_read_addr;
static bool fake_stop_saw_buffer;
static bool fake_close_saw_buffer;
static bool fake_terminate_saw_unclosed_stream;
static char fake_stream_storage;

struct dma_gate {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool enabled;
    bool entered;
    bool release;
    guest_paddr_t addr;
    guest_size_t len;
};

static struct dma_gate snd_dma_read_gate = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

struct async_snd_call {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    virtio_snd_state_t *vsnd;
    bool done;
    int ret;
};

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

static void require_u32(const char *name, uint32_t got, uint32_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got 0x%x, want 0x%x\n", name, got, want);
    exit(1);
}

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

    dma_gate_block_if_enabled(&snd_dma_read_gate, addr, len);
    return ram_dma_read(dma, addr, buf, len);
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

PaError Pa_Initialize(void)
{
    fake_initialize_count++;
    return paNoError;
}

PaError Pa_Terminate(void)
{
    fake_terminate_count++;
    fake_terminate_saw_unclosed_stream = fake_open_count > fake_close_count;
    return paNoError;
}

PaDeviceIndex Pa_GetDefaultOutputDevice(void)
{
    return 0;
}

PaError Pa_OpenStream(PaStream **stream,
                      const PaStreamParameters *inputParameters,
                      const PaStreamParameters *outputParameters,
                      double sampleRate,
                      unsigned long framesPerBuffer,
                      unsigned long streamFlags,
                      int (*streamCallback)(const void *,
                                            void *,
                                            unsigned long,
                                            const PaStreamCallbackTimeInfo *,
                                            PaStreamCallbackFlags,
                                            void *),
                      void *userData)
{
    (void) inputParameters;
    (void) outputParameters;
    (void) sampleRate;
    (void) framesPerBuffer;
    (void) streamFlags;
    (void) streamCallback;
    (void) userData;
    fake_open_count++;
    *stream = (PaStream *) &fake_stream_storage;
    return paNoError;
}

PaError Pa_StartStream(PaStream *stream)
{
    (void) stream;
    fake_start_count++;
    return fake_start_error;
}

PaError Pa_StopStream(PaStream *stream)
{
    (void) stream;
    fake_stop_count++;
    fake_stop_saw_buffer = !list_empty(&vsnd_props[0].buf_queue_head);
    return paNoError;
}

PaError Pa_CloseStream(PaStream *stream)
{
    (void) stream;
    fake_close_count++;
    fake_close_saw_buffer = !list_empty(&vsnd_props[0].buf_queue_head);
    return paNoError;
}

const char *Pa_GetErrorText(PaError errorCode)
{
    (void) errorCode;
    return "fake portaudio";
}

static void mmio_write(uint32_t reg, uint32_t value)
{
    int ret = virtio_mmio_write(&emu.vsnd.common, reg, sizeof(uint32_t), value);

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
        virtio_mmio_read(&emu.vsnd.common, reg, sizeof(uint32_t), &value), 0);
    return value;
}

static void dma_write(guest_paddr_t addr, const void *buf, guest_size_t len)
{
    require_bool("dma write", ram_dma_write(&emu.ram_dma, addr, buf, len),
                 true);
}

static void write16(guest_paddr_t addr, uint16_t value)
{
    dma_write(addr, &value, sizeof(value));
}

static void dma_read(guest_paddr_t addr, void *buf, guest_size_t len)
{
    require_bool("dma read", ram_dma_read(&emu.ram_dma, addr, buf, len),
                 true);
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

static void write_desc(guest_paddr_t table,
                       uint16_t index,
                       guest_paddr_t addr,
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

    dma_write(table + (guest_paddr_t) index * sizeof(desc), &desc,
              sizeof(desc));
}

static void configure_snd_fixture(void)
{
    memset(&emu, 0, sizeof(emu));
    memset(ram_words, 0, sizeof(ram_words));
    wake_count = 0;
    fake_initialize_count = 0;
    fake_terminate_count = 0;
    fake_open_count = 0;
    fake_start_count = 0;
    fake_stop_count = 0;
    fake_close_count = 0;
    fake_start_error = paNoError;
    reset_start_on_avail_read_common = NULL;
    reset_start_on_avail_read_addr = 0;
    fake_stop_saw_buffer = false;
    fake_close_saw_buffer = false;
    fake_terminate_saw_unclosed_stream = false;

    ram_dma_init(&emu.ram_dma, ram_words, TEST_RAM_SIZE, NULL);
    emu.ram = ram_words;
    require_int("lifecycle init", semu_vm_lifecycle_init(&emu.lifecycle), 0);
    require_int("lifecycle running",
                semu_vm_lifecycle_enter_running(&emu.lifecycle), 0);
    require_int("plic lock init", pthread_mutex_init(&emu.plic_lock, NULL), 0);

    require_bool("snd init", virtio_snd_init(&emu.vsnd, &emu), true);
}

static void destroy_snd_fixture(void)
{
    virtio_snd_destroy(&emu.vsnd);
    pthread_mutex_destroy(&emu.plic_lock);
    semu_vm_lifecycle_destroy(&emu.lifecycle);
}

static void configure_all_snd_queues(void)
{
    mmio_write(REG(DeviceFeaturesSel), 1);
    require_u32("VERSION_1 advertised", mmio_read(REG(DeviceFeatures)), 1);
    mmio_write(REG(DriverFeaturesSel), 1);
    mmio_write(REG(DriverFeatures), 1);
    mmio_write(REG(Status), VIRTIO_STATUS__ACKNOWLEDGE);
    mmio_write(REG(Status), VIRTIO_STATUS__DRIVER);
    mmio_write(REG(Status), VIRTIO_STATUS__FEATURES_OK);

    for (uint32_t q = 0; q < 4; q++) {
        mmio_write(REG(QueueSel), q);
        mmio_write(REG(QueueNum), QUEUE_SIZE);
        mmio_write(REG(QueueDescLow), DESC_ADDR(q));
        mmio_write(REG(QueueDriverLow), AVAIL_ADDR(q));
        mmio_write(REG(QueueDeviceLow), USED_ADDR(q));
        mmio_write(REG(QueueReady), 1);
    }

    mmio_write(REG(Status), VIRTIO_STATUS__DRIVER_OK);
}

static void async_snd_call_init(struct async_snd_call *call,
                                virtio_snd_state_t *vsnd)
{
    memset(call, 0, sizeof(*call));
    call->vsnd = vsnd;
    require_int("async lock init", pthread_mutex_init(&call->lock, NULL), 0);
    require_int("async cond init", pthread_cond_init(&call->cond, NULL), 0);
}

static void async_snd_call_finish(struct async_snd_call *call, int ret)
{
    pthread_mutex_lock(&call->lock);
    call->ret = ret;
    call->done = true;
    pthread_cond_broadcast(&call->cond);
    pthread_mutex_unlock(&call->lock);
}

static bool async_snd_call_wait_done(struct async_snd_call *call,
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

static void async_snd_call_destroy(struct async_snd_call *call)
{
    pthread_cond_destroy(&call->cond);
    pthread_mutex_destroy(&call->lock);
}

static void *notify_queue_thread(void *opaque)
{
    struct async_snd_call *call = opaque;
    int ret = virtio_mmio_write(&call->vsnd->common, REG(QueueNotify), 4, 0);

    async_snd_call_finish(call, ret);
    return NULL;
}

static void *reset_snd_thread(void *opaque)
{
    struct async_snd_call *call = opaque;
    int ret = virtio_device_common_reset(&call->vsnd->common);

    async_snd_call_finish(call, ret);
    return NULL;
}

static void *stop_snd_thread(void *opaque)
{
    struct async_snd_call *call = opaque;
    int ret = virtio_actor_stop(&call->vsnd->actor);

    async_snd_call_finish(call, ret);
    return NULL;
}

static void *destroy_snd_thread(void *opaque)
{
    struct async_snd_call *call = opaque;

    virtio_snd_destroy(call->vsnd);
    async_snd_call_finish(call, 0);
    return NULL;
}

static void async_snd_call_join(struct async_snd_call *call)
{
    require_int("async join", pthread_join(call->thread, NULL), 0);
}


static void sync_after_snd_actor_completion(void)
{
    require_int("completion sync lock",
                pthread_mutex_lock(&emu.vsnd.common.transport_lock), 0);
    require_int("completion sync unlock",
                pthread_mutex_unlock(&emu.vsnd.common.transport_lock), 0);
}

static bool wait_for_used_idx(unsigned queue,
                              uint16_t expected,
                              unsigned timeout_ms)
{
    struct timespec deadline = deadline_after_ms(timeout_ms);

    for (;;) {
        struct timespec now;
        struct timespec pause = {
            .tv_sec = 0,
            .tv_nsec = 1000000L,
        };

        if (read16(USED_ADDR(queue) + 2) == expected) {
            sync_after_snd_actor_completion();
            return true;
        }

        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec &&
             now.tv_nsec >= deadline.tv_nsec))
            return false;
        nanosleep(&pause, NULL);
    }
}

static bool wait_for_ctrl_used_idx(uint16_t expected, unsigned timeout_ms)
{
    return wait_for_used_idx(VSND_QUEUE_CTRL, expected, timeout_ms);
}

static uint32_t submit_control_request(const char *name,
                                       const void *request,
                                       guest_size_t request_len,
                                       guest_paddr_t req_addr,
                                       guest_paddr_t resp_addr)
{
    virtio_snd_hdr_t response = {
        .code = 0xffffffffU,
    };
    uint16_t avail_idx = read16(AVAIL_ADDR(VSND_QUEUE_CTRL) + 2);
    uint16_t used_idx = read16(USED_ADDR(VSND_QUEUE_CTRL) + 2);
    uint16_t head = (uint16_t) ((avail_idx % (QUEUE_SIZE / 2)) * 2);
    guest_paddr_t used_elem = USED_ADDR(VSND_QUEUE_CTRL) + 4 +
                              (guest_paddr_t) (used_idx % QUEUE_SIZE) * 8;

    dma_write(req_addr, request, request_len);
    dma_write(resp_addr, &response, sizeof(response));
    write_desc(DESC_ADDR(VSND_QUEUE_CTRL), head, req_addr,
               (uint32_t) request_len, VIRTIO_DESC_F_NEXT, head + 1);
    write_desc(DESC_ADDR(VSND_QUEUE_CTRL), head + 1, resp_addr,
               sizeof(response), VIRTIO_DESC_F_WRITE, 0);
    write16(AVAIL_ADDR(VSND_QUEUE_CTRL) + 4 +
                (guest_paddr_t) (avail_idx % QUEUE_SIZE) * sizeof(uint16_t),
            head);
    write16(AVAIL_ADDR(VSND_QUEUE_CTRL) + 2, avail_idx + 1);

    mmio_write(REG(QueueNotify), VSND_QUEUE_CTRL);

    require_bool(name, wait_for_ctrl_used_idx(used_idx + 1, 1000), true);
    require_u32("control used id", read32(used_elem), head);
    require_u32("control used len", read32(used_elem + 4),
                sizeof(virtio_snd_hdr_t));
    dma_read(resp_addr, &response, sizeof(response));
    return response.code;
}

static void submit_prepare_then_failed_start(void)
{
    virtio_snd_prop_t *props = &vsnd_props[0];
    virtio_snd_pcm_hdr_t prepare = {
        .hdr.code = VIRTIO_SND_R_PCM_PREPARE,
        .stream_id = 0,
    };
    virtio_snd_pcm_hdr_t start = {
        .hdr.code = VIRTIO_SND_R_PCM_START,
        .stream_id = 0,
    };
    uint32_t status;

    status = submit_control_request("PCM_PREPARE completion", &prepare,
                                    sizeof(prepare), CTRL_PREPARE_REQ_ADDR,
                                    CTRL_PREPARE_RESP_ADDR);
    require_u32("PCM_PREPARE status", status, VIRTIO_SND_S_OK);
    require_int("Pa_OpenStream called", (int) fake_open_count, 1);
    require_bool("prepared stream opened", props->pa_stream != NULL, true);
    require_u32("state after prepare", props->pp.hdr.hdr.code,
                VIRTIO_SND_R_PCM_PREPARE);
    require_bool("prepare IRQ published", virtio_snd_irq_pending(&emu.vsnd),
                 true);

    virtio_irq_ack(&emu.vsnd.common.irq, VIRTIO_INT__USED_RING);
    require_bool("IRQ acked before START", virtio_snd_irq_pending(&emu.vsnd),
                 false);

    fake_start_error = -1;
    status = submit_control_request("PCM_START failure completion", &start,
                                    sizeof(start), CTRL_START_REQ_ADDR,
                                    CTRL_START_RESP_ADDR);
    require_u32("PCM_START failure status", status, VIRTIO_SND_S_IO_ERR);
    require_int("Pa_StartStream called", (int) fake_start_count, 1);
    require_bool("failed START IRQ published", virtio_snd_irq_pending(&emu.vsnd),
                 true);
    require_u32("failed START leaves stream prepared", props->pp.hdr.hdr.code,
                VIRTIO_SND_R_PCM_PREPARE);
    require_bool("failed START keeps stream handle", props->pa_stream != NULL,
                 true);
}

static void publish_pcm_prepare_request(void)
{
    virtio_snd_pcm_hdr_t request = {
        .hdr.code = VIRTIO_SND_R_PCM_PREPARE,
        .stream_id = 0,
    };
    virtio_snd_hdr_t response = {
        .code = 0xffffffffU,
    };

    dma_write(CTRL_PREPARE_REQ_ADDR, &request, sizeof(request));
    dma_write(CTRL_PREPARE_RESP_ADDR, &response, sizeof(response));
    write_desc(DESC_ADDR(VSND_QUEUE_CTRL), 0, CTRL_PREPARE_REQ_ADDR,
               sizeof(request), VIRTIO_DESC_F_NEXT, 1);
    write_desc(DESC_ADDR(VSND_QUEUE_CTRL), 1, CTRL_PREPARE_RESP_ADDR,
               sizeof(response), VIRTIO_DESC_F_WRITE, 0);
    write16(AVAIL_ADDR(VSND_QUEUE_CTRL) + 4, 0);
    write16(AVAIL_ADDR(VSND_QUEUE_CTRL) + 2, 1);
}

static void test_common_init_and_config(void)
{
    configure_snd_fixture();

    require_u32("device id", mmio_read(REG(DeviceID)), 25);
    require_u32("queue max", mmio_read(REG(QueueNumMax)), VSND_QUEUE_NUM_MAX);
    require_u32("jacks", mmio_read(REG(Config)), 1);
    require_u32("streams", mmio_read(REG(Config) + 4), 1);
    require_u32("chmaps", mmio_read(REG(Config) + 8), 1);
    require_u32("controls", mmio_read(REG(Config) + 12), 0);
    require_bool("irq initially quiet", virtio_snd_irq_pending(&emu.vsnd),
                 false);
    require_int("Pa initialized once", (int) fake_initialize_count, 1);

    destroy_snd_fixture();
}

static void test_queue_notify_wakes_actor_without_draining_on_caller(void)
{
    struct async_snd_call call;

    configure_snd_fixture();
    configure_all_snd_queues();
    dma_gate_enable(&snd_dma_read_gate, AVAIL_ADDR(VSND_QUEUE_CTRL) + 2,
                    sizeof(uint16_t));

    async_snd_call_init(&call, &emu.vsnd);
    require_int("notify thread create",
                pthread_create(&call.thread, NULL, notify_queue_thread, &call),
                0);

    require_bool("QueueNotify returns while actor read is blocked",
                 async_snd_call_wait_done(&call, 200), true);
    require_int("QueueNotify result", call.ret, 0);
    require_bool("actor owns available-ring read",
                 dma_gate_wait_entered(&snd_dma_read_gate, 1000), true);

    dma_gate_release(&snd_dma_read_gate);
    require_int("notify join", pthread_join(call.thread, NULL), 0);
    async_snd_call_destroy(&call);
    destroy_snd_fixture();
}


static void test_pcm_start_failure_completes_without_started_state(void)
{
    configure_snd_fixture();
    configure_all_snd_queues();

    submit_prepare_then_failed_start();

    destroy_snd_fixture();
    require_int("failed START destroy closes stream", (int) fake_close_count,
                1);
    require_int("failed START destroy does not stop", (int) fake_stop_count,
                0);
    require_bool("failed START destroy leaves no unclosed stream",
                 fake_terminate_saw_unclosed_stream, false);
}

static void test_pcm_start_failure_reset_closes_prepared_stream(void)
{
    virtio_snd_prop_t *props = &vsnd_props[0];

    configure_snd_fixture();
    configure_all_snd_queues();

    submit_prepare_then_failed_start();

    require_int("common reset after failed START",
                virtio_device_common_reset(&emu.vsnd.common), 0);
    require_int("failed START reset closes stream", (int) fake_close_count, 1);
    require_int("failed START reset does not stop", (int) fake_stop_count, 0);
    require_bool("failed START reset clears stream", props->pa_stream == NULL,
                 true);

    destroy_snd_fixture();
    require_bool("failed START reset/destroy leaves no unclosed stream",
                 fake_terminate_saw_unclosed_stream, false);
}

static void test_pcm_release_flushes_pending_tx_queue(void)
{
    static const uint8_t payload[] = { 0x10, 0x20, 0x30, 0x40, 0x50 };
    virtio_snd_prop_t *props = &vsnd_props[0];
    virtio_snd_pcm_xfer_t tx_request = {
        .stream_id = 0,
    };
    virtio_snd_pcm_status_t tx_response = {
        .status = 0xffffffffU,
        .latency_bytes = 0xffffffffU,
    };
    virtio_snd_pcm_hdr_t release = {
        .hdr.code = VIRTIO_SND_R_PCM_RELEASE,
        .stream_id = 0,
    };
    virtio_snd_hdr_t release_response = {
        .code = 0xffffffffU,
    };
    guest_paddr_t tx_used_elem = USED_ADDR(VSND_QUEUE_TX) + 4;
    guest_paddr_t ctrl_used_elem = USED_ADDR(VSND_QUEUE_CTRL) + 4;

    configure_snd_fixture();
    configure_all_snd_queues();

    dma_write(TX_RELEASE_XFER_ADDR, &tx_request, sizeof(tx_request));
    dma_write(TX_RELEASE_PAYLOAD_ADDR, payload, sizeof(payload));
    dma_write(TX_RELEASE_RESP_ADDR, &tx_response, sizeof(tx_response));
    write_desc(DESC_ADDR(VSND_QUEUE_TX), 0, TX_RELEASE_XFER_ADDR,
               sizeof(tx_request), VIRTIO_DESC_F_NEXT, 1);
    write_desc(DESC_ADDR(VSND_QUEUE_TX), 1, TX_RELEASE_PAYLOAD_ADDR,
               sizeof(payload), VIRTIO_DESC_F_NEXT, 2);
    write_desc(DESC_ADDR(VSND_QUEUE_TX), 2, TX_RELEASE_RESP_ADDR,
               sizeof(tx_response), VIRTIO_DESC_F_WRITE, 0);
    write16(AVAIL_ADDR(VSND_QUEUE_TX) + 4, 0);
    write16(AVAIL_ADDR(VSND_QUEUE_TX) + 2, 1);

    dma_write(CTRL_RELEASE_REQ_ADDR, &release, sizeof(release));
    dma_write(CTRL_RELEASE_RESP_ADDR, &release_response,
              sizeof(release_response));
    write_desc(DESC_ADDR(VSND_QUEUE_CTRL), 0, CTRL_RELEASE_REQ_ADDR,
               sizeof(release), VIRTIO_DESC_F_NEXT, 1);
    write_desc(DESC_ADDR(VSND_QUEUE_CTRL), 1, CTRL_RELEASE_RESP_ADDR,
               sizeof(release_response), VIRTIO_DESC_F_WRITE, 0);
    write16(AVAIL_ADDR(VSND_QUEUE_CTRL) + 4, 0);
    write16(AVAIL_ADDR(VSND_QUEUE_CTRL) + 2, 1);

    mmio_write(REG(QueueNotify), VSND_QUEUE_CTRL);

    require_bool("PCM_RELEASE flushes TX used idx",
                 wait_for_used_idx(VSND_QUEUE_TX, 1, 1000), true);
    require_bool("PCM_RELEASE control used idx",
                 wait_for_ctrl_used_idx(1, 1000), true);
    require_u32("flushed TX used id", read32(tx_used_elem), 0);
    require_u32("flushed TX used len", read32(tx_used_elem + 4),
                sizeof(virtio_snd_pcm_status_t));
    dma_read(TX_RELEASE_RESP_ADDR, &tx_response, sizeof(tx_response));
    require_u32("flushed TX status", tx_response.status, VIRTIO_SND_S_OK);
    require_u32("flushed TX latency", tx_response.latency_bytes,
                sizeof(payload));
    require_u32("PCM_RELEASE used id", read32(ctrl_used_elem), 0);
    require_u32("PCM_RELEASE used len", read32(ctrl_used_elem + 4),
                sizeof(virtio_snd_hdr_t));
    dma_read(CTRL_RELEASE_RESP_ADDR, &release_response,
             sizeof(release_response));
    require_u32("PCM_RELEASE status", release_response.code, VIRTIO_SND_S_OK);
    require_u32("PCM_RELEASE no device reset",
                virtio_snd_status_load(&emu.vsnd) &
                    VIRTIO_STATUS__DEVICE_NEEDS_RESET,
                0);
    require_bool("PCM_RELEASE leaves TX callback queue empty",
                 list_empty(&props->buf_queue_head), true);
    require_int("PCM_RELEASE leaves TX callback notify clear",
                props->lock.buf_ev_notify, 0);

    destroy_snd_fixture();
}

static void test_rx_queue_notify_completes_io_error(void)
{
    virtio_snd_pcm_xfer_t rx_request = {
        .stream_id = 0,
    };
    virtio_snd_pcm_status_t rx_response = {
        .status = 0xffffffffU,
        .latency_bytes = 0xffffffffU,
    };
    guest_paddr_t rx_used_elem = USED_ADDR(VSND_QUEUE_RX) + 4;

    configure_snd_fixture();
    configure_all_snd_queues();

    dma_write(RX_XFER_ADDR, &rx_request, sizeof(rx_request));
    dma_write(RX_STATUS_ADDR, &rx_response, sizeof(rx_response));
    write_desc(DESC_ADDR(VSND_QUEUE_RX), 0, RX_XFER_ADDR, sizeof(rx_request),
               VIRTIO_DESC_F_NEXT, 1);
    write_desc(DESC_ADDR(VSND_QUEUE_RX), 1, RX_STATUS_ADDR,
               sizeof(rx_response), VIRTIO_DESC_F_WRITE, 0);
    write16(AVAIL_ADDR(VSND_QUEUE_RX) + 4, 0);
    write16(AVAIL_ADDR(VSND_QUEUE_RX) + 2, 1);

    mmio_write(REG(QueueNotify), VSND_QUEUE_RX);

    require_bool("RX used idx", wait_for_used_idx(VSND_QUEUE_RX, 1, 1000),
                 true);
    require_u32("RX used id", read32(rx_used_elem), 0);
    require_u32("RX used len", read32(rx_used_elem + 4),
                sizeof(virtio_snd_pcm_status_t));
    dma_read(RX_STATUS_ADDR, &rx_response, sizeof(rx_response));
    require_u32("RX status", rx_response.status, VIRTIO_SND_S_IO_ERR);
    require_u32("RX latency", rx_response.latency_bytes, 0);
    require_u32("RX no device reset",
                virtio_snd_status_load(&emu.vsnd) &
                    VIRTIO_STATUS__DEVICE_NEEDS_RESET,
                0);
    require_bool("RX IRQ published", virtio_snd_irq_pending(&emu.vsnd), true);

    destroy_snd_fixture();
}

static void test_reset_closes_callbacks_before_freeing_buffers(void)
{
    virtio_snd_prop_t *props;
    vsnd_buf_queue_node_t *node;

    configure_snd_fixture();
    configure_all_snd_queues();

    props = &vsnd_props[0];
    INIT_LIST_HEAD(&props->buf_queue_head);
    props->pp.hdr.hdr.code = VIRTIO_SND_R_PCM_START;
    props->pa_stream = (PaStream *) &fake_stream_storage;
    props->lock.releasing = 0;

    node = calloc(1, sizeof(*node));
    require_bool("buffer node alloc", node != NULL, true);
    node->addr = malloc(4);
    require_bool("buffer payload alloc", node->addr != NULL, true);
    node->len = 4;
    list_push(&node->q, &props->buf_queue_head);

    require_int("common reset", virtio_device_common_reset(&emu.vsnd.common),
                0);
    require_int("Pa_StopStream called", (int) fake_stop_count, 1);
    require_int("Pa_CloseStream called", (int) fake_close_count, 1);
    require_bool("stop ran before buffer free", fake_stop_saw_buffer, true);
    require_bool("close ran before buffer free", fake_close_saw_buffer, true);
    require_bool("buffers freed after close",
                 list_empty(&props->buf_queue_head), true);
    require_bool("stream cleared", props->pa_stream == NULL, true);

    destroy_snd_fixture();
}

static void test_reset_closes_stream_opened_by_inflight_actor(void)
{
    struct async_snd_call call;

    configure_snd_fixture();
    configure_all_snd_queues();
    publish_pcm_prepare_request();
    dma_gate_enable(&snd_dma_read_gate, CTRL_PREPARE_REQ_ADDR,
                    sizeof(virtio_snd_pcm_hdr_t));
    mmio_write(REG(QueueNotify), VSND_QUEUE_CTRL);

    require_bool("actor entered PCM_PREPARE read",
                 dma_gate_wait_entered(&snd_dma_read_gate, 1000), true);
    async_snd_call_init(&call, &emu.vsnd);
    require_int("reset thread create",
                pthread_create(&call.thread, NULL, reset_snd_thread, &call),
                0);
    require_bool("reset waits for actor quiescence",
                 async_snd_call_wait_done(&call, 200), false);

    dma_gate_release(&snd_dma_read_gate);
    require_bool("reset completes after actor quiesces",
                 async_snd_call_wait_done(&call, 1000), true);
    async_snd_call_join(&call);
    require_int("reset result", call.ret, 0);
    require_int("Pa_OpenStream called", (int) fake_open_count, 1);
    require_int("Pa_CloseStream called after quiescence",
                (int) fake_close_count, 1);
    require_int("reset actor does not publish used idx",
                read16(USED_ADDR(VSND_QUEUE_CTRL) + 2), 0);
    require_u32("reset actor does not raise IRQ status",
                virtio_irq_read_status(&emu.vsnd.common.irq), 0);
    require_bool("reset actor has no pending IRQ",
                 virtio_snd_irq_pending(&emu.vsnd), false);
    async_snd_call_destroy(&call);
    destroy_snd_fixture();
}

static void test_common_reset_start_cancels_stale_avail_failure(void)
{
    int ret;

    configure_snd_fixture();
    configure_all_snd_queues();
    write16(AVAIL_ADDR(VSND_QUEUE_CTRL) + 2, QUEUE_SIZE + 1);

    reset_start_on_avail_read_common = &emu.vsnd.common;
    reset_start_on_avail_read_addr = AVAIL_ADDR(VSND_QUEUE_CTRL) + 2;
    ret = virtio_snd_actor_drain_queue(
        &emu.vsnd, &emu.vsnd.actor, VSND_QUEUE_CTRL,
        virtio_actor_generation(&emu.vsnd.actor));

    require_int("common reset stale avail drain return", ret, 0);
    require_bool("common reset stale avail hook consumed",
                 reset_start_on_avail_read_common == NULL, true);
    require_int("common reset stale avail used idx remains clear",
                read16(USED_ADDR(VSND_QUEUE_CTRL) + 2), 0);
    require_u32("common reset stale avail interrupt remains clear",
                virtio_irq_read_status(&emu.vsnd.common.irq), 0);
    require_bool("common reset stale avail irq remains clear",
                 virtio_snd_irq_pending(&emu.vsnd), false);
    require_u32(
        "common reset stale avail needs-reset remains clear",
        virtio_snd_status_load(&emu.vsnd) & VIRTIO_STATUS__DEVICE_NEEDS_RESET,
        0);

    destroy_snd_fixture();
}

static void test_standalone_stop_drops_inflight_actor_completion(void)
{
    struct async_snd_call call;

    configure_snd_fixture();
    configure_all_snd_queues();
    publish_pcm_prepare_request();
    dma_gate_enable(&snd_dma_read_gate, CTRL_PREPARE_REQ_ADDR,
                    sizeof(virtio_snd_pcm_hdr_t));
    mmio_write(REG(QueueNotify), VSND_QUEUE_CTRL);

    require_bool("actor entered PCM_PREPARE read",
                 dma_gate_wait_entered(&snd_dma_read_gate, 1000), true);
    async_snd_call_init(&call, &emu.vsnd);
    require_int("stop thread create",
                pthread_create(&call.thread, NULL, stop_snd_thread, &call),
                0);
    require_bool("stop waits for actor quiescence",
                 async_snd_call_wait_done(&call, 200), false);

    dma_gate_release(&snd_dma_read_gate);
    require_bool("stop completes after actor quiesces",
                 async_snd_call_wait_done(&call, 1000), true);
    async_snd_call_join(&call);
    require_int("stop result", call.ret, 0);
    require_int("stopped actor does not publish used idx",
                read16(USED_ADDR(VSND_QUEUE_CTRL) + 2), 0);
    require_u32("stopped actor does not raise IRQ status",
                virtio_irq_read_status(&emu.vsnd.common.irq), 0);
    require_bool("stopped actor has no pending IRQ",
                 virtio_snd_irq_pending(&emu.vsnd), false);
    async_snd_call_destroy(&call);

    destroy_snd_fixture();
    require_bool("standalone stop destroy leaves no unclosed stream",
                 fake_terminate_saw_unclosed_stream, false);
}

static void test_destroy_closes_stream_opened_by_inflight_actor(void)
{
    struct async_snd_call call;

    configure_snd_fixture();
    configure_all_snd_queues();
    publish_pcm_prepare_request();
    dma_gate_enable(&snd_dma_read_gate, CTRL_PREPARE_REQ_ADDR,
                    sizeof(virtio_snd_pcm_hdr_t));
    mmio_write(REG(QueueNotify), VSND_QUEUE_CTRL);

    require_bool("actor entered PCM_PREPARE read",
                 dma_gate_wait_entered(&snd_dma_read_gate, 1000), true);
    async_snd_call_init(&call, &emu.vsnd);
    require_int("destroy thread create",
                pthread_create(&call.thread, NULL, destroy_snd_thread, &call),
                0);
    require_bool("destroy waits for actor quiescence",
                 async_snd_call_wait_done(&call, 200), false);

    dma_gate_release(&snd_dma_read_gate);
    require_bool("destroy completes after actor quiesces",
                 async_snd_call_wait_done(&call, 1000), true);
    async_snd_call_join(&call);
    require_int("destroy result", call.ret, 0);
    require_int("Pa_OpenStream called", (int) fake_open_count, 1);
    require_int("Pa_CloseStream called before terminate",
                (int) fake_close_count, 1);
    require_int("Pa_Terminate called", (int) fake_terminate_count, 1);
    require_bool("terminate saw no unclosed stream",
                 fake_terminate_saw_unclosed_stream, false);

    async_snd_call_destroy(&call);
    pthread_mutex_destroy(&emu.plic_lock);
    semu_vm_lifecycle_destroy(&emu.lifecycle);
}

int main(void)
{
    test_common_init_and_config();
    test_queue_notify_wakes_actor_without_draining_on_caller();
    test_pcm_start_failure_completes_without_started_state();
    test_pcm_start_failure_reset_closes_prepared_stream();
    test_pcm_release_flushes_pending_tx_queue();
    test_rx_queue_notify_completes_io_error();
    test_reset_closes_callbacks_before_freeing_buffers();
    test_reset_closes_stream_opened_by_inflight_actor();
    test_common_reset_start_cancels_stale_avail_failure();
    test_standalone_stop_drops_inflight_actor_completion();
    test_destroy_closes_stream_opened_by_inflight_actor();
    return 0;
}
