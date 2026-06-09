#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main semu_main_under_test
#include "../main.c"
#undef main


static bool gdbstub_init_result;
static bool gdbstub_run_result;
static int gdbstub_init_calls;
static int gdbstub_run_calls;
static int gdbstub_close_calls;

void u8250_update_interrupts(u8250_state_t *uart UNUSED)
{
}

void u8250_check_ready(u8250_state_t *uart UNUSED)
{
}

void u8250_flush_out(u8250_state_t *uart UNUSED)
{
}

void semu_irq_source_set(emu_state_t *emu UNUSED,
                         enum semu_irq_source source UNUSED,
                         bool level UNUSED)
{
}

bool gdbstub_init(gdbstub_t *gdbstub UNUSED,
                  struct target_ops *ops UNUSED,
                  arch_info_t arch UNUSED,
                  char *s UNUSED)
{
    gdbstub_init_calls++;
    return gdbstub_init_result;
}

bool gdbstub_run(gdbstub_t *gdbstub UNUSED, void *args UNUSED)
{
    gdbstub_run_calls++;
    return gdbstub_run_result;
}

void gdbstub_close(gdbstub_t *gdbstub UNUSED)
{
    gdbstub_close_calls++;
}


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
static int lifecycle_start_failure_start_calls;
static int lifecycle_start_failure_request_stop_calls;
static int lifecycle_start_failure_join_calls;
static emu_state_t *lifecycle_stop_expected_emu;

static int lifecycle_stop_start(struct emu_state *emu)
{
    lifecycle_stop_start_calls++;
    require_bool("runtime start gets expected emu",
                 emu == lifecycle_stop_expected_emu, true);
    require_int("runtime lifecycle running before executor start",
                semu_vm_lifecycle_state(&emu->lifecycle), SEMU_VM_RUNNING);
    require_bool("runtime accepting before executor start",
                 semu_vm_accepting_device_work(&emu->lifecycle), true);
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

static int lifecycle_start_failure_start(struct emu_state *emu)
{
    lifecycle_start_failure_start_calls++;
    require_int("start failure lifecycle running before start",
                semu_vm_lifecycle_state(&emu->lifecycle), SEMU_VM_RUNNING);
    require_bool("start failure accepting before start",
                 semu_vm_accepting_device_work(&emu->lifecycle), true);
    return -EIO;
}

static void lifecycle_start_failure_request_stop(struct emu_state *emu)
{
    lifecycle_start_failure_request_stop_calls++;
    semu_set_stopped(emu, true);
}

static int lifecycle_start_failure_join(struct emu_state *emu UNUSED)
{
    lifecycle_start_failure_join_calls++;
    return 0;
}

static const struct hart_executor_ops lifecycle_start_failure_ops = {
    .start = lifecycle_start_failure_start,
    .request_stop = lifecycle_start_failure_request_stop,
    .join = lifecycle_start_failure_join,
};


static void test_threaded_start_failure_leaves_lifecycle_failed(void)
{
    emu_state_t emu;
    memset(&emu, 0, sizeof(emu));
    require_int("lifecycle init", semu_vm_lifecycle_init(&emu.lifecycle), 0);
    emu.executor.ops = &lifecycle_start_failure_ops;
    lifecycle_start_failure_start_calls = 0;
    lifecycle_start_failure_request_stop_calls = 0;
    lifecycle_start_failure_join_calls = 0;

    semu_run_threaded(&emu);

    require_int("start failure start called", lifecycle_start_failure_start_calls,
                1);
    require_int("start failure request_stop called",
                lifecycle_start_failure_request_stop_calls, 1);
    require_int("start failure join called", lifecycle_start_failure_join_calls,
                1);
    require_int("start failure exit code", emu.exit_code, 1);
    require_int("start failure lifecycle failed",
                semu_vm_lifecycle_state(&emu.lifecycle), SEMU_VM_FAILED);
    require_bool("start failure lifecycle not accepting",
                 semu_vm_accepting_device_work(&emu.lifecycle), false);
    semu_vm_lifecycle_destroy(&emu.lifecycle);
}

static void test_single_thread_runtime_exit_stops_lifecycle(void)
{
    emu_state_t emu;
    memset(&emu, 0, sizeof(emu));
    require_int("lifecycle init", semu_vm_lifecycle_init(&emu.lifecycle), 0);
    emu.executor_backend = HART_EXEC_SINGLE_THREAD;
    semu_set_stopped(&emu, true);

    semu_run(&emu);

    require_int("single-thread exit code", emu.exit_code, 0);
    require_int("single-thread lifecycle stopped",
                semu_vm_lifecycle_state(&emu.lifecycle), SEMU_VM_STOPPED);
    require_bool("single-thread lifecycle not accepting",
                 semu_vm_accepting_device_work(&emu.lifecycle), false);
    semu_vm_lifecycle_destroy(&emu.lifecycle);
}

static void test_debug_runtime_exit_stops_lifecycle(void)
{
    emu_state_t emu;
    memset(&emu, 0, sizeof(emu));
    require_int("lifecycle init", semu_vm_lifecycle_init(&emu.lifecycle), 0);
    emu.vm.n_hart = 1;
    gdbstub_init_result = true;
    gdbstub_run_result = true;
    gdbstub_init_calls = 0;
    gdbstub_run_calls = 0;
    gdbstub_close_calls = 0;

    semu_run_debug(&emu);

    require_int("debug init called", gdbstub_init_calls, 1);
    require_int("debug run called", gdbstub_run_calls, 1);
    require_int("debug close called", gdbstub_close_calls, 1);
    require_int("debug exit code", emu.exit_code, 0);
    require_int("debug lifecycle stopped",
                semu_vm_lifecycle_state(&emu.lifecycle), SEMU_VM_STOPPED);
    require_bool("debug lifecycle not accepting",
                 semu_vm_accepting_device_work(&emu.lifecycle), false);
    semu_vm_lifecycle_destroy(&emu.lifecycle);
}

static void test_debug_unsupported_config_fails_lifecycle(void)
{
    emu_state_t emu;
    memset(&emu, 0, sizeof(emu));
    require_int("lifecycle init", semu_vm_lifecycle_init(&emu.lifecycle), 0);
    emu.vm.n_hart = 2;
    emu.exit_code = 99;
    gdbstub_init_result = true;
    gdbstub_run_result = true;
    gdbstub_init_calls = 0;
    gdbstub_run_calls = 0;
    gdbstub_close_calls = 0;

    semu_run_debug(&emu);

    require_int("debug unsupported exit code", emu.exit_code, 1);
    require_int("debug unsupported skips init", gdbstub_init_calls, 0);
    require_int("debug unsupported skips run", gdbstub_run_calls, 0);
    require_int("debug unsupported skips close", gdbstub_close_calls, 0);
    require_int("debug unsupported lifecycle failed",
                semu_vm_lifecycle_state(&emu.lifecycle), SEMU_VM_FAILED);
    require_bool("debug unsupported lifecycle not accepting",
                 semu_vm_accepting_device_work(&emu.lifecycle), false);
    semu_vm_lifecycle_destroy(&emu.lifecycle);
}

static void test_debug_init_failure_fails_lifecycle(void)
{
    emu_state_t emu;
    memset(&emu, 0, sizeof(emu));
    require_int("lifecycle init", semu_vm_lifecycle_init(&emu.lifecycle), 0);
    emu.vm.n_hart = 1;
    emu.exit_code = 99;
    gdbstub_init_result = false;
    gdbstub_run_result = true;
    gdbstub_init_calls = 0;
    gdbstub_run_calls = 0;
    gdbstub_close_calls = 0;

    semu_run_debug(&emu);

    require_int("debug init failure init called", gdbstub_init_calls, 1);
    require_int("debug init failure skips run", gdbstub_run_calls, 0);
    require_int("debug init failure skips close", gdbstub_close_calls, 0);
    require_int("debug init failure exit code", emu.exit_code, 1);
    require_int("debug init failure lifecycle failed",
                semu_vm_lifecycle_state(&emu.lifecycle), SEMU_VM_FAILED);
    require_bool("debug init failure lifecycle not accepting",
                 semu_vm_accepting_device_work(&emu.lifecycle), false);
    semu_vm_lifecycle_destroy(&emu.lifecycle);
}

static void test_runtime_enters_running_before_threaded_executor_start(void)
{
    emu_state_t emu;
    memset(&emu, 0, sizeof(emu));
    require_int("lifecycle init", semu_vm_lifecycle_init(&emu.lifecycle), 0);
    require_int("initial lifecycle created",
                semu_vm_lifecycle_state(&emu.lifecycle), SEMU_VM_CREATED);
    require_bool("initial lifecycle not accepting",
                 semu_vm_accepting_device_work(&emu.lifecycle), false);
    emu.executor.ops = &lifecycle_stop_ops;
    lifecycle_stop_expected_emu = &emu;
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
    test_threaded_start_failure_leaves_lifecycle_failed();
    test_single_thread_runtime_exit_stops_lifecycle();
    test_debug_runtime_exit_stops_lifecycle();
    test_debug_unsupported_config_fails_lifecycle();
    test_debug_init_failure_fails_lifecycle();
    test_runtime_enters_running_before_threaded_executor_start();
    return 0;
}
