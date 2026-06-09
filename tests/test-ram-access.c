#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest-types.h"
#include "ram_access.h"
#include "riscv.h"

static void fail(const char *name)
{
    fprintf(stderr, "%s\n", name);
    exit(1);
}

static void require_true(const char *name, bool got)
{
    if (!got)
        fail(name);
}

static void require_false(const char *name, bool got)
{
    if (got)
        fail(name);
}

static void require_u32(const char *name, uint32_t got, uint32_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got 0x%08x, want 0x%08x\n", name, got, want);
    exit(1);
}

static void require_u64(const char *name, uint64_t got, uint64_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got 0x%016llx, want 0x%016llx\n", name,
            (unsigned long long) got, (unsigned long long) want);
    exit(1);
}

static void require_size(const char *name, size_t got, size_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %zu, want %zu\n", name, got, want);
    exit(1);
}

static void require_bytes(const char *name,
                          const uint8_t *got,
                          const uint8_t *want,
                          size_t len)
{
    if (memcmp(got, want, len) == 0)
        return;

    fprintf(stderr, "%s: byte mismatch\n", name);
    exit(1);
}

static void init_machine(vm_t *machine,
                         reservation_entry_t *reservations,
                         uint32_t n_hart)
{
    memset(machine, 0, sizeof(*machine));
    memset(reservations, 0, sizeof(*reservations) * n_hart);
    machine->n_hart = n_hart;
    machine->reservations = reservations;
    __atomic_store_n(&machine->any_reservation_active, false, __ATOMIC_RELAXED);
    if (pthread_mutex_init(&machine->reservation_lock, NULL) != 0)
        fail("reservation lock init failed");
}

struct dma_callback_log {
    size_t calls;
    void *opaque_seen;
    guest_paddr_t addr;
    guest_size_t len;
};

static void record_dma_write_invalidation(void *opaque,
                                          guest_paddr_t addr,
                                          guest_size_t len)
{
    struct dma_callback_log *log = opaque;

    log->calls++;
    log->opaque_seen = opaque;
    log->addr = addr;
    log->len = len;
}

static void destroy_machine(vm_t *machine)
{
    pthread_mutex_destroy(&machine->reservation_lock);
}

static void test_guest_types_are_64_bit_capable(void)
{
    guest_paddr_t paddr = UINT64_C(1) << 40;
    guest_vaddr_t vaddr = UINT64_C(1) << 41;
    guest_size_t size = UINT64_C(1) << 42;

    require_u64("guest_paddr_t holds above 32-bit", paddr, UINT64_C(1) << 40);
    require_u64("guest_vaddr_t holds above 32-bit", vaddr, UINT64_C(1) << 41);
    require_u64("guest_size_t holds above 32-bit", size, UINT64_C(1) << 42);
}

static void test_dma_unaligned_round_trip_preserves_neighbors(void)
{
    uint32_t words[2] = {0x11223344U, 0xaabbccddU};
    uint8_t src[5] = {0x80, 0x81, 0x82, 0x83, 0x84};
    uint8_t dst[5] = {0};
    ram_dma_t dma;

    ram_dma_init(&dma, words, sizeof(words), NULL);

    require_true("unaligned DMA write succeeds",
                 ram_dma_write(&dma, 1, src, sizeof(src)));
    require_u32("first word keeps byte 0", words[0], 0x82818044U);
    require_u32("second word keeps high bytes", words[1], 0xaabb8483U);

    require_true("unaligned DMA read succeeds",
                 ram_dma_read(&dma, 1, dst, sizeof(dst)));
    require_bytes("DMA read returns written bytes", dst, src, sizeof(src));
}

static void test_oob_access_fails_without_side_effects(void)
{
    uint32_t words[2] = {0x01020304U, 0x05060708U};
    uint8_t dst[2] = {0xaa, 0xbb};
    uint8_t src[2] = {0x10, 0x11};
    guest_paddr_t dirty_start = 0;
    guest_paddr_t dirty_end = 0;
    ram_dma_t dma;

    ram_dma_init(&dma, words, sizeof(words), NULL);

    require_false("OOB DMA read fails", ram_dma_read(&dma, 7, dst, 2));
    require_u32("OOB read leaves destination", dst[0] | ((uint32_t) dst[1] << 8),
                0xbbaaU);

    require_false("OOB DMA write fails", ram_dma_write(&dma, 7, src, 2));
    require_u32("OOB write leaves word 0", words[0], 0x01020304U);
    require_u32("OOB write leaves word 1", words[1], 0x05060708U);
    require_u64("OOB write leaves dirty byte count", ram_dma_dirty_bytes(&dma),
                0);
    require_false("OOB write leaves dirty range empty",
                  ram_dma_dirty_range(&dma, &dirty_start, &dirty_end));
}

static void test_zero_length_access_is_noop_success(void)
{
    uint32_t words[1] = {0x11223344U};
    uint8_t byte = 0x5a;
    guest_paddr_t dirty_start = 0;
    guest_paddr_t dirty_end = 0;
    ram_dma_t dma;

    ram_dma_init(&dma, words, sizeof(words), NULL);

    require_true("zero-length read succeeds", ram_dma_read(&dma, 4, &byte, 0));
    require_true("zero-length write succeeds",
                 ram_dma_write(&dma, 4, &byte, 0));
    require_u32("zero-length write leaves RAM", words[0], 0x11223344U);
    require_u64("zero-length write leaves dirty byte count",
                ram_dma_dirty_bytes(&dma), 0);
    require_false("zero-length write leaves dirty range empty",
                  ram_dma_dirty_range(&dma, &dirty_start, &dirty_end));
}

static void test_dma_write_invalidation_callback_records_metadata(void)
{
    uint32_t words[2] = {0};
    uint8_t src[3] = {0x11, 0x22, 0x33};
    struct dma_callback_log log = {0};
    ram_dma_t dma;

    ram_dma_init(&dma, words, sizeof(words), NULL);
    ram_dma_set_write_invalidate_callback(
        &dma, record_dma_write_invalidation, &log);

    require_true("DMA write with invalidation callback succeeds",
                 ram_dma_write(&dma, 3, src, sizeof(src)));
    require_size("DMA write invalidation callback fires once", log.calls, 1);
    require_true("DMA write invalidation callback receives opaque",
                 log.opaque_seen == &log);
    require_u64("DMA write invalidation callback addr", log.addr, 3);
    require_u64("DMA write invalidation callback len", log.len, sizeof(src));
    require_u64("callback write still records dirty byte count",
                ram_dma_dirty_bytes(&dma), sizeof(src));
}

static void test_dma_note_write_invalidation_callback_records_metadata(void)
{
    uint32_t words[2] = {0};
    struct dma_callback_log log = {0};
    ram_dma_t dma;

    ram_dma_init(&dma, words, sizeof(words), NULL);
    ram_dma_set_write_invalidate_callback(
        &dma, record_dma_write_invalidation, &log);

    ram_note_dma_write(&dma, 2, 4);

    require_size("DMA note invalidation callback fires once", log.calls, 1);
    require_true("DMA note invalidation callback receives opaque",
                 log.opaque_seen == &log);
    require_u64("DMA note invalidation callback addr", log.addr, 2);
    require_u64("DMA note invalidation callback len", log.len, 4);
}

static void test_dma_invalidation_callback_ignores_invalid_writes(void)
{
    uint32_t words[1] = {0x11223344U};
    uint8_t byte = 0xaa;
    struct dma_callback_log log = {0};
    guest_paddr_t dirty_start = 0;
    guest_paddr_t dirty_end = 0;
    ram_dma_t dma;

    ram_dma_init(&dma, words, sizeof(words), NULL);
    ram_dma_set_write_invalidate_callback(
        &dma, record_dma_write_invalidation, &log);

    require_true("zero-length callback write succeeds",
                 ram_dma_write(&dma, 4, &byte, 0));
    ram_note_dma_write(&dma, 4, 0);
    require_false("OOB callback write fails", ram_dma_write(&dma, 4, &byte, 1));
    ram_note_dma_write(&dma, 4, 1);

    require_size("invalid DMA writes do not call callback", log.calls, 0);
    require_u32("invalid callback writes leave RAM", words[0], 0x11223344U);
    require_u64("invalid callback writes leave dirty byte count",
                ram_dma_dirty_bytes(&dma), 0);
    require_false("invalid callback writes leave dirty range empty",
                  ram_dma_dirty_range(&dma, &dirty_start, &dirty_end));
}

static void test_dma_write_invalidates_overlapping_lr_reservations(void)
{
    uint32_t words[4] = {0};
    vm_t machine;
    reservation_entry_t reservations[2];
    ram_dma_t dma;
    uint8_t byte = 0xaa;

    init_machine(&machine, reservations, 2);
    reservations[0] = (reservation_entry_t) {.addr = 0, .valid = true};
    reservations[1] = (reservation_entry_t) {.addr = 4, .valid = true};
    __atomic_store_n(&machine.any_reservation_active, true, __ATOMIC_RELAXED);
    ram_dma_init(&dma, words, sizeof(words), &machine);

    require_true("DMA write to overlapping byte succeeds",
                 ram_dma_write(&dma, 2, &byte, 1));
    require_false("overlapping reservation invalidated", reservations[0].valid);
    require_true("non-overlapping reservation remains", reservations[1].valid);
    require_true("some reservation still active",
                 __atomic_load_n(&machine.any_reservation_active,
                                 __ATOMIC_RELAXED));

    require_true("DMA write to second word succeeds",
                 ram_dma_write(&dma, 7, &byte, 1));
    require_false("second overlapping reservation invalidated",
                  reservations[1].valid);
    require_false("no reservations remain active",
                  __atomic_load_n(&machine.any_reservation_active,
                                  __ATOMIC_RELAXED));

    destroy_machine(&machine);
}

static void test_dirty_tracking_accumulates_range_and_resets(void)
{
    uint32_t words[8] = {0};
    uint8_t src[4] = {0xde, 0xad, 0xbe, 0xef};
    guest_paddr_t dirty_start = 0;
    guest_paddr_t dirty_end = 0;
    ram_dma_t dma;

    ram_dma_init(&dma, words, sizeof(words), NULL);

    require_true("first dirty write succeeds", ram_dma_write(&dma, 10, src, 2));
    require_true("second dirty write succeeds",
                 ram_dma_write(&dma, 20, src + 2, 2));
    require_u64("dirty byte count accumulates", ram_dma_dirty_bytes(&dma), 4);
    require_true("dirty range is present",
                 ram_dma_dirty_range(&dma, &dirty_start, &dirty_end));
    require_u64("dirty range start", dirty_start, 10);
    require_u64("dirty range end is exclusive", dirty_end, 22);

    ram_dma_clear_dirty(&dma);
    require_u64("dirty byte count resets", ram_dma_dirty_bytes(&dma), 0);
    require_false("dirty range resets",
                  ram_dma_dirty_range(&dma, &dirty_start, &dirty_end));
}

int main(void)
{
    test_guest_types_are_64_bit_capable();
    test_dma_unaligned_round_trip_preserves_neighbors();
    test_oob_access_fails_without_side_effects();
    test_zero_length_access_is_noop_success();
    test_dma_write_invalidation_callback_records_metadata();
    test_dma_note_write_invalidation_callback_records_metadata();
    test_dma_invalidation_callback_ignores_invalid_writes();
    test_dma_write_invalidates_overlapping_lr_reservations();
    test_dirty_tracking_accumulates_range_and_resets();
    return 0;
}
