#include "device.h"
#include "riscv.h"
#include "riscv_private.h"

/* Make PLIC as simple as possible: 32 interrupts, no priority */

void plic_update_interrupts(vm_t *vm, plic_state_t *plic)
{
    /* Update pending interrupts */
    plic->ip |= plic->active & ~plic->masked;
    plic->masked |= plic->active;
    /* Send interrupt to target */
    for (uint32_t i = 0; i < vm->n_hart; i++) {
        if (plic->ip & plic->ie[i]) {
            vm->hart[i]->sip |= RV_INT_SEI_BIT;
            /* Clear WFI flag when external interrupt is injected */
            vm->hart[i]->in_wfi = false;
        } else {
            vm->hart[i]->sip &= ~RV_INT_SEI_BIT;
        }
    }
}

static bool plic_reg_read(plic_state_t *plic, uint32_t addr, uint32_t *value)
{
    /* no priority support: source priority hardwired to 1 */
    if (1 <= addr && addr <= 31)
        return true;

    if (addr == 0x400) {
        *value = plic->ip;
        return true;
    }

    /* Enable registers: word 0x800 + context * 0x20 (byte 0x2000, stride
     * 0x80). Only the first word per context is meaningful for <= 32 sources.
     */
    if (addr >= 0x800 && addr < 0xC00) {
        int context = (addr - 0x800) / 0x20;
        if ((addr - 0x800) % 0x20 == 0)
            *value = plic->ie[context];
        return true;
    }

    /* Context registers: word 0x80000 + context * 0x400 (byte 0x200000,
     * stride 0x1000).  Offset 0 = threshold, offset 1 = claim/complete.
     */
    if (addr >= 0x80000 && addr < 0x88000) {
        int context = (addr - 0x80000) / 0x400;
        int offset = (addr - 0x80000) % 0x400;
        if (offset == 0) {
            /* no priority support: target priority threshold hardwired to 0 */
            *value = 0;
            return true;
        }
        if (offset == 1) {
            /* claim */
            *value = 0;
            uint32_t candidates = plic->ip & plic->ie[context];
            if (candidates) {
                *value = ilog2(candidates);
                plic->ip &= ~(1 << (*value));
            }
            return true;
        }
        return true;
    }

    return false;
}

static bool plic_reg_write(plic_state_t *plic, uint32_t addr, uint32_t value)
{
    /* no priority support: source priority hardwired to 1 */
    if (1 <= addr && addr <= 31)
        return true;

    /* Enable registers */
    if (addr >= 0x800 && addr < 0xC00) {
        int context = (addr - 0x800) / 0x20;
        if ((addr - 0x800) % 0x20 == 0) {
            value &= ~1;
            plic->ie[context] = value;
        }
        return true;
    }

    /* Context registers */
    if (addr >= 0x80000 && addr < 0x88000) {
        int context = (addr - 0x80000) / 0x400;
        int offset = (addr - 0x80000) % 0x400;
        if (offset == 0) {
            /* no priority support: target priority threshold hardwired to 0 */
            return true;
        }
        if (offset == 1) {
            /* completion */
            if (plic->ie[context] & (1 << value))
                plic->masked &= ~(1 << value);
            return true;
        }
        return true;
    }

    return false;
}

void plic_read(hart_t *vm,
               plic_state_t *plic,
               uint32_t addr,
               uint8_t width,
               uint32_t *value)
{
    switch (width) {
    case RV_MEM_LW:
        if (!plic_reg_read(plic, addr >> 2, value))
            vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
        break;
    case RV_MEM_LBU:
    case RV_MEM_LB:
    case RV_MEM_LHU:
    case RV_MEM_LH:
        vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
        return;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }
}

void plic_write(hart_t *vm,
                plic_state_t *plic,
                uint32_t addr,
                uint8_t width,
                uint32_t value)
{
    switch (width) {
    case RV_MEM_SW:
        if (!plic_reg_write(plic, addr >> 2, value))
            vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
        break;
    case RV_MEM_SB:
    case RV_MEM_SH:
        vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
        return;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }
}
