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
    hart_t harts[2];
    hart_t *hart_slots[2];
    hart_wait_t hart_wait[2];
} hsm_fixture_t;

static void fixture_init(hsm_fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));

    fixture->emu.vm.n_hart = 2;
    fixture->emu.vm.hart = fixture->hart_slots;
    fixture->emu.hart_wait = fixture->hart_wait;

    for (uint32_t i = 0; i < fixture->emu.vm.n_hart; i++) {
        fixture->hart_slots[i] = &fixture->harts[i];
        if (pthread_mutex_init(&fixture->hart_wait[i].mutex, NULL) != 0)
            fail("hart wait mutex init failed");
        if (pthread_cond_init(&fixture->hart_wait[i].cond, NULL) != 0)
            fail("hart wait cond init failed");

        fixture->harts[i].priv = &fixture->emu;
        fixture->harts[i].vm = &fixture->emu.vm;
        fixture->harts[i].mhartid = i;
        fixture->harts[i].s_mode = true;
        hart_hsm_status_store(&fixture->harts[i], SBI_HSM_STATE_STOPPED);
    }

    hart_hsm_status_store(&fixture->harts[0], SBI_HSM_STATE_STARTED);
}

static void fixture_destroy(hsm_fixture_t *fixture)
{
    for (uint32_t i = 0; i < fixture->emu.vm.n_hart; i++) {
        pthread_cond_destroy(&fixture->hart_wait[i].cond);
        pthread_mutex_destroy(&fixture->hart_wait[i].mutex);
    }
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
    hart_t *hart = &fixture->harts[0];

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
    hart_t *hart = &fixture.harts[0];
    fixture_init(&fixture);

    uint32_t stale_page_table = 0;
    hart->pc = 0x1004;
    hart->satp = 0xdeadbeef;
    hart->page_table = &stale_page_table;
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
    require_bool("non-retentive resume clears page table",
                 hart->page_table == NULL, true);
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
    hart_t *hart = &fixture.harts[0];
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

static void test_suspend_pending_interrupt_resumes_after_finalize(void)
{
    hsm_fixture_t fixture;
    hart_t *hart = &fixture.harts[0];
    fixture_init(&fixture);

    uint32_t stale_page_table = 0;
    hart->pc = 0x3004;
    hart->satp = 0x80000001u;
    hart->page_table = &stale_page_table;
    hart->sstatus_sie = true;
    hart_sie_store(hart, RV_INT_SSI_BIT);
    hart_sip_set_bits(hart, RV_INT_SSI_BIT);

    prepare_hsm_ecall(hart, SBI_HSM__HART_SUSPEND);
    hart->x_regs[RV_R_A0] = 0x80000000u;
    hart->x_regs[RV_R_A1] = 0x91234000u;
    hart->x_regs[RV_R_A2] = 0x1234abcd;

    sbi_ret_t suspend_ret = handle_sbi_ecall_HSM(hart, SBI_HSM__HART_SUSPEND);
    require_int("HART_SUSPEND handler returns success before parking",
                suspend_ret.error, SBI_SUCCESS);
    require_int("HART_SUSPEND enters suspend-pending",
                hart_hsm_status_load(hart), SBI_HSM_STATE_SUSPEND_PENDING);
    require_int("HART_SUSPEND asks execution loop to park", hart->error,
                ERR_USER);

    semu_finish_hsm_park(&fixture.emu, hart);
    hart->error = ERR_NONE;
    require_int("pending interrupt resumes finalized suspend",
                hart_hsm_status_load(hart), SBI_HSM_STATE_STARTED);
    require_bool("resume context remains pending after wake",
                 hart_hsm_resume_pending_load(hart), true);

    semu_process_hsm_resume_if_started(hart);
    require_bool("started-slice hook clears interrupt wake pending flag",
                 hart_hsm_resume_pending_load(hart), false);
    require_u32("interrupt wake non-retentive resume jumps to resume_addr",
                hart->pc, 0x91234000u);
    require_u32("interrupt wake non-retentive resume clears satp", hart->satp,
                0);
    require_bool("interrupt wake non-retentive resume clears page table",
                 hart->page_table == NULL, true);
    require_bool("interrupt wake non-retentive resume clears SIE",
                 hart->sstatus_sie, false);
    require_u32("interrupt wake non-retentive resume passes opaque in a1",
                hart->x_regs[RV_R_A1], 0x1234abcdu);

    fixture_destroy(&fixture);
}

static void test_stop_pending_blocks_restart_until_stop_is_final(void)
{
    hsm_fixture_t fixture;
    fixture_init(&fixture);

    hart_t *starter = &fixture.harts[0];
    hart_t *target = &fixture.harts[1];
    hart_hsm_status_store(target, SBI_HSM_STATE_STARTED);
    uint32_t stale_page_table = 0;
    target->pc = 0x1004;
    target->satp = 0x80000001u;
    target->page_table = &stale_page_table;

    sbi_ret_t stop_ret = handle_sbi_ecall_HSM(target, SBI_HSM__HART_STOP);
    require_int("HART_STOP returns success before parking", stop_ret.error,
                SBI_SUCCESS);
    require_int("HART_STOP enters stop-pending", hart_hsm_status_load(target),
                SBI_HSM_STATE_STOP_PENDING);
    require_int("HART_STOP asks execution loop to park", target->error,
                ERR_USER);

    starter->x_regs[RV_R_A0] = target->mhartid;
    starter->x_regs[RV_R_A1] = 0x81234000u;
    starter->x_regs[RV_R_A2] = 0xabcdef01u;
    sbi_ret_t early_start = handle_sbi_ecall_HSM(starter, SBI_HSM__HART_START);
    require_int("HART_START rejects stop-pending target", early_start.error,
                SBI_ERR_ALREADY_AVAILABLE);
    require_int("early HART_START leaves target stop-pending",
                hart_hsm_status_load(target), SBI_HSM_STATE_STOP_PENDING);
    require_u32("early HART_START does not replace target pc", target->pc,
                0x1004u);

    semu_finish_hsm_park(&fixture.emu, target);
    target->error = ERR_NONE;
    require_int("stop finalize publishes STOPPED", hart_hsm_status_load(target),
                SBI_HSM_STATE_STOPPED);

    sbi_ret_t start_ret = handle_sbi_ecall_HSM(starter, SBI_HSM__HART_START);
    require_int("HART_START succeeds after STOPPED is final", start_ret.error,
                SBI_SUCCESS);
    require_int("HART_START publishes STARTED", hart_hsm_status_load(target),
                SBI_HSM_STATE_STARTED);
    require_u32("HART_START installs start address", target->pc, 0x81234000u);
    require_u32("HART_START clears satp", target->satp, 0);
    require_bool("HART_START clears page table", target->page_table == NULL,
                 true);
    require_u32("HART_START passes hartid in a0", target->x_regs[RV_R_A0], 1);
    require_u32("HART_START passes opaque in a1", target->x_regs[RV_R_A1],
                0xabcdef01u);

    fixture_destroy(&fixture);
}

int main(void)
{
    test_nonretentive_suspend_fast_resume_applies_resume_context();
    test_retentive_suspend_fast_resume_preserves_context();
    test_suspend_pending_interrupt_resumes_after_finalize();
    test_stop_pending_blocks_restart_until_stop_is_final();
    return 0;
}
