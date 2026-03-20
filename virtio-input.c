#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "riscv.h"
#include "riscv_private.h"
#include "utils.h"
#include "virtio-input-codes.h"
#include "virtio.h"
#include "window.h"

extern const struct window_backend g_window;

#define BUS_VIRTUAL 0x06

#define VINPUT_KEYBOARD_NAME "VirtIO Keyboard"
#define VINPUT_MOUSE_NAME "VirtIO Mouse"

#define VIRTIO_INPUT_SERIAL "None"

#define VINPUT_FEATURES_0 0
#define VINPUT_FEATURES_1 1 /* VIRTIO_F_VERSION_1 */

#define VINPUT_QUEUE_NUM_MAX 1024
#define VINPUT_QUEUE (vinput->queues[vinput->QueueSel])

#define PRIV(x) ((struct virtio_input_data *) (x)->priv)

enum {
    VINPUT_KEYBOARD_ID = 0,
    VINPUT_MOUSE_ID = 1,
    VINPUT_DEV_CNT,
};

enum {
    EVENTQ = 0,
    STATUSQ = 1,
};

enum {
    VIRTIO_INPUT_REG_SELECT = 0x100,
    VIRTIO_INPUT_REG_SUBSEL = 0x101,
    VIRTIO_INPUT_REG_SIZE = 0x102,
};

enum virtio_input_config_select {
    VIRTIO_INPUT_CFG_UNSET = 0x00,
    VIRTIO_INPUT_CFG_ID_NAME = 0x01,
    VIRTIO_INPUT_CFG_ID_SERIAL = 0x02,
    VIRTIO_INPUT_CFG_ID_DEVIDS = 0x03,
    VIRTIO_INPUT_CFG_PROP_BITS = 0x10,
    VIRTIO_INPUT_CFG_EV_BITS = 0x11,
    VIRTIO_INPUT_CFG_ABS_INFO = 0x12,
};

PACKED(struct virtio_input_absinfo {
    uint32_t min;
    uint32_t max;
    uint32_t fuzz;
    uint32_t flat;
    uint32_t res;
});

PACKED(struct virtio_input_devids {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
});

PACKED(struct virtio_input_config {
    uint8_t select;
    uint8_t subsel;
    uint8_t size;
    uint8_t reserved[5];
    union {
        char string[128];
        uint8_t bitmap[128];
        struct virtio_input_absinfo abs;
        struct virtio_input_devids ids;
    } u;
});

PACKED(struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
});

struct virtio_input_data {
    virtio_input_state_t *vinput;
    struct virtio_input_config cfg;
    int type; /* VINPUT_KEYBOARD_ID or VINPUT_MOUSE_ID */
};

static pthread_mutex_t virtio_input_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct virtio_input_data vinput_dev[VINPUT_DEV_CNT];
static char *vinput_dev_name[VINPUT_DEV_CNT] = {
    VINPUT_KEYBOARD_NAME,
    VINPUT_MOUSE_NAME,
};

static inline void bitmap_set_bit(uint8_t *map, unsigned long bit)
{
    /* Each byte holds 8 bits. Index into the byte with bit/8,
     * then set the corresponding bit within that byte with 1 << (bit%8).
     */
    map[bit / 8] |= (uint8_t) (1U << (bit % 8));
}

static inline void virtio_input_set_fail(virtio_input_state_t *vinput)
{
    vinput->Status |= VIRTIO_STATUS__DEVICE_NEEDS_RESET;
    if (vinput->Status & VIRTIO_STATUS__DRIVER_OK)
        vinput->InterruptStatus |= VIRTIO_INT__CONF_CHANGE;
}

static inline bool vinput_is_config_access(uint32_t addr, size_t access_size)
{
    const uint32_t base = VIRTIO_Config << 2;
    const uint32_t end = base + (uint32_t) sizeof(struct virtio_input_config);

    /* [base, end) */
    if (access_size == 0)
        return false;
    if (addr < base)
        return false;
    if (addr + access_size > end)
        return false;
    return true;
}

static inline uint32_t vinput_preprocess(virtio_input_state_t *vinput,
                                         uint32_t addr)
{
    if ((addr >= RAM_SIZE) || (addr & 0b11))
        return virtio_input_set_fail(vinput), 0;

    return addr >> 2;
}

/* Consume all pending buffers from the status queue and return them to the
 * used ring. The guest driver uses statusq to send EV_LED events (Caps Lock,
 * Num Lock, etc.) and acknowledges each buffer so the queue never stalls.
 * SDL has no portable LED-control API, so LED state is not applied to the host
 * keyboard here. Must be called with virtio_input_mutex held.
 */
static void virtio_statusq_drain(virtio_input_state_t *vinput)
{
    virtio_input_queue_t *queue = &vinput->queues[STATUSQ];
    uint32_t *ram = vinput->ram;

    if (!(vinput->Status & VIRTIO_STATUS__DRIVER_OK) || !queue->ready)
        return;

    uint16_t new_avail = ram[queue->QueueAvail] >> 16;
    uint16_t avail_delta = (uint16_t) (new_avail - queue->last_avail);
    uint16_t new_used = ram[queue->QueueUsed] >> 16;
    bool consumed = false;

    if (avail_delta > (uint16_t) queue->QueueNum) {
        virtio_input_set_fail(vinput);
        return;
    }

    while (queue->last_avail != new_avail) {
        uint16_t queue_idx = queue->last_avail % queue->QueueNum;
        uint16_t buffer_idx = ram[queue->QueueAvail + 1 + queue_idx / 2] >>
                              (16 * (queue_idx % 2));

        if (buffer_idx >= queue->QueueNum) {
            virtio_input_set_fail(vinput);
            return;
        }

        uint32_t vq_used_addr =
            queue->QueueUsed + 1 + (new_used % queue->QueueNum) * 2;
        ram[vq_used_addr] = buffer_idx;
        ram[vq_used_addr + 1] = sizeof(struct virtio_input_event);
        new_used++;
        queue->last_avail++;
        consumed = true;
    }

    if (consumed) {
        uint16_t *used_hdr = (uint16_t *) &ram[queue->QueueUsed];
        used_hdr[0] = 0;
        used_hdr[1] = new_used;
        if (!(ram[queue->QueueAvail] & 1))
            vinput->InterruptStatus |= VIRTIO_INT__USED_RING;
    }
}

static void virtio_input_update_status(virtio_input_state_t *vinput,
                                       uint32_t status)
{
    vinput->Status |= status;
    if (status)
        return;

    /* Reset */
    uint32_t *ram = vinput->ram;
    void *priv = vinput->priv;
    memset(vinput, 0, sizeof(*vinput));
    vinput->ram = ram;
    vinput->priv = priv;
}

/* Returns true if any events were written to used ring, false otherwise */
static bool virtio_input_desc_handler(virtio_input_state_t *vinput,
                                      struct virtio_input_event *input_ev,
                                      uint32_t ev_cnt,
                                      virtio_input_queue_t *queue)
{
    uint32_t *desc;
    struct virtq_desc vq_desc;
    struct virtio_input_event *ev;

    uint32_t *ram = vinput->ram;
    uint16_t new_avail =
        ram[queue->QueueAvail] >> 16; /* virtq_avail.idx (le16) */
    uint16_t new_used = ram[queue->QueueUsed] >> 16; /* virtq_used.idx (le16) */

    /* For checking if the event buffer has enough space to write */
    uint32_t end = queue->last_avail + ev_cnt;
    uint32_t flattened_avail_idx = new_avail;

    /* Handle if the available index has overflowed and returned to the
     * beginning
     */
    if (new_avail < queue->last_avail)
        flattened_avail_idx += (1U << 16);

    /* Check if need to wait until the driver supplies new buffers */
    if (flattened_avail_idx < end)
        return false;

    for (uint32_t i = 0; i < ev_cnt; i++) {
        /* Obtain the available ring index */
        uint16_t queue_idx = queue->last_avail % queue->QueueNum;
        uint16_t buffer_idx = ram[queue->QueueAvail + 1 + queue_idx / 2] >>
                              (16 * (queue_idx % 2));

        if (buffer_idx >= queue->QueueNum) {
            virtio_input_set_fail(vinput);
            return false;
        }

        desc = &ram[queue->QueueDesc + buffer_idx * 4];
        vq_desc.addr = desc[0];
        uint32_t addr_high = desc[1];
        vq_desc.len = desc[2];
        vq_desc.flags = desc[3] & 0xFFFF;

        /* Validate descriptor: 32-bit addressing only, WRITE flag set,
         * buffer large enough, and address within RAM bounds.
         */
        if (addr_high != 0 || !(vq_desc.flags & VIRTIO_DESC_F_WRITE) ||
            vq_desc.len < sizeof(struct virtio_input_event) ||
            vq_desc.addr + sizeof(struct virtio_input_event) > RAM_SIZE) {
            virtio_input_set_fail(vinput);
            return false;
        }

        /* Write event into guest buffer directly */
        ev = (struct virtio_input_event *) ((uintptr_t) ram + vq_desc.addr);
        ev->type = input_ev[i].type;
        ev->code = input_ev[i].code;
        ev->value = input_ev[i].value;

        /* Update used ring */
        uint32_t vq_used_addr =
            queue->QueueUsed + 1 + (new_used % queue->QueueNum) * 2;
        ram[vq_used_addr] = buffer_idx;
        ram[vq_used_addr + 1] = sizeof(struct virtio_input_event);

        new_used++;
        queue->last_avail++;
    }

    /* Update used ring header */
    uint16_t *used_hdr = (uint16_t *) &ram[queue->QueueUsed];
    used_hdr[0] = 0;        /* virtq_used.flags */
    used_hdr[1] = new_used; /* virtq_used.idx */

    return true;
}

static void virtio_queue_event_update(int dev_id,
                                      struct virtio_input_event *input_ev,
                                      uint32_t ev_cnt)
{
    virtio_input_state_t *vinput = vinput_dev[dev_id].vinput;
    if (!vinput)
        return;

    int index = EVENTQ;

    /* Start of the critical section */
    pthread_mutex_lock(&virtio_input_mutex);

    uint32_t *ram = vinput->ram;
    virtio_input_queue_t *queue = &vinput->queues[index];

    /* Check device status */
    if (vinput->Status & VIRTIO_STATUS__DEVICE_NEEDS_RESET)
        goto out;

    if (!((vinput->Status & VIRTIO_STATUS__DRIVER_OK) && queue->ready))
        goto out;

    /* Check for new buffers */
    uint16_t new_avail = ram[queue->QueueAvail] >> 16;
    uint16_t avail_delta = (uint16_t) (new_avail - queue->last_avail);
    if (avail_delta > (uint16_t) queue->QueueNum) {
        fprintf(stderr, "%s(): size check failed\n", __func__);
        goto fail;
    }

    /* No buffers available - drop event or handle later */
    if (queue->last_avail == new_avail) {
        /* TODO: Consider buffering events instead of dropping them */
        goto out;
    }

    /* Try to write events to used ring */
    bool wrote_events =
        virtio_input_desc_handler(vinput, input_ev, ev_cnt, queue);

    /* Send interrupt only if we actually wrote events, unless
     * VIRTQ_AVAIL_F_NO_INTERRUPT is set
     */
    if (wrote_events && !(ram[queue->QueueAvail] & 1))
        vinput->InterruptStatus |= VIRTIO_INT__USED_RING;

    goto out;

fail:
    virtio_input_set_fail(vinput);

out:
    /* End of the critical section */
    pthread_mutex_unlock(&virtio_input_mutex);
}

void virtio_input_update_key(uint32_t key, uint32_t ev_value)
{
    /* ev_value follows Linux evdev: 0=release, 1=press, 2=repeat */
    struct virtio_input_event input_ev[] = {
        {.type = SEMU_EV_KEY, .code = key, .value = ev_value},
        {.type = SEMU_EV_SYN, .code = SEMU_SYN_REPORT, .value = 0},
    };

    size_t ev_cnt = ARRAY_SIZE(input_ev);
    virtio_queue_event_update(VINPUT_KEYBOARD_ID, input_ev, ev_cnt);
}

void virtio_input_update_mouse_button_state(uint32_t button, bool pressed)
{
    struct virtio_input_event input_ev[] = {
        {.type = SEMU_EV_KEY, .code = button, .value = pressed},
        {.type = SEMU_EV_SYN, .code = SEMU_SYN_REPORT, .value = 0},
    };

    size_t ev_cnt = ARRAY_SIZE(input_ev);
    virtio_queue_event_update(VINPUT_MOUSE_ID, input_ev, ev_cnt);
}

void virtio_input_update_cursor(uint32_t x, uint32_t y)
{
    struct virtio_input_event input_ev[] = {
        {.type = SEMU_EV_ABS, .code = SEMU_ABS_X, .value = x},
        {.type = SEMU_EV_ABS, .code = SEMU_ABS_Y, .value = y},
        {.type = SEMU_EV_SYN, .code = SEMU_SYN_REPORT, .value = 0},
    };

    size_t ev_cnt = ARRAY_SIZE(input_ev);
    virtio_queue_event_update(VINPUT_MOUSE_ID, input_ev, ev_cnt);
}

void virtio_input_update_scroll(int32_t dx, int32_t dy)
{
    /* Build only the non-zero axis events and always terminate with SYN_REPORT.
     * dx > 0: scroll right, dy > 0: scroll up (matches Linux evdev convention).
     */
    struct virtio_input_event input_ev[3];
    uint32_t ev_cnt = 0;

    if (dx)
        input_ev[ev_cnt++] =
            (struct virtio_input_event){.type = SEMU_EV_REL,
                                        .code = SEMU_REL_HWHEEL,
                                        .value = (uint32_t) dx};
    if (dy)
        input_ev[ev_cnt++] =
            (struct virtio_input_event){.type = SEMU_EV_REL,
                                        .code = SEMU_REL_WHEEL,
                                        .value = (uint32_t) dy};
    if (!ev_cnt)
        return;

    input_ev[ev_cnt++] = (struct virtio_input_event){
        .type = SEMU_EV_SYN, .code = SEMU_SYN_REPORT, .value = 0};

    virtio_queue_event_update(VINPUT_MOUSE_ID, input_ev, ev_cnt);
}

static void virtio_input_properties(int dev_id)
{
    struct virtio_input_config *cfg = &vinput_dev[dev_id].cfg;

    memset(cfg->u.bitmap, 0, 128);

    switch (dev_id) {
    case VINPUT_KEYBOARD_ID:
        cfg->size = 0;
        break;
    case VINPUT_MOUSE_ID:
        /* INPUT_PROP_POINTER marks this as a pointer device. */
        bitmap_set_bit(cfg->u.bitmap, SEMU_INPUT_PROP_POINTER);
        /* SEMU_INPUT_PROP_POINTER is bit 0, so the bitmap fits in byte 0. */
        cfg->size = 1;
        break;
    }
}

static void virtio_keyboard_support_events(int dev_id, uint8_t event)
{
    struct virtio_input_config *cfg = &vinput_dev[dev_id].cfg;

    memset(cfg->u.bitmap, 0, 128);

    switch (event) {
    case SEMU_EV_KEY:
        /* Only advertise key codes that key_map[] actually generates. */
        cfg->size = (uint8_t) g_window.fill_ev_key_bitmap(cfg->u.bitmap, 128);
        break;
    case SEMU_EV_LED:
        bitmap_set_bit(cfg->u.bitmap, SEMU_LED_NUML);
        bitmap_set_bit(cfg->u.bitmap, SEMU_LED_CAPSL);
        bitmap_set_bit(cfg->u.bitmap, SEMU_LED_SCROLLL);
        /* LED_NUML=0, LED_CAPSL=1, LED_SCROLLL=2 — all in byte 0 */
        cfg->size = 1;
        break;
    case SEMU_EV_REP:
        bitmap_set_bit(cfg->u.bitmap, SEMU_REP_DELAY);
        bitmap_set_bit(cfg->u.bitmap, SEMU_REP_PERIOD);
        /* REP_DELAY=0 and REP_PERIOD=1 both live in byte 0 */
        cfg->size = 1;
        break;
    default:
        cfg->size = 0;
    }
}

static void virtio_mouse_support_events(int dev_id, uint8_t event)
{
    struct virtio_input_config *cfg = &vinput_dev[dev_id].cfg;
    memset(cfg->u.bitmap, 0, 128);

    /* cfg->size is the number of bytes in the bitmap that are actually used,
     * i.e. (highest set bit index / 8) + 1. The guest driver uses this to
     * know how many bytes to read and does not scan the full 128-byte array.
     *
     * Check virtinput_cfg_bits() function in linux kernel also.
     */
    switch (event) {
    case SEMU_EV_KEY:
        bitmap_set_bit(cfg->u.bitmap, SEMU_BTN_LEFT);
        bitmap_set_bit(cfg->u.bitmap, SEMU_BTN_RIGHT);
        bitmap_set_bit(cfg->u.bitmap, SEMU_BTN_MIDDLE);
        /* BTN_LEFT=0x110=272, BTN_MIDDLE=0x112=274 → byte 34 is highest */
        cfg->size = 35;
        break;
    case SEMU_EV_REL:
        bitmap_set_bit(cfg->u.bitmap, SEMU_REL_HWHEEL);
        bitmap_set_bit(cfg->u.bitmap, SEMU_REL_WHEEL);
        /* REL_HWHEEL=6 (byte 0), REL_WHEEL=8 (byte 1) → highest byte is 1 */
        cfg->size = 2;
        break;
    case SEMU_EV_ABS:
        bitmap_set_bit(cfg->u.bitmap, SEMU_ABS_X);
        bitmap_set_bit(cfg->u.bitmap, SEMU_ABS_Y);
        /* ABS_X=0, ABS_Y=1 both in byte 0 */
        cfg->size = 1;
        break;
    default:
        cfg->size = 0;
    }
}

static void virtio_input_support_events(int dev_id, uint8_t event)
{
    switch (dev_id) {
    case VINPUT_KEYBOARD_ID:
        virtio_keyboard_support_events(dev_id, event);
        break;
    case VINPUT_MOUSE_ID:
        virtio_mouse_support_events(dev_id, event);
        break;
    }
}

static void virtio_input_abs_range(int dev_id, uint8_t code)
{
    struct virtio_input_config *cfg = &vinput_dev[dev_id].cfg;

    switch (code) {
    case SEMU_ABS_X:
        /* [abs.min, abs.max] is [0, 1023] */
        cfg->u.abs.min = 0;
        cfg->u.abs.max = SCREEN_WIDTH - 1;
        cfg->u.abs.fuzz = 0;
        cfg->u.abs.flat = 0;
        cfg->u.abs.res = 1;
        cfg->size = sizeof(struct virtio_input_absinfo);
        break;
    case SEMU_ABS_Y:
        /* [abs.min, abs.max] is [0, 767] */
        cfg->u.abs.min = 0;
        cfg->u.abs.max = SCREEN_HEIGHT - 1;
        cfg->u.abs.fuzz = 0;
        cfg->u.abs.flat = 0;
        cfg->u.abs.res = 1;
        cfg->size = sizeof(struct virtio_input_absinfo);
        break;
    default:
        cfg->size = 0;
    }
}

static bool virtio_input_cfg_read(int dev_id)
{
    struct virtio_input_config *cfg = &vinput_dev[dev_id].cfg;

    switch (cfg->select) {
    case VIRTIO_INPUT_CFG_UNSET:
        cfg->size = 0;
        return true;
    case VIRTIO_INPUT_CFG_ID_NAME:
        strcpy(cfg->u.string, vinput_dev_name[dev_id]);
        cfg->size = strlen(vinput_dev_name[dev_id]);
        return true;
    case VIRTIO_INPUT_CFG_ID_SERIAL:
        strcpy(cfg->u.string, VIRTIO_INPUT_SERIAL);
        cfg->size = strlen(VIRTIO_INPUT_SERIAL);
        return true;
    case VIRTIO_INPUT_CFG_ID_DEVIDS:
        cfg->u.ids.bustype = BUS_VIRTUAL;
        cfg->u.ids.vendor = 0;
        cfg->u.ids.product = 0;
        cfg->u.ids.version = 1;
        cfg->size = sizeof(struct virtio_input_devids);
        return true;
    case VIRTIO_INPUT_CFG_PROP_BITS:
        virtio_input_properties(dev_id);
        return true;
    case VIRTIO_INPUT_CFG_EV_BITS:
        virtio_input_support_events(dev_id, cfg->subsel);
        return true;
    case VIRTIO_INPUT_CFG_ABS_INFO:
        virtio_input_abs_range(dev_id, cfg->subsel);
        return true;
    default:
        fprintf(stderr,
                "virtio-input: Unknown value written to select register.\n");
        return false;
    }
}

static bool virtio_input_reg_read(virtio_input_state_t *vinput,
                                  uint32_t addr,
                                  uint32_t *value,
                                  size_t size)
{
#define _(reg) (VIRTIO_##reg << 2)
    switch (addr) {
    case _(MagicValue):
        *value = 0x74726976;
        return true;
    case _(Version):
        *value = 2;
        return true;
    case _(DeviceID):
        *value = 18;
        return true;
    case _(VendorID):
        *value = VIRTIO_VENDOR_ID;
        return true;
    case _(DeviceFeatures):
        *value = vinput->DeviceFeaturesSel == 0
                     ? VINPUT_FEATURES_0
                     : (vinput->DeviceFeaturesSel == 1 ? VINPUT_FEATURES_1 : 0);
        return true;
    case _(QueueNumMax):
        *value = VINPUT_QUEUE_NUM_MAX;
        return true;
    case _(QueueReady):
        *value = VINPUT_QUEUE.ready ? 1 : 0;
        return true;
    case _(InterruptStatus):
        *value = vinput->InterruptStatus;
        return true;
    case _(Status):
        *value = vinput->Status;
        return true;
    case _(ConfigGeneration):
        *value = 0;
        return true;
    case VIRTIO_INPUT_REG_SIZE:
        if (!virtio_input_cfg_read(PRIV(vinput)->type))
            return false;
        *value = PRIV(vinput)->cfg.size;
        return true;
    default:
        /* Invalid address which exceeded the range */
        if (!RANGE_CHECK(addr, _(Config), sizeof(struct virtio_input_config)))
            return false;

        /* Read virtio-input specific registers */
        uint32_t offset = addr - VIRTIO_INPUT_REG_SELECT;
        uint8_t *reg = (uint8_t *) ((uintptr_t) &PRIV(vinput)->cfg + offset);

        /* Clear value first to avoid returning dirty high bits on partial reads
         */
        *value = 0;
        memcpy(value, reg, size);

        return true;
    }
#undef _
}

static bool virtio_input_reg_write(virtio_input_state_t *vinput,
                                   uint32_t addr,
                                   uint32_t value)
{
#define _(reg) (VIRTIO_##reg << 2)
    switch (addr) {
    case _(DeviceFeaturesSel):
        vinput->DeviceFeaturesSel = value;
        return true;
    case _(DriverFeatures):
        if (vinput->DriverFeaturesSel == 0)
            vinput->DriverFeatures = value;
        return true;
    case _(DriverFeaturesSel):
        vinput->DriverFeaturesSel = value;
        return true;
    case _(QueueSel):
        if (value < ARRAY_SIZE(vinput->queues))
            vinput->QueueSel = value;
        else
            virtio_input_set_fail(vinput);
        return true;
    case _(QueueNum):
        if (value > 0 && value <= VINPUT_QUEUE_NUM_MAX)
            VINPUT_QUEUE.QueueNum = value;
        else
            virtio_input_set_fail(vinput);
        return true;
    case _(QueueReady):
        VINPUT_QUEUE.ready = value & 1;
        if (VINPUT_QUEUE.ready) {
            uint32_t qnum = VINPUT_QUEUE.QueueNum;
            uint32_t ram_words = RAM_SIZE / 4;

            /* Validate that the entire avail ring, desc table, and used ring
             * fit within guest RAM. vinput_preprocess() only checks the base
             * address of each ring — without this check a guest could place a
             * ring near the end of RAM and cause out-of-bounds host accesses
             * when the ring entries are subsequently dereferenced.
             *
             * Max words accessed per ring:
             *   avail: QueueAvail + 1 + (qnum-1)/2
             *   desc:  QueueDesc  + qnum*4 - 1
             *   used:  QueueUsed  + qnum*2      (vq_used_addr+1)
             */
            if (qnum == 0 ||
                VINPUT_QUEUE.QueueAvail + 1 + (qnum - 1) / 2 >= ram_words ||
                VINPUT_QUEUE.QueueDesc + qnum * 4 > ram_words ||
                VINPUT_QUEUE.QueueUsed + qnum * 2 >= ram_words) {
                virtio_input_set_fail(vinput);
                return true;
            }

            VINPUT_QUEUE.last_avail =
                vinput->ram[VINPUT_QUEUE.QueueAvail] >> 16;
        }
        return true;
    case _(QueueDescLow):
        VINPUT_QUEUE.QueueDesc = vinput_preprocess(vinput, value);
        return true;
    case _(QueueDescHigh):
        if (value)
            virtio_input_set_fail(vinput);
        return true;
    case _(QueueDriverLow):
        VINPUT_QUEUE.QueueAvail = vinput_preprocess(vinput, value);
        return true;
    case _(QueueDriverHigh):
        if (value)
            virtio_input_set_fail(vinput);
        return true;
    case _(QueueDeviceLow):
        VINPUT_QUEUE.QueueUsed = vinput_preprocess(vinput, value);
        return true;
    case _(QueueDeviceHigh):
        if (value)
            virtio_input_set_fail(vinput);
        return true;
    case _(QueueNotify):
        if (value >= ARRAY_SIZE(vinput->queues)) {
            virtio_input_set_fail(vinput);
            return true;
        }
        /* EVENTQ: actual buffer availability is checked lazily in
         * virtio_queue_event_update() when the next event arrives.
         * STATUSQ: drain LED-state buffers from the guest immediately so
         * the driver's status queue never runs out of available entries.
         */
        if (value == STATUSQ)
            virtio_statusq_drain(vinput);
        return true;
    case _(InterruptACK):
        vinput->InterruptStatus &= ~value;
        return true;
    case _(Status):
        virtio_input_update_status(vinput, value);
        return true;
    case _(SHMSel):
        return true;
    case VIRTIO_INPUT_REG_SELECT:
        PRIV(vinput)->cfg.select = value;
        return true;
    case VIRTIO_INPUT_REG_SUBSEL:
        PRIV(vinput)->cfg.subsel = value;
        return true;
    default:
        /* No other writable registers */
        return false;
    }
#undef _
}

void virtio_input_read(hart_t *vm,
                       virtio_input_state_t *vinput,
                       uint32_t addr,
                       uint8_t width,
                       uint32_t *value)
{
    size_t access_size = 0;
    bool is_cfg = false;

    pthread_mutex_lock(&virtio_input_mutex);

    switch (width) {
    case RV_MEM_LW:
        access_size = 4;
        break;
    case RV_MEM_LBU:
    case RV_MEM_LB:
        access_size = 1;
        break;
    case RV_MEM_LHU:
    case RV_MEM_LH:
        access_size = 2;
        break;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        goto out;
    }

    is_cfg = vinput_is_config_access(addr, access_size);

    /*
     * Common registers (before Config): only allow aligned 32-bit LW.
     * Device-specific config (Config and after): allow 8/16/32-bit with
     * natural alignment.
     */
    if (!is_cfg) {
        if (access_size != 4 || (addr & 0x3)) {
            vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
            goto out;
        }
    } else {
        if (addr & (access_size - 1)) {
            vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
            goto out;
        }
    }

    if (!virtio_input_reg_read(vinput, addr, value, access_size))
        vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);

out:
    pthread_mutex_unlock(&virtio_input_mutex);
}

void virtio_input_write(hart_t *vm,
                        virtio_input_state_t *vinput,
                        uint32_t addr,
                        uint8_t width,
                        uint32_t value)
{
    size_t access_size = 0;
    bool is_cfg = false;

    pthread_mutex_lock(&virtio_input_mutex);

    switch (width) {
    case RV_MEM_SW:
        access_size = 4;
        break;
    case RV_MEM_SB:
        access_size = 1;
        break;
    case RV_MEM_SH:
        access_size = 2;
        break;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        goto out;
    }

    is_cfg = vinput_is_config_access(addr, access_size);

    /*
     * Common registers (before Config): only allow aligned 32-bit SW.
     * Device-specific config (Config and after): allow 8/16/32-bit with
     * natural alignment. Note: only select/subsel are writable — others
     * will return false and be reported as STORE_FAULT below.
     */
    if (!is_cfg) {
        if (access_size != 4 || (addr & 0x3)) {
            vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
            goto out;
        }
    } else {
        if (addr & (access_size - 1)) {
            vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
            goto out;
        }
    }

    if (!virtio_input_reg_write(vinput, addr, value))
        vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);

out:
    pthread_mutex_unlock(&virtio_input_mutex);
}

bool virtio_input_irq_pending(virtio_input_state_t *vinput)
{
    pthread_mutex_lock(&virtio_input_mutex);
    bool pending = vinput->InterruptStatus != 0;
    pthread_mutex_unlock(&virtio_input_mutex);
    return pending;
}

void virtio_input_init(virtio_input_state_t *vinput)
{
    static int vinput_dev_cnt = 0;
    if (vinput_dev_cnt >= VINPUT_DEV_CNT) {
        fprintf(stderr,
                "Exceeded the number of virtio-input devices that can be "
                "allocated.\n");
        exit(2);
    }

    vinput->priv = &vinput_dev[vinput_dev_cnt];
    PRIV(vinput)->type = vinput_dev_cnt;
    PRIV(vinput)->vinput = vinput;
    vinput_dev_cnt++;
}
