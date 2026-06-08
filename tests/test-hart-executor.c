#include "device.h"
#include "riscv_private.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int failures;

static void check_true(bool condition, const char *message)
{
    if (condition)
        return;
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
}

static void *test_hart_thread(void *arg)
{
    hart_t *hart = arg;
    check_true(hart != NULL, "hart thread receives hart");
    return NULL;
}

static void *test_io_thread(void *arg)
{
    emu_state_t *emu = arg;
    check_true(emu != NULL, "I/O thread receives emulator");
    return NULL;
}


struct wake_waiter {
    hart_wait_t *wait;
    pthread_mutex_t ready_lock;
    pthread_cond_t ready_cond;
    bool ready;
    bool woke;
};

static void deadline_after_ms(struct timespec *deadline, long ms)
{
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += ms / 1000;
    deadline->tv_nsec += (ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

static void wake_waiter_init(struct wake_waiter *waiter, hart_wait_t *wait)
{
    *waiter = (struct wake_waiter) {.wait = wait};
    pthread_mutex_init(&waiter->ready_lock, NULL);
    pthread_cond_init(&waiter->ready_cond, NULL);
}

static void wake_waiter_destroy(struct wake_waiter *waiter)
{
    pthread_cond_destroy(&waiter->ready_cond);
    pthread_mutex_destroy(&waiter->ready_lock);
}

static void *wake_waiter_thread(void *arg)
{
    struct wake_waiter *waiter = arg;

    pthread_mutex_lock(&waiter->wait->mutex);
    pthread_mutex_lock(&waiter->ready_lock);
    waiter->ready = true;
    pthread_cond_signal(&waiter->ready_cond);
    pthread_mutex_unlock(&waiter->ready_lock);

    struct timespec deadline;
    deadline_after_ms(&deadline, 1000);
    int rc = pthread_cond_timedwait(&waiter->wait->cond, &waiter->wait->mutex,
                                    &deadline);
    waiter->woke = rc == 0;
    pthread_mutex_unlock(&waiter->wait->mutex);
    return NULL;
}

static void wait_for_waiter_ready(struct wake_waiter *waiter)
{
    pthread_mutex_lock(&waiter->ready_lock);
    while (!waiter->ready) {
        struct timespec deadline;
        deadline_after_ms(&deadline, 1000);
        int rc = pthread_cond_timedwait(&waiter->ready_cond,
                                        &waiter->ready_lock, &deadline);
        if (rc == ETIMEDOUT)
            break;
    }
    pthread_mutex_unlock(&waiter->ready_lock);
    check_true(waiter->ready, "waiter reached cond wait");
}

static void test_harts_init(emu_state_t *emu,
                            hart_t harts_storage[],
                            hart_t *harts[],
                            uint32_t n_hart)
{
    emu->vm.n_hart = n_hart;
    emu->vm.hart = harts;

    for (uint32_t i = 0; i < n_hart; i++) {
        harts_storage[i] = (hart_t) {
            .priv = emu,
            .vm = &emu->vm,
            .mhartid = i,
        };
        semu_hart_mailbox_init(&harts_storage[i].mailbox);
        hart_hsm_status_store(&harts_storage[i], SBI_HSM_STATE_STARTED);
        harts[i] = &harts_storage[i];
    }
}

static void test_dedicated_request_pause_publishes_and_wakes_started_harts(void)
{
    emu_state_t emu = {0};
    hart_t harts_storage[3];
    hart_t *harts[3];
    test_harts_init(&emu, harts_storage, harts, 3);
    hart_hsm_status_store(harts[1], SBI_HSM_STATE_STOPPED);

    check_true(hart_executor_init(&emu, HART_EXEC_DEDICATED_THREADS, NULL, NULL,
                                  NULL) == 0,
               "dedicated executor initializes for pause request");

    struct wake_waiter waiter0;
    struct wake_waiter waiter2;
    wake_waiter_init(&waiter0, hart_executor_wait_for_hart(&emu, 0));
    wake_waiter_init(&waiter2, hart_executor_wait_for_hart(&emu, 2));
    pthread_t thread0;
    pthread_t thread2;
    pthread_create(&thread0, NULL, wake_waiter_thread, &waiter0);
    pthread_create(&thread2, NULL, wake_waiter_thread, &waiter2);
    wait_for_waiter_ready(&waiter0);
    wait_for_waiter_ready(&waiter2);

    bool targets[] = {true, false, true};
    check_true(hart_executor_request_pause(&emu, 17, targets) == 0,
               "dedicated request_pause succeeds");
    pthread_join(thread0, NULL);
    pthread_join(thread2, NULL);

    check_true(hart_pause_request_seq_load(harts[0]) == 17,
               "pause request published to started hart 0");
    check_true(hart_pause_request_seq_load(harts[1]) == 0,
               "pause request skips untargeted stopped hart");
    check_true(hart_pause_request_seq_load(harts[2]) == 17,
               "pause request published to started hart 2");
    check_true(waiter0.woke, "pause request wakes started hart 0");
    check_true(waiter2.woke, "pause request wakes started hart 2");

    wake_waiter_destroy(&waiter0);
    wake_waiter_destroy(&waiter2);
    hart_executor_destroy(&emu);
}


static void test_dedicated_request_pause_uses_snapshot_targets_without_refilter(
    void)
{
    emu_state_t emu = {0};
    hart_t harts_storage[2];
    hart_t *harts[2];
    test_harts_init(&emu, harts_storage, harts, 2);

    check_true(hart_executor_init(&emu, HART_EXEC_DEDICATED_THREADS, NULL, NULL,
                                  NULL) == 0,
               "dedicated executor initializes for snapshotted pause request");

    struct wake_waiter waiter1;
    wake_waiter_init(&waiter1, hart_executor_wait_for_hart(&emu, 1));
    pthread_t thread1;
    pthread_create(&thread1, NULL, wake_waiter_thread, &waiter1);
    wait_for_waiter_ready(&waiter1);

    bool targets[] = {false, true};
    hart_hsm_status_store(harts[1], SBI_HSM_STATE_STOP_PENDING);
    check_true(hart_executor_request_pause(&emu, 19, targets) == 0,
               "dedicated request_pause succeeds for snapshotted target");
    pthread_join(thread1, NULL);

    check_true(hart_pause_request_seq_load(harts[0]) == 0,
               "pause request skips untargeted hart");
    check_true(hart_pause_request_seq_load(harts[1]) == 19,
               "pause request publishes snapshotted target after HSM change");
    check_true(waiter1.woke,
               "pause request wakes snapshotted target after HSM change");

    wake_waiter_destroy(&waiter1);
    hart_executor_destroy(&emu);
}

static void test_dedicated_request_rfence_publishes_and_wakes_masked_harts(void)
{
    emu_state_t emu = {0};
    hart_t harts_storage[4];
    hart_t *harts[4];
    test_harts_init(&emu, harts_storage, harts, 4);
    hart_hsm_status_store(harts[2], SBI_HSM_STATE_STOPPED);

    check_true(hart_executor_init(&emu, HART_EXEC_DEDICATED_THREADS, NULL, NULL,
                                  NULL) == 0,
               "dedicated executor initializes for RFENCE request");

    struct wake_waiter waiter1;
    struct wake_waiter waiter2;
    struct wake_waiter waiter3;
    wake_waiter_init(&waiter1, hart_executor_wait_for_hart(&emu, 1));
    wake_waiter_init(&waiter2, hart_executor_wait_for_hart(&emu, 2));
    wake_waiter_init(&waiter3, hart_executor_wait_for_hart(&emu, 3));
    pthread_t thread1;
    pthread_t thread2;
    pthread_t thread3;
    pthread_create(&thread1, NULL, wake_waiter_thread, &waiter1);
    pthread_create(&thread2, NULL, wake_waiter_thread, &waiter2);
    pthread_create(&thread3, NULL, wake_waiter_thread, &waiter3);
    wait_for_waiter_ready(&waiter1);
    wait_for_waiter_ready(&waiter2);
    wait_for_waiter_ready(&waiter3);

    check_true(hart_executor_request_rfence(&emu, 0x7u, 1, 23) == 0,
               "dedicated request_rfence succeeds");
    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);
    pthread_join(thread3, NULL);

    check_true(!hart_pending_rfence_load(harts[0]),
               "RFENCE request skips unmasked hart");
    check_true(hart_pending_rfence_load(harts[1]),
               "RFENCE request published to masked started hart 1");
    check_true(hart_pending_rfence_load(harts[2]),
               "RFENCE request publishes already-counted stopped hart");
    check_true(hart_pending_rfence_load(harts[3]),
               "RFENCE request published to masked started hart 3");
    check_true(waiter1.woke, "RFENCE request wakes masked hart 1");
    check_true(waiter2.woke,
               "RFENCE request wakes already-counted stopped hart 2");
    check_true(waiter3.woke, "RFENCE request wakes masked hart 3");

    wake_waiter_destroy(&waiter1);
    wake_waiter_destroy(&waiter2);
    wake_waiter_destroy(&waiter3);
    hart_executor_destroy(&emu);
}

static void test_single_thread_request_ops_are_noop_success(void)
{
    emu_state_t emu = {0};
    hart_t harts_storage[1];
    hart_t *harts[1];
    test_harts_init(&emu, harts_storage, harts, 1);

    check_true(hart_executor_init(&emu, HART_EXEC_SINGLE_THREAD, NULL, NULL,
                                  NULL) == 0,
               "single-thread executor initializes for request ops");
    bool targets[] = {true};
    check_true(hart_executor_request_pause(&emu, 31, targets) == 0,
               "single-thread request_pause succeeds");
    check_true(hart_executor_request_rfence(&emu, 1, 0, 37) == 0,
               "single-thread request_rfence succeeds");
    check_true(hart_pause_request_seq_load(harts[0]) == 0,
               "single-thread request_pause does not publish mailbox");
    check_true(!hart_pending_rfence_load(harts[0]),
               "single-thread request_rfence does not publish mailbox");

    hart_executor_destroy(&emu);
}

static void test_dedicated_backend_lifecycle(void)
{
    emu_state_t emu = {0};
    hart_t hart0 = {.priv = &emu, .mhartid = 0};
    hart_t hart1 = {.priv = &emu, .mhartid = 1};
    hart_t *harts[] = {&hart0, &hart1};
    emu.vm.n_hart = 2;
    emu.vm.hart = harts;

    check_true(hart_executor_init(&emu, HART_EXEC_DEDICATED_THREADS,
                                  test_hart_thread, test_io_thread, NULL) == 0,
               "dedicated executor initializes");
    check_true(emu.executor.backend == HART_EXEC_DEDICATED_THREADS,
               "backend stored on executor");
    check_true(emu.executor.backend_state != NULL,
               "dedicated backend owns state");
    check_true(hart_executor_wait_for_hart(&emu, 0) != NULL,
               "hart 0 wait state is available");
    check_true(hart_executor_wait_for_hart(&emu, 1) != NULL,
               "hart 1 wait state is available");
    check_true(hart_executor_wait_for_hart(&emu, 2) == NULL,
               "out-of-range wait state is rejected");

    hart_executor_wake_hart(&emu, 0);
    hart_executor_wake_all(&emu);
    check_true(hart_executor_start(&emu) == 0,
               "dedicated executor starts threads");
    hart_executor_request_stop(&emu);
    check_true(__atomic_load_n(&emu.stopped, __ATOMIC_RELAXED),
               "request_stop marks emulator stopped");
    check_true(hart_executor_join(&emu) == 0,
               "dedicated executor joins threads");
    hart_executor_destroy(&emu);
    check_true(emu.executor.backend_state == NULL,
               "destroy clears backend state");
}

static void test_single_thread_backend_uses_no_wait_state(void)
{
    emu_state_t emu = {0};
    emu.vm.n_hart = 1;

    check_true(hart_executor_init(&emu, HART_EXEC_SINGLE_THREAD, NULL, NULL,
                                  NULL) == 0,
               "single-thread executor initializes");
    check_true(hart_executor_wait_for_hart(&emu, 0) == NULL,
               "single-thread executor has no wait state");
    hart_executor_wake_hart(&emu, 0);
    hart_executor_wake_all(&emu);
    hart_executor_request_stop(&emu);
    check_true(__atomic_load_n(&emu.stopped, __ATOMIC_RELAXED),
               "single-thread request_stop marks stopped");
    check_true(hart_executor_join(&emu) == 0, "single-thread join is a no-op");
    hart_executor_destroy(&emu);
}

int main(void)
{
    test_dedicated_backend_lifecycle();
    test_single_thread_backend_uses_no_wait_state();
    test_dedicated_request_pause_publishes_and_wakes_started_harts();
    test_dedicated_request_pause_uses_snapshot_targets_without_refilter();
    test_dedicated_request_rfence_publishes_and_wakes_masked_harts();
    test_single_thread_request_ops_are_noop_success();

    if (failures != 0) {
        fprintf(stderr, "%d hart executor checks failed\n", failures);
        return 1;
    }
    return 0;
}
