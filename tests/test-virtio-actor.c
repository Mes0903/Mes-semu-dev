#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "virtio-actor.h"

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

static void require_u32(const char *name, uint32_t got, uint32_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got 0x%x, want 0x%x\n", name, got, want);
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
                          enum virtio_actor_state got,
                          enum virtio_actor_state want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %d, want %d\n", name, got, want);
    exit(1);
}

static void sleep_ms(long ms)
{
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L,
    };

    nanosleep(&ts, NULL);
}

struct test_backend {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    unsigned drain_count[8];
    unsigned lock_held_count;
    unsigned old_generation_after_reset_entries;
    uint64_t last_generation;
    atomic_bool reset_done;
    bool work[8];
    bool inject_work_after_clear[8];
    bool block_drain_queue;
    bool drain_entered;
    bool allow_drain_exit;
};

static void backend_init(struct test_backend *backend)
{
    memset(backend, 0, sizeof(*backend));
    require_int("backend mutex init", pthread_mutex_init(&backend->lock, NULL),
                0);
    require_int("backend cond init", pthread_cond_init(&backend->cond, NULL),
                0);
    atomic_init(&backend->reset_done, false);
}

static void backend_destroy(struct test_backend *backend)
{
    pthread_cond_destroy(&backend->cond);
    pthread_mutex_destroy(&backend->lock);
}

static unsigned backend_drain_count(struct test_backend *backend,
                                    uint16_t queue_index)
{
    unsigned count;

    pthread_mutex_lock(&backend->lock);
    count = backend->drain_count[queue_index];
    pthread_mutex_unlock(&backend->lock);
    return count;
}

static unsigned backend_total_drains(struct test_backend *backend)
{
    unsigned total = 0;

    pthread_mutex_lock(&backend->lock);
    for (size_t i = 0; i < ARRAY_SIZE(backend->drain_count); i++)
        total += backend->drain_count[i];
    pthread_mutex_unlock(&backend->lock);
    return total;
}

static void wait_for_actor_state(struct virtio_actor *actor,
                                 enum virtio_actor_state want)
{
    for (unsigned i = 0; i < 200; i++) {
        if (virtio_actor_get_state(actor) == want)
            return;
        sleep_ms(1);
    }

    fprintf(stderr, "actor did not reach state %d\n", want);
    exit(1);
}

static void backend_wait_until_drain_entered(struct test_backend *backend)
{
    for (unsigned i = 0; i < 200; i++) {
        pthread_mutex_lock(&backend->lock);
        bool entered = backend->drain_entered;
        pthread_mutex_unlock(&backend->lock);
        if (entered)
            return;
        sleep_ms(1);
    }

    fprintf(stderr, "drain callback did not enter\n");
    exit(1);
}

static void backend_wait_drains(struct test_backend *backend,
                                uint16_t queue_index,
                                unsigned want)
{
    for (unsigned i = 0; i < 200; i++) {
        pthread_mutex_lock(&backend->lock);
        unsigned got = backend->drain_count[queue_index];
        pthread_mutex_unlock(&backend->lock);
        if (got >= want)
            return;
        sleep_ms(1);
    }

    pthread_mutex_lock(&backend->lock);
    fprintf(stderr, "queue %u drain count got %u, want at least %u\n",
            queue_index, backend->drain_count[queue_index], want);
    pthread_mutex_unlock(&backend->lock);
    exit(1);
}

static int test_drain_queue(void *opaque,
                            struct virtio_actor *actor,
                            uint16_t queue_index,
                            uint64_t generation)
{
    struct test_backend *backend = opaque;

    if (virtio_actor_lock_is_held(actor))
        backend->lock_held_count++;

    pthread_mutex_lock(&backend->lock);
    if (generation == 0 &&
        atomic_load_explicit(&backend->reset_done, memory_order_acquire))
        backend->old_generation_after_reset_entries++;
    backend->drain_count[queue_index]++;
    backend->last_generation = generation;
    backend->work[queue_index] = false;
    if (backend->block_drain_queue && queue_index == 0) {
        backend->drain_entered = true;
        pthread_cond_broadcast(&backend->cond);
        while (!backend->allow_drain_exit)
            pthread_cond_wait(&backend->cond, &backend->lock);
    }
    pthread_mutex_unlock(&backend->lock);
    return 0;
}

static bool test_queue_has_work(void *opaque,
                                struct virtio_actor *actor,
                                uint16_t queue_index,
                                uint64_t generation UNUSED)
{
    struct test_backend *backend = opaque;
    bool has_work;

    if (virtio_actor_lock_is_held(actor))
        backend->lock_held_count++;

    pthread_mutex_lock(&backend->lock);
    if (backend->inject_work_after_clear[queue_index]) {
        backend->inject_work_after_clear[queue_index] = false;
        backend->work[queue_index] = true;
    }
    has_work = backend->work[queue_index];
    pthread_mutex_unlock(&backend->lock);
    return has_work;
}

static const struct virtio_actor_ops test_ops = {
    .drain_queue = test_drain_queue,
    .queue_has_work = test_queue_has_work,
};

static void test_init_destroy_state_and_generation(void)
{
    struct virtio_actor actor;
    struct test_backend backend;

    backend_init(&backend);
    memset(&actor, 0xff, sizeof(actor));
    require_int("init", virtio_actor_init(&actor, &test_ops, &backend, 4), 0);
    require_state("initial state", virtio_actor_get_state(&actor),
                  VIRTIO_ACTOR_CREATED);
    require_u64("initial generation", virtio_actor_generation(&actor), 0);
    require_u32("initial pending mask", virtio_actor_pending_mask(&actor), 0);
    require_false("initial wake pending", actor.wake_pending);
    require_false("initial stop requested", actor.stop_requested);
    require_false("initial failed", actor.failed);

    virtio_actor_destroy(&actor);
    backend_destroy(&backend);
}

static void test_configure_activate_transitions(void)
{
    struct virtio_actor actor;
    struct test_backend backend;

    backend_init(&backend);
    require_int("init", virtio_actor_init(&actor, &test_ops, &backend, 4), 0);
    require_int("configure", virtio_actor_enter_configuring(&actor), 0);
    require_state("configuring state", virtio_actor_get_state(&actor),
                  VIRTIO_ACTOR_CONFIGURING);
    require_int("activate", virtio_actor_activate(&actor), 0);
    require_state("active state", virtio_actor_get_state(&actor),
                  VIRTIO_ACTOR_ACTIVE);
    require_int("activate twice rejected", virtio_actor_activate(&actor),
                -EINVAL);

    require_int("stop", virtio_actor_stop(&actor), 0);
    virtio_actor_destroy(&actor);
    backend_destroy(&backend);
}

static void test_thread_rejects_queue_notify_before_active(void)
{
    struct virtio_actor actor;
    struct test_backend backend;

    backend_init(&backend);
    require_int("init", virtio_actor_init(&actor, &test_ops, &backend, 4), 0);
    require_int("start thread", virtio_actor_start(&actor), 0);
    require_int("notify before active", virtio_actor_notify_queue(&actor, 1),
                -EAGAIN);
    sleep_ms(30);
    require_int("no drain before active", (int) backend_total_drains(&backend),
                0);
    require_u32("no pending before active", virtio_actor_pending_mask(&actor),
                0);
    require_int("configure", virtio_actor_enter_configuring(&actor), 0);
    require_int("activate", virtio_actor_activate(&actor), 0);
    sleep_ms(30);
    require_int("still no stale drain", (int) backend_total_drains(&backend),
                0);

    require_int("stop", virtio_actor_stop(&actor), 0);
    virtio_actor_destroy(&actor);
    backend_destroy(&backend);
}

static void test_repeated_notify_coalesces_until_ack(void)
{
    struct virtio_actor actor;
    struct test_backend backend;

    backend_init(&backend);
    require_int("init", virtio_actor_init(&actor, &test_ops, &backend, 4), 0);
    require_int("configure", virtio_actor_enter_configuring(&actor), 0);
    require_int("activate", virtio_actor_activate(&actor), 0);
    require_int("notify first", virtio_actor_notify_queue(&actor, 2), 0);
    require_int("notify second coalesced", virtio_actor_notify_queue(&actor, 2),
                0);
    require_u32("pending bit coalesced", virtio_actor_pending_mask(&actor),
                1u << 2);
    require_true("wake pending set", actor.wake_pending);
    require_int("wake ack", virtio_actor_ack_wake(&actor), 0);
    require_false("wake pending cleared", actor.wake_pending);
    require_int("notify after ack while pending",
                virtio_actor_notify_queue(&actor, 2), 0);
    require_false("wake remains coalesced", actor.wake_pending);

    virtio_actor_clear_queue_pending(&actor, 2);
    require_int("notify after clear", virtio_actor_notify_queue(&actor, 2), 0);
    require_true("wake set after clear", actor.wake_pending);

    virtio_actor_destroy(&actor);
    backend_destroy(&backend);
}

static void test_invalid_queue_index_rejected(void)
{
    struct virtio_actor actor;
    struct test_backend backend;

    backend_init(&backend);
    require_int("init", virtio_actor_init(&actor, &test_ops, &backend, 4), 0);
    require_int("invalid queue", virtio_actor_notify_queue(&actor, 4), -EINVAL);
    virtio_actor_destroy(&actor);
    require_int("too many queues rejected",
                virtio_actor_init(&actor, &test_ops, &backend, 33), -EINVAL);

    backend_destroy(&backend);
}

static void test_clear_recheck_prevents_lost_wakeup(void)
{
    struct virtio_actor actor;
    struct test_backend backend;

    backend_init(&backend);
    require_int("init", virtio_actor_init(&actor, &test_ops, &backend, 4), 0);
    require_int("start thread", virtio_actor_start(&actor), 0);
    require_int("configure", virtio_actor_enter_configuring(&actor), 0);
    require_int("activate", virtio_actor_activate(&actor), 0);

    pthread_mutex_lock(&backend.lock);
    backend.inject_work_after_clear[3] = true;
    pthread_mutex_unlock(&backend.lock);
    require_int("notify", virtio_actor_notify_queue(&actor, 3), 0);
    backend_wait_drains(&backend, 3, 2);
    require_u32("pending cleared after second drain",
                virtio_actor_pending_mask(&actor), 0);

    require_int("stop", virtio_actor_stop(&actor), 0);
    virtio_actor_destroy(&actor);
    backend_destroy(&backend);
}

static void test_reset_while_paused_does_not_deadlock(void)
{
    struct virtio_actor actor;
    struct test_backend backend;

    backend_init(&backend);
    require_int("init", virtio_actor_init(&actor, &test_ops, &backend, 4), 0);
    require_int("start thread", virtio_actor_start(&actor), 0);
    require_int("configure", virtio_actor_enter_configuring(&actor), 0);
    require_int("activate", virtio_actor_activate(&actor), 0);
    require_int("pause", virtio_actor_pause(&actor), 0);
    require_state("paused", virtio_actor_get_state(&actor),
                  VIRTIO_ACTOR_PAUSED);
    require_int("notify while paused", virtio_actor_notify_queue(&actor, 1),
                -EAGAIN);
    require_u32("paused notify does not pend",
                virtio_actor_pending_mask(&actor), 0);
    require_int("reset", virtio_actor_reset(&actor), 0);
    require_state("resetting", virtio_actor_get_state(&actor),
                  VIRTIO_ACTOR_RESETTING);
    require_u64("reset generation", virtio_actor_generation(&actor), 1);
    require_u32("reset clears pending", virtio_actor_pending_mask(&actor), 0);
    require_int("reconfigure", virtio_actor_enter_configuring(&actor), 0);
    require_int("reactivate", virtio_actor_activate(&actor), 0);
    require_int("notify after reset", virtio_actor_notify_queue(&actor, 1), 0);
    backend_wait_drains(&backend, 1, 1);

    require_int("stop", virtio_actor_stop(&actor), 0);
    virtio_actor_destroy(&actor);
    backend_destroy(&backend);
}


struct reset_args {
    struct virtio_actor *actor;
    struct test_backend *backend;
    int ret;
};

static void *reset_thread(void *arg)
{
    struct reset_args *args = arg;

    args->ret = virtio_actor_reset(args->actor);
    atomic_store_explicit(&args->backend->reset_done, true,
                          memory_order_release);
    return NULL;
}

static void test_reset_waits_for_claimed_backend_mutation(void)
{
    struct virtio_actor actor;
    struct test_backend backend;
    struct reset_args args = {0};
    pthread_t thread;

    backend_init(&backend);
    require_int("init", virtio_actor_init(&actor, &test_ops, &backend, 4), 0);
    require_int("start thread", virtio_actor_start(&actor), 0);
    require_int("configure", virtio_actor_enter_configuring(&actor), 0);
    require_int("activate", virtio_actor_activate(&actor), 0);

    pthread_mutex_lock(&backend.lock);
    backend.block_drain_queue = true;
    pthread_mutex_unlock(&backend.lock);
    require_int("notify", virtio_actor_notify_queue(&actor, 0), 0);
    backend_wait_until_drain_entered(&backend);

    args.actor = &actor;
    args.backend = &backend;
    args.ret = -EAGAIN;
    require_int("reset thread create",
                pthread_create(&thread, NULL, reset_thread, &args), 0);
    wait_for_actor_state(&actor, VIRTIO_ACTOR_RESETTING);
    require_int("notify while resetting", virtio_actor_notify_queue(&actor, 1),
                -EAGAIN);
    sleep_ms(30);
    require_false(
        "reset waits for backend exit",
        atomic_load_explicit(&backend.reset_done, memory_order_acquire));

    pthread_mutex_lock(&backend.lock);
    backend.allow_drain_exit = true;
    pthread_cond_broadcast(&backend.cond);
    pthread_mutex_unlock(&backend.lock);
    require_int("reset thread join", pthread_join(thread, NULL), 0);
    require_int("reset ret", args.ret, 0);
    require_u64("reset generation", virtio_actor_generation(&actor), 1);
    require_u32("resetting notify did not survive",
                virtio_actor_pending_mask(&actor), 0);
    pthread_mutex_lock(&backend.lock);
    require_int("no old generation callback after reset",
                (int) backend.old_generation_after_reset_entries, 0);
    backend.block_drain_queue = false;
    backend.drain_entered = false;
    backend.allow_drain_exit = false;
    pthread_mutex_unlock(&backend.lock);

    require_int("reconfigure", virtio_actor_enter_configuring(&actor), 0);
    require_int("reactivate", virtio_actor_activate(&actor), 0);
    require_int("notify after reset", virtio_actor_notify_queue(&actor, 0), 0);
    backend_wait_drains(&backend, 0, 2);
    sleep_ms(30);
    require_int("no stale reset queue drain",
                (int) backend_drain_count(&backend, 1), 0);
    require_u64("new generation drain", backend.last_generation, 1);

    require_int("stop", virtio_actor_stop(&actor), 0);
    virtio_actor_destroy(&actor);
    backend_destroy(&backend);
}

static void test_completion_section_blocks_reset(void)
{
    struct virtio_actor actor;
    struct test_backend backend;
    struct reset_args args = {0};
    pthread_t thread;

    backend_init(&backend);
    require_int("init", virtio_actor_init(&actor, &test_ops, &backend, 4), 0);
    require_int("configure", virtio_actor_enter_configuring(&actor), 0);
    require_int("activate", virtio_actor_activate(&actor), 0);
    require_true("begin completion", virtio_actor_begin_completion(&actor, 0));

    args.actor = &actor;
    args.backend = &backend;
    args.ret = -EAGAIN;
    require_int("reset thread create",
                pthread_create(&thread, NULL, reset_thread, &args), 0);
    sleep_ms(30);
    require_false(
        "reset waits for completion section",
        atomic_load_explicit(&backend.reset_done, memory_order_acquire));

    require_int("end completion", virtio_actor_end_completion(&actor), 0);
    require_int("reset thread join", pthread_join(thread, NULL), 0);
    require_int("reset ret", args.ret, 0);
    require_u64("reset generation", virtio_actor_generation(&actor), 1);

    virtio_actor_destroy(&actor);
    backend_destroy(&backend);
}

static void test_completion_begin_fails_after_reset_wins(void)
{
    struct virtio_actor actor;
    struct test_backend backend;

    backend_init(&backend);
    require_int("init", virtio_actor_init(&actor, &test_ops, &backend, 4), 0);
    require_int("configure", virtio_actor_enter_configuring(&actor), 0);
    require_int("activate", virtio_actor_activate(&actor), 0);
    require_int("reset", virtio_actor_reset(&actor), 0);
    require_false("old generation completion rejected",
                  virtio_actor_begin_completion(&actor, 0));

    virtio_actor_destroy(&actor);
    backend_destroy(&backend);
}

static void test_stop_wakes_sleeping_or_paused_actor(void)
{
    struct virtio_actor actor;
    struct test_backend backend;

    backend_init(&backend);
    require_int("init", virtio_actor_init(&actor, &test_ops, &backend, 4), 0);
    require_int("start thread", virtio_actor_start(&actor), 0);
    require_int("configure", virtio_actor_enter_configuring(&actor), 0);
    require_int("activate", virtio_actor_activate(&actor), 0);
    require_int("pause", virtio_actor_pause(&actor), 0);
    require_int("stop", virtio_actor_stop(&actor), 0);
    require_state("stopped", virtio_actor_get_state(&actor),
                  VIRTIO_ACTOR_STOPPED);

    virtio_actor_destroy(&actor);
    backend_destroy(&backend);
}

struct wait_args {
    struct virtio_actor *actor;
    enum virtio_actor_state observed;
    int ret;
};

static void *wait_changed_thread(void *arg)
{
    struct wait_args *args = arg;

    args->ret = virtio_actor_wait_changed_from(args->actor, VIRTIO_ACTOR_ACTIVE,
                                               &args->observed);
    return NULL;
}

static void test_fail_marks_failed_and_wakes_waiters(void)
{
    struct virtio_actor actor;
    struct test_backend backend;
    struct wait_args args = {0};
    pthread_t thread;

    backend_init(&backend);
    require_int("init", virtio_actor_init(&actor, &test_ops, &backend, 4), 0);
    require_int("configure", virtio_actor_enter_configuring(&actor), 0);
    require_int("activate", virtio_actor_activate(&actor), 0);
    args.actor = &actor;
    require_int("waiter create",
                pthread_create(&thread, NULL, wait_changed_thread, &args), 0);
    sleep_ms(10);
    require_int("fail", virtio_actor_fail(&actor), 0);
    require_int("waiter join", pthread_join(thread, NULL), 0);
    require_int("waiter ret", args.ret, 0);
    require_state("waiter observed failed", args.observed, VIRTIO_ACTOR_FAILED);
    require_state("failed state", virtio_actor_get_state(&actor),
                  VIRTIO_ACTOR_FAILED);
    require_true("failed flag", actor.failed);
    require_int("wait until failed",
                virtio_actor_wait_until(&actor, VIRTIO_ACTOR_FAILED), 0);

    virtio_actor_destroy(&actor);
    backend_destroy(&backend);
}


struct wait_until_args {
    struct virtio_actor *actor;
    enum virtio_actor_state target;
    int ret;
};

static void *wait_until_thread(void *arg)
{
    struct wait_until_args *args = arg;

    args->ret = virtio_actor_wait_until(args->actor, args->target);
    return NULL;
}

static void require_thread_joins(const char *name, pthread_t thread)
{
    struct timespec deadline;
    int ret;

    require_int("clock_gettime", clock_gettime(CLOCK_REALTIME, &deadline), 0);
    deadline.tv_sec += 1;
    ret = pthread_timedjoin_np(thread, NULL, &deadline);
    if (ret == 0)
        return;

    fprintf(stderr, "%s: pthread_timedjoin_np got %d, want 0\n", name, ret);
    exit(1);
}

static void test_wait_until_returns_error_on_failed_state(void)
{
    struct virtio_actor actor;
    struct test_backend backend;
    struct wait_until_args args = {0};
    pthread_t thread;

    backend_init(&backend);
    require_int("init", virtio_actor_init(&actor, &test_ops, &backend, 4), 0);
    require_int("configure", virtio_actor_enter_configuring(&actor), 0);
    require_int("activate", virtio_actor_activate(&actor), 0);

    args.actor = &actor;
    args.target = VIRTIO_ACTOR_PAUSED;
    args.ret = 1;
    require_int("waiter create",
                pthread_create(&thread, NULL, wait_until_thread, &args), 0);
    sleep_ms(10);
    require_int("fail", virtio_actor_fail(&actor), 0);
    require_thread_joins("failed wait_until waiter", thread);
    require_int("wait_until failed ret", args.ret, -EIO);

    virtio_actor_destroy(&actor);
    backend_destroy(&backend);
}

static void test_wait_until_returns_error_on_stopped_state(void)
{
    struct virtio_actor actor;
    struct test_backend backend;
    struct wait_until_args args = {0};
    pthread_t thread;

    backend_init(&backend);
    require_int("init", virtio_actor_init(&actor, &test_ops, &backend, 4), 0);
    require_int("configure", virtio_actor_enter_configuring(&actor), 0);
    require_int("activate", virtio_actor_activate(&actor), 0);

    args.actor = &actor;
    args.target = VIRTIO_ACTOR_PAUSED;
    args.ret = 1;
    require_int("waiter create",
                pthread_create(&thread, NULL, wait_until_thread, &args), 0);
    sleep_ms(10);
    require_int("stop", virtio_actor_stop(&actor), 0);
    require_thread_joins("stopped wait_until waiter", thread);
    require_int("wait_until stopped ret", args.ret, -ECANCELED);

    virtio_actor_destroy(&actor);
    backend_destroy(&backend);
}

static void test_callbacks_run_outside_actor_lock(void)
{
    struct virtio_actor actor;
    struct test_backend backend;

    backend_init(&backend);
    require_int("init", virtio_actor_init(&actor, &test_ops, &backend, 4), 0);
    require_int("start thread", virtio_actor_start(&actor), 0);
    require_int("configure", virtio_actor_enter_configuring(&actor), 0);
    require_int("activate", virtio_actor_activate(&actor), 0);
    require_int("notify", virtio_actor_notify_queue(&actor, 0), 0);
    backend_wait_drains(&backend, 0, 1);
    pthread_mutex_lock(&backend.lock);
    require_int("actor lock not held in callbacks",
                (int) backend.lock_held_count, 0);
    pthread_mutex_unlock(&backend.lock);

    require_int("stop", virtio_actor_stop(&actor), 0);
    virtio_actor_destroy(&actor);
    backend_destroy(&backend);
}

int main(void)
{
    test_init_destroy_state_and_generation();
    test_configure_activate_transitions();
    test_thread_rejects_queue_notify_before_active();
    test_repeated_notify_coalesces_until_ack();
    test_invalid_queue_index_rejected();
    test_clear_recheck_prevents_lost_wakeup();
    test_reset_while_paused_does_not_deadlock();
    test_reset_waits_for_claimed_backend_mutation();
    test_completion_section_blocks_reset();
    test_completion_begin_fails_after_reset_wins();
    test_stop_wakes_sleeping_or_paused_actor();
    test_fail_marks_failed_and_wakes_waiters();
    test_wait_until_returns_error_on_failed_state();
    test_wait_until_returns_error_on_stopped_state();
    test_callbacks_run_outside_actor_lock();
    return 0;
}
