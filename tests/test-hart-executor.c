#include "device.h"

#include <stdio.h>
#include <stdlib.h>

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
    check_true(hart_executor_join(&emu) == 0,
               "single-thread join is a no-op");
    hart_executor_destroy(&emu);
}

int main(void)
{
    test_dedicated_backend_lifecycle();
    test_single_thread_backend_uses_no_wait_state();

    if (failures != 0) {
        fprintf(stderr, "%d hart executor checks failed\n", failures);
        return 1;
    }
    return 0;
}
