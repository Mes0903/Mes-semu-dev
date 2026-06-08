#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "irq-source.h"
#include "platform.h"
#include "riscv_private.h"

static unsigned wake_count;

void vm_set_exception(hart_t *hart, uint32_t cause, uint32_t val)
{
    hart->error = ERR_EXCEPTION;
    hart->exc_cause = cause;
    hart->exc_val = val;
}

void semu_wake_interruptible_harts(emu_state_t *emu)
{
    (void) emu;
    wake_count++;
}

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

static void require_u32(const char *name, uint32_t got, uint32_t want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got 0x%x, want 0x%x\n", name, got, want);
    exit(1);
}

static void require_unsigned(const char *name, unsigned got, unsigned want)
{
    if (got == want)
        return;

    fprintf(stderr, "%s: got %u, want %u\n", name, got, want);
    exit(1);
}

static void init_one_hart_vm(vm_t *vm, hart_t *hart, hart_t **harts)
{
    memset(hart, 0, sizeof(*hart));
    memset(vm, 0, sizeof(*vm));
    hart->vm = vm;
    harts[0] = hart;
    vm->n_hart = 1;
    vm->hart = harts;
}

static void test_source_mapping_uses_platform_irq_ids(void)
{
    static const struct {
        enum semu_irq_source source;
        uint32_t id;
    } cases[] = {
        { SEMU_IRQ_SOURCE_UART, SEMU_PLATFORM_IRQ_UART },
        { SEMU_IRQ_SOURCE_VNET, SEMU_PLATFORM_IRQ_VNET },
        { SEMU_IRQ_SOURCE_VBLK, SEMU_PLATFORM_IRQ_VBLK },
        { SEMU_IRQ_SOURCE_VRNG, SEMU_PLATFORM_IRQ_VRNG },
        { SEMU_IRQ_SOURCE_VSND, SEMU_PLATFORM_IRQ_VSND },
        { SEMU_IRQ_SOURCE_VFS, SEMU_PLATFORM_IRQ_VFS },
        { SEMU_IRQ_SOURCE_VINPUT_KEYBOARD,
          SEMU_PLATFORM_IRQ_VINPUT_KEYBOARD },
        { SEMU_IRQ_SOURCE_VINPUT_MOUSE, SEMU_PLATFORM_IRQ_VINPUT_MOUSE },
        { SEMU_IRQ_SOURCE_VGPU, SEMU_PLATFORM_IRQ_VGPU },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        require_u32("irq source plic id",
                    semu_irq_source_plic_id(cases[i].source), cases[i].id);
        require_u32("irq source plic bit",
                    semu_irq_source_plic_bit(cases[i].source),
                    UINT32_C(1) << cases[i].id);
    }

    require_u32("invalid source plic id",
                semu_irq_source_plic_id((enum semu_irq_source) -1),
                SEMU_PLATFORM_IRQ_NONE);
    require_u32("invalid source plic bit",
                semu_irq_source_plic_bit((enum semu_irq_source) -1), 0);
}

static void test_apply_level_sets_active_and_pending_like_old_path(void)
{
    hart_t hart;
    hart_t *harts[1];
    vm_t vm;
    plic_state_t plic = {0};
    uint32_t bit = semu_irq_source_plic_bit(SEMU_IRQ_SOURCE_VGPU);

    init_one_hart_vm(&vm, &hart, harts);
    plic.ie[0] = bit;
    hart_in_wfi_store(&hart, true);

    require_true("apply vgpu high",
                 semu_irq_source_apply_level(&vm, &plic, SEMU_IRQ_SOURCE_VGPU,
                                             true));

    require_u32("active set", plic.active & bit, bit);
    require_u32("pending set", plic.ip & bit, bit);
    require_u32("masked set", plic.masked & bit, bit);
    require_u32("hart external interrupt set", hart_sip_load(&hart),
                RV_INT_SEI_BIT);
    require_false("wfi cleared", hart_in_wfi_load(&hart));
}

static void test_apply_level_clears_only_selected_active_bit(void)
{
    hart_t hart;
    hart_t *harts[1];
    vm_t vm;
    uint32_t uart_bit = semu_irq_source_plic_bit(SEMU_IRQ_SOURCE_UART);
    uint32_t vblk_bit = semu_irq_source_plic_bit(SEMU_IRQ_SOURCE_VBLK);
    plic_state_t plic = {
        .active = uart_bit | vblk_bit,
        .ip = uart_bit | vblk_bit,
        .masked = uart_bit | vblk_bit,
    };

    init_one_hart_vm(&vm, &hart, harts);

    require_true("apply uart low",
                 semu_irq_source_apply_level(&vm, &plic, SEMU_IRQ_SOURCE_UART,
                                             false));

    require_u32("uart active cleared", plic.active & uart_bit, 0);
    require_u32("vblk active preserved", plic.active & vblk_bit, vblk_bit);
    require_u32("vblk pending preserved", plic.ip & vblk_bit, vblk_bit);
    require_u32("vblk masked preserved", plic.masked & vblk_bit, vblk_bit);
}

static void test_invalid_source_does_not_touch_plic(void)
{
    hart_t hart;
    hart_t *harts[1];
    vm_t vm;
    plic_state_t plic = {
        .active = 0x2a,
        .ip = 0x55,
        .masked = 0xaa,
    };

    init_one_hart_vm(&vm, &hart, harts);

    require_false("invalid apply rejected",
                  semu_irq_source_apply_level(
                      &vm, &plic, (enum semu_irq_source) 99, true));

    require_u32("invalid preserves active", plic.active, 0x2a);
    require_u32("invalid preserves pending", plic.ip, 0x55);
    require_u32("invalid preserves masked", plic.masked, 0xaa);
}

static void test_public_set_wakes_after_valid_level_update(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    uint32_t bit = semu_irq_source_plic_bit(SEMU_IRQ_SOURCE_VRNG);

    memset(&emu, 0, sizeof(emu));
    init_one_hart_vm(&emu.vm, &hart, harts);
    pthread_mutex_init(&emu.plic_lock, NULL);
    wake_count = 0;

    semu_irq_source_set(&emu, SEMU_IRQ_SOURCE_VRNG, true);

    require_u32("public set active bit", emu.plic.active & bit, bit);
    require_unsigned("public set wake count", wake_count, 1);

    semu_irq_source_set(&emu, (enum semu_irq_source) 99, true);
    require_unsigned("invalid public set wake count", wake_count, 1);

    pthread_mutex_destroy(&emu.plic_lock);
}

int main(void)
{
    test_source_mapping_uses_platform_irq_ids();
    test_apply_level_sets_active_and_pending_like_old_path();
    test_apply_level_clears_only_selected_active_bit();
    test_invalid_source_does_not_touch_plic();
    test_public_set_wakes_after_valid_level_update();
    return 0;
}
