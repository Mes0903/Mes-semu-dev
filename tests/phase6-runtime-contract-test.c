#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main semu_main_under_test
#include "../main.c"
#undef main

static void require_bool(const char *name, bool got, bool want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %s, want %s\n", name, got ? "true" : "false",
            want ? "true" : "false");
    exit(1);
}

static void require_int(const char *name, int got, int want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %d, want %d\n", name, got, want);
    exit(1);
}

static void test_default_selector_maps_hart_count_to_executor_mode(void)
{
    require_int("one hart defaults to single-thread",
                semu_executor_default_mode(1), SEMU_EXECUTOR_SINGLE_THREAD);
    require_int("SMP defaults to legacy gate", semu_executor_default_mode(2),
                SEMU_EXECUTOR_THREADED_CPU_WITH_LEGACY_DEVICE_GATE);
    require_int("SMP default uses dedicated backend",
                semu_executor_backend_for_mode(semu_executor_default_mode(2)),
                HART_EXEC_DEDICATED_THREADS);
}

static void test_runtime_follows_explicit_executor_backend(void)
{
    emu_state_t emu;
    memset(&emu, 0, sizeof(emu));

    emu.vm.n_hart = 2;
    emu.executor_mode = SEMU_EXECUTOR_SINGLE_THREAD;
    emu.executor_backend = semu_executor_backend_for_mode(emu.executor_mode);
    require_bool("explicit single-thread disables threaded runtime",
                 semu_should_use_threaded_runtime(&emu), false);
    require_bool("explicit single-thread selects nonblocking WFI",
                 semu_wfi_handler_for_config(&emu) == wfi_handler_single_hart,
                 true);

    emu.executor_mode = SEMU_EXECUTOR_THREADED_CPU_WITH_LEGACY_DEVICE_GATE;
    emu.executor_backend = semu_executor_backend_for_mode(emu.executor_mode);
    require_bool("legacy gate enables threaded runtime",
                 semu_should_use_threaded_runtime(&emu), true);
    require_bool("legacy gate selects threaded WFI",
                 semu_wfi_handler_for_config(&emu) == wfi_handler_threaded,
                 true);

    emu.executor_mode = SEMU_EXECUTOR_THREADED_CPU_WITH_DEVICE_ACTORS;
    emu.executor_backend = semu_executor_backend_for_mode(emu.executor_mode);
    require_bool("actor mode enables threaded runtime",
                 semu_should_use_threaded_runtime(&emu), true);
    require_bool("actor mode selects threaded WFI",
                 semu_wfi_handler_for_config(&emu) == wfi_handler_threaded,
                 true);
}

static void test_single_hart_wfi_does_not_block_without_interrupt(void)
{
    emu_state_t emu;
    hart_t hart;
    memset(&emu, 0, sizeof(emu));
    memset(&hart, 0, sizeof(hart));

    emu.vm.n_hart = 1;
    hart.priv = &emu;
    hart.vm = &emu.vm;
    hart_in_wfi_store(&hart, false);

    wfi_handler_single_hart(&hart);

    require_bool("single-hart WFI leaves no-wait state alone",
                 hart_in_wfi_load(&hart), false);
}

static void test_single_hart_wfi_clears_wait_flag_with_interrupt(void)
{
    emu_state_t emu;
    hart_t hart;
    memset(&emu, 0, sizeof(emu));
    memset(&hart, 0, sizeof(hart));

    emu.vm.n_hart = 1;
    hart.priv = &emu;
    hart.vm = &emu.vm;
    hart_sie_store(&hart, RV_INT_SSI_BIT);
    hart_sip_set_bits(&hart, RV_INT_SSI_BIT);
    hart_in_wfi_store(&hart, true);

    wfi_handler_single_hart(&hart);

    require_bool("single-hart WFI clears wait flag for pending interrupt",
                 hart_in_wfi_load(&hart), false);
}


static int lifecycle_stop_start_calls;
static int lifecycle_stop_request_stop_calls;
static int lifecycle_stop_join_calls;

static int lifecycle_stop_start(struct emu_state *emu)
{
    lifecycle_stop_start_calls++;
    semu_set_stopped(emu, true);
    return 0;
}

static void lifecycle_stop_request_stop(struct emu_state *emu)
{
    lifecycle_stop_request_stop_calls++;
    semu_set_stopped(emu, true);
}

static int lifecycle_stop_join(struct emu_state *emu UNUSED)
{
    lifecycle_stop_join_calls++;
    return 0;
}

static const struct hart_executor_ops lifecycle_stop_ops = {
    .start = lifecycle_stop_start,
    .request_stop = lifecycle_stop_request_stop,
    .join = lifecycle_stop_join,
};

static void test_threaded_shutdown_enters_stopped_lifecycle_state(void)
{
    emu_state_t emu;
    memset(&emu, 0, sizeof(emu));
    require_int("lifecycle init", semu_vm_lifecycle_init(&emu.lifecycle), 0);
    require_int("lifecycle running",
                semu_vm_lifecycle_enter_running(&emu.lifecycle), 0);
    emu.executor.ops = &lifecycle_stop_ops;
    lifecycle_stop_start_calls = 0;
    lifecycle_stop_request_stop_calls = 0;
    lifecycle_stop_join_calls = 0;

    semu_run_threaded(&emu);

    require_int("threaded start called", lifecycle_stop_start_calls, 1);
    require_int("threaded request_stop called",
                lifecycle_stop_request_stop_calls, 1);
    require_int("threaded join called", lifecycle_stop_join_calls, 1);
    require_int("threaded shutdown lifecycle stopped",
                semu_vm_lifecycle_state(&emu.lifecycle), SEMU_VM_STOPPED);
    semu_vm_lifecycle_destroy(&emu.lifecycle);
}

int main(void)
{
    test_default_selector_maps_hart_count_to_executor_mode();
    test_runtime_follows_explicit_executor_backend();
    test_single_hart_wfi_does_not_block_without_interrupt();
    test_single_hart_wfi_clears_wait_flag_with_interrupt();
    test_threaded_shutdown_enters_stopped_lifecycle_state();
    return 0;
}
