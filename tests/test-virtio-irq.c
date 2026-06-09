#define _GNU_SOURCE

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "irq-source.h"
#include "platform.h"
#include "riscv_private.h"
#include "virtio-irq.h"
#include "virtio.h"

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

static void init_one_hart_vm(vm_t *vm, hart_t *hart, hart_t **harts)
{
    memset(hart, 0, sizeof(*hart));
    memset(vm, 0, sizeof(*vm));
    hart->vm = vm;
    harts[0] = hart;
    vm->n_hart = 1;
    vm->hart = harts;
}

static void init_test_emu(emu_state_t *emu, hart_t *hart, hart_t **harts)
{
    memset(emu, 0, sizeof(*emu));
    init_one_hart_vm(&emu->vm, hart, harts);
    require_int("plic lock init", pthread_mutex_init(&emu->plic_lock, NULL), 0);
    wake_count = 0;
}

static void destroy_test_emu(emu_state_t *emu)
{
    pthread_mutex_destroy(&emu->plic_lock);
}

static bool source_asserted(emu_state_t *emu, enum semu_irq_source source)
{
    return (emu->plic.active & semu_irq_source_plic_bit(source)) != 0;
}

static void ack_irq_and_require_level(struct virtio_irq *irq,
                                      emu_state_t *emu,
                                      enum semu_irq_source source,
                                      uint32_t bits,
                                      uint32_t want_status,
                                      bool want_asserted)
{
    virtio_irq_ack(irq, bits);
    require_u32("ordered ack status", virtio_irq_read_status(irq),
                want_status);
    require_true("ordered ack level",
                 source_asserted(emu, source) == want_asserted);
}

static void trigger_irq_and_require_level(struct virtio_irq *irq,
                                          emu_state_t *emu,
                                          enum semu_irq_source source,
                                          uint32_t bits,
                                          uint32_t want_status,
                                          bool want_asserted)
{
    virtio_irq_trigger(irq, bits);
    require_u32("ordered trigger status", virtio_irq_read_status(irq),
                want_status);
    require_true("ordered trigger level",
                 source_asserted(emu, source) == want_asserted);
}

static void test_init_read_zero_and_invalid_inputs_are_safe(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_irq irq;

    virtio_irq_trigger(NULL, VIRTIO_INT__USED_RING);
    virtio_irq_ack(NULL, VIRTIO_INT__USED_RING);
    require_u32("null irq read", virtio_irq_read_status(NULL), 0);
    virtio_irq_destroy(NULL);

    init_test_emu(&emu, &hart, harts);
    require_int("valid irq init",
                virtio_irq_init(&irq, &emu, SEMU_IRQ_SOURCE_VRNG), 0);
    require_u32("initial status", virtio_irq_read_status(&irq), 0);
    require_false("initial line", source_asserted(&emu, SEMU_IRQ_SOURCE_VRNG));

    virtio_irq_trigger(&irq, 0);
    virtio_irq_ack(&irq, 0);
    require_u32("zero-bit operations keep status zero",
                virtio_irq_read_status(&irq), 0);
    require_false("zero-bit operations keep line low",
                  source_asserted(&emu, SEMU_IRQ_SOURCE_VRNG));

    virtio_irq_destroy(&irq);
    require_int("null irq init rejected",
                virtio_irq_init(NULL, &emu, SEMU_IRQ_SOURCE_VRNG), -1);
    require_int("null emu init rejected",
                virtio_irq_init(&irq, NULL, SEMU_IRQ_SOURCE_VRNG), -1);
    require_int("invalid source init rejected",
                virtio_irq_init(&irq, &emu, SEMU_IRQ_SOURCE_COUNT), -1);
    destroy_test_emu(&emu);
}

static void test_trigger_ors_status_and_asserts_source(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_irq irq;

    init_test_emu(&emu, &hart, harts);
    require_int("irq init", virtio_irq_init(&irq, &emu, SEMU_IRQ_SOURCE_VGPU),
                0);

    virtio_irq_trigger(&irq, VIRTIO_INT__USED_RING);
    require_u32("used ring status", virtio_irq_read_status(&irq),
                VIRTIO_INT__USED_RING);
    require_true("used ring line asserted",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VGPU));

    virtio_irq_trigger(&irq, VIRTIO_INT__CONF_CHANGE);
    require_u32("combined status", virtio_irq_read_status(&irq),
                VIRTIO_INT__USED_RING | VIRTIO_INT__CONF_CHANGE);
    require_true("combined line asserted",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VGPU));

    virtio_irq_destroy(&irq);
    destroy_test_emu(&emu);
}

static void test_ack_clears_selected_bits_and_deasserts_only_at_zero(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_irq irq;

    init_test_emu(&emu, &hart, harts);
    require_int("irq init", virtio_irq_init(&irq, &emu, SEMU_IRQ_SOURCE_VBLK),
                0);

    virtio_irq_trigger(&irq, VIRTIO_INT__USED_RING | VIRTIO_INT__CONF_CHANGE);
    virtio_irq_ack(&irq, VIRTIO_INT__USED_RING);
    require_u32("ack selected bit", virtio_irq_read_status(&irq),
                VIRTIO_INT__CONF_CHANGE);
    require_true("ack selected keeps line asserted",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VBLK));

    virtio_irq_ack(&irq, VIRTIO_INT__CONF_CHANGE);
    require_u32("ack final bit", virtio_irq_read_status(&irq), 0);
    require_false("ack final deasserts line",
                  source_asserted(&emu, SEMU_IRQ_SOURCE_VBLK));

    virtio_irq_destroy(&irq);
    destroy_test_emu(&emu);
}

static void test_repeated_trigger_coalesces_status_and_line(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_irq irq;

    init_test_emu(&emu, &hart, harts);
    require_int("irq init", virtio_irq_init(&irq, &emu, SEMU_IRQ_SOURCE_VFS),
                0);

    virtio_irq_trigger(&irq, VIRTIO_INT__USED_RING);
    virtio_irq_trigger(&irq, VIRTIO_INT__USED_RING);
    require_u32("repeated trigger status", virtio_irq_read_status(&irq),
                VIRTIO_INT__USED_RING);
    require_true("repeated trigger line asserted",
                 source_asserted(&emu, SEMU_IRQ_SOURCE_VFS));

    virtio_irq_ack(&irq, VIRTIO_INT__USED_RING);
    require_u32("repeated trigger ack status", virtio_irq_read_status(&irq), 0);
    require_false("repeated trigger ack line",
                  source_asserted(&emu, SEMU_IRQ_SOURCE_VFS));

    virtio_irq_destroy(&irq);
    destroy_test_emu(&emu);
}

static void test_ordered_trigger_ack_interleavings_keep_level_consistent(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_irq irq;
    enum semu_irq_source source = SEMU_IRQ_SOURCE_VSND;

    init_test_emu(&emu, &hart, harts);
    require_int("irq init", virtio_irq_init(&irq, &emu, source), 0);

    trigger_irq_and_require_level(&irq, &emu, source, VIRTIO_INT__USED_RING,
                                  VIRTIO_INT__USED_RING, true);
    ack_irq_and_require_level(&irq, &emu, source, VIRTIO_INT__USED_RING, 0,
                              false);
    trigger_irq_and_require_level(&irq, &emu, source, VIRTIO_INT__CONF_CHANGE,
                                  VIRTIO_INT__CONF_CHANGE, true);
    ack_irq_and_require_level(&irq, &emu, source, VIRTIO_INT__CONF_CHANGE, 0,
                              false);

    trigger_irq_and_require_level(&irq, &emu, source, VIRTIO_INT__USED_RING,
                                  VIRTIO_INT__USED_RING, true);
    trigger_irq_and_require_level(
        &irq, &emu, source, VIRTIO_INT__CONF_CHANGE,
        VIRTIO_INT__USED_RING | VIRTIO_INT__CONF_CHANGE, true);
    ack_irq_and_require_level(&irq, &emu, source, VIRTIO_INT__USED_RING,
                              VIRTIO_INT__CONF_CHANGE, true);
    ack_irq_and_require_level(&irq, &emu, source, VIRTIO_INT__CONF_CHANGE, 0,
                              false);

    virtio_irq_destroy(&irq);
    destroy_test_emu(&emu);
}

struct race_gate {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    unsigned ready;
    bool go;
};

struct race_args {
    struct virtio_irq *irq;
    struct race_gate *gate;
};

static void race_gate_init(struct race_gate *gate)
{
    memset(gate, 0, sizeof(*gate));
    require_int("race mutex init", pthread_mutex_init(&gate->lock, NULL), 0);
    require_int("race cond init", pthread_cond_init(&gate->cond, NULL), 0);
}

static void race_gate_destroy(struct race_gate *gate)
{
    pthread_cond_destroy(&gate->cond);
    pthread_mutex_destroy(&gate->lock);
}

static void race_gate_wait(struct race_gate *gate)
{
    pthread_mutex_lock(&gate->lock);
    gate->ready++;
    pthread_cond_broadcast(&gate->cond);
    while (!gate->go)
        pthread_cond_wait(&gate->cond, &gate->lock);
    pthread_mutex_unlock(&gate->lock);
}

static void race_gate_release_when_ready(struct race_gate *gate)
{
    pthread_mutex_lock(&gate->lock);
    while (gate->ready != 2)
        pthread_cond_wait(&gate->cond, &gate->lock);
    gate->go = true;
    pthread_cond_broadcast(&gate->cond);
    pthread_mutex_unlock(&gate->lock);
}

static void *race_ack_used(void *opaque)
{
    struct race_args *args = opaque;

    race_gate_wait(args->gate);
    virtio_irq_ack(args->irq, VIRTIO_INT__USED_RING);
    return NULL;
}

static void *race_trigger_conf(void *opaque)
{
    struct race_args *args = opaque;

    race_gate_wait(args->gate);
    virtio_irq_trigger(args->irq, VIRTIO_INT__CONF_CHANGE);
    return NULL;
}

static void test_trigger_racing_with_ack_keeps_status_and_line_consistent(void)
{
    emu_state_t emu;
    hart_t hart;
    hart_t *harts[1];
    struct virtio_irq irq;

    init_test_emu(&emu, &hart, harts);
    require_int("irq init", virtio_irq_init(&irq, &emu, SEMU_IRQ_SOURCE_VNET),
                0);

    for (unsigned i = 0; i < 200; i++) {
        struct race_gate gate;
        struct race_args args = {.irq = &irq, .gate = &gate};
        pthread_t ack_thread;
        pthread_t trigger_thread;

        virtio_irq_ack(&irq, VIRTIO_INT__USED_RING | VIRTIO_INT__CONF_CHANGE);
        virtio_irq_trigger(&irq, VIRTIO_INT__USED_RING);

        race_gate_init(&gate);
        require_int("ack thread create",
                    pthread_create(&ack_thread, NULL, race_ack_used, &args), 0);
        require_int(
            "trigger thread create",
            pthread_create(&trigger_thread, NULL, race_trigger_conf, &args), 0);
        race_gate_release_when_ready(&gate);
        require_int("ack thread join", pthread_join(ack_thread, NULL), 0);
        require_int("trigger thread join", pthread_join(trigger_thread, NULL),
                    0);
        race_gate_destroy(&gate);

        require_u32("racing status", virtio_irq_read_status(&irq),
                    VIRTIO_INT__CONF_CHANGE);
        require_true("racing line asserted",
                     source_asserted(&emu, SEMU_IRQ_SOURCE_VNET));
    }

    virtio_irq_destroy(&irq);
    destroy_test_emu(&emu);
}

int main(void)
{
    test_init_read_zero_and_invalid_inputs_are_safe();
    test_trigger_ors_status_and_asserts_source();
    test_ack_clears_selected_bits_and_deasserts_only_at_zero();
    test_repeated_trigger_coalesces_status_and_line();
    test_ordered_trigger_ack_interleavings_keep_level_consistent();
    test_trigger_racing_with_ack_keeps_status_and_line_consistent();
    return 0;
}
