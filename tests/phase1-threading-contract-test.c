#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>

enum {
    ITERATIONS = 200000,
    RV_INT_SSI_BIT = 1u << 1,
    RV_INT_STI_BIT = 1u << 5,
    RV_INT_SEI_BIT = 1u << 9,
    PTE_A_BIT = 1u << 6,
    PTE_D_BIT = 1u << 7,
    MAX_HARTS = 4,
};

typedef struct {
    atomic_uint ready;
    atomic_bool go;
} start_gate_t;

static void gate_release(start_gate_t *gate, unsigned participants)
{
    while (atomic_load_explicit(&gate->ready, memory_order_acquire) <
           participants)
        thrd_yield();
    atomic_store_explicit(&gate->go, true, memory_order_release);
}

static void gate_arrive_and_wait(start_gate_t *gate)
{
    atomic_fetch_add_explicit(&gate->ready, 1, memory_order_acq_rel);
    while (!atomic_load_explicit(&gate->go, memory_order_acquire))
        thrd_yield();
}

static void require_u32(const char *name, uint32_t got, uint32_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got 0x%08x, want 0x%08x\n", name, got, want);
    exit(1);
}

static void require_bool(const char *name, bool got, bool want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %s, want %s\n", name, got ? "true" : "false",
            want ? "true" : "false");
    exit(1);
}

static void phase1_sip_set(atomic_uint *sip, uint32_t bit)
{
    atomic_fetch_or_explicit(sip, bit, memory_order_relaxed);
}

static void phase1_sip_clear(atomic_uint *sip, uint32_t bit)
{
    atomic_fetch_and_explicit(sip, ~bit, memory_order_relaxed);
}

static void phase1_sip_csr_write(atomic_uint *sip, uint32_t value)
{
    const uint32_t sip_mask = RV_INT_SSI_BIT;
    uint32_t old = atomic_load_explicit(sip, memory_order_relaxed);

    for (;;) {
        uint32_t next = (old & ~sip_mask) | (value & sip_mask);
        if (atomic_compare_exchange_weak_explicit(
                sip, &old, next, memory_order_relaxed, memory_order_relaxed))
            return;
    }
}

typedef struct {
    atomic_uint *sip;
    uint32_t bit;
    start_gate_t *gate;
} sip_set_arg_t;

static int sip_set_thread(void *opaque)
{
    sip_set_arg_t *arg = opaque;
    gate_arrive_and_wait(arg->gate);
    for (int i = 0; i < ITERATIONS; i++)
        phase1_sip_set(arg->sip, arg->bit);
    return 0;
}

static void test_sip_fetch_or_preserves_concurrent_bits(void)
{
    atomic_uint sip;
    atomic_init(&sip, 0);
    start_gate_t gate = {0};
    sip_set_arg_t a = {.sip = &sip, .bit = RV_INT_STI_BIT, .gate = &gate};
    sip_set_arg_t b = {.sip = &sip, .bit = RV_INT_SEI_BIT, .gate = &gate};
    thrd_t threads[2];

    if (thrd_create(&threads[0], sip_set_thread, &a) != thrd_success ||
        thrd_create(&threads[1], sip_set_thread, &b) != thrd_success) {
        fputs("failed to create sip setter threads\n", stderr);
        exit(1);
    }

    gate_release(&gate, 2);
    thrd_join(threads[0], NULL);
    thrd_join(threads[1], NULL);

    require_u32("sip concurrent fetch_or bits",
                atomic_load_explicit(&sip, memory_order_relaxed),
                RV_INT_STI_BIT | RV_INT_SEI_BIT);

    phase1_sip_csr_write(&sip, RV_INT_SSI_BIT);
    require_u32("sip CSR write preserves external/timer bits",
                atomic_load_explicit(&sip, memory_order_relaxed),
                RV_INT_SSI_BIT | RV_INT_STI_BIT | RV_INT_SEI_BIT);

    phase1_sip_clear(&sip, RV_INT_STI_BIT);
    require_u32("sip fetch_and clears only selected bit",
                atomic_load_explicit(&sip, memory_order_relaxed),
                RV_INT_SSI_BIT | RV_INT_SEI_BIT);
}

static void phase1_ram_store_subword(atomic_uint *cell,
                                     uint32_t mask,
                                     uint32_t value,
                                     unsigned shift)
{
    uint32_t old = atomic_load_explicit(cell, memory_order_relaxed);

    for (;;) {
        uint32_t next = (old & ~mask) | ((value << shift) & mask);
        if (atomic_compare_exchange_weak_explicit(
                cell, &old, next, memory_order_relaxed, memory_order_relaxed))
            return;
    }
}

typedef struct {
    atomic_uint *cell;
    uint32_t mask;
    uint32_t value;
    unsigned shift;
    start_gate_t *gate;
} subword_arg_t;

static int subword_thread(void *opaque)
{
    subword_arg_t *arg = opaque;
    gate_arrive_and_wait(arg->gate);
    for (int i = 0; i < ITERATIONS; i++)
        phase1_ram_store_subword(arg->cell, arg->mask, arg->value, arg->shift);
    return 0;
}

static void run_two_subword_writers(subword_arg_t *a, subword_arg_t *b)
{
    thrd_t threads[2];
    if (thrd_create(&threads[0], subword_thread, a) != thrd_success ||
        thrd_create(&threads[1], subword_thread, b) != thrd_success) {
        fputs("failed to create subword writer threads\n", stderr);
        exit(1);
    }

    gate_release(a->gate, 2);
    thrd_join(threads[0], NULL);
    thrd_join(threads[1], NULL);
}

static void test_ram_subword_cas_preserves_other_bytes(void)
{
    atomic_uint byte_cell;
    atomic_init(&byte_cell, 0);
    start_gate_t byte_gate = {0};
    subword_arg_t byte0 = {
        .cell = &byte_cell,
        .mask = 0x000000ffu,
        .value = 0x5au,
        .shift = 0,
        .gate = &byte_gate,
    };
    subword_arg_t byte1 = {
        .cell = &byte_cell,
        .mask = 0x0000ff00u,
        .value = 0xa5u,
        .shift = 8,
        .gate = &byte_gate,
    };
    run_two_subword_writers(&byte0, &byte1);
    require_u32("byte-lane CAS keeps adjacent byte",
                atomic_load_explicit(&byte_cell, memory_order_relaxed),
                0x0000a55au);

    atomic_uint half_cell;
    atomic_init(&half_cell, 0);
    start_gate_t half_gate = {0};
    subword_arg_t half0 = {
        .cell = &half_cell,
        .mask = 0x0000ffffu,
        .value = 0x1357u,
        .shift = 0,
        .gate = &half_gate,
    };
    subword_arg_t half1 = {
        .cell = &half_cell,
        .mask = 0xffff0000u,
        .value = 0x2468u,
        .shift = 16,
        .gate = &half_gate,
    };
    run_two_subword_writers(&half0, &half1);
    require_u32("halfword CAS keeps adjacent halfword",
                atomic_load_explicit(&half_cell, memory_order_relaxed),
                0x24681357u);
}

typedef struct {
    atomic_uint *pte;
    uint32_t bit;
    start_gate_t *gate;
} pte_arg_t;

static int pte_thread(void *opaque)
{
    pte_arg_t *arg = opaque;
    gate_arrive_and_wait(arg->gate);
    for (int i = 0; i < ITERATIONS; i++)
        atomic_fetch_or_explicit(arg->pte, arg->bit, memory_order_relaxed);
    return 0;
}

static void test_pte_ad_fetch_or_preserves_concurrent_bits(void)
{
    atomic_uint pte;
    atomic_init(&pte, 0x00000c03u);
    start_gate_t gate = {0};
    pte_arg_t a = {.pte = &pte, .bit = PTE_A_BIT, .gate = &gate};
    pte_arg_t b = {.pte = &pte, .bit = PTE_D_BIT, .gate = &gate};
    thrd_t threads[2];

    if (thrd_create(&threads[0], pte_thread, &a) != thrd_success ||
        thrd_create(&threads[1], pte_thread, &b) != thrd_success) {
        fputs("failed to create PTE updater threads\n", stderr);
        exit(1);
    }

    gate_release(&gate, 2);
    thrd_join(threads[0], NULL);
    thrd_join(threads[1], NULL);

    require_u32("PTE A/D fetch_or keeps both bits",
                atomic_load_explicit(&pte, memory_order_relaxed),
                0x00000c03u | PTE_A_BIT | PTE_D_BIT);
}

typedef struct {
    uint32_t word_addr;
    atomic_bool valid;
} reservation_entry_t;

typedef struct {
    reservation_entry_t entries[MAX_HARTS];
    atomic_bool any_active;
    mtx_t lock;
} reservation_table_t;

static void reservation_table_init(reservation_table_t *table)
{
    for (int i = 0; i < MAX_HARTS; i++) {
        table->entries[i].word_addr = 0;
        atomic_init(&table->entries[i].valid, false);
    }
    atomic_init(&table->any_active, false);
    if (mtx_init(&table->lock, mtx_plain) != thrd_success) {
        fputs("failed to initialize reservation-table lock\n", stderr);
        exit(1);
    }
}

static void reservation_table_destroy(reservation_table_t *table)
{
    mtx_destroy(&table->lock);
}

static void reservation_recompute_any_locked(reservation_table_t *table)
{
    bool active = false;
    for (int i = 0; i < MAX_HARTS; i++)
        active |= atomic_load_explicit(&table->entries[i].valid,
                                       memory_order_relaxed);
    atomic_store_explicit(&table->any_active, active, memory_order_relaxed);
}

static void reservation_lr(reservation_table_t *table,
                           unsigned hart_id,
                           uint32_t phys_addr)
{
    mtx_lock(&table->lock);
    table->entries[hart_id].word_addr = phys_addr >> 2;
    atomic_store_explicit(&table->entries[hart_id].valid, true,
                          memory_order_relaxed);
    atomic_store_explicit(&table->any_active, true, memory_order_relaxed);
    mtx_unlock(&table->lock);
}

static void reservation_invalidate_locked(reservation_table_t *table,
                                          uint32_t phys_addr)
{
    uint32_t word_addr = phys_addr >> 2;

    for (int i = 0; i < MAX_HARTS; i++) {
        if (table->entries[i].word_addr == word_addr)
            atomic_store_explicit(&table->entries[i].valid, false,
                                  memory_order_relaxed);
    }
    reservation_recompute_any_locked(table);
}

static void reservation_regular_store(reservation_table_t *table,
                                      atomic_uint *cell,
                                      uint32_t phys_addr,
                                      uint32_t value)
{
    mtx_lock(&table->lock);
    atomic_store_explicit(cell, value, memory_order_relaxed);
    if (atomic_load_explicit(&table->any_active, memory_order_relaxed))
        reservation_invalidate_locked(table, phys_addr);
    mtx_unlock(&table->lock);
}

static bool reservation_sc(reservation_table_t *table,
                           unsigned hart_id,
                           atomic_uint *cell,
                           uint32_t phys_addr,
                           uint32_t value)
{
    bool success;
    uint32_t word_addr = phys_addr >> 2;

    mtx_lock(&table->lock);
    success = atomic_load_explicit(&table->entries[hart_id].valid,
                                   memory_order_relaxed) &&
              table->entries[hart_id].word_addr == word_addr;
    if (success) {
        atomic_store_explicit(cell, value, memory_order_relaxed);
        reservation_invalidate_locked(table, phys_addr);
    } else {
        atomic_store_explicit(&table->entries[hart_id].valid, false,
                              memory_order_relaxed);
        reservation_recompute_any_locked(table);
    }
    mtx_unlock(&table->lock);
    return success;
}

static void test_global_reservation_table_lr_sc_contract(void)
{
    reservation_table_t table;
    atomic_uint cell;
    reservation_table_init(&table);
    atomic_init(&cell, 0x11111111u);

    reservation_lr(&table, 0, 0x1000u);
    reservation_regular_store(&table, &cell, 0x1002u, 0x22222222u);
    require_bool(
        "same-word regular store invalidates LR",
        atomic_load_explicit(&table.entries[0].valid, memory_order_relaxed),
        false);
    require_bool("SC fails after same-word store",
                 reservation_sc(&table, 0, &cell, 0x1000u, 0x33333333u), false);
    require_u32("failed SC does not overwrite memory",
                atomic_load_explicit(&cell, memory_order_relaxed), 0x22222222u);

    reservation_lr(&table, 1, 0x2000u);
    reservation_regular_store(&table, &cell, 0x2004u, 0x44444444u);
    require_bool(
        "different-word store preserves LR",
        atomic_load_explicit(&table.entries[1].valid, memory_order_relaxed),
        true);
    require_bool("SC succeeds while reservation is valid",
                 reservation_sc(&table, 1, &cell, 0x2000u, 0x55555555u), true);
    require_u32("successful SC stores value",
                atomic_load_explicit(&cell, memory_order_relaxed), 0x55555555u);
    require_bool(
        "successful SC invalidates own reservation",
        atomic_load_explicit(&table.entries[1].valid, memory_order_relaxed),
        false);
    require_bool("any_active clears after last invalidation",
                 atomic_load_explicit(&table.any_active, memory_order_relaxed),
                 false);

    reservation_table_destroy(&table);
}

int main(void)
{
    test_sip_fetch_or_preserves_concurrent_bits();
    test_ram_subword_cas_preserves_other_bytes();
    test_pte_ad_fetch_or_preserves_concurrent_bits();
    test_global_reservation_table_lr_sc_contract();
    return 0;
}
