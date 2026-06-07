#include <stdio.h>
#include <stdlib.h>

#include "device.h"
#include "riscv.h"
#include "riscv_private.h"

enum {
    PLIC_ENABLE_BASE = 0x2000,
    PLIC_ENABLE_STRIDE = 0x80,
    PLIC_CONTEXT_BASE = 0x200000,
    PLIC_CONTEXT_STRIDE = 0x1000,
    PLIC_CLAIM_OFFSET = 0x4,
    TEST_IRQ = 7,
    TEST_IRQ_BIT = 1U << TEST_IRQ,
};

void vm_set_exception(hart_t *vm, uint32_t cause, uint32_t val)
{
    vm->error = ERR_EXCEPTION;
    vm->exc_cause = cause;
    vm->exc_val = val;
}

static void require_int(const char *name, uint32_t got, uint32_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got 0x%x, want 0x%x\n", name, got, want);
    exit(1);
}

static void clear_exception(hart_t *hart)
{
    hart->error = ERR_NONE;
    hart->exc_cause = 0;
    hart->exc_val = 0;
}

static void test_enable_context_two(void)
{
    hart_t hart = {0};
    plic_state_t plic = {0};
    uint32_t value = 0;

    plic_write(&hart, &plic, PLIC_ENABLE_BASE + 2 * PLIC_ENABLE_STRIDE,
               RV_MEM_SW, TEST_IRQ_BIT);

    require_int("enable context 2 exception", hart.error, ERR_NONE);
    require_int("enable context 2 target", plic.ie[2], TEST_IRQ_BIT);
    require_int("enable context 0 untouched", plic.ie[0], 0);

    plic_read(&hart, &plic, PLIC_ENABLE_BASE + 2 * PLIC_ENABLE_STRIDE,
              RV_MEM_LW, &value);

    require_int("read enable context 2 exception", hart.error, ERR_NONE);
    require_int("read enable context 2 value", value, TEST_IRQ_BIT);
}

static void test_claim_context_two(void)
{
    hart_t hart = {0};
    plic_state_t plic = {
        .ip = TEST_IRQ_BIT,
    };
    uint32_t value = 0;

    plic.ie[2] = TEST_IRQ_BIT;
    clear_exception(&hart);

    plic_read(&hart, &plic,
              PLIC_CONTEXT_BASE + 2 * PLIC_CONTEXT_STRIDE + PLIC_CLAIM_OFFSET,
              RV_MEM_LW, &value);

    require_int("claim context 2 exception", hart.error, ERR_NONE);
    require_int("claim context 2 irq", value, TEST_IRQ);
    require_int("claim clears pending bit", plic.ip & TEST_IRQ_BIT, 0);
}

int main(void)
{
    test_enable_context_two();
    test_claim_context_two();
    return 0;
}
