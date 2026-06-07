#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main semu_main_under_test
#include "../main.c"
#undef main

static void fail(const char *name)
{
    fprintf(stderr, "%s\n", name);
    exit(1);
}

static void require_int(const char *name, int got, int want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %d, want %d\n", name, got, want);
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

typedef struct {
    emu_state_t emu;
    hart_t hart;
    hart_t *hart_slots[1];
    hart_wait_t hart_wait[1];
} hsm_fixture_t;

static void fixture_init(hsm_fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));

    fixture->emu.vm.n_hart = 1;
    fixture->emu.vm.hart = fixture->hart_slots;
    fixture->emu.hart_wait = fixture->hart_wait;
    fixture->hart_slots[0] = &fixture->hart;

    if (pthread_mutex_init(&fixture->hart_wait[0].mutex, NULL) != 0)
        fail("hart wait mutex init failed");
    if (pthread_cond_init(&fixture->hart_wait[0].cond, NULL) != 0)
        fail("hart wait cond init failed");

    fixture->hart.priv = &fixture->emu;
    fixture->hart.vm = &fixture->emu.vm;
    fixture->hart.mhartid = 0;
    fixture->hart.s_mode = true;
    hart_hsm_status_store(&fixture->hart, SBI_HSM_STATE_STARTED);
}

static void fixture_destroy(hsm_fixture_t *fixture)
{
    pthread_cond_destroy(&fixture->hart_wait[0].cond);
    pthread_mutex_destroy(&fixture->hart_wait[0].mutex);
}

static void prepare_hsm_ecall(hart_t *hart, uint32_t fid)
{
    hart->error = ERR_EXCEPTION;
    hart->exc_cause = RV_EXC_ECALL_S;
    hart->x_regs[RV_R_A6] = fid;
    hart->x_regs[RV_R_A7] = SBI_EID_HSM;
}

static void suspend_current_hart(hsm_fixture_t *fixture,
                                 uint32_t suspend_type,
                                 uint32_t resume_addr,
                                 uint32_t opaque)
{
    hart_t *hart = &fixture->hart;

    prepare_hsm_ecall(hart, SBI_HSM__HART_SUSPEND);
    hart->x_regs[RV_R_A0] = suspend_type;
    hart->x_regs[RV_R_A1] = resume_addr;
    hart->x_regs[RV_R_A2] = opaque;

    require_int("HART_SUSPEND ecall is handled",
                semu_step_chunk(&fixture->emu, hart, 8), 0);
    require_int("HART_SUSPEND parks hart", hart_hsm_status_load(hart),
                SBI_HSM_STATE_SUSPENDED);
    require_bool("HART_SUSPEND records a pending resume",
                 hart_hsm_resume_pending_load(hart), true);
}

static void test_nonretentive_suspend_fast_resume_applies_resume_context(void)
{
    hsm_fixture_t fixture;
    hart_t *hart = &fixture.hart;
    fixture_init(&fixture);

    hart->pc = 0x1004;
    hart->satp = 0xdeadbeef;
    hart->sstatus_sie = true;
    hart->s_mode = false;
    hart->x_regs[5] = 0xaaaa5555;

    suspend_current_hart(&fixture, 0x80000000u, 0x81234000u, 0x55aa77ccu);
    semu_resume_hart(&fixture.emu, hart->mhartid);
    require_int("interrupt wake resumes suspended hart",
                hart_hsm_status_load(hart), SBI_HSM_STATE_STARTED);

    semu_process_hsm_resume_if_started(hart);
    require_bool("started-slice hook clears pending flag",
                 hart_hsm_resume_pending_load(hart), false);
    require_u32("non-retentive resume jumps to resume_addr", hart->pc,
                0x81234000u);
    require_u32("non-retentive resume clears satp", hart->satp, 0);
    require_bool("non-retentive resume clears SIE", hart->sstatus_sie, false);
    require_bool("non-retentive resume enters S-mode", hart->s_mode, true);
    require_u32("non-retentive resume passes hartid in a0",
                hart->x_regs[RV_R_A0], 0);
    require_u32("non-retentive resume passes opaque in a1",
                hart->x_regs[RV_R_A1], 0x55aa77ccu);

    fixture_destroy(&fixture);
}

static void test_retentive_suspend_fast_resume_preserves_context(void)
{
    hsm_fixture_t fixture;
    hart_t *hart = &fixture.hart;
    fixture_init(&fixture);

    hart->pc = 0x2004;
    hart->satp = 0xcafebabe;
    hart->sstatus_sie = true;
    hart->s_mode = true;
    hart->x_regs[5] = 0x12345678;

    suspend_current_hart(&fixture, 0x00000000u, 0x90000000u, 0xabcdef01u);
    semu_resume_hart(&fixture.emu, hart->mhartid);
    semu_process_hsm_resume_if_started(hart);

    require_bool("started-slice hook clears retentive pending flag",
                 hart_hsm_resume_pending_load(hart), false);
    require_u32("retentive resume preserves pc", hart->pc, 0x2004u);
    require_u32("retentive resume preserves satp", hart->satp, 0xcafebabeu);
    require_bool("retentive resume preserves SIE", hart->sstatus_sie, true);
    require_u32("retentive resume preserves general register", hart->x_regs[5],
                0x12345678u);

    fixture_destroy(&fixture);
}

int main(void)
{
    test_nonretentive_suspend_fast_resume_applies_resume_context();
    test_retentive_suspend_fast_resume_preserves_context();
    return 0;
}
