#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main semu_main_under_test
#include "../main.c"
#undef main

static int gdbstub_init_calls;
static int gdbstub_run_calls;
static int gdbstub_close_calls;
static int gdbstub_last_smp;

bool gdbstub_init(gdbstub_t *gdbstub,
                  struct target_ops *ops,
                  arch_info_t arch,
                  char *s)
{
    (void) gdbstub;
    (void) ops;
    (void) s;
    gdbstub_init_calls++;
    gdbstub_last_smp = arch.smp;
    return false;
}

bool gdbstub_run(gdbstub_t *gdbstub, void *args)
{
    (void) gdbstub;
    (void) args;
    gdbstub_run_calls++;
    return false;
}

void gdbstub_close(gdbstub_t *gdbstub)
{
    (void) gdbstub;
    gdbstub_close_calls++;
}

static void require_int(const char *name, int got, int want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %d, want %d\n", name, got, want);
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

static void reset_gdbstub_stubs(void)
{
    gdbstub_init_calls = 0;
    gdbstub_run_calls = 0;
    gdbstub_close_calls = 0;
    gdbstub_last_smp = -1;
}

static void test_multi_hart_debug_is_rejected_before_gdbstub(void)
{
    emu_state_t emu;
    memset(&emu, 0, sizeof(emu));
    reset_gdbstub_stubs();

    emu.debug = true;
    emu.vm.n_hart = 2;
    emu.exit_code = 99;

    require_bool("multi-hart debug unsupported",
                 semu_debug_config_supported(&emu), false);

    semu_run_debug(&emu);

    require_int("multi-hart debug exit code", emu.exit_code, 1);
    require_int("multi-hart debug skips gdbstub init", gdbstub_init_calls, 0);
    require_int("multi-hart debug skips gdbstub run", gdbstub_run_calls, 0);
    require_int("multi-hart debug skips gdbstub close", gdbstub_close_calls, 0);
}

static void test_single_hart_debug_config_is_accepted(void)
{
    emu_state_t emu;
    memset(&emu, 0, sizeof(emu));
    reset_gdbstub_stubs();

    emu.debug = true;
    emu.vm.n_hart = 1;
    emu.exit_code = 99;

    require_bool("single-hart debug supported",
                 semu_debug_config_supported(&emu), true);

    semu_run_debug(&emu);

    require_int("single-hart debug enters gdbstub init", gdbstub_init_calls, 1);
    require_int("single-hart debug advertises one hart", gdbstub_last_smp, 1);
    require_int("single-hart failed init exit code", emu.exit_code, 1);
    require_int("failed init skips gdbstub run", gdbstub_run_calls, 0);
    require_int("failed init skips gdbstub close", gdbstub_close_calls, 0);
}

static void test_invalid_cpu_selection_does_not_change_current_cpu(void)
{
    emu_state_t emu;
    memset(&emu, 0, sizeof(emu));

    emu.vm.n_hart = 2;
    emu.curr_cpuid = 1;

    semu_set_cpu(&emu, 7);
    require_int("out-of-range CPU selection keeps current CPU",
                emu.curr_cpuid, 1);

    semu_set_cpu(&emu, -1);
    require_int("negative CPU selection keeps current CPU", emu.curr_cpuid,
                1);

    semu_set_cpu(&emu, 0);
    require_int("valid CPU selection updates current CPU", emu.curr_cpuid, 0);
}

int main(void)
{
    test_multi_hart_debug_is_rejected_before_gdbstub();
    test_single_hart_debug_config_is_accepted();
    test_invalid_cpu_selection_does_not_change_current_cpu();
    return 0;
}
