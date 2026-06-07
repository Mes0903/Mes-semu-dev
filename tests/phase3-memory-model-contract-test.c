#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    AMO_THREADS = 4,
    AMO_ITERATIONS = 50000,
    LRSC_ITERATIONS = 20000,
    MAX_HARTS = 4,
};

typedef struct {
    uint32_t addr;
    bool valid;
} reservation_entry_t;

typedef struct {
    reservation_entry_t entries[MAX_HARTS];
    atomic_bool any_active;
    pthread_mutex_t lock;
} reservation_table_t;

typedef struct {
    atomic_uint ready;
    atomic_bool go;
} start_gate_t;

static void fail(const char *name)
{
    fputs(name, stderr);
    fputc('\n', stderr);
    exit(1);
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

static uint32_t reservation_addr(uint32_t phys_addr)
{
    return phys_addr & ~3u;
}

static void gate_release(start_gate_t *gate, unsigned participants)
{
    while (atomic_load_explicit(&gate->ready, memory_order_acquire) <
           participants)
        sched_yield();
    atomic_store_explicit(&gate->go, true, memory_order_release);
}

static void gate_arrive_and_wait(start_gate_t *gate)
{
    atomic_fetch_add_explicit(&gate->ready, 1, memory_order_acq_rel);
    while (!atomic_load_explicit(&gate->go, memory_order_acquire))
        sched_yield();
}

static void reservation_table_init(reservation_table_t *table)
{
    for (int i = 0; i < MAX_HARTS; i++) {
        table->entries[i].addr = 0;
        table->entries[i].valid = false;
    }
    atomic_init(&table->any_active, false);
    if (pthread_mutex_init(&table->lock, NULL) != 0)
        fail("reservation lock init failed");
}

static void reservation_table_destroy(reservation_table_t *table)
{
    pthread_mutex_destroy(&table->lock);
}

static void reservation_recompute_any_locked(reservation_table_t *table)
{
    bool any_active = false;

    for (int i = 0; i < MAX_HARTS; i++)
        any_active |= table->entries[i].valid;
    atomic_store_explicit(&table->any_active, any_active, memory_order_relaxed);
}

static void lr_locked(reservation_table_t *table,
                      unsigned hart_id,
                      atomic_uint *cell,
                      uint32_t phys_addr,
                      uint32_t *value)
{
    pthread_mutex_lock(&table->lock);
    *value = atomic_load_explicit(cell, memory_order_relaxed);
    table->entries[hart_id].addr = reservation_addr(phys_addr);
    table->entries[hart_id].valid = true;
    atomic_store_explicit(&table->any_active, true, memory_order_relaxed);
    pthread_mutex_unlock(&table->lock);
}

static void invalidate_locked(reservation_table_t *table, uint32_t phys_addr)
{
    uint32_t addr = reservation_addr(phys_addr);

    for (int i = 0; i < MAX_HARTS; i++) {
        if (table->entries[i].valid && table->entries[i].addr == addr)
            table->entries[i].valid = false;
    }
    reservation_recompute_any_locked(table);
}

static void regular_store_locked(reservation_table_t *table,
                                 atomic_uint *cell,
                                 uint32_t phys_addr,
                                 uint32_t value)
{
    pthread_mutex_lock(&table->lock);
    atomic_store_explicit(cell, value, memory_order_relaxed);
    invalidate_locked(table, phys_addr);
    pthread_mutex_unlock(&table->lock);
}

static bool sc_locked(reservation_table_t *table,
                      unsigned hart_id,
                      atomic_uint *cell,
                      uint32_t phys_addr,
                      uint32_t value)
{
    bool success;

    pthread_mutex_lock(&table->lock);
    success = table->entries[hart_id].valid &&
              table->entries[hart_id].addr == reservation_addr(phys_addr);
    if (success) {
        atomic_store_explicit(cell, value, memory_order_relaxed);
        invalidate_locked(table, phys_addr);
    } else {
        table->entries[hart_id].valid = false;
        reservation_recompute_any_locked(table);
    }
    pthread_mutex_unlock(&table->lock);
    return success;
}

static uint32_t amo_fetch_minmax(atomic_uint *cell,
                                 uint32_t rhs,
                                 bool signed_compare,
                                 bool take_max)
{
    uint32_t old = atomic_load_explicit(cell, memory_order_seq_cst);

    for (;;) {
        bool take_rhs;
        if (signed_compare) {
            int32_t lhs_s = (int32_t) old;
            int32_t rhs_s = (int32_t) rhs;
            take_rhs = take_max ? rhs_s > lhs_s : rhs_s < lhs_s;
        } else {
            take_rhs = take_max ? rhs > old : rhs < old;
        }

        uint32_t desired = take_rhs ? rhs : old;
        if (atomic_compare_exchange_weak_explicit(cell, &old, desired,
                                                  memory_order_seq_cst,
                                                  memory_order_seq_cst))
            return old;
    }
}

typedef struct {
    atomic_uint *cell;
    start_gate_t *gate;
} amo_add_arg_t;

static void *amo_add_thread(void *opaque)
{
    amo_add_arg_t *arg = opaque;

    gate_arrive_and_wait(arg->gate);
    for (int i = 0; i < AMO_ITERATIONS; i++)
        atomic_fetch_add_explicit(arg->cell, 1, memory_order_seq_cst);
    return NULL;
}

static void test_amo_fetch_add_is_atomic(void)
{
    pthread_t threads[AMO_THREADS];
    atomic_uint cell;
    start_gate_t gate = {0};
    amo_add_arg_t arg = {.cell = &cell, .gate = &gate};

    atomic_init(&cell, 0);
    for (int i = 0; i < AMO_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, amo_add_thread, &arg) != 0)
            fail("pthread_create AMOADD failed");
    }

    gate_release(&gate, AMO_THREADS);
    for (int i = 0; i < AMO_THREADS; i++)
        pthread_join(threads[i], NULL);

    require_u32("AMOADD preserves concurrent increments",
                atomic_load_explicit(&cell, memory_order_relaxed),
                AMO_THREADS * AMO_ITERATIONS);
}

static void test_amo_minmax_semantics(void)
{
    atomic_uint cell;

    atomic_init(&cell, 10);
    require_u32("AMOMIN returns old", amo_fetch_minmax(&cell, 5, true, false),
                10);
    require_u32("AMOMIN stores signed min",
                atomic_load_explicit(&cell, memory_order_relaxed), 5);

    atomic_store_explicit(&cell, (uint32_t) -1, memory_order_relaxed);
    require_u32("AMOMAX returns old", amo_fetch_minmax(&cell, 1, true, true),
                (uint32_t) -1);
    require_u32("AMOMAX keeps signed max",
                atomic_load_explicit(&cell, memory_order_relaxed), 1);

    atomic_store_explicit(&cell, 1, memory_order_relaxed);
    require_u32("AMOMAXU returns old",
                amo_fetch_minmax(&cell, 0xffffffffu, false, true), 1);
    require_u32("AMOMAXU stores unsigned max",
                atomic_load_explicit(&cell, memory_order_relaxed), 0xffffffffu);
}

typedef struct {
    reservation_table_t *table;
    atomic_uint *cell;
    atomic_uint successes;
    start_gate_t *gate;
    unsigned hart_id;
} lrsc_arg_t;

static void *lrsc_thread(void *opaque)
{
    lrsc_arg_t *arg = opaque;

    gate_arrive_and_wait(arg->gate);
    for (int i = 0; i < LRSC_ITERATIONS; i++) {
        uint32_t value;
        lr_locked(arg->table, arg->hart_id, arg->cell, 0x1000, &value);
        if (sc_locked(arg->table, arg->hart_id, arg->cell, 0x1000, value + 1))
            atomic_fetch_add_explicit(&arg->successes, 1, memory_order_relaxed);
    }
    return NULL;
}

static void test_lr_sc_check_store_and_invalidate_are_atomic(void)
{
    reservation_table_t table;
    atomic_uint cell;
    pthread_t a, b;
    start_gate_t gate = {0};
    lrsc_arg_t args[2];

    reservation_table_init(&table);
    atomic_init(&cell, 0);
    for (unsigned i = 0; i < 2; i++) {
        args[i] = (lrsc_arg_t) {
            .table = &table,
            .cell = &cell,
            .successes = 0,
            .gate = &gate,
            .hart_id = i,
        };
    }

    if (pthread_create(&a, NULL, lrsc_thread, &args[0]) != 0 ||
        pthread_create(&b, NULL, lrsc_thread, &args[1]) != 0)
        fail("pthread_create LR/SC failed");

    gate_release(&gate, 2);
    pthread_join(a, NULL);
    pthread_join(b, NULL);

    uint32_t successes =
        atomic_load_explicit(&args[0].successes, memory_order_relaxed) +
        atomic_load_explicit(&args[1].successes, memory_order_relaxed);
    require_u32("successful SC count matches memory", atomic_load(&cell),
                successes);
    require_bool("reservation table inactive after SC loop",
                 atomic_load_explicit(&table.any_active, memory_order_relaxed),
                 false);
    reservation_table_destroy(&table);
}

static void test_regular_store_invalidates_lr(void)
{
    reservation_table_t table;
    atomic_uint cell;
    uint32_t value;

    reservation_table_init(&table);
    atomic_init(&cell, 0x11111111u);

    lr_locked(&table, 0, &cell, 0x2000, &value);
    regular_store_locked(&table, &cell, 0x2002, 0x22222222u);
    require_bool("same-word store invalidates LR", table.entries[0].valid,
                 false);
    require_bool("SC fails after invalidating store",
                 sc_locked(&table, 0, &cell, 0x2000, 0x33333333u), false);
    require_u32("failed SC leaves regular store value",
                atomic_load_explicit(&cell, memory_order_relaxed), 0x22222222u);

    reservation_table_destroy(&table);
}

int main(void)
{
    test_amo_fetch_add_is_atomic();
    test_amo_minmax_semantics();
    test_lr_sc_check_store_and_invalidate_are_atomic();
    test_regular_store_invalidates_lr();
    return 0;
}
