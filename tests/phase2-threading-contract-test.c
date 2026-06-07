#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum {
    ITERATIONS = 100000,
    WFI_RACE_ITERATIONS = 1000,
    SBI_SUCCESS = 0,
    SBI_ERR_ALREADY_STARTED = -7,
    SBI_HSM_STATE_STARTED = 0,
    SBI_HSM_STATE_STOPPED = 1,
    SBI_HSM_STATE_START_PENDING = 2,
    SBI_HSM_STATE_STOP_PENDING = 3,
    SBI_HSM_STATE_SUSPENDED = 4,
    SBI_HSM_STATE_SUSPEND_PENDING = 5,
    SBI_HSM_STATE_RESUME_PENDING = 6,
};

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    atomic_int hsm_status;
    atomic_bool in_wfi;
    atomic_bool pending_interrupt;
    atomic_bool stop_requested;
    atomic_bool parked;
    atomic_bool waiter_ready;
    atomic_uint wake_count;
    atomic_uint guest_steps;
    uint32_t pc;
    uint32_t opaque;
} hart_model_t;

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

static struct timespec deadline_ms(long ms)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        perror("clock_gettime");
        exit(1);
    }

    ts.tv_nsec += (ms % 1000) * 1000000L;
    ts.tv_sec += ms / 1000 + ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    return ts;
}

static void sleep_ms(long ms)
{
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

static void wait_until_true(const char *name, atomic_bool *flag)
{
    struct timespec end = deadline_ms(1000);

    while (!atomic_load_explicit(flag, memory_order_acquire)) {
        struct timespec now;
        if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
            perror("clock_gettime");
            exit(1);
        }
        if (now.tv_sec > end.tv_sec ||
            (now.tv_sec == end.tv_sec && now.tv_nsec >= end.tv_nsec))
            fail(name);
        sched_yield();
    }
}

static void hart_model_init(hart_model_t *hart, int state)
{
    if (pthread_mutex_init(&hart->mutex, NULL) != 0)
        fail("pthread_mutex_init failed");
    if (pthread_cond_init(&hart->cond, NULL) != 0)
        fail("pthread_cond_init failed");
    atomic_init(&hart->hsm_status, state);
    atomic_init(&hart->in_wfi, false);
    atomic_init(&hart->pending_interrupt, false);
    atomic_init(&hart->stop_requested, false);
    atomic_init(&hart->parked, false);
    atomic_init(&hart->waiter_ready, false);
    atomic_init(&hart->wake_count, 0);
    atomic_init(&hart->guest_steps, 0);
    hart->pc = 0;
    hart->opaque = 0;
}

static void hart_model_destroy(hart_model_t *hart)
{
    pthread_cond_destroy(&hart->cond);
    pthread_mutex_destroy(&hart->mutex);
}

static void hart_model_wake(hart_model_t *hart)
{
    pthread_mutex_lock(&hart->mutex);
    atomic_fetch_add_explicit(&hart->wake_count, 1, memory_order_relaxed);
    pthread_cond_broadcast(&hart->cond);
    pthread_mutex_unlock(&hart->mutex);
}

static int hsm_start(hart_model_t *target, uint32_t start_addr, uint32_t opaque)
{
    int expected = SBI_HSM_STATE_STOPPED;

    if (!atomic_compare_exchange_strong_explicit(
            &target->hsm_status, &expected, SBI_HSM_STATE_START_PENDING,
            memory_order_acq_rel, memory_order_acquire))
        return SBI_ERR_ALREADY_STARTED;

    target->pc = start_addr;
    target->opaque = opaque;
    atomic_store_explicit(&target->hsm_status, SBI_HSM_STATE_STARTED,
                          memory_order_release);
    hart_model_wake(target);
    return SBI_SUCCESS;
}

static void hsm_stop(hart_model_t *hart)
{
    atomic_store_explicit(&hart->hsm_status, SBI_HSM_STATE_STOPPED,
                          memory_order_release);
    hart_model_wake(hart);
}

static void hsm_suspend(hart_model_t *hart)
{
    atomic_store_explicit(&hart->hsm_status, SBI_HSM_STATE_SUSPENDED,
                          memory_order_release);
    hart_model_wake(hart);
}

static void *guest_loop(void *opaque)
{
    hart_model_t *hart = opaque;

    while (!atomic_load_explicit(&hart->stop_requested, memory_order_acquire)) {
        pthread_mutex_lock(&hart->mutex);
        while (atomic_load_explicit(&hart->hsm_status, memory_order_acquire) !=
                   SBI_HSM_STATE_STARTED &&
               !atomic_load_explicit(&hart->stop_requested,
                                     memory_order_acquire)) {
            atomic_store_explicit(&hart->parked, true, memory_order_release);
            pthread_cond_wait(&hart->cond, &hart->mutex);
        }
        atomic_store_explicit(&hart->parked, false, memory_order_release);
        pthread_mutex_unlock(&hart->mutex);

        if (atomic_load_explicit(&hart->hsm_status, memory_order_acquire) !=
            SBI_HSM_STATE_STARTED)
            continue;

        atomic_fetch_add_explicit(&hart->guest_steps, 1, memory_order_relaxed);
        sched_yield();
    }

    return NULL;
}

static void test_hsm_start_only_from_stopped_and_wakes_target(void)
{
    hart_model_t target;
    hart_model_init(&target, SBI_HSM_STATE_STOPPED);

    require_int("HSM START from STOPPED succeeds",
                hsm_start(&target, 0x80200000u, 0x1234u), SBI_SUCCESS);
    require_int("HSM START moves target to STARTED",
                atomic_load_explicit(&target.hsm_status, memory_order_acquire),
                SBI_HSM_STATE_STARTED);
    require_u32("HSM START sets target pc", target.pc, 0x80200000u);
    require_u32("HSM START sets target opaque", target.opaque, 0x1234u);
    require_int("HSM START wakes target",
                atomic_load_explicit(&target.wake_count, memory_order_relaxed),
                1);

    require_int("HSM START from STARTED is denied",
                hsm_start(&target, 0x90000000u, 0), SBI_ERR_ALREADY_STARTED);
    require_u32("denied HSM START does not rewrite pc", target.pc, 0x80200000u);
    require_int("denied HSM START does not wake target",
                atomic_load_explicit(&target.wake_count, memory_order_relaxed),
                1);

    atomic_store_explicit(&target.hsm_status, SBI_HSM_STATE_SUSPENDED,
                          memory_order_release);
    require_int("HSM START from SUSPENDED is denied",
                hsm_start(&target, 0x90000000u, 0), SBI_ERR_ALREADY_STARTED);
    require_int("denied HSM START keeps SUSPENDED state",
                atomic_load_explicit(&target.hsm_status, memory_order_acquire),
                SBI_HSM_STATE_SUSPENDED);

    hart_model_destroy(&target);
}

static void test_stop_and_suspend_park_threaded_guest_loop(void)
{
    pthread_t thread;
    hart_model_t hart;
    hart_model_init(&hart, SBI_HSM_STATE_STARTED);

    if (pthread_create(&thread, NULL, guest_loop, &hart) != 0)
        fail("pthread_create guest loop failed");

    while (atomic_load_explicit(&hart.guest_steps, memory_order_relaxed) == 0)
        sched_yield();

    hsm_stop(&hart);
    wait_until_true("guest loop did not park after HART_STOP", &hart.parked);
    uint32_t stopped_steps =
        atomic_load_explicit(&hart.guest_steps, memory_order_relaxed);
    sleep_ms(20);
    require_u32("HART_STOP parks guest execution",
                atomic_load_explicit(&hart.guest_steps, memory_order_relaxed),
                stopped_steps);

    require_int("restart stopped hart", hsm_start(&hart, 0x1000u, 0),
                SBI_SUCCESS);
    while (atomic_load_explicit(&hart.guest_steps, memory_order_relaxed) ==
           stopped_steps)
        sched_yield();

    hsm_suspend(&hart);
    wait_until_true("guest loop did not park after HART_SUSPEND", &hart.parked);
    uint32_t suspended_steps =
        atomic_load_explicit(&hart.guest_steps, memory_order_relaxed);
    sleep_ms(20);
    require_u32("HART_SUSPEND parks guest execution",
                atomic_load_explicit(&hart.guest_steps, memory_order_relaxed),
                suspended_steps);

    atomic_store_explicit(&hart.stop_requested, true, memory_order_release);
    hart_model_wake(&hart);
    pthread_join(thread, NULL);
    hart_model_destroy(&hart);
}

static void inject_interrupt(hart_model_t *hart)
{
    pthread_mutex_lock(&hart->mutex);
    atomic_store_explicit(&hart->pending_interrupt, true, memory_order_release);
    atomic_store_explicit(&hart->in_wfi, false, memory_order_release);
    pthread_cond_broadcast(&hart->cond);
    pthread_mutex_unlock(&hart->mutex);
}

static void *wfi_waiter(void *opaque)
{
    hart_model_t *hart = opaque;
    struct timespec end = deadline_ms(1000);

    pthread_mutex_lock(&hart->mutex);
    atomic_store_explicit(&hart->in_wfi, true, memory_order_release);
    atomic_store_explicit(&hart->waiter_ready, true, memory_order_release);
    while (
        atomic_load_explicit(&hart->in_wfi, memory_order_acquire) &&
        !atomic_load_explicit(&hart->pending_interrupt, memory_order_acquire)) {
        int rc = pthread_cond_timedwait(&hart->cond, &hart->mutex, &end);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&hart->mutex);
            return (void *) 1;
        }
    }
    atomic_store_explicit(&hart->pending_interrupt, false,
                          memory_order_release);
    atomic_store_explicit(&hart->in_wfi, false, memory_order_release);
    pthread_mutex_unlock(&hart->mutex);
    return NULL;
}

static void test_signal_only_does_not_resume_suspended_hart(void)
{
    hart_model_t hart;
    hart_model_init(&hart, SBI_HSM_STATE_SUSPENDED);

    hart_model_wake(&hart);
    require_int("generic signal keeps suspended hart parked",
                atomic_load_explicit(&hart.hsm_status, memory_order_acquire),
                SBI_HSM_STATE_SUSPENDED);

    hart_model_destroy(&hart);
}

static void test_wfi_missed_wakeup_protocol(void)
{
    for (int i = 0; i < WFI_RACE_ITERATIONS; i++) {
        pthread_t thread;
        void *thread_ret = NULL;
        hart_model_t hart;
        hart_model_init(&hart, SBI_HSM_STATE_STARTED);

        if (pthread_create(&thread, NULL, wfi_waiter, &hart) != 0)
            fail("pthread_create WFI waiter failed");
        wait_until_true("WFI waiter never reached wait protocol",
                        &hart.waiter_ready);
        inject_interrupt(&hart);
        pthread_join(thread, &thread_ret);
        if (thread_ret != NULL)
            fail("WFI waiter timed out after interrupt wake");
        require_bool("WFI wake clears in_wfi",
                     atomic_load_explicit(&hart.in_wfi, memory_order_acquire),
                     false);
        hart_model_destroy(&hart);
    }
}

typedef struct {
    pthread_mutex_t mutex;
    atomic_uint lock_entries;
} device_lock_t;

static void device_lock_init(device_lock_t *lock)
{
    if (pthread_mutex_init(&lock->mutex, NULL) != 0)
        fail("pthread_mutex_init device lock failed");
    atomic_init(&lock->lock_entries, 0);
}

static void device_lock_destroy(device_lock_t *lock)
{
    pthread_mutex_destroy(&lock->mutex);
}

static void emu_device_lock(device_lock_t *lock)
{
#ifdef ENABLE_THREADED
    pthread_mutex_lock(&lock->mutex);
    atomic_fetch_add_explicit(&lock->lock_entries, 1, memory_order_relaxed);
#else
    (void) lock;
#endif
}

static void emu_device_unlock(device_lock_t *lock)
{
#ifdef ENABLE_THREADED
    pthread_mutex_unlock(&lock->mutex);
#else
    (void) lock;
#endif
}

#ifdef ENABLE_THREADED
typedef struct {
    device_lock_t *lock;
    int *counter;
} device_worker_arg_t;

static void *device_worker(void *opaque)
{
    device_worker_arg_t *arg = opaque;

    for (int i = 0; i < ITERATIONS; i++) {
        emu_device_lock(arg->lock);
        (*arg->counter)++;
        emu_device_unlock(arg->lock);
    }

    return NULL;
}
#endif

static void test_device_lock_helper_build_mode_contract(void)
{
    device_lock_t lock;
    device_lock_init(&lock);

    emu_device_lock(&lock);
    emu_device_unlock(&lock);

#ifdef ENABLE_THREADED
    require_int("threaded device lock helper takes real lock",
                atomic_load_explicit(&lock.lock_entries, memory_order_relaxed),
                1);

    pthread_t threads[2];
    int counter = 0;
    device_worker_arg_t arg = {.lock = &lock, .counter = &counter};
    if (pthread_create(&threads[0], NULL, device_worker, &arg) != 0 ||
        pthread_create(&threads[1], NULL, device_worker, &arg) != 0)
        fail("pthread_create device workers failed");
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);
    require_int("threaded device lock serializes MMIO-style critical section",
                counter, ITERATIONS * 2);
#else
    require_int("default device lock helper remains a no-op",
                atomic_load_explicit(&lock.lock_entries, memory_order_relaxed),
                0);
#endif

    device_lock_destroy(&lock);
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    atomic_bool waiting;
    atomic_bool stopped;
    atomic_uint wakes;
} io_stop_model_t;

static void io_stop_model_init(io_stop_model_t *model)
{
    if (pthread_mutex_init(&model->mutex, NULL) != 0)
        fail("pthread_mutex_init io model failed");
    if (pthread_cond_init(&model->cond, NULL) != 0)
        fail("pthread_cond_init io model failed");
    atomic_init(&model->waiting, false);
    atomic_init(&model->stopped, false);
    atomic_init(&model->wakes, 0);
}

static void io_stop_model_destroy(io_stop_model_t *model)
{
    pthread_cond_destroy(&model->cond);
    pthread_mutex_destroy(&model->mutex);
}

static void io_request_stop_and_wake(io_stop_model_t *model)
{
    pthread_mutex_lock(&model->mutex);
    atomic_store_explicit(&model->stopped, true, memory_order_release);
    atomic_fetch_add_explicit(&model->wakes, 1, memory_order_relaxed);
    pthread_cond_broadcast(&model->cond);
    pthread_mutex_unlock(&model->mutex);
}

static void *scheduler_wait_loop(void *opaque)
{
    io_stop_model_t *model = opaque;
    struct timespec end = deadline_ms(1000);

    pthread_mutex_lock(&model->mutex);
    atomic_store_explicit(&model->waiting, true, memory_order_release);
    while (!atomic_load_explicit(&model->stopped, memory_order_acquire)) {
        int rc = pthread_cond_timedwait(&model->cond, &model->mutex, &end);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&model->mutex);
            return (void *) 1;
        }
    }
    pthread_mutex_unlock(&model->mutex);
    return NULL;
}

static void test_io_thread_can_request_stop_and_wake_without_deadlock(void)
{
    pthread_t scheduler;
    void *thread_ret = NULL;
    io_stop_model_t model;
    io_stop_model_init(&model);

    if (pthread_create(&scheduler, NULL, scheduler_wait_loop, &model) != 0)
        fail("pthread_create scheduler wait loop failed");

    wait_until_true("scheduler did not enter wait loop", &model.waiting);
    io_request_stop_and_wake(&model);
    pthread_join(scheduler, &thread_ret);

    if (thread_ret != NULL)
        fail("scheduler timed out after I/O stop wake");
    require_bool("I/O request stores stopped",
                 atomic_load_explicit(&model.stopped, memory_order_acquire),
                 true);
    require_int("I/O request wakes scheduler",
                atomic_load_explicit(&model.wakes, memory_order_relaxed), 1);

    io_stop_model_destroy(&model);
}

int main(void)
{
    test_hsm_start_only_from_stopped_and_wakes_target();
    test_stop_and_suspend_park_threaded_guest_loop();
    test_signal_only_does_not_resume_suspended_hart();
    test_wfi_missed_wakeup_protocol();
    test_device_lock_helper_build_mode_contract();
    test_io_thread_can_request_stop_and_wake_without_deadlock();
    return 0;
}
