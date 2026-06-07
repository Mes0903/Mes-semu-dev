#include <stdio.h>
#include <stdlib.h>

#include "device.h"
#include "riscv.h"
#include "riscv_private.h"

void vm_set_exception(hart_t *vm, uint32_t cause, uint32_t val)
{
    vm->error = ERR_EXCEPTION;
    vm->exc_cause = cause;
    vm->exc_val = val;
}

uint64_t semu_timer_get(semu_timer_t *timer)
{
    (void) timer;
    return 0;
}

void semu_timer_rebase(semu_timer_t *timer, uint64_t time)
{
    (void) timer;
    (void) time;
}

static void require_int(const char *name, uint32_t got, uint32_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got 0x%x, want 0x%x\n", name, got, want);
    exit(1);
}

static void test_mswi_pending_survives_sswi_clear(void)
{
    uint32_t msip[1] = {1};
    uint32_t ssip[1] = {0};
    hart_t hart = {.mhartid = 0};
    mswi_state_t mswi = {.msip = msip, .n_hart = 1};
    sswi_state_t sswi = {.ssip = ssip, .n_hart = 1};

    aclint_mswi_update_interrupts(&hart, &mswi);
    aclint_sswi_update_interrupts(&hart, &sswi);

    require_int("MSWI pending keeps SSI set", hart.sip & RV_INT_SSI_BIT,
                RV_INT_SSI_BIT);
}


static void test_combined_swi_update_clears_when_no_source_pending(void)
{
    uint32_t msip[1] = {0};
    uint32_t ssip[1] = {0};
    hart_t hart = {.mhartid = 0, .sip = RV_INT_SSI_BIT};
    mswi_state_t mswi = {.msip = msip, .n_hart = 1};
    sswi_state_t sswi = {.ssip = ssip, .n_hart = 1};

    aclint_swi_update_interrupts(&hart, &mswi, &sswi);

    require_int("combined SWI clears SSI", hart.sip & RV_INT_SSI_BIT, 0);
}

int main(void)
{
    test_mswi_pending_survives_sswi_clear();
    test_combined_swi_update_clears_when_no_source_pending();
    return 0;
}
