#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../virtio-input.c"

int virtio_input_fill_ev_key_bitmap(uint8_t *bitmap, size_t bitmap_size)
{
    if (bitmap_size > 0)
        bitmap[0] = 1;
    return bitmap_size > 0 ? 1 : 0;
}

static void require_true(const char *name, bool got)
{
    if (got)
        return;

    fprintf(stderr, "%s: got false, want true\n", name);
    exit(1);
}

static void require_false(const char *name, bool got)
{
    if (!got)
        return;

    fprintf(stderr, "%s: got true, want false\n", name);
    exit(1);
}

static void require_u8(const char *name, uint8_t got, uint8_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got 0x%x, want 0x%x\n", name, got, want);
    exit(1);
}

static uint8_t value_byte(uint32_t value, unsigned byte)
{
    return (uint8_t) (value >> (byte * 8));
}

static struct vinput_data *init_test_input(virtio_input_state_t *vinput)
{
    memset(vinput, 0, sizeof(*vinput));
    memset(vinput_dev, 0, sizeof(vinput_dev));
    vinput->priv = &vinput_dev[VINPUT_KEYBOARD_ID];
    PRIV(vinput)->type = VINPUT_KEYBOARD_ID;
    return PRIV(vinput);
}

static void test_size_refresh_on_overlapping_read(void)
{
    virtio_input_state_t vinput;
    struct vinput_data *data;
    uint32_t value;

    data = init_test_input(&vinput);
    data->cfg.select = VIRTIO_INPUT_CFG_ID_NAME;
    data->cfg.size = 0;

    value = virtio_input_read_config(&vinput, 0, sizeof(value));
    require_u8("select preserved", value_byte(value, 0),
               VIRTIO_INPUT_CFG_ID_NAME);
    require_u8("size refreshed on overlapping read", value_byte(value, 2),
               strlen(VINPUT_KEYBOARD_NAME));
    require_u8("name payload refreshed", data->cfg.u.string[0], 'V');
}

static void test_config_write_allowed_checks_whole_range(void)
{
    const uint32_t base = VIRTIO_Config << 2;

    require_true("select byte write allowed",
                 virtio_input_config_write_allowed(base, 1));
    require_true("subsel byte write allowed",
                 virtio_input_config_write_allowed(base + 1, 1));
    require_true("select+subsel halfword write allowed",
                 virtio_input_config_write_allowed(base, 2));

    require_false("word write covers readonly size",
                  virtio_input_config_write_allowed(base, 4));
    require_false("halfword from subsel covers readonly size",
                  virtio_input_config_write_allowed(base + 1, 2));
    require_false("size byte write rejected",
                  virtio_input_config_write_allowed(base + 2, 1));
}

static void test_multi_byte_config_write_updates_each_writable_byte(void)
{
    virtio_input_state_t vinput;
    struct vinput_data *data;

    data = init_test_input(&vinput);
    virtio_input_write_config(&vinput, 0, 2, 0x1201);
    require_u8("halfword select", data->cfg.select, 0x01);
    require_u8("halfword subsel", data->cfg.subsel, 0x12);

    virtio_input_write_config(&vinput, 1, 1, 0x34);
    require_u8("byte subsel", data->cfg.subsel, 0x34);
}

int main(void)
{
    test_size_refresh_on_overlapping_read();
    test_config_write_allowed_checks_whole_range();
    test_multi_byte_config_write_updates_each_writable_byte();
    return 0;
}
