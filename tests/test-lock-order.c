#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "lock-order.h"

static void require_int(const char *name, int got, int want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %d, want %d\n", name, got, want);
    exit(1);
}

static void require_rank(const char *name,
                         enum semu_lock_rank got,
                         enum semu_lock_rank want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %d, want %d\n", name, got, want);
    exit(1);
}

static void require_true(const char *name, bool got)
{
    if (got)
        return;

    fprintf(stderr, "%s: got false, want true\n", name);
    exit(1);
}

static void test_rank_monotonicity_and_release(void)
{
    struct semu_lock_order_guard lifecycle;
    struct semu_lock_order_guard transport;
    struct semu_lock_order_guard actor;
    struct semu_lock_order_guard queue;
    struct semu_lock_order_guard backend;

    require_rank("initial rank", semu_lock_order_current_rank(),
                 SEMU_LOCK_RANK_NONE);

    require_int("enter lifecycle",
                semu_lock_order_enter(SEMU_LOCK_RANK_VM_LIFECYCLE, &lifecycle),
                0);
    require_rank("current lifecycle", semu_lock_order_current_rank(),
                 SEMU_LOCK_RANK_VM_LIFECYCLE);

    require_int(
        "enter transport",
        semu_lock_order_enter(SEMU_LOCK_RANK_DEVICE_TRANSPORT, &transport), 0);
    require_rank("current transport", semu_lock_order_current_rank(),
                 SEMU_LOCK_RANK_DEVICE_TRANSPORT);

    require_int("enter actor",
                semu_lock_order_enter(SEMU_LOCK_RANK_ACTOR_MAILBOX, &actor), 0);
    require_int("enter queue",
                semu_lock_order_enter(SEMU_LOCK_RANK_QUEUE_STATE, &queue), 0);
    require_int("enter backend",
                semu_lock_order_enter(SEMU_LOCK_RANK_BACKEND_LOCAL, &backend),
                0);
    require_rank("current backend", semu_lock_order_current_rank(),
                 SEMU_LOCK_RANK_BACKEND_LOCAL);

    require_int("leave backend", semu_lock_order_leave(&backend), 0);
    require_rank("current queue", semu_lock_order_current_rank(),
                 SEMU_LOCK_RANK_QUEUE_STATE);
    require_int("leave queue", semu_lock_order_leave(&queue), 0);
    require_rank("current actor", semu_lock_order_current_rank(),
                 SEMU_LOCK_RANK_ACTOR_MAILBOX);
    require_int("leave actor", semu_lock_order_leave(&actor), 0);
    require_rank("current transport after actor",
                 semu_lock_order_current_rank(),
                 SEMU_LOCK_RANK_DEVICE_TRANSPORT);
    require_int("leave transport", semu_lock_order_leave(&transport), 0);
    require_rank("current lifecycle after transport",
                 semu_lock_order_current_rank(), SEMU_LOCK_RANK_VM_LIFECYCLE);
    require_int("leave lifecycle", semu_lock_order_leave(&lifecycle), 0);
    require_rank("final rank", semu_lock_order_current_rank(),
                 SEMU_LOCK_RANK_NONE);
}

static void test_same_rank_is_allowed_but_released_as_stack(void)
{
    struct semu_lock_order_guard first;
    struct semu_lock_order_guard second;

    require_int("enter first backend",
                semu_lock_order_enter(SEMU_LOCK_RANK_BACKEND_LOCAL, &first), 0);
    require_int("enter second backend",
                semu_lock_order_enter(SEMU_LOCK_RANK_BACKEND_LOCAL, &second),
                0);

    require_int("reject out-of-order same-rank release",
                semu_lock_order_leave(&first), -EPERM);
    require_int("leave second backend", semu_lock_order_leave(&second), 0);
    require_int("leave first backend", semu_lock_order_leave(&first), 0);
    require_rank("rank after same-rank release", semu_lock_order_current_rank(),
                 SEMU_LOCK_RANK_NONE);
}

static void test_rank_inversion_is_rejected_without_changing_state(void)
{
    struct semu_lock_order_guard backend;
    struct semu_lock_order_guard transport;

    require_int("enter backend first",
                semu_lock_order_enter(SEMU_LOCK_RANK_BACKEND_LOCAL, &backend),
                0);
    require_int(
        "reject transport below backend",
        semu_lock_order_enter(SEMU_LOCK_RANK_DEVICE_TRANSPORT, &transport),
        -EDEADLK);
    require_rank("current rank still backend", semu_lock_order_current_rank(),
                 SEMU_LOCK_RANK_BACKEND_LOCAL);
    require_int("leave backend after inversion",
                semu_lock_order_leave(&backend), 0);
    require_rank("rank after inversion test", semu_lock_order_current_rank(),
                 SEMU_LOCK_RANK_NONE);
}

static void test_invalid_release_is_rejected(void)
{
    struct semu_lock_order_guard guard = {0};

    require_int("reject empty release", semu_lock_order_leave(&guard), -EINVAL);
}

static void *thread_local_worker(void *opaque)
{
    bool *ok = opaque;
    struct semu_lock_order_guard lifecycle;

    *ok = semu_lock_order_current_rank() == SEMU_LOCK_RANK_NONE &&
          semu_lock_order_enter(SEMU_LOCK_RANK_VM_LIFECYCLE, &lifecycle) == 0 &&
          semu_lock_order_leave(&lifecycle) == 0 &&
          semu_lock_order_current_rank() == SEMU_LOCK_RANK_NONE;
    return NULL;
}

static void test_rank_stack_is_thread_local(void)
{
    struct semu_lock_order_guard backend;
    pthread_t thread;
    bool worker_ok = false;

    require_int("enter backend in main",
                semu_lock_order_enter(SEMU_LOCK_RANK_BACKEND_LOCAL, &backend),
                0);
    require_int("create worker",
                pthread_create(&thread, NULL, thread_local_worker, &worker_ok),
                0);
    require_int("join worker", pthread_join(thread, NULL), 0);
    require_true("worker had independent rank stack", worker_ok);
    require_rank("main still backend", semu_lock_order_current_rank(),
                 SEMU_LOCK_RANK_BACKEND_LOCAL);
    require_int("leave backend in main", semu_lock_order_leave(&backend), 0);
}

int main(void)
{
    test_rank_monotonicity_and_release();
    test_same_rank_is_allowed_but_released_as_stack();
    test_rank_inversion_is_rejected_without_changing_state();
    test_invalid_release_is_rejected();
    test_rank_stack_is_thread_local();

    return 0;
}
