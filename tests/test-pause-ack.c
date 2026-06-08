#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SEMU_PAUSE_ACK_TEST_HOOKS 1
#define main semu_main_under_test
#include "../main.c"
#undef main

static void require_int(const char *name, int got, int want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %d, want %d\n", name, got, want);
    exit(1);
}

static void require_int_negative(const char *name, int got)
{
    if (got < 0)
        return;

    fprintf(stderr, "%s: got %d, want negative error\n", name, got);
    exit(1);
}

static void require_u32(const char *name, uint32_t got, uint32_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %u, want %u\n", name, got, want);
    exit(1);
}

static void require_u64(const char *name, uint64_t got, uint64_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %llu, want %llu\n", name, (unsigned long long) got,
            (unsigned long long) want);
    exit(1);
}

static void sleep_for_ns(long ns)
{
    struct timespec ts = {.tv_sec = 0, .tv_nsec = ns};
    nanosleep(&ts, NULL);
}

static void wait_until(const char *name, bool (*predicate)(void *), void *arg)
{
    for (int i = 0; i < 2000; i++) {
        if (predicate(arg))
            return;
        sleep_for_ns(1000000L);
    }

    fprintf(stderr, "%s: timed out\n", name);
    exit(1);
}

static void test_emu_init(emu_state_t *emu, uint32_t n_hart)
{
    memset(emu, 0, sizeof(*emu));
    require_int("lifecycle init", semu_vm_lifecycle_init(&emu->lifecycle), 0);
    require_int("enter running",
                semu_vm_lifecycle_enter_running(&emu->lifecycle), 0);
    require_int("rfence init", semu_rfence_init(emu), 0);

    emu->vm.n_hart = n_hart;
    semu_timer_init(&emu->mtimer.mtime, CLOCK_FREQ, n_hart);
    emu->mtimer.mtimecmp = calloc(n_hart, sizeof(*emu->mtimer.mtimecmp));
    emu->mtimer.n_hart = n_hart;
    emu->mswi.msip = calloc(n_hart, sizeof(*emu->mswi.msip));
    emu->mswi.n_hart = n_hart;
    emu->sswi.ssip = calloc(n_hart, sizeof(*emu->sswi.ssip));
    emu->sswi.n_hart = n_hart;
    emu->vm.hart = calloc(n_hart, sizeof(*emu->vm.hart));
    emu->hart_wait = calloc(n_hart, sizeof(*emu->hart_wait));
    if (!emu->mtimer.mtimecmp || !emu->mswi.msip || !emu->sswi.ssip ||
        !emu->vm.hart || !emu->hart_wait) {
        fprintf(stderr, "allocation failed\n");
        exit(1);
    }

    for (uint32_t i = 0; i < n_hart; i++) {
        hart_t *hart = calloc(1, sizeof(*hart));
        if (!hart) {
            fprintf(stderr, "hart allocation failed\n");
            exit(1);
        }
        hart->priv = emu;
        hart->vm = &emu->vm;
        hart->mhartid = i;
        semu_hart_mailbox_init(&hart->mailbox);
        hart_hsm_status_store(hart, SBI_HSM_STATE_STARTED);
        __atomic_store_n(&emu->mtimer.mtimecmp[i], UINT64_MAX,
                         __ATOMIC_RELAXED);
        emu->vm.hart[i] = hart;

        if (pthread_mutex_init(&emu->hart_wait[i].mutex, NULL) != 0 ||
            pthread_cond_init(&emu->hart_wait[i].cond, NULL) != 0) {
            fprintf(stderr, "hart wait init failed\n");
            exit(1);
        }
    }
}

static void test_emu_destroy(emu_state_t *emu)
{
    for (uint32_t i = 0; i < emu->vm.n_hart; i++) {
        pthread_cond_destroy(&emu->hart_wait[i].cond);
        pthread_mutex_destroy(&emu->hart_wait[i].mutex);
        free(emu->vm.hart[i]);
    }
    free(emu->hart_wait);
    free(emu->vm.hart);
    free(emu->sswi.ssip);
    free(emu->mswi.msip);
    free(emu->mtimer.mtimecmp);
    pthread_cond_destroy(&emu->rfence.completion_cond);
    pthread_mutex_destroy(&emu->rfence.completion_mutex);
    pthread_mutex_destroy(&emu->rfence.issue_mutex);
    semu_vm_lifecycle_destroy(&emu->lifecycle);
}

struct pause_safe_point_ctx {
    emu_state_t *emu;
    hart_t *hart;
    _Atomic bool done;
    _Atomic int ret;
};

static bool pause_request_visible(void *arg)
{
    struct pause_safe_point_ctx *ctx = arg;
    return hart_pause_pending_load(ctx->hart) || emu_stopped_load(ctx->emu);
}

static void *pause_safe_point_thread(void *arg)
{
    struct pause_safe_point_ctx *ctx = arg;

    wait_until("pause request visible", pause_request_visible, ctx);
    int ret = semu_hart_pause_safe_point(ctx->hart);
    __atomic_store_n(&ctx->ret, ret, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->done, true, __ATOMIC_RELEASE);
    return NULL;
}

static void test_single_thread_scheduler_round_robins_started_harts(void)
{
    emu_state_t emu;
    test_emu_init(&emu, 3);
    hart_hsm_status_store(emu.vm.hart[1], SBI_HSM_STATE_STOPPED);

    uint32_t next_hart = 0;
    hart_t *hart = NULL;
    require_int(
        "single-thread pick first started",
        semu_single_thread_pick_next_started_hart(&emu, &next_hart, &hart),
        true);
    require_u32("single-thread first hart", hart->mhartid, 0);
    require_u32("single-thread cursor after first", next_hart, 1);

    require_int(
        "single-thread skips stopped hart",
        semu_single_thread_pick_next_started_hart(&emu, &next_hart, &hart),
        true);
    require_u32("single-thread second hart", hart->mhartid, 2);
    require_u32("single-thread cursor wraps", next_hart, 0);

    hart_hsm_status_store(emu.vm.hart[1], SBI_HSM_STATE_STARTED);
    next_hart = 1;
    require_int(
        "single-thread sees HSM-started hart",
        semu_single_thread_pick_next_started_hart(&emu, &next_hart, &hart),
        true);
    require_u32("single-thread HSM-started hart", hart->mhartid, 1);

    hart_hsm_status_store(emu.vm.hart[0], SBI_HSM_STATE_STOPPED);
    hart_hsm_status_store(emu.vm.hart[1], SBI_HSM_STATE_STOPPED);
    hart_hsm_status_store(emu.vm.hart[2], SBI_HSM_STATE_STOPPED);
    require_int(
        "single-thread reports no started harts",
        semu_single_thread_pick_next_started_hart(&emu, &next_hart, &hart),
        false);

    test_emu_destroy(&emu);
}

static void test_pause_targets_only_started_harts_captured_at_request(void)
{
    emu_state_t emu;
    test_emu_init(&emu, 3);
    hart_hsm_status_store(emu.vm.hart[1], SBI_HSM_STATE_STOPPED);
    hart_hsm_status_store(emu.vm.hart[2], SBI_HSM_STATE_SUSPENDED);

    struct pause_safe_point_ctx ctx = {.emu = &emu, .hart = emu.vm.hart[0]};
    pthread_t thread;
    pthread_create(&thread, NULL, pause_safe_point_thread, &ctx);

    require_int("pause all harts", semu_pause_all_harts(&emu), 0);
    require_int("lifecycle paused", semu_vm_lifecycle_state(&emu.lifecycle),
                SEMU_VM_PAUSED);
    require_u64("started hart request seq",
                hart_pause_request_seq_load(emu.vm.hart[0]),
                semu_vm_lifecycle_pause_seq(&emu.lifecycle));
    require_u64("started hart ack seq", hart_pause_ack_seq_load(emu.vm.hart[0]),
                semu_vm_lifecycle_pause_seq(&emu.lifecycle));
    require_u64("stopped hart not requested",
                hart_pause_request_seq_load(emu.vm.hart[1]), 0);
    require_u64("suspended hart not requested",
                hart_pause_request_seq_load(emu.vm.hart[2]), 0);

    require_int("resume lifecycle",
                semu_vm_lifecycle_enter_running(&emu.lifecycle), 0);
    pthread_join(thread, NULL);
    require_int("safe point thread ret",
                __atomic_load_n(&ctx.ret, __ATOMIC_ACQUIRE), 0);
    test_emu_destroy(&emu);
}

static bool hart_is_in_wfi(void *arg)
{
    return hart_in_wfi_load(arg);
}

static void *wfi_thread(void *arg)
{
    wfi_handler_threaded(arg);
    return NULL;
}

static void test_pause_wakes_wfi_started_hart_and_acknowledges(void)
{
    emu_state_t emu;
    test_emu_init(&emu, 1);

    pthread_t thread;
    pthread_create(&thread, NULL, wfi_thread, emu.vm.hart[0]);
    wait_until("hart entered WFI", hart_is_in_wfi, emu.vm.hart[0]);

    require_int("pause WFI hart", semu_pause_all_harts(&emu), 0);
    uint64_t seq = semu_vm_lifecycle_pause_seq(&emu.lifecycle);
    require_u64("WFI hart ack seq", hart_pause_ack_seq_load(emu.vm.hart[0]),
                seq);

    require_int("resume lifecycle",
                semu_vm_lifecycle_enter_running(&emu.lifecycle), 0);
    semu_set_stopped(&emu, true);
    semu_signal_hart(&emu, 0);
    pthread_join(thread, NULL);
    test_emu_destroy(&emu);
}

struct progress_ctx {
    emu_state_t *emu;
    hart_t *hart;
    _Atomic uint32_t progress;
    uint32_t resume_threshold;
};

static void *progress_thread(void *arg)
{
    struct progress_ctx *ctx = arg;

    while (!emu_stopped_load(ctx->emu)) {
        int ret = semu_hart_pause_safe_point(ctx->hart);
        if (ret < 0)
            break;
        __atomic_add_fetch(&ctx->progress, 1, __ATOMIC_ACQ_REL);
        sleep_for_ns(1000000L);
    }
    return NULL;
}

static bool progress_at_least_three(void *arg)
{
    struct progress_ctx *ctx = arg;
    return __atomic_load_n(&ctx->progress, __ATOMIC_ACQUIRE) >= 3;
}

static bool progress_above_paused(void *arg)
{
    struct progress_ctx *ctx = arg;
    return __atomic_load_n(&ctx->progress, __ATOMIC_ACQUIRE) >
           ctx->resume_threshold;
}

static void test_hart_does_not_progress_after_pause_ack_until_resume(void)
{
    emu_state_t emu;
    test_emu_init(&emu, 1);

    struct progress_ctx ctx = {.emu = &emu, .hart = emu.vm.hart[0]};
    pthread_t thread;
    pthread_create(&thread, NULL, progress_thread, &ctx);
    wait_until("initial progress", progress_at_least_three, &ctx);

    require_int("pause progressing hart", semu_pause_all_harts(&emu), 0);
    uint32_t paused_progress =
        __atomic_load_n(&ctx.progress, __ATOMIC_ACQUIRE);
    ctx.resume_threshold = paused_progress;
    sleep_for_ns(20000000L);
    require_u64("progress stable while paused",
                __atomic_load_n(&ctx.progress, __ATOMIC_ACQUIRE),
                paused_progress);

    require_int("resume lifecycle",
                semu_vm_lifecycle_enter_running(&emu.lifecycle), 0);
    wait_until("progress resumed", progress_above_paused, &ctx);
    semu_set_stopped(&emu, true);
    semu_signal_hart(&emu, 0);
    pthread_join(thread, NULL);
    test_emu_destroy(&emu);
}

struct coordinator_ctx {
    emu_state_t *emu;
    _Atomic bool done;
    _Atomic int ret;
};

static void *coordinator_thread(void *arg)
{
    struct coordinator_ctx *ctx = arg;
    int ret = semu_pause_all_harts(ctx->emu);
    __atomic_store_n(&ctx->ret, ret, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->done, true, __ATOMIC_RELEASE);
    return NULL;
}

static bool pause_requested(void *arg)
{
    emu_state_t *emu = arg;
    return semu_vm_lifecycle_state(&emu->lifecycle) == SEMU_VM_PAUSE_REQUESTED;
}

static bool coordinator_done(void *arg)
{
    struct coordinator_ctx *ctx = arg;
    return __atomic_load_n(&ctx->done, __ATOMIC_ACQUIRE);
}

static void test_paused_hart_services_rfence_without_guest_progress(void)
{
    emu_state_t emu;
    test_emu_init(&emu, 1);

    struct progress_ctx ctx = {.emu = &emu, .hart = emu.vm.hart[0]};
    pthread_t thread;
    pthread_create(&thread, NULL, progress_thread, &ctx);
    wait_until("initial progress for rfence", progress_at_least_three, &ctx);

    require_int("pause progressing hart for rfence", semu_pause_all_harts(&emu),
                0);
    uint32_t paused_progress =
        __atomic_load_n(&ctx.progress, __ATOMIC_ACQUIRE);

    emu.rfence.start_addr = 0x1000;
    emu.rfence.size = 0x1000;
    semu_rfence_type_store(&emu, SEMU_RFENCE_VMA);
    semu_rfence_pending_count_store(&emu, 1);
    hart_pending_rfence_store(emu.vm.hart[0], true);
    semu_signal_hart(&emu, 0);

    for (int i = 0; i < 2000 && semu_rfence_pending_count_load(&emu) != 0;
         i++)
        sleep_for_ns(1000000L);

    require_int("paused hart rfence ack", semu_rfence_pending_count_load(&emu),
                0);
    require_u32("paused hart made no guest progress",
                __atomic_load_n(&ctx.progress, __ATOMIC_ACQUIRE),
                paused_progress);

    require_int("resume lifecycle after rfence",
                semu_vm_lifecycle_enter_running(&emu.lifecycle), 0);
    ctx.resume_threshold = paused_progress;
    wait_until("progress resumed after rfence", progress_above_paused, &ctx);
    semu_set_stopped(&emu, true);
    semu_signal_hart(&emu, 0);
    pthread_join(thread, NULL);
    test_emu_destroy(&emu);
}

static void test_hsm_start_rejected_while_pause_active(void)
{
    emu_state_t emu;
    test_emu_init(&emu, 2);
    hart_t *starter = emu.vm.hart[0];
    hart_t *target = emu.vm.hart[1];
    hart_hsm_status_store(target, SBI_HSM_STATE_STOPPED);

    require_int("request pause for HSM start", semu_vm_lifecycle_request_pause(
                    &emu.lifecycle), 0);
    starter->x_regs[RV_R_A0] = 1;
    starter->x_regs[RV_R_A1] = 0x81234000u;
    starter->x_regs[RV_R_A2] = 0x55aa1234u;

    sbi_ret_t ret = handle_sbi_ecall_HSM(starter, SBI_HSM__HART_START);

    require_int("HART_START rejected during pause", ret.error, SBI_ERR_FAILED);
    require_int("paused HART_START leaves target stopped",
                hart_hsm_status_load(target), SBI_HSM_STATE_STOPPED);
    require_u64("paused HART_START does not publish pause request",
                hart_pause_request_seq_load(target), 0);

    semu_set_stopped(&emu, true);
    test_emu_destroy(&emu);
}


static _Atomic bool hsm_publish_hook_called;
static _Atomic bool hsm_publish_saw_lifecycle_locked;

static void hsm_start_publish_lock_probe(emu_state_t *emu, hart_t *target)
{
    (void) target;

    int ret = pthread_mutex_trylock(&emu->lifecycle.lock);
    if (ret == EBUSY) {
        __atomic_store_n(&hsm_publish_saw_lifecycle_locked, true,
                         __ATOMIC_RELEASE);
    } else if (ret == 0) {
        __atomic_store_n(&hsm_publish_saw_lifecycle_locked, false,
                         __ATOMIC_RELEASE);
        pthread_mutex_unlock(&emu->lifecycle.lock);
    } else {
        fprintf(stderr, "lifecycle trylock failed: %d\n", ret);
        exit(1);
    }
    __atomic_store_n(&hsm_publish_hook_called, true, __ATOMIC_RELEASE);
}

static void test_hsm_start_holds_lifecycle_lock_until_started_publish(void)
{
    emu_state_t emu;
    test_emu_init(&emu, 2);
    hart_t *starter = emu.vm.hart[0];
    hart_t *target = emu.vm.hart[1];
    hart_hsm_status_store(target, SBI_HSM_STATE_STOPPED);

    starter->x_regs[RV_R_A0] = 1;
    starter->x_regs[RV_R_A1] = 0x81234000u;
    starter->x_regs[RV_R_A2] = 0x55aa1234u;
    __atomic_store_n(&hsm_publish_hook_called, false, __ATOMIC_RELEASE);
    __atomic_store_n(&hsm_publish_saw_lifecycle_locked, false,
                     __ATOMIC_RELEASE);
    semu_hsm_start_before_publish_hook = hsm_start_publish_lock_probe;

    sbi_ret_t ret = handle_sbi_ecall_HSM(starter, SBI_HSM__HART_START);

    semu_hsm_start_before_publish_hook = NULL;
    require_int("HART_START succeeds while running", ret.error, SBI_SUCCESS);
    require_int("HART_START publishes target started",
                hart_hsm_status_load(target), SBI_HSM_STATE_STARTED);
    require_int("HART_START publish hook ran",
                __atomic_load_n(&hsm_publish_hook_called, __ATOMIC_ACQUIRE),
                true);
    require_int("HART_START publish held lifecycle lock",
                __atomic_load_n(&hsm_publish_saw_lifecycle_locked,
                                __ATOMIC_ACQUIRE),
                true);

    semu_set_stopped(&emu, true);
    test_emu_destroy(&emu);
}


static void test_suspended_interrupt_resume_defers_while_paused(void)
{
    emu_state_t emu;
    test_emu_init(&emu, 1);
    hart_t *hart = emu.vm.hart[0];
    hart_hsm_status_store(hart, SBI_HSM_STATE_SUSPENDED);
    hart_sie_store(hart, RV_INT_SSI_BIT);
    hart_sip_set_bits(hart, RV_INT_SSI_BIT);

    require_int("pause skips suspended hart", semu_pause_all_harts(&emu), 0);
    require_int("pause reached paused", semu_vm_lifecycle_state(&emu.lifecycle),
                SEMU_VM_PAUSED);

    semu_wake_hart_if_interrupt_pending(&emu, 0);

    require_int("paused interrupt wake keeps hart suspended",
                hart_hsm_status_load(hart), SBI_HSM_STATE_SUSPENDED);
    require_u64("paused interrupt wake does not publish pause request",
                hart_pause_request_seq_load(hart), 0);

    require_int("resume lifecycle for suspended wake",
                semu_vm_lifecycle_enter_running(&emu.lifecycle), 0);
    semu_wake_hart_if_interrupt_pending(&emu, 0);
    require_int("interrupt wake resumes after lifecycle running",
                hart_hsm_status_load(hart), SBI_HSM_STATE_STARTED);

    semu_set_stopped(&emu, true);
    test_emu_destroy(&emu);
}

static void test_stop_pending_target_acknowledges_pause_on_park(void)
{
    emu_state_t emu;
    test_emu_init(&emu, 1);
    hart_t *hart = emu.vm.hart[0];

    require_int("request pause for stop park", semu_vm_lifecycle_request_pause(
                    &emu.lifecycle), 0);
    uint64_t seq = semu_vm_lifecycle_pause_seq(&emu.lifecycle);
    hart_pause_request(hart, seq);
    hart_hsm_status_store(hart, SBI_HSM_STATE_STOP_PENDING);

    semu_finish_hsm_park(&emu, hart);

    require_int("stop park finalized", hart_hsm_status_load(hart),
                SBI_HSM_STATE_STOPPED);
    require_u64("stop park acked pause", hart_pause_ack_seq_load(hart), seq);
    require_int("pause coordinator can enter paused after stop park",
                semu_pause_wait_for_targets(&emu, (bool[]){true}, seq), 0);

    semu_set_stopped(&emu, true);
    test_emu_destroy(&emu);
}

static void test_suspend_pending_target_acknowledges_pause_on_park(void)
{
    emu_state_t emu;
    test_emu_init(&emu, 1);
    hart_t *hart = emu.vm.hart[0];

    require_int("request pause for suspend park", semu_vm_lifecycle_request_pause(
                    &emu.lifecycle), 0);
    uint64_t seq = semu_vm_lifecycle_pause_seq(&emu.lifecycle);
    hart_pause_request(hart, seq);
    hart_hsm_status_store(hart, SBI_HSM_STATE_SUSPEND_PENDING);

    semu_finish_hsm_park(&emu, hart);

    require_int("suspend park finalized", hart_hsm_status_load(hart),
                SBI_HSM_STATE_SUSPENDED);
    require_u64("suspend park acked pause", hart_pause_ack_seq_load(hart), seq);
    require_int("pause coordinator can enter paused after suspend park",
                semu_pause_wait_for_targets(&emu, (bool[]){true}, seq), 0);

    semu_set_stopped(&emu, true);
    test_emu_destroy(&emu);
}

static void test_pause_wait_returns_if_stop_happens_during_wait(void)
{
    emu_state_t emu;
    test_emu_init(&emu, 1);

    struct coordinator_ctx ctx = {.emu = &emu};
    pthread_t thread;
    pthread_create(&thread, NULL, coordinator_thread, &ctx);
    wait_until("pause requested", pause_requested, &emu);

    require_int("enter stopping",
                semu_vm_lifecycle_enter_stopping(&emu.lifecycle), 0);
    semu_set_stopped(&emu, true);
    wait_until("coordinator returned", coordinator_done, &ctx);
    pthread_join(thread, NULL);
    require_int_negative("coordinator stop ret",
                         __atomic_load_n(&ctx.ret, __ATOMIC_ACQUIRE));
    test_emu_destroy(&emu);
}

int main(void)
{
    test_single_thread_scheduler_round_robins_started_harts();
    test_pause_targets_only_started_harts_captured_at_request();
    test_pause_wakes_wfi_started_hart_and_acknowledges();
    test_hart_does_not_progress_after_pause_ack_until_resume();
    test_paused_hart_services_rfence_without_guest_progress();
    test_hsm_start_rejected_while_pause_active();
    test_hsm_start_holds_lifecycle_lock_until_started_publish();
    test_suspended_interrupt_resume_defers_while_paused();
    test_stop_pending_target_acknowledges_pause_on_park();
    test_suspend_pending_target_acknowledges_pause_on_park();
    test_pause_wait_returns_if_stop_happens_during_wait();
    return 0;
}
