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

static uint32_t ram_words[TEST_RAM_SIZE / 4];
static ram_dma_t dma;

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

static void require_int(const char *name, int got, int want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %d, want %d\n", name, got, want);
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

static void require_u64(const char *name, uint64_t got, uint64_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got 0x%llx, want 0x%llx\n", name,
            (unsigned long long) got, (unsigned long long) want);
    exit(1);
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

static void write_desc(uint16_t index,
                       uint64_t addr,
                       uint32_t len,
                       uint16_t flags,
                       uint16_t next)
{
    write_desc_at(DESC_ADDR, index, addr, len, flags, next);
}

static void publish_avail(uint16_t slot, uint16_t head, uint16_t idx)
{
    write16(AVAIL_ADDR + 4 + (guest_paddr_t) slot * 2, head);
    write16(AVAIL_ADDR + 2, idx);
}

static void init_ram(void)
{
    memset(ram_words, 0, sizeof(ram_words));
    ram_dma_init(&dma, ram_words, TEST_RAM_SIZE, NULL);
}

static void init_queue(struct virtq *vq, uint16_t queue_size, uint64_t features)
{
    virtq_init(vq);
    require_int("configure",
                virtq_configure(vq, &dma, queue_size, DESC_ADDR, AVAIL_ADDR,
                                USED_ADDR, features),
                0);
}

static void init_chain(struct virtq_chain *chain,
                       struct virtq_iov *readable,
                       size_t readable_cap,
                       struct virtq_iov *writable,
                       size_t writable_cap)
{
    memset(chain, 0, sizeof(*chain));
    chain->readable = readable;
    chain->readable_capacity = readable_cap;
    chain->writable = writable;
    chain->writable_capacity = writable_cap;
}

static void test_init_configure_and_bounds_validation(void)
{
    struct virtq vq;

    init_ram();
    memset(&vq, 0xff, sizeof(vq));
    virtq_init(&vq);
    require_u16("initial queue size", vq.queue_size, 0);
    require_u16("initial last avail", vq.last_avail, 0);
    require_false("initial ready", vq.ready);
    require_u64("initial desc addr", vq.desc_addr, 0);
    require_u64("initial driver addr", vq.driver_addr, 0);
    require_u64("initial device addr", vq.device_addr, 0);

    require_int(
        "null queue configure",
        virtq_configure(NULL, &dma, 8, DESC_ADDR, AVAIL_ADDR, USED_ADDR, 0),
        -EINVAL);
    require_int(
        "zero queue configure",
        virtq_configure(&vq, &dma, 0, DESC_ADDR, AVAIL_ADDR, USED_ADDR, 0),
        -EINVAL);
    require_int("desc table oob",
                virtq_configure(&vq, &dma, 8, TEST_RAM_SIZE - 8, AVAIL_ADDR,
                                USED_ADDR, 0),
                -EFAULT);
    require_int("avail ring oob",
                virtq_configure(&vq, &dma, 8, DESC_ADDR, TEST_RAM_SIZE - 4,
                                USED_ADDR, 0),
                -EFAULT);
    require_int("used ring oob",
                virtq_configure(&vq, &dma, 8, DESC_ADDR, AVAIL_ADDR,
                                TEST_RAM_SIZE - 4, 0),
                -EFAULT);

    write16(AVAIL_ADDR + 2, 5);
    require_int(
        "valid configure",
        virtq_configure(&vq, &dma, 8, DESC_ADDR, AVAIL_ADDR, USED_ADDR, 0), 0);
    require_true("valid ready", vq.ready);
    require_u16("last avail snapshots avail idx", vq.last_avail, 5);
}

static void test_empty_queue_does_not_consume(void)
{
    struct virtq vq;
    struct virtq_chain chain;

    init_ram();
    init_queue(&vq, 8, 0);
    init_chain(&chain, NULL, 0, NULL, 0);

    require_int("empty pop", virtq_pop(&dma, &vq, &chain), 0);
    require_u16("empty keeps last avail", vq.last_avail, 0);
}

static void test_single_descriptor_pop_and_used_publish(void)
{
    struct virtq vq;
    struct virtq_chain chain;
    struct virtq_iov readable[1];

    init_ram();
    init_queue(&vq, 8, 0);
    init_chain(&chain, readable, ARRAY_SIZE(readable), NULL, 0);
    write_desc(3, DATA_ADDR, 64, 0, 0);
    publish_avail(0, 3, 1);

    require_int("single pop", virtq_pop(&dma, &vq, &chain), 1);
    require_u16("single head", chain.head, 3);
    require_size("single readable count", chain.readable_count, 1);
    require_u64("single readable addr", chain.readable[0].addr, DATA_ADDR);
    require_u32("single readable len", chain.readable[0].len, 64);
    require_u16("single consumed", vq.last_avail, 1);

    require_int("add used", virtq_add_used(&dma, &vq, chain.head, 37), 0);
    require_u32("used elem id before idx", read32(USED_ADDR + 4), 3);
    require_u32("used elem len before idx", read32(USED_ADDR + 8), 37);
    require_u16("used idx", read16(USED_ADDR + 2), 1);
}

static void test_multi_sg_readable_and_writable_chain(void)
{
    struct virtq vq;
    struct virtq_chain chain;
    struct virtq_iov readable[2];
    struct virtq_iov writable[2];

    init_ram();
    init_queue(&vq, 8, 0);
    init_chain(&chain, readable, ARRAY_SIZE(readable), writable,
               ARRAY_SIZE(writable));

    write_desc(0, DATA_ADDR, 8, VIRTIO_DESC_F_NEXT, 1);
    write_desc(1, DATA_ADDR + 8, 16, VIRTIO_DESC_F_NEXT, 2);
    write_desc(2, DATA_ADDR + 24, 32, VIRTIO_DESC_F_WRITE | VIRTIO_DESC_F_NEXT,
               3);
    write_desc(3, DATA_ADDR + 56, 4, VIRTIO_DESC_F_WRITE, 0);
    publish_avail(0, 0, 1);

    require_int("multi sg pop", virtq_pop(&dma, &vq, &chain), 1);
    require_size("multi readable count", chain.readable_count, 2);
    require_size("multi writable count", chain.writable_count, 2);
    require_u64("multi readable 1 addr", chain.readable[1].addr, DATA_ADDR + 8);
    require_u32("multi writable 0 len", chain.writable[0].len, 32);
    require_u64("multi writable 1 addr", chain.writable[1].addr,
                DATA_ADDR + 56);
}

static void test_arbitrary_length_within_queue_limit(void)
{
    struct virtq vq;
    struct virtq_chain chain;
    struct virtq_iov readable[6];

    init_ram();
    init_queue(&vq, 8, 0);
    init_chain(&chain, readable, ARRAY_SIZE(readable), NULL, 0);

    for (size_t i = 0; i < ARRAY_SIZE(readable); i++) {
        uint16_t index = (uint16_t) i;
        uint16_t flags = i + 1 < ARRAY_SIZE(readable) ? VIRTIO_DESC_F_NEXT : 0;

        write_desc(index, DATA_ADDR + (guest_paddr_t) i * 8, 8, flags,
                   index + 1);
    }
    publish_avail(0, 0, 1);

    require_int("six sg pop", virtq_pop(&dma, &vq, &chain), 1);
    require_size("six sg readable count", chain.readable_count,
                 ARRAY_SIZE(readable));
    require_u16("six sg consumed", vq.last_avail, 1);
}

static void test_loop_rejection_does_not_consume(void)
{
    struct virtq vq;
    struct virtq_chain chain;
    struct virtq_iov readable[8];

    init_ram();
    init_queue(&vq, 8, 0);
    init_chain(&chain, readable, ARRAY_SIZE(readable), NULL, 0);
    write_desc(0, DATA_ADDR, 8, VIRTIO_DESC_F_NEXT, 1);
    write_desc(1, DATA_ADDR + 8, 8, VIRTIO_DESC_F_NEXT, 0);
    publish_avail(0, 0, 1);

    require_int("loop pop", virtq_pop(&dma, &vq, &chain), -ELOOP);
    require_u16("loop not consumed", vq.last_avail, 0);
}

static void test_out_of_bounds_descriptor_iov_rejection(void)
{
    struct virtq vq;
    struct virtq_chain chain;
    struct virtq_iov readable[1];

    init_ram();
    init_queue(&vq, 8, 0);
    init_chain(&chain, readable, ARRAY_SIZE(readable), NULL, 0);
    write_desc(0, TEST_RAM_SIZE - 4, 8, 0, 0);
    publish_avail(0, 0, 1);

    require_int("oob descriptor iov", virtq_pop(&dma, &vq, &chain), -EFAULT);
    require_u16("oob not consumed", vq.last_avail, 0);
}

static void test_output_capacity_rejection_does_not_consume(void)
{
    struct virtq vq;
    struct virtq_chain chain;
    struct virtq_iov readable[1];

    init_ram();
    init_queue(&vq, 8, 0);
    init_chain(&chain, readable, ARRAY_SIZE(readable), NULL, 0);
    write_desc(0, DATA_ADDR, 8, VIRTIO_DESC_F_NEXT, 1);
    write_desc(1, DATA_ADDR + 8, 8, 0, 0);
    publish_avail(0, 0, 1);

    require_int("capacity pop", virtq_pop(&dma, &vq, &chain), -ENOSPC);
    require_u16("capacity not consumed", vq.last_avail, 0);
}

static void test_interrupt_suppression_flag_helper(void)
{
    struct virtq vq;

    init_ram();
    init_queue(&vq, 8, 0);
    require_false("initial interrupt suppressed",
                  virtq_interrupt_suppressed(&dma, &vq));
    write16(AVAIL_ADDR, VRING_AVAIL_F_NO_INTERRUPT);
    require_true("no interrupt suppressed",
                 virtq_interrupt_suppressed(&dma, &vq));
}

static void test_indirect_descriptor_rejected_when_disabled(void)
{
    struct virtq vq;
    struct virtq_chain chain;
    struct virtq_iov readable[1];

    init_ram();
    init_queue(&vq, 8, 0);
    init_chain(&chain, readable, ARRAY_SIZE(readable), NULL, 0);
    write_desc(0, DATA_ADDR, sizeof(struct virtq_desc), VIRTIO_DESC_F_INDIRECT,
               0);
    publish_avail(0, 0, 1);

    require_int("indirect disabled", virtq_pop(&dma, &vq, &chain), -ENOTSUP);
    require_u16("indirect disabled not consumed", vq.last_avail, 0);
}


static void test_indirect_descriptor_supported_when_enabled(void)
{
    enum { INDIRECT_ADDR = 0x600 };
    struct virtq vq;
    struct virtq_chain chain;
    struct virtq_iov readable[1];
    struct virtq_iov writable[1];

    init_ram();
    init_queue(&vq, 8, VIRTQ_F_INDIRECT_DESC);
    init_chain(&chain, readable, ARRAY_SIZE(readable), writable,
               ARRAY_SIZE(writable));
    write_desc(0, INDIRECT_ADDR, 2 * sizeof(struct virtq_desc),
               VIRTIO_DESC_F_INDIRECT, 0);
    write_desc_at(INDIRECT_ADDR, 0, DATA_ADDR, 8, VIRTIO_DESC_F_NEXT, 1);
    write_desc_at(INDIRECT_ADDR, 1, DATA_ADDR + 8, 16, VIRTIO_DESC_F_WRITE, 0);
    publish_avail(0, 0, 1);

    require_int("indirect enabled", virtq_pop(&dma, &vq, &chain), 1);
    require_u16("indirect head", chain.head, 0);
    require_size("indirect readable count", chain.readable_count, 1);
    require_size("indirect writable count", chain.writable_count, 1);
    require_u64("indirect readable addr", chain.readable[0].addr, DATA_ADDR);
    require_u32("indirect writable len", chain.writable[0].len, 16);
}


static void test_invalid_avail_head_rejected(void)
{
    struct virtq vq;
    struct virtq_chain chain;
    struct virtq_iov readable[1];

    init_ram();
    init_queue(&vq, 8, 0);
    init_chain(&chain, readable, ARRAY_SIZE(readable), NULL, 0);
    publish_avail(0, 8, 1);

    require_int("invalid head", virtq_pop(&dma, &vq, &chain), -EINVAL);
    require_u16("invalid head not consumed", vq.last_avail, 0);
}

static void test_invalid_next_rejected(void)
{
    struct virtq vq;
    struct virtq_chain chain;
    struct virtq_iov readable[1];

    init_ram();
    init_queue(&vq, 8, 0);
    init_chain(&chain, readable, ARRAY_SIZE(readable), NULL, 0);
    write_desc(0, DATA_ADDR, 8, VIRTIO_DESC_F_NEXT, 8);
    publish_avail(0, 0, 1);

    require_int("invalid next", virtq_pop(&dma, &vq, &chain), -EINVAL);
    require_u16("invalid next not consumed", vq.last_avail, 0);
}

static void test_malformed_indirect_descriptor_rejected(void)
{
    struct virtq vq;
    struct virtq_chain chain;
    struct virtq_iov readable[1];

    init_ram();
    init_queue(&vq, 8, VIRTQ_F_INDIRECT_DESC);
    init_chain(&chain, readable, ARRAY_SIZE(readable), NULL, 0);
    write_desc(0, DATA_ADDR, sizeof(struct virtq_desc) - 1,
               VIRTIO_DESC_F_INDIRECT, 0);
    publish_avail(0, 0, 1);

    require_int("malformed indirect", virtq_pop(&dma, &vq, &chain), -EINVAL);
    require_u16("malformed indirect not consumed", vq.last_avail, 0);
}

static void test_nested_indirect_descriptor_rejected(void)
{
    enum { INDIRECT_ADDR = 0x600 };
    struct virtq vq;
    struct virtq_chain chain;
    struct virtq_iov readable[1];

    init_ram();
    init_queue(&vq, 8, VIRTQ_F_INDIRECT_DESC);
    init_chain(&chain, readable, ARRAY_SIZE(readable), NULL, 0);
    write_desc(0, INDIRECT_ADDR, sizeof(struct virtq_desc),
               VIRTIO_DESC_F_INDIRECT, 0);
    write_desc_at(INDIRECT_ADDR, 0, DATA_ADDR, sizeof(struct virtq_desc),
                  VIRTIO_DESC_F_INDIRECT, 0);
    publish_avail(0, 0, 1);

    require_int("nested indirect", virtq_pop(&dma, &vq, &chain), -ENOTSUP);
    require_u16("nested indirect not consumed", vq.last_avail, 0);
}

static void test_indirect_descriptor_loop_rejected(void)
{
    enum { INDIRECT_ADDR = 0x600 };
    struct virtq vq;
    struct virtq_chain chain;
    struct virtq_iov readable[2];

    init_ram();
    init_queue(&vq, 8, VIRTQ_F_INDIRECT_DESC);
    init_chain(&chain, readable, ARRAY_SIZE(readable), NULL, 0);
    write_desc(0, INDIRECT_ADDR, 2 * sizeof(struct virtq_desc),
               VIRTIO_DESC_F_INDIRECT, 0);
    write_desc_at(INDIRECT_ADDR, 0, DATA_ADDR, 8, VIRTIO_DESC_F_NEXT, 1);
    write_desc_at(INDIRECT_ADDR, 1, DATA_ADDR + 8, 8, VIRTIO_DESC_F_NEXT, 0);
    publish_avail(0, 0, 1);

    require_int("indirect loop", virtq_pop(&dma, &vq, &chain), -ELOOP);
    require_u16("indirect loop not consumed", vq.last_avail, 0);
}

static void test_used_idx_wraparound_publish(void)
{
    struct virtq vq;
    struct virtq_chain chain;
    struct virtq_iov readable[1];
    guest_paddr_t wrapped_elem = USED_ADDR + 4 + (guest_paddr_t) 7 * 8;

    init_ram();
    write16(USED_ADDR + 2, UINT16_MAX);
    init_queue(&vq, 8, 0);
    init_chain(&chain, readable, ARRAY_SIZE(readable), NULL, 0);
    write_desc(0, DATA_ADDR, 8, 0, 0);
    publish_avail(0, 0, 1);

    require_int("wrap pop", virtq_pop(&dma, &vq, &chain), 1);
    require_int("wrap add used", virtq_add_used(&dma, &vq, chain.head, 9), 0);
    require_u32("wrap used id", read32(wrapped_elem), 0);
    require_u32("wrap used len", read32(wrapped_elem + 4), 9);
    require_u16("wrap used idx", read16(USED_ADDR + 2), 0);
}

int main(void)
{
    test_init_configure_and_bounds_validation();
    test_empty_queue_does_not_consume();
    test_single_descriptor_pop_and_used_publish();
    test_multi_sg_readable_and_writable_chain();
    test_arbitrary_length_within_queue_limit();
    test_loop_rejection_does_not_consume();
    test_out_of_bounds_descriptor_iov_rejection();
    test_output_capacity_rejection_does_not_consume();
    test_interrupt_suppression_flag_helper();
    test_indirect_descriptor_rejected_when_disabled();
    test_indirect_descriptor_supported_when_enabled();
    test_invalid_avail_head_rejected();
    test_invalid_next_rejected();
    test_malformed_indirect_descriptor_rejected();
    test_nested_indirect_descriptor_rejected();
    test_indirect_descriptor_loop_rejected();
    test_used_idx_wraparound_publish();

    return 0;
}
