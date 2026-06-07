#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum {
    MAX_HARTS = 4,
    RFENCE_NONE = 0,
    RFENCE_I,
    RFENCE_VMA,
    HART_STARTED = 0,
    HART_STOPPED = 1,
    HART_SUSPENDED = 4,
};

typedef struct {
    atomic_bool pending_rfence;
    atomic_int hsm_state;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    atomic_uint wake_count;
    atomic_uint fence_i_count;
    atomic_uint fence_vma_count;
    atomic_bool stop_worker;
} hart_model_t;

typedef struct {
    atomic_int type;
    uint32_t start_addr;
    uint32_t size;
    uint32_t asid;
    atomic_bool stopped;
    atomic_int pending_count;
    pthread_mutex_t issue_mutex;
    pthread_mutex_t completion_mutex;
    pthread_cond_t completion_cond;
    hart_model_t harts[MAX_HARTS];
} rfence_model_t;

typedef struct {
    rfence_model_t *model;
    unsigned hartid;
} hart_arg_t;

static void fail(const char *name)
{
    fprintf(stderr, "%s\n", name);
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

static void wait_until_u32(const char *name, atomic_uint *value, uint32_t want)
{
    struct timespec end = deadline_ms(1000);

    while (atomic_load_explicit(value, memory_order_acquire) != want) {
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

static void wait_until_int(const char *name, atomic_int *value, int want)
{
    struct timespec end = deadline_ms(1000);

    while (atomic_load_explicit(value, memory_order_acquire) != want) {
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
    atomic_init(&hart->pending_rfence, false);
    atomic_init(&hart->hsm_state, state);
    atomic_init(&hart->wake_count, 0);
    atomic_init(&hart->fence_i_count, 0);
    atomic_init(&hart->fence_vma_count, 0);
    atomic_init(&hart->stop_worker, false);
    if (pthread_mutex_init(&hart->mutex, NULL) != 0)
        fail("hart mutex init failed");
    if (pthread_cond_init(&hart->cond, NULL) != 0)
        fail("hart cond init failed");
}

static void hart_model_destroy(hart_model_t *hart)
{
    pthread_cond_destroy(&hart->cond);
    pthread_mutex_destroy(&hart->mutex);
}

static void rfence_model_init(rfence_model_t *model)
{
    atomic_init(&model->type, RFENCE_NONE);
    atomic_init(&model->stopped, false);
    atomic_init(&model->pending_count, 0);
    model->start_addr = 0;
    model->size = 0;
    model->asid = 0;
    if (pthread_mutex_init(&model->issue_mutex, NULL) != 0)
        fail("issue mutex init failed");
    if (pthread_mutex_init(&model->completion_mutex, NULL) != 0)
        fail("completion mutex init failed");
    if (pthread_cond_init(&model->completion_cond, NULL) != 0)
        fail("completion cond init failed");
    for (int i = 0; i < MAX_HARTS; i++)
        hart_model_init(&model->harts[i], HART_STARTED);
}

static void rfence_model_destroy(rfence_model_t *model)
{
    for (int i = 0; i < MAX_HARTS; i++)
        hart_model_destroy(&model->harts[i]);
    pthread_cond_destroy(&model->completion_cond);
    pthread_mutex_destroy(&model->completion_mutex);
    pthread_mutex_destroy(&model->issue_mutex);
}

static bool rfence_targets_hart(uint32_t hartid,
                                uint32_t hart_mask,
                                uint32_t hart_mask_base)
{
    if (hart_mask_base == UINT32_MAX)
        return true;
    if (hartid < hart_mask_base)
        return false;

    uint32_t bit = hartid - hart_mask_base;
    if (bit >= 32)
        return false;
    return ((hart_mask >> bit) & 1U) != 0;
}

static void apply_rfence(rfence_model_t *model, unsigned hartid, int type)
{
    if (type == RFENCE_I)
        atomic_fetch_add_explicit(&model->harts[hartid].fence_i_count, 1,
                                  memory_order_relaxed);
    else if (type == RFENCE_VMA)
        atomic_fetch_add_explicit(&model->harts[hartid].fence_vma_count, 1,
                                  memory_order_relaxed);
}

static void rfence_ack(rfence_model_t *model)
{
    int old = atomic_fetch_sub_explicit(&model->pending_count, 1,
                                        memory_order_acq_rel);
    if (old == 1) {
        pthread_mutex_lock(&model->completion_mutex);
        pthread_cond_broadcast(&model->completion_cond);
        pthread_mutex_unlock(&model->completion_mutex);
    }
}

static void stop_model(rfence_model_t *model)
{
    atomic_store_explicit(&model->stopped, true, memory_order_release);
    pthread_mutex_lock(&model->completion_mutex);
    pthread_cond_broadcast(&model->completion_cond);
    pthread_mutex_unlock(&model->completion_mutex);
}

static void process_pending_rfence(rfence_model_t *model, unsigned hartid)
{
    hart_model_t *hart = &model->harts[hartid];

    if (!atomic_load_explicit(&hart->pending_rfence, memory_order_acquire))
        return;

    int type = atomic_load_explicit(&model->type, memory_order_acquire);
    atomic_store_explicit(&hart->pending_rfence, false, memory_order_release);
    apply_rfence(model, hartid, type);
    rfence_ack(model);
}

static void lock_rfence_issue(rfence_model_t *model, unsigned hartid)
{
    for (;;) {
        process_pending_rfence(model, hartid);
        if (pthread_mutex_trylock(&model->issue_mutex) == 0)
            return;
        sched_yield();
    }
}

static void signal_hart(rfence_model_t *model, unsigned hartid)
{
    hart_model_t *hart = &model->harts[hartid];

    pthread_mutex_lock(&hart->mutex);
    atomic_fetch_add_explicit(&hart->wake_count, 1, memory_order_relaxed);
    pthread_cond_signal(&hart->cond);
    pthread_mutex_unlock(&hart->mutex);
}

static void issue_rfence(rfence_model_t *model,
                         unsigned issuer,
                         int type,
                         uint32_t hart_mask,
                         uint32_t hart_mask_base)
{
    int pending_count = 0;
    bool pending_targets[MAX_HARTS] = {0};

    lock_rfence_issue(model, issuer);
    atomic_store_explicit(&model->type, type, memory_order_release);

    for (unsigned i = 0; i < MAX_HARTS; i++) {
        if (!rfence_targets_hart(i, hart_mask, hart_mask_base))
            continue;
        if (i == issuer ||
            atomic_load_explicit(&model->harts[i].hsm_state,
                                 memory_order_acquire) != HART_STARTED) {
            apply_rfence(model, i, type);
            continue;
        }
        pending_targets[i] = true;
        pending_count++;
    }

    atomic_store_explicit(&model->pending_count, pending_count,
                          memory_order_release);
    for (unsigned i = 0; i < MAX_HARTS; i++) {
        if (!pending_targets[i])
            continue;
        atomic_store_explicit(&model->harts[i].pending_rfence, true,
                              memory_order_release);
        signal_hart(model, i);
    }

    pthread_mutex_lock(&model->completion_mutex);
    while (atomic_load_explicit(&model->pending_count, memory_order_acquire) >
               0 &&
           !atomic_load_explicit(&model->stopped, memory_order_acquire)) {
        pthread_cond_wait(&model->completion_cond, &model->completion_mutex);
    }
    bool stopped =
        atomic_load_explicit(&model->pending_count, memory_order_acquire) > 0;
    pthread_mutex_unlock(&model->completion_mutex);

    if (!stopped)
        atomic_store_explicit(&model->type, RFENCE_NONE, memory_order_release);
    pthread_mutex_unlock(&model->issue_mutex);
}

static void *hart_worker(void *opaque)
{
    hart_arg_t *arg = opaque;
    hart_model_t *hart = &arg->model->harts[arg->hartid];

    pthread_mutex_lock(&hart->mutex);
    while (!atomic_load_explicit(&hart->stop_worker, memory_order_acquire)) {
        while (
            !atomic_load_explicit(&hart->pending_rfence,
                                  memory_order_acquire) &&
            !atomic_load_explicit(&hart->stop_worker, memory_order_acquire)) {
            pthread_cond_wait(&hart->cond, &hart->mutex);
        }
        pthread_mutex_unlock(&hart->mutex);
        process_pending_rfence(arg->model, arg->hartid);
        pthread_mutex_lock(&hart->mutex);
    }
    pthread_mutex_unlock(&hart->mutex);
    return NULL;
}

static void stop_worker(rfence_model_t *model,
                        unsigned hartid,
                        pthread_t thread)
{
    hart_model_t *hart = &model->harts[hartid];

    pthread_mutex_lock(&hart->mutex);
    atomic_store_explicit(&hart->stop_worker, true, memory_order_release);
    pthread_cond_signal(&hart->cond);
    pthread_mutex_unlock(&hart->mutex);
    pthread_join(thread, NULL);
}

static void test_mask_selection(void)
{
    require_bool("all mask selects hart 0",
                 rfence_targets_hart(0, 0, UINT32_MAX), true);
    require_bool("all mask selects hart 3",
                 rfence_targets_hart(3, 0, UINT32_MAX), true);
    require_bool("sparse mask selects base bit", rfence_targets_hart(2, 0x5, 2),
                 true);
    require_bool("sparse mask skips cleared bit",
                 rfence_targets_hart(3, 0x5, 2), false);
    require_bool("sparse mask selects second set bit",
                 rfence_targets_hart(4, 0x5, 2), true);
    require_bool("base beyond hart skips",
                 rfence_targets_hart(1, 0xffffffffu, 2), false);
    require_bool("empty mask skips", rfence_targets_hart(2, 0, 2), false);
}

static void test_self_target_does_not_wait(void)
{
    rfence_model_t model;
    rfence_model_init(&model);

    issue_rfence(&model, 1, RFENCE_I, 1u << 1, 0);
    require_u32("self RFENCE applies locally",
                atomic_load_explicit(&model.harts[1].fence_i_count,
                                     memory_order_relaxed),
                1);
    require_u32(
        "self RFENCE leaves no remote pending",
        atomic_load_explicit(&model.pending_count, memory_order_relaxed), 0);
    rfence_model_destroy(&model);
}

static void test_wfi_target_wakes_and_acks(void)
{
    rfence_model_t model;
    pthread_t worker;
    hart_arg_t arg = {.model = &model, .hartid = 2};
    rfence_model_init(&model);

    if (pthread_create(&worker, NULL, hart_worker, &arg) != 0)
        fail("hart worker create failed");

    issue_rfence(&model, 0, RFENCE_VMA, 1u << 2, 0);
    require_u32(
        "WFI target receives wake",
        atomic_load_explicit(&model.harts[2].wake_count, memory_order_relaxed),
        1);
    require_u32("WFI target applies VMA RFENCE",
                atomic_load_explicit(&model.harts[2].fence_vma_count,
                                     memory_order_relaxed),
                1);
    require_u32(
        "WFI target ack clears pending count",
        atomic_load_explicit(&model.pending_count, memory_order_relaxed), 0);

    stop_worker(&model, 2, worker);
    rfence_model_destroy(&model);
}

static void test_pending_targets_are_snapshotted_before_signal(void)
{
    rfence_model_t model;
    pthread_t worker;
    hart_arg_t arg = {.model = &model, .hartid = 2};
    rfence_model_init(&model);

    if (pthread_create(&worker, NULL, hart_worker, &arg) != 0)
        fail("hart worker create failed");

    pthread_mutex_lock(&model.issue_mutex);
    atomic_store_explicit(&model.type, RFENCE_I, memory_order_release);
    atomic_store_explicit(&model.pending_count, 1, memory_order_release);
    atomic_store_explicit(&model.harts[2].hsm_state, HART_STOPPED,
                          memory_order_release);
    atomic_store_explicit(&model.harts[2].pending_rfence, true,
                          memory_order_release);
    signal_hart(&model, 2);

    pthread_mutex_lock(&model.completion_mutex);
    while (atomic_load_explicit(&model.pending_count, memory_order_acquire) >
           0) {
        pthread_cond_wait(&model.completion_cond, &model.completion_mutex);
    }
    pthread_mutex_unlock(&model.completion_mutex);

    require_u32("snapshotted target applies despite later HSM change",
                atomic_load_explicit(&model.harts[2].fence_i_count,
                                     memory_order_relaxed),
                1);
    pthread_mutex_unlock(&model.issue_mutex);

    stop_worker(&model, 2, worker);
    rfence_model_destroy(&model);
}

static void test_stopped_targets_apply_without_remote_ack(void)
{
    rfence_model_t model;
    rfence_model_init(&model);
    atomic_store_explicit(&model.harts[2].hsm_state, HART_STOPPED,
                          memory_order_release);
    atomic_store_explicit(&model.harts[3].hsm_state, HART_SUSPENDED,
                          memory_order_release);

    issue_rfence(&model, 0, RFENCE_I, 0x3u, 2);
    require_u32("stopped target applies synchronously",
                atomic_load_explicit(&model.harts[2].fence_i_count,
                                     memory_order_relaxed),
                1);
    require_u32("suspended target applies synchronously",
                atomic_load_explicit(&model.harts[3].fence_i_count,
                                     memory_order_relaxed),
                1);
    require_u32(
        "non-running targets do not need ack",
        atomic_load_explicit(&model.pending_count, memory_order_relaxed), 0);
    rfence_model_destroy(&model);
}

static void *lock_issue_thread(void *opaque)
{
    hart_arg_t *arg = opaque;

    lock_rfence_issue(arg->model, arg->hartid);
    pthread_mutex_unlock(&arg->model->issue_mutex);
    return NULL;
}

static void test_waiting_issuer_processes_own_pending_first(void)
{
    rfence_model_t model;
    pthread_t waiter;
    hart_arg_t arg = {.model = &model, .hartid = 1};
    rfence_model_init(&model);

    pthread_mutex_lock(&model.issue_mutex);
    atomic_store_explicit(&model.type, RFENCE_VMA, memory_order_release);
    atomic_store_explicit(&model.pending_count, 1, memory_order_release);
    atomic_store_explicit(&model.harts[1].pending_rfence, true,
                          memory_order_release);

    if (pthread_create(&waiter, NULL, lock_issue_thread, &arg) != 0)
        fail("lock waiter create failed");

    wait_until_u32("waiting issuer did not process pending RFENCE",
                   &model.harts[1].fence_vma_count, 1);
    require_u32(
        "processing pending RFENCE acks original requester",
        atomic_load_explicit(&model.pending_count, memory_order_relaxed), 0);

    pthread_mutex_unlock(&model.issue_mutex);
    pthread_join(waiter, NULL);
    rfence_model_destroy(&model);
}

typedef struct {
    rfence_model_t *model;
    atomic_uint returned;
} blocking_issue_arg_t;

static void *blocking_issue_thread(void *opaque)
{
    blocking_issue_arg_t *arg = opaque;

    issue_rfence(arg->model, 0, RFENCE_I, 1u << 2, 0);
    atomic_store_explicit(&arg->returned, 1, memory_order_release);
    return NULL;
}

static void test_stop_wakes_waiting_requester(void)
{
    rfence_model_t model;
    pthread_t issuer;
    blocking_issue_arg_t arg = {.model = &model};
    rfence_model_init(&model);
    atomic_init(&arg.returned, 0);

    if (pthread_create(&issuer, NULL, blocking_issue_thread, &arg) != 0)
        fail("blocking issue thread create failed");

    wait_until_int("blocking RFENCE did not publish pending count",
                   &model.pending_count, 1);
    stop_model(&model);
    wait_until_u32("stopped RFENCE requester did not return", &arg.returned, 1);
    pthread_join(issuer, NULL);
    rfence_model_destroy(&model);
}

static void test_duplicate_service_does_not_double_ack(void)
{
    rfence_model_t model;
    rfence_model_init(&model);

    atomic_store_explicit(&model.type, RFENCE_I, memory_order_release);
    atomic_store_explicit(&model.pending_count, 1, memory_order_release);
    atomic_store_explicit(&model.harts[1].pending_rfence, true,
                          memory_order_release);
    process_pending_rfence(&model, 1);
    process_pending_rfence(&model, 1);

    require_u32("RFENCE service applies once",
                atomic_load_explicit(&model.harts[1].fence_i_count,
                                     memory_order_relaxed),
                1);
    require_u32(
        "RFENCE service acks once",
        atomic_load_explicit(&model.pending_count, memory_order_relaxed), 0);
    rfence_model_destroy(&model);
}

int main(void)
{
    test_mask_selection();
    test_self_target_does_not_wait();
    test_wfi_target_wakes_and_acks();
    test_pending_targets_are_snapshotted_before_signal();
    test_stopped_targets_apply_without_remote_ack();
    test_waiting_issuer_processes_own_pending_first();
    test_stop_wakes_waiting_requester();
    test_duplicate_service_does_not_double_ack();
    return 0;
}
