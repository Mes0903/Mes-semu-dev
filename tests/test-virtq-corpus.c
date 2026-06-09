#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ram_access.h"
#include "virtio.h"
#include "virtq.h"

#define TEST_RAM_SIZE 4096
#define DESC_ADDR 0x100
#define AVAIL_ADDR 0x200
#define USED_ADDR 0x300
#define DATA_ADDR 0x500
#define INDIRECT_ADDR 0x700

static uint32_t ram_words[TEST_RAM_SIZE / 4];
static ram_dma_t dma;
static struct virtq vq;
static struct virtq_chain chain;
static struct virtq_iov readable[8];
static struct virtq_iov writable[8];
static const char *current_vector;
static size_t current_step;

enum corpus_op_type {
    OP_CONFIGURE,
    OP_DESC,
    OP_INDIRECT_DESC,
    OP_AVAIL_FLAGS,
    OP_PUBLISH_AVAIL,
    OP_POP,
    OP_ADD_USED,
    OP_EXPECT_USED,
    OP_EXPECT_SUPPRESSED,
};

struct corpus_op {
    enum corpus_op_type type;
    uint16_t queue_size;
    uint64_t features;
    uint16_t index;
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
    uint16_t slot;
    uint16_t head;
    uint16_t idx;
    uint16_t id;
    uint16_t used_idx;
    int want_ret;
    size_t want_readable;
    size_t want_writable;
    bool want_bool;
};

struct corpus_vector {
    const char *name;
    const struct corpus_op *ops;
    size_t op_count;
};

static void failf(const char *name, unsigned long long got,
                  unsigned long long want)
{
    fprintf(stderr, "%s step %zu %s: got %llu, want %llu\n", current_vector,
            current_step, name, got, want);
    exit(1);
}

static void require_true(const char *name, bool got)
{
    if (!got)
        failf(name, 0, 1);
}

static void require_int(const char *name, int got, int want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s step %zu %s: got %d, want %d\n", current_vector,
            current_step, name, got, want);
    exit(1);
}

static void require_u16(const char *name, uint16_t got, uint16_t want)
{
    if (got != want)
        failf(name, got, want);
}

static void require_u32(const char *name, uint32_t got, uint32_t want)
{
    if (got != want)
        failf(name, got, want);
}

static void require_size(const char *name, size_t got, size_t want)
{
    if (got != want)
        failf(name, (unsigned long long) got, (unsigned long long) want);
}

static void write16(guest_paddr_t addr, uint16_t value)
{
    require_true("write16", ram_dma_write(&dma, addr, &value, sizeof(value)));
}

static uint16_t read16(guest_paddr_t addr)
{
    uint16_t value;

    require_true("read16", ram_dma_read(&dma, addr, &value, sizeof(value)));
    return value;
}

static uint32_t read32(guest_paddr_t addr)
{
    uint32_t value;

    require_true("read32", ram_dma_read(&dma, addr, &value, sizeof(value)));
    return value;
}

static void write_desc_at(guest_paddr_t table,
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

    require_true(
        "write desc",
        ram_dma_write(&dma, table + (guest_paddr_t) index * sizeof(desc), &desc,
                      sizeof(desc)));
}

static void init_context(void)
{
    memset(ram_words, 0, sizeof(ram_words));
    ram_dma_init(&dma, ram_words, TEST_RAM_SIZE, NULL);
    virtq_init(&vq);
    memset(&chain, 0, sizeof(chain));
    chain.readable = readable;
    chain.readable_capacity = ARRAY_SIZE(readable);
    chain.writable = writable;
    chain.writable_capacity = ARRAY_SIZE(writable);
}

static void run_configure(const struct corpus_op *op)
{
    virtq_init(&vq);
    require_int("configure",
                virtq_configure(&vq, &dma, op->queue_size, DESC_ADDR,
                                AVAIL_ADDR, USED_ADDR, op->features),
                op->want_ret);
}

static void run_pop(const struct corpus_op *op)
{
    int ret = virtq_pop(&dma, &vq, &chain);

    require_int("pop", ret, op->want_ret);
    if (ret != 1)
        return;

    require_u16("head", chain.head, op->head);
    require_size("readable count", chain.readable_count, op->want_readable);
    require_size("writable count", chain.writable_count, op->want_writable);
}

static void run_add_used(const struct corpus_op *op)
{
    require_int("add used", virtq_add_used(&dma, &vq, op->id, op->len),
                op->want_ret);
    if (op->want_ret == 0)
        require_u16("used idx", read16(USED_ADDR + 2), op->used_idx);
}

static void run_expect_used(const struct corpus_op *op)
{
    guest_paddr_t elem = USED_ADDR + 4 + (guest_paddr_t) op->slot * 8;

    require_u32("used id", read32(elem), op->id);
    require_u32("used len", read32(elem + 4), op->len);
}

static void run_op(const struct corpus_op *op)
{
    switch (op->type) {
    case OP_CONFIGURE:
        run_configure(op);
        break;
    case OP_DESC:
        write_desc_at(DESC_ADDR, op->index, op->addr, op->len, op->flags,
                      op->next);
        break;
    case OP_INDIRECT_DESC:
        write_desc_at(INDIRECT_ADDR, op->index, op->addr, op->len, op->flags,
                      op->next);
        break;
    case OP_AVAIL_FLAGS:
        write16(AVAIL_ADDR, op->flags);
        break;
    case OP_PUBLISH_AVAIL:
        write16(AVAIL_ADDR + 4 + (guest_paddr_t) op->slot * 2, op->head);
        write16(AVAIL_ADDR + 2, op->idx);
        break;
    case OP_POP:
        run_pop(op);
        break;
    case OP_ADD_USED:
        run_add_used(op);
        break;
    case OP_EXPECT_USED:
        run_expect_used(op);
        break;
    case OP_EXPECT_SUPPRESSED:
        require_int("interrupt suppressed", virtq_interrupt_suppressed(&dma, &vq),
                    op->want_bool);
        break;
    }
}

static const struct corpus_op single_read_used_ops[] = {
    {.type = OP_CONFIGURE, .queue_size = 8, .want_ret = 0},
    {.type = OP_DESC, .index = 0, .addr = DATA_ADDR, .len = 64},
    {.type = OP_PUBLISH_AVAIL, .slot = 0, .head = 0, .idx = 1},
    {.type = OP_POP, .want_ret = 1, .head = 0, .want_readable = 1},
    {.type = OP_ADD_USED, .id = 0, .len = 64, .used_idx = 1, .want_ret = 0},
    {.type = OP_EXPECT_USED, .slot = 0, .id = 0, .len = 64},
};

static const struct corpus_op mixed_chain_ops[] = {
    {.type = OP_CONFIGURE, .queue_size = 8, .want_ret = 0},
    {.type = OP_DESC,
     .index = 2,
     .addr = DATA_ADDR,
     .len = 16,
     .flags = VIRTIO_DESC_F_NEXT,
     .next = 3},
    {.type = OP_DESC,
     .index = 3,
     .addr = DATA_ADDR + 32,
     .len = 32,
     .flags = VIRTIO_DESC_F_WRITE},
    {.type = OP_PUBLISH_AVAIL, .slot = 0, .head = 2, .idx = 1},
    {.type = OP_POP,
     .want_ret = 1,
     .head = 2,
     .want_readable = 1,
     .want_writable = 1},
};

static const struct corpus_op reset_reconfigure_ops[] = {
    {.type = OP_CONFIGURE, .queue_size = 8, .want_ret = 0},
    {.type = OP_DESC, .index = 0, .addr = DATA_ADDR, .len = 8},
    {.type = OP_PUBLISH_AVAIL, .slot = 0, .head = 0, .idx = 1},
    {.type = OP_POP, .want_ret = 1, .head = 0, .want_readable = 1},
    {.type = OP_CONFIGURE, .queue_size = 8, .want_ret = 0},
    {.type = OP_POP, .want_ret = 0},
    {.type = OP_DESC, .index = 1, .addr = DATA_ADDR + 8, .len = 8},
    {.type = OP_PUBLISH_AVAIL, .slot = 1, .head = 1, .idx = 2},
    {.type = OP_POP, .want_ret = 1, .head = 1, .want_readable = 1},
};

static const struct corpus_op malformed_then_fixed_ops[] = {
    {.type = OP_CONFIGURE, .queue_size = 8, .want_ret = 0},
    {.type = OP_DESC,
     .index = 0,
     .addr = DATA_ADDR,
     .len = 8,
     .flags = VIRTIO_DESC_F_NEXT,
     .next = 0},
    {.type = OP_PUBLISH_AVAIL, .slot = 0, .head = 0, .idx = 1},
    {.type = OP_POP, .want_ret = -ELOOP},
    {.type = OP_DESC, .index = 0, .addr = DATA_ADDR, .len = 8},
    {.type = OP_POP, .want_ret = 1, .head = 0, .want_readable = 1},
};

static const struct corpus_op indirect_and_notify_suppression_ops[] = {
    {.type = OP_CONFIGURE,
     .queue_size = 8,
     .features = VIRTQ_F_INDIRECT_DESC,
     .want_ret = 0},
    {.type = OP_AVAIL_FLAGS, .flags = VRING_AVAIL_F_NO_INTERRUPT},
    {.type = OP_EXPECT_SUPPRESSED, .want_bool = true},
    {.type = OP_DESC,
     .index = 0,
     .addr = INDIRECT_ADDR,
     .len = 2 * sizeof(struct virtq_desc),
     .flags = VIRTIO_DESC_F_INDIRECT},
    {.type = OP_INDIRECT_DESC,
     .index = 0,
     .addr = DATA_ADDR,
     .len = 4,
     .flags = VIRTIO_DESC_F_NEXT,
     .next = 1},
    {.type = OP_INDIRECT_DESC,
     .index = 1,
     .addr = DATA_ADDR + 4,
     .len = 4,
     .flags = VIRTIO_DESC_F_WRITE},
    {.type = OP_PUBLISH_AVAIL, .slot = 0, .head = 0, .idx = 1},
    {.type = OP_POP,
     .want_ret = 1,
     .head = 0,
     .want_readable = 1,
     .want_writable = 1},
};

static const struct corpus_vector corpus[] = {
    {"single-read-used", single_read_used_ops, ARRAY_SIZE(single_read_used_ops)},
    {"mixed-readable-writable", mixed_chain_ops, ARRAY_SIZE(mixed_chain_ops)},
    {"reset-reconfigure", reset_reconfigure_ops,
     ARRAY_SIZE(reset_reconfigure_ops)},
    {"malformed-then-fixed", malformed_then_fixed_ops,
     ARRAY_SIZE(malformed_then_fixed_ops)},
    {"indirect-and-notify-suppression", indirect_and_notify_suppression_ops,
     ARRAY_SIZE(indirect_and_notify_suppression_ops)},
};

int main(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(corpus); i++) {
        current_vector = corpus[i].name;
        init_context();
        for (current_step = 0; current_step < corpus[i].op_count;
             current_step++)
            run_op(&corpus[i].ops[current_step]);
    }

    return 0;
}
