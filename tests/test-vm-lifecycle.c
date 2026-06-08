#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vm-lifecycle.h"

static void require_true(const char *name, bool got)
{
    if (got)
        return;

    fprintf(stderr, "%s: got false, want true\n", name);
    exit(1);
}

static void require_false(const char *name, bool got)
{
    if (!got)
        return;

    fprintf(stderr, "%s: got true, want false\n", name);
    exit(1);
}

static void require_int(const char *name, int got, int want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %d, want %d\n", name, got, want);
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

static void require_state(const char *name,
                          enum semu_vm_lifecycle_state got,
                          enum semu_vm_lifecycle_state want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %d, want %d\n", name, got, want);
    exit(1);
}

static void test_init_starts_created(void)
{
    struct semu_vm_lifecycle lc;

    memset(&lc, 0xff, sizeof(lc));
    require_int("init", semu_vm_lifecycle_init(&lc), 0);
    require_state("initial state", semu_vm_lifecycle_state(&lc),
                  SEMU_VM_CREATED);
    require_u64("initial generation", semu_vm_lifecycle_generation(&lc), 0);
    require_u64("initial pause seq", semu_vm_lifecycle_pause_seq(&lc), 0);
    require_u64("initial drain seq", semu_vm_lifecycle_drain_seq(&lc), 0);
    require_false("initial accepting", semu_vm_accepting_device_work(&lc));

    semu_vm_lifecycle_destroy(&lc);
}

static void test_running_accepts_device_work(void)
{
    struct semu_vm_lifecycle lc;

    require_int("init", semu_vm_lifecycle_init(&lc), 0);
    require_int("running", semu_vm_lifecycle_enter_running(&lc), 0);
    require_state("running state", semu_vm_lifecycle_state(&lc),
                  SEMU_VM_RUNNING);
    require_true("running accepting", semu_vm_accepting_device_work(&lc));

    semu_vm_lifecycle_destroy(&lc);
}

static void test_pause_request_blocks_work_and_paused_state(void)
{
    struct semu_vm_lifecycle lc;

    require_int("init", semu_vm_lifecycle_init(&lc), 0);
    require_int("running", semu_vm_lifecycle_enter_running(&lc), 0);
    require_int("pause request", semu_vm_lifecycle_request_pause(&lc), 0);
    require_state("pause requested state", semu_vm_lifecycle_state(&lc),
                  SEMU_VM_PAUSE_REQUESTED);
    require_u64("pause seq incremented", semu_vm_lifecycle_pause_seq(&lc), 1);
    require_false("pause request accepting",
                  semu_vm_accepting_device_work(&lc));
    require_int("paused", semu_vm_lifecycle_enter_paused(&lc), 0);
    require_state("paused state", semu_vm_lifecycle_state(&lc), SEMU_VM_PAUSED);
    require_false("paused accepting", semu_vm_accepting_device_work(&lc));

    semu_vm_lifecycle_destroy(&lc);
}

static void test_draining_increments_seq_and_blocks_work(void)
{
    struct semu_vm_lifecycle lc;

    require_int("init", semu_vm_lifecycle_init(&lc), 0);
    require_int("running", semu_vm_lifecycle_enter_running(&lc), 0);
    require_int("draining", semu_vm_lifecycle_enter_draining(&lc), 0);
    require_state("draining state", semu_vm_lifecycle_state(&lc),
                  SEMU_VM_DRAINING);
    require_u64("drain seq incremented", semu_vm_lifecycle_drain_seq(&lc), 1);
    require_false("draining accepting", semu_vm_accepting_device_work(&lc));

    semu_vm_lifecycle_destroy(&lc);
}

static void test_reset_generation_and_running_reenables_work(void)
{
    struct semu_vm_lifecycle lc;

    require_int("init", semu_vm_lifecycle_init(&lc), 0);
    require_int("running", semu_vm_lifecycle_enter_running(&lc), 0);
    require_int("resetting", semu_vm_lifecycle_enter_resetting(&lc), 0);
    require_state("resetting state", semu_vm_lifecycle_state(&lc),
                  SEMU_VM_RESETTING);
    require_u64("reset generation", semu_vm_lifecycle_generation(&lc), 1);
    require_false("reset accepting", semu_vm_accepting_device_work(&lc));
    require_int("running after reset", semu_vm_lifecycle_enter_running(&lc), 0);
    require_state("running after reset state", semu_vm_lifecycle_state(&lc),
                  SEMU_VM_RUNNING);
    require_true("running after reset accepting",
                 semu_vm_accepting_device_work(&lc));

    semu_vm_lifecycle_destroy(&lc);
}

static void test_stop_generation_blocks_work_and_reaches_stopped(void)
{
    struct semu_vm_lifecycle lc;

    require_int("init", semu_vm_lifecycle_init(&lc), 0);
    require_int("running", semu_vm_lifecycle_enter_running(&lc), 0);
    require_int("stopping", semu_vm_lifecycle_enter_stopping(&lc), 0);
    require_state("stopping state", semu_vm_lifecycle_state(&lc),
                  SEMU_VM_STOPPING);
    require_u64("stop generation", semu_vm_lifecycle_generation(&lc), 1);
    require_false("stopping accepting", semu_vm_accepting_device_work(&lc));
    require_int("stopped", semu_vm_lifecycle_enter_stopped(&lc), 0);
    require_state("stopped state", semu_vm_lifecycle_state(&lc),
                  SEMU_VM_STOPPED);
    require_u64("stopped generation unchanged",
                semu_vm_lifecycle_generation(&lc), 1);
    require_false("stopped accepting", semu_vm_accepting_device_work(&lc));

    semu_vm_lifecycle_destroy(&lc);
}

static void test_failed_blocks_work(void)
{
    struct semu_vm_lifecycle lc;

    require_int("init", semu_vm_lifecycle_init(&lc), 0);
    require_int("running", semu_vm_lifecycle_enter_running(&lc), 0);
    require_int("failed", semu_vm_lifecycle_enter_failed(&lc), 0);
    require_state("failed state", semu_vm_lifecycle_state(&lc), SEMU_VM_FAILED);
    require_false("failed accepting", semu_vm_accepting_device_work(&lc));

    semu_vm_lifecycle_destroy(&lc);
}

static void test_invalid_transition_does_not_mutate(void)
{
    struct semu_vm_lifecycle lc;

    require_int("init", semu_vm_lifecycle_init(&lc), 0);
    require_int("created to paused invalid",
                semu_vm_lifecycle_enter_paused(&lc), -EINVAL);
    require_state("invalid keeps state", semu_vm_lifecycle_state(&lc),
                  SEMU_VM_CREATED);
    require_u64("invalid keeps generation", semu_vm_lifecycle_generation(&lc),
                0);
    require_u64("invalid keeps pause seq", semu_vm_lifecycle_pause_seq(&lc), 0);
    require_u64("invalid keeps drain seq", semu_vm_lifecycle_drain_seq(&lc), 0);
    require_false("invalid keeps accepting",
                  semu_vm_accepting_device_work(&lc));

    semu_vm_lifecycle_destroy(&lc);
}

struct wait_args {
    struct semu_vm_lifecycle *lc;
    enum semu_vm_lifecycle_state state;
    enum semu_vm_lifecycle_state observed;
    int ret;
};

static void *wait_changed_thread(void *arg)
{
    struct wait_args *args = (struct wait_args *) arg;

    args->ret = semu_vm_lifecycle_wait_changed_from(args->lc, args->state,
                                                    &args->observed);
    return NULL;
}

static void test_wait_changed_wakes_on_transition(void)
{
    struct semu_vm_lifecycle lc;
    struct wait_args args = {0};
    pthread_t thread;

    require_int("init", semu_vm_lifecycle_init(&lc), 0);
    require_int("running", semu_vm_lifecycle_enter_running(&lc), 0);

    args.lc = &lc;
    args.state = SEMU_VM_RUNNING;
    args.observed = SEMU_VM_CREATED;
    args.ret = -EAGAIN;
    require_int("pthread create",
                pthread_create(&thread, NULL, wait_changed_thread, &args), 0);
    struct timespec delay = {0, 10000000};
    nanosleep(&delay, NULL);
    require_int("pause request", semu_vm_lifecycle_request_pause(&lc), 0);
    require_int("pthread join", pthread_join(thread, NULL), 0);
    require_int("wait changed ret", args.ret, 0);
    require_state("wait changed observed", args.observed,
                  SEMU_VM_PAUSE_REQUESTED);

    semu_vm_lifecycle_destroy(&lc);
}

static void test_wait_until_target(void)
{
    struct semu_vm_lifecycle lc;

    require_int("init", semu_vm_lifecycle_init(&lc), 0);
    require_int("running", semu_vm_lifecycle_enter_running(&lc), 0);
    require_int("pause request", semu_vm_lifecycle_request_pause(&lc), 0);
    require_int("wait already target",
                semu_vm_lifecycle_wait_until(&lc, SEMU_VM_PAUSE_REQUESTED), 0);

    semu_vm_lifecycle_destroy(&lc);
}

int main(void)
{
    test_init_starts_created();
    test_running_accepts_device_work();
    test_pause_request_blocks_work_and_paused_state();
    test_draining_increments_seq_and_blocks_work();
    test_reset_generation_and_running_reenables_work();
    test_stop_generation_blocks_work_and_reaches_stopped();
    test_failed_blocks_work();
    test_invalid_transition_does_not_mutate();
    test_wait_changed_wakes_on_transition();
    test_wait_until_target();
    return 0;
}
