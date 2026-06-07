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

static void test_threaded_runtime_skips_single_hart(void)
{
    emu_state_t emu;
    memset(&emu, 0, sizeof(emu));

    emu.vm.n_hart = 1;
    require_bool("threaded runtime disabled for one hart",
                 semu_should_use_threaded_runtime(&emu), false);
    require_bool("one hart selects single-hart WFI",
                 semu_wfi_handler_for_config(&emu) == wfi_handler_single_hart,
                 true);

    emu.vm.n_hart = 2;
    require_bool("threaded runtime enabled for SMP",
                 semu_should_use_threaded_runtime(&emu), true);
    require_bool("SMP selects threaded WFI",
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

int main(void)
{
    test_threaded_runtime_skips_single_hart();
    test_single_hart_wfi_does_not_block_without_interrupt();
    test_single_hart_wfi_clears_wait_flag_with_interrupt();
    return 0;
}
