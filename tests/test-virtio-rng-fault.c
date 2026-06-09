#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static ssize_t test_read(int fd, void *buf, size_t count);

#define read test_read
#include "../virtio-rng.c"
#undef read

struct read_step {
    ssize_t result;
    int err;
    uint8_t fill;
};

static const struct read_step *read_steps;
static size_t read_step_count;
static size_t read_step_index;
static size_t read_call_count;

static ssize_t test_read(int fd, void *buf, size_t count)
{
    const struct read_step *step;

    (void) fd;
    if (read_step_index >= read_step_count) {
        fprintf(stderr, "unexpected read call %zu\n", read_step_index);
        exit(1);
    }

    read_call_count++;
    step = &read_steps[read_step_index++];
    if (step->result < 0) {
        errno = step->err;
        return -1;
    }

    if ((size_t) step->result > count) {
        fprintf(stderr, "scripted read result exceeds request\n");
        exit(1);
    }
    memset(buf, step->fill, (size_t) step->result);
    return step->result;
}

static void set_read_script(const struct read_step *steps, size_t step_count)
{
    read_steps = steps;
    read_step_count = step_count;
    read_step_index = 0;
    read_call_count = 0;
}

static void require_bool(const char *name, bool got, bool want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %s, want %s\n", name, got ? "true" : "false",
            want ? "true" : "false");
    exit(1);
}

static void require_size(const char *name, size_t got, size_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %zu, want %zu\n", name, got, want);
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

static void require_u8(const char *name, uint8_t got, uint8_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got 0x%x, want 0x%x\n", name, got, want);
    exit(1);
}

static size_t read_entropy(void *buf, size_t len, bool *permanent_failure)
{
    return virtio_rng_read_entropy(buf, len, permanent_failure);
}

static void test_eintr_retries_before_success(void)
{
    const struct read_step steps[] = {
        {.result = -1, .err = EINTR},
        {.result = 4, .fill = 0xa5},
    };
    uint8_t buf[8] = {0};
    bool permanent_failure = true;
    size_t total;

    set_read_script(steps, ARRAY_SIZE(steps));
    total = read_entropy(buf, sizeof(buf), &permanent_failure);

    require_size("read call count", read_call_count, 2);
    require_size("used byte count", total, 4);
    require_bool("permanent failure", permanent_failure, false);
    require_u8("entropy byte 0", buf[0], 0xa5);
    require_u8("entropy byte 3", buf[3], 0xa5);
}

static void test_eagain_transient_zero_completion(void)
{
    const struct read_step steps[] = {
        {.result = -1, .err = EAGAIN},
    };
    uint8_t buf[8] = {0};
    bool permanent_failure = true;
    size_t total;

    set_read_script(steps, ARRAY_SIZE(steps));
    total = read_entropy(buf, sizeof(buf), &permanent_failure);

    require_size("read call count", read_call_count, 1);
    require_size("used byte count", total, 0);
    require_bool("permanent failure", permanent_failure, false);
}

static void test_ewouldblock_transient_zero_completion(void)
{
    const struct read_step steps[] = {
        {.result = -1, .err = EWOULDBLOCK},
    };
    uint8_t buf[8] = {0};
    bool permanent_failure = true;
    size_t total;

    set_read_script(steps, ARRAY_SIZE(steps));
    total = read_entropy(buf, sizeof(buf), &permanent_failure);

    require_size("read call count", read_call_count, 1);
    require_size("used byte count", total, 0);
    require_bool("permanent failure", permanent_failure, false);
}

static void test_eio_completes_zero_and_reports_permanent_failure(void)
{
    const struct read_step steps[] = {
        {.result = -1, .err = EIO},
    };
    uint8_t buf[8] = {0};
    bool permanent_failure = false;
    size_t total;

    set_read_script(steps, ARRAY_SIZE(steps));
    total = read_entropy(buf, sizeof(buf), &permanent_failure);

    require_size("read call count", read_call_count, 1);
    require_size("used byte count", total, 0);
    require_bool("permanent failure", permanent_failure, true);
}

static void test_short_read_completes_actual_length(void)
{
    const struct read_step steps[] = {
        {.result = 3, .fill = 0x5a},
    };
    uint8_t buf[8] = {0};
    bool permanent_failure = true;
    size_t total;

    set_read_script(steps, ARRAY_SIZE(steps));
    total = read_entropy(buf, sizeof(buf), &permanent_failure);

    require_size("read call count", read_call_count, 1);
    require_size("used byte count", total, 3);
    require_bool("permanent failure", permanent_failure, false);
    require_u8("entropy byte 0", buf[0], 0x5a);
    require_u8("entropy byte 2", buf[2], 0x5a);
}

static void test_init_opens_entropy_fd_nonblocking(void)
{
    int flags;

    rng_fd = -1;
    virtio_rng_init();

    flags = fcntl(rng_fd, F_GETFL);
    require_bool("rng fd flags readable", flags >= 0, true);
    require_bool("rng fd nonblocking flag", (flags & O_NONBLOCK) != 0,
                 true);

    close(rng_fd);
    rng_fd = -1;
}

#define TEST_RAM_WORDS 256
#define DESC_ADDR 0x80
#define AVAIL_ADDR 0x100
#define USED_ADDR 0x140
#define DATA_ADDR 0x180
#define DATA_LEN 8

static uint32_t queue_ram[TEST_RAM_WORDS];

static uint16_t read_high16(uint32_t word)
{
    return (uint16_t) (word >> 16);
}

static void init_notify_queue(virtio_rng_state_t *vrng)
{
    struct virtq_desc *desc;

    memset(queue_ram, 0, sizeof(queue_ram));
    memset(vrng, 0, sizeof(*vrng));
    vrng->ram = queue_ram;
    vrng->Status = VIRTIO_STATUS__DRIVER_OK;
    vrng->QueueSel = 0;
    vrng->queues[0].QueueNum = 2;
    vrng->queues[0].QueueDesc = DESC_ADDR / 4;
    vrng->queues[0].QueueAvail = AVAIL_ADDR / 4;
    vrng->queues[0].QueueUsed = USED_ADDR / 4;
    vrng->queues[0].ready = true;
    vrng->queues[0].last_avail = 0;

    desc = (struct virtq_desc *) &queue_ram[DESC_ADDR / 4];
    desc->addr = DATA_ADDR;
    desc->len = DATA_LEN;
    desc->flags = VIRTIO_DESC_F_WRITE;

    queue_ram[AVAIL_ADDR / 4] = 1u << 16;
    queue_ram[AVAIL_ADDR / 4 + 1] = 0;
}

static void require_used_completion(const char *name,
                                    uint16_t id,
                                    uint32_t len)
{
    char label[128];

    snprintf(label, sizeof(label), "%s used idx", name);
    require_u16(label, read_high16(queue_ram[USED_ADDR / 4]), 1);
    snprintf(label, sizeof(label), "%s used id", name);
    require_u32(label, queue_ram[USED_ADDR / 4 + 1], id);
    snprintf(label, sizeof(label), "%s used len", name);
    require_u32(label, queue_ram[USED_ADDR / 4 + 2], len);
}

static void test_queue_notify_transient_zero_completion_without_reset(int err)
{
    const struct read_step steps[] = {
        {.result = -1, .err = err},
    };
    virtio_rng_state_t vrng;

    init_notify_queue(&vrng);
    set_read_script(steps, ARRAY_SIZE(steps));
    virtio_queue_notify_handler(&vrng, &vrng.queues[0]);

    require_size("notify transient read call count", read_call_count, 1);
    require_used_completion("notify transient", 0, 0);
    require_u32("notify transient status", vrng.Status,
                VIRTIO_STATUS__DRIVER_OK);
    require_u32("notify transient interrupt", vrng.InterruptStatus,
                VIRTIO_INT__USED_RING);
}

static void test_queue_notify_eio_marks_needs_reset_and_conf_change(void)
{
    const struct read_step steps[] = {
        {.result = -1, .err = EIO},
    };
    virtio_rng_state_t vrng;

    init_notify_queue(&vrng);
    set_read_script(steps, ARRAY_SIZE(steps));
    virtio_queue_notify_handler(&vrng, &vrng.queues[0]);

    require_size("notify eio read call count", read_call_count, 1);
    require_used_completion("notify eio", 0, 0);
    require_u32("notify eio status", vrng.Status,
                VIRTIO_STATUS__DRIVER_OK |
                    VIRTIO_STATUS__DEVICE_NEEDS_RESET);
    require_u32("notify eio interrupt", vrng.InterruptStatus,
                VIRTIO_INT__CONF_CHANGE | VIRTIO_INT__USED_RING);
}

int main(void)
{
    test_eintr_retries_before_success();
    test_eagain_transient_zero_completion();
    test_ewouldblock_transient_zero_completion();
    test_eio_completes_zero_and_reports_permanent_failure();
    test_short_read_completes_actual_length();
    test_init_opens_entropy_fd_nonblocking();
    test_queue_notify_transient_zero_completion_without_reset(EAGAIN);
    test_queue_notify_transient_zero_completion_without_reset(EWOULDBLOCK);
    test_queue_notify_eio_marks_needs_reset_and_conf_change();
    return 0;
}
