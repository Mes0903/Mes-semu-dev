#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "ram_access.h"
#include "riscv.h"
#include "riscv_private.h"

enum {
    TEST_RAM_BYTES = 4096,
    TEST_RAM_WORDS = TEST_RAM_BYTES / sizeof(uint32_t),
    TEST_DATA_ADDR = 0x100,
    TEST_MMIO_ADDR = TEST_RAM_BYTES + 4,
    AMO_ITERATIONS = 50000,
    STORE_VALUE = 0x11112222u,
    SC_VALUE = 0x33334444u,
};

typedef struct {
    uint32_t ram[TEST_RAM_WORDS];
    vm_t *machine;
    bool check_mmio_store_lock;
    unsigned mmio_loads;
    unsigned mmio_stores;
    unsigned mmio_store_lock_held;
} test_mem_t;

typedef struct {
    hart_t *hart;
    int iterations;
} amo_worker_arg_t;

static void fail(const char *name)
{
    fprintf(stderr, "%s\n", name);
    exit(1);
}

static void require_u32(const char *name, uint32_t got, uint32_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got 0x%08x, want 0x%08x\n", name, got, want);
    exit(1);
}

static uint32_t encode_amo(uint8_t funct5, uint8_t rd, uint8_t rs1, uint8_t rs2)
{
    return ((uint32_t) funct5 << 27) | ((uint32_t) rs2 << 20) |
           ((uint32_t) rs1 << 15) | ((uint32_t) RV_MEM_LW << 12) |
           ((uint32_t) rd << 7) | RV32_AMO;
}

static uint32_t encode_store(uint8_t width,
                             uint8_t rs2,
                             uint8_t rs1,
                             int16_t imm)
{
    uint32_t uimm = (uint16_t) imm;

    return ((uimm & 0xfe0u) << 20) | ((uint32_t) rs2 << 20) |
           ((uint32_t) rs1 << 15) | ((uint32_t) width << 12) |
           ((uimm & 0x1fu) << 7) | RV32_STORE;
}

static void test_mem_fetch(hart_t *hart, uint32_t n_pages, uint32_t **page_addr)
{
    test_mem_t *mem = hart->priv;

    if (n_pages >= TEST_RAM_BYTES / RV_PAGE_SIZE) {
        vm_set_exception(hart, RV_EXC_FETCH_FAULT, hart->exc_val);
        return;
    }
    *page_addr = &mem->ram[n_pages << (RV_PAGE_SHIFT - 2)];
}

static void test_mem_load(hart_t *hart,
                          uint32_t addr,
                          uint8_t width,
                          uint32_t *value)
{
    test_mem_t *mem = hart->priv;

    if (addr == TEST_MMIO_ADDR && width == RV_MEM_LW) {
        mem->mmio_loads++;
        *value = 0x5a5a0000u;
        return;
    }

    vm_set_exception(hart, RV_EXC_LOAD_FAULT, hart->exc_val);
}

static void test_mem_store(hart_t *hart,
                           uint32_t addr,
                           uint8_t width,
                           uint32_t value UNUSED)
{
    test_mem_t *mem = hart->priv;

    if (addr == TEST_MMIO_ADDR && width == RV_MEM_SW) {
        if (mem->check_mmio_store_lock && mem->machine) {
            if (pthread_mutex_trylock(&mem->machine->reservation_lock) == 0)
                pthread_mutex_unlock(&mem->machine->reservation_lock);
            else
                mem->mmio_store_lock_held++;
        }
        mem->mmio_stores++;
        return;
    }

    vm_set_exception(hart, RV_EXC_STORE_FAULT, hart->exc_val);
}

static uint32_t *test_mem_page_table(const hart_t *hart, uint32_t ppn)
{
    test_mem_t *mem = hart->priv;

    if (ppn < TEST_RAM_BYTES / RV_PAGE_SIZE)
        return &mem->ram[ppn << (RV_PAGE_SHIFT - 2)];
    return NULL;
}

static void init_machine(vm_t *machine,
                         reservation_entry_t *reservations,
                         hart_t **harts,
                         uint32_t n_hart)
{
    memset(machine, 0, sizeof(*machine));
    memset(reservations, 0, sizeof(*reservations) * n_hart);
    machine->n_hart = n_hart;
    machine->hart = harts;
    machine->reservations = reservations;
    __atomic_store_n(&machine->any_reservation_active, false, __ATOMIC_RELAXED);
    if (pthread_mutex_init(&machine->reservation_lock, NULL) != 0)
        fail("reservation lock init failed");
}

static void destroy_machine(vm_t *machine)
{
    pthread_mutex_destroy(&machine->reservation_lock);
}

static void init_hart(hart_t *hart,
                      vm_t *machine,
                      test_mem_t *mem,
                      uint32_t hartid)
{
    memset(hart, 0, sizeof(*hart));
    vm_init(hart);
    hart->mhartid = hartid;
    hart->priv = mem;
    hart->vm = machine;
    hart->s_mode = true;
    hart_hsm_status_store(hart, SBI_HSM_STATE_STARTED);
    hart->mem_fetch = test_mem_fetch;
    hart->mem_load = test_mem_load;
    hart->mem_store = test_mem_store;
    hart->mem_page_table = test_mem_page_table;
    hart->ram_base = mem->ram;
    hart->ram_size = TEST_RAM_BYTES;
}

static void step_expect_ok(hart_t *hart, const char *name)
{
    vm_step(hart);
    if (hart->error) {
        fprintf(stderr, "%s: exception cause=%u val=0x%08x pc=0x%08x\n", name,
                hart->exc_cause, hart->exc_val, hart->pc);
        fail(name);
    }
}

static void test_mmio_amo_faults_without_callback_rmw(void)
{
    test_mem_t mem = {0};
    vm_t machine;
    reservation_entry_t reservations[1];
    hart_t hart;
    hart_t *harts[] = {&hart};

    init_machine(&machine, reservations, harts, 1);
    init_hart(&hart, &machine, &mem, 0);

    mem.ram[0] = encode_amo(0b00000, 5, 1, 2); /* AMOADD.W x5, x2, (x1) */
    hart.x_regs[1] = TEST_MMIO_ADDR;
    hart.x_regs[2] = 1;

    vm_step(&hart);

    require_u32("out-of-RAM AMO raises Store/AMO access fault", hart.error,
                ERR_EXCEPTION);
    require_u32("out-of-RAM AMO fault cause", hart.exc_cause,
                RV_EXC_STORE_FAULT);
    require_u32("out-of-RAM AMO does not call load callback", mem.mmio_loads,
                0);
    require_u32("out-of-RAM AMO does not call store callback", mem.mmio_stores,
                0);

    destroy_machine(&machine);
}

static void test_mmio_store_callback_runs_without_reservation_lock(void)
{
    test_mem_t mem = {0};
    vm_t machine;
    reservation_entry_t reservations[1];
    hart_t hart;
    hart_t *harts[] = {&hart};

    init_machine(&machine, reservations, harts, 1);
    init_hart(&hart, &machine, &mem, 0);
    mem.machine = &machine;
    mem.check_mmio_store_lock = true;

    mem.ram[0] = encode_store(RV_MEM_SW, 2, 1, 0);
    hart.x_regs[1] = TEST_MMIO_ADDR;
    hart.x_regs[2] = STORE_VALUE;

    step_expect_ok(&hart, "MMIO store callback lock scope");

    require_u32("MMIO store callback runs", mem.mmio_stores, 1);
    require_u32("MMIO store callback is outside reservation lock",
                mem.mmio_store_lock_held, 0);

    destroy_machine(&machine);
}

static void test_lr_sc_regular_store_invalidation(void)
{
    test_mem_t mem = {0};
    vm_t machine;
    reservation_entry_t reservations[2];
    hart_t h0, h1;
    hart_t *harts[] = {&h0, &h1};

    init_machine(&machine, reservations, harts, 2);
    init_hart(&h0, &machine, &mem, 0);
    init_hart(&h1, &machine, &mem, 1);

    mem.ram[0] = encode_amo(0b00010, 5, 1, 0);     /* LR.W x5, (x1) */
    mem.ram[1] = encode_amo(0b00011, 6, 1, 2);     /* SC.W x6, x2, (x1) */
    mem.ram[2] = encode_store(RV_MEM_SW, 3, 1, 0); /* SW x3, 0(x1) */
    mem.ram[3] = encode_store(RV_MEM_SH, 3, 1, 2); /* SH x3, 2(x1) */
    mem.ram[4] = encode_store(RV_MEM_SB, 3, 1, 1); /* SB x3, 1(x1) */
    mem.ram[TEST_DATA_ADDR >> 2] = 0;

    h0.pc = 0;
    h0.x_regs[1] = TEST_DATA_ADDR;
    h0.x_regs[2] = SC_VALUE;
    step_expect_ok(&h0, "LR before SW");
    require_u32("LR returns original word", h0.x_regs[5], 0);

    h1.pc = 8;
    h1.x_regs[1] = TEST_DATA_ADDR;
    h1.x_regs[3] = STORE_VALUE;
    step_expect_ok(&h1, "intervening SW");

    h0.pc = 4;
    step_expect_ok(&h0, "SC after SW");
    require_u32("SC after same-word SW fails", h0.x_regs[6], 1);
    require_u32("failed SC leaves SW value", mem.ram[TEST_DATA_ADDR >> 2],
                STORE_VALUE);

    h0.pc = 0;
    step_expect_ok(&h0, "LR before successful SC");
    h0.pc = 4;
    step_expect_ok(&h0, "successful SC");
    require_u32("SC succeeds while reservation is valid", h0.x_regs[6], 0);
    require_u32("successful SC writes memory", mem.ram[TEST_DATA_ADDR >> 2],
                SC_VALUE);

    h0.pc = 0;
    step_expect_ok(&h0, "LR before SH");
    h1.pc = 12;
    h1.x_regs[3] = 0x7777u;
    step_expect_ok(&h1, "intervening SH");
    h0.pc = 4;
    step_expect_ok(&h0, "SC after SH");
    require_u32("SC after same-word SH fails", h0.x_regs[6], 1);

    h0.pc = 0;
    step_expect_ok(&h0, "LR before SB");
    h1.pc = 16;
    h1.x_regs[3] = 0x88u;
    step_expect_ok(&h1, "intervening SB");
    h0.pc = 4;
    step_expect_ok(&h0, "SC after SB");
    require_u32("SC after same-word SB fails", h0.x_regs[6], 1);

    destroy_machine(&machine);
}

static void *amo_worker(void *opaque)
{
    amo_worker_arg_t *arg = opaque;

    for (int i = 0; i < arg->iterations; i++) {
        arg->hart->pc = 0;
        vm_step(arg->hart);
        if (arg->hart->error)
            return NULL;
    }
    return NULL;
}

static void test_amoadd_w_is_atomic_under_threads(void)
{
    test_mem_t mem = {0};
    vm_t machine;
    reservation_entry_t reservations[2];
    hart_t h0, h1;
    hart_t *harts[] = {&h0, &h1};
    pthread_t threads[2];
    amo_worker_arg_t args[2] = {
        {.hart = &h0, .iterations = AMO_ITERATIONS},
        {.hart = &h1, .iterations = AMO_ITERATIONS},
    };

    init_machine(&machine, reservations, harts, 2);
    init_hart(&h0, &machine, &mem, 0);
    init_hart(&h1, &machine, &mem, 1);

    mem.ram[0] = encode_amo(0b00000, 5, 1, 2); /* AMOADD.W x5, x2, (x1) */
    h0.x_regs[1] = TEST_DATA_ADDR;
    h0.x_regs[2] = 1;
    h1.x_regs[1] = TEST_DATA_ADDR;
    h1.x_regs[2] = 1;

    if (pthread_create(&threads[0], NULL, amo_worker, &args[0]) != 0 ||
        pthread_create(&threads[1], NULL, amo_worker, &args[1]) != 0)
        fail("pthread_create AMO workers failed");
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);

    if (h0.error || h1.error)
        fail("AMOADD worker raised an unexpected exception");

    require_u32("concurrent AMOADD.W increments are not lost",
                ram_load_w(&mem.ram[TEST_DATA_ADDR >> 2]), AMO_ITERATIONS * 2u);

    destroy_machine(&machine);
}

int main(void)
{
    test_mmio_amo_faults_without_callback_rmw();
    test_mmio_store_callback_runs_without_reservation_lock();
    test_lr_sc_regular_store_invalidation();
    test_amoadd_w_is_atomic_under_threads();
    return 0;
}
