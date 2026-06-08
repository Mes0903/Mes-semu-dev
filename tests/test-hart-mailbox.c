#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hart-mailbox.h"
#include "riscv.h"

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

static void test_init_starts_empty(void)
{
    semu_hart_mailbox_t mailbox;

    memset(&mailbox, 0xff, sizeof(mailbox));
    semu_hart_mailbox_init(&mailbox);

    require_u32("initial pending", semu_hart_mailbox_pending_load(&mailbox),
                0);
    require_false("initial rfence",
                  semu_hart_mailbox_has(&mailbox,
                                        SEMU_HART_MAILBOX_RFENCE));
    require_u32("initial ack generation",
                semu_hart_mailbox_ack_generation(&mailbox), 0);
}

static void test_request_ors_events_and_clear_preserves_unrelated(void)
{
    semu_hart_mailbox_t mailbox = {0};

    semu_hart_mailbox_request(&mailbox, SEMU_HART_MAILBOX_RFENCE);
    semu_hart_mailbox_request(&mailbox, SEMU_HART_MAILBOX_HSM_RESUME);

    require_u32("request ORs events",
                semu_hart_mailbox_pending_load(&mailbox),
                SEMU_HART_MAILBOX_RFENCE | SEMU_HART_MAILBOX_HSM_RESUME);
    require_true("has both events",
                 semu_hart_mailbox_has(&mailbox,
                                       SEMU_HART_MAILBOX_RFENCE |
                                           SEMU_HART_MAILBOX_HSM_RESUME));

    semu_hart_mailbox_clear(&mailbox, SEMU_HART_MAILBOX_RFENCE);

    require_false("rfence cleared",
                  semu_hart_mailbox_has(&mailbox,
                                        SEMU_HART_MAILBOX_RFENCE));
    require_true("hsm resume preserved",
                 semu_hart_mailbox_has(&mailbox,
                                       SEMU_HART_MAILBOX_HSM_RESUME));
}

static void test_hart_wrappers_use_independent_mailbox_events(void)
{
    hart_t hart = {0};

    hart_pending_rfence_store(&hart, true);
    require_true("wrapper rfence set", hart_pending_rfence_load(&hart));
    require_false("wrapper hsm unaffected by rfence",
                  hart_hsm_resume_pending_load(&hart));
    require_u32("mailbox rfence bit",
                semu_hart_mailbox_pending_load(&hart.mailbox),
                SEMU_HART_MAILBOX_RFENCE);

    hart_hsm_resume_pending_store(&hart, true);
    require_true("wrapper hsm set", hart_hsm_resume_pending_load(&hart));
    require_true("wrapper rfence still set", hart_pending_rfence_load(&hart));

    hart_pending_rfence_store(&hart, false);
    require_false("wrapper rfence cleared", hart_pending_rfence_load(&hart));
    require_true("wrapper hsm still set", hart_hsm_resume_pending_load(&hart));

    hart_hsm_resume_pending_store(&hart, false);
    require_u32("mailbox all clear",
                semu_hart_mailbox_pending_load(&hart.mailbox), 0);
}

static void test_ack_generation_increments_without_clearing_pending(void)
{
    semu_hart_mailbox_t mailbox = {0};

    semu_hart_mailbox_request(&mailbox, SEMU_HART_MAILBOX_RFENCE);

    require_u32("first ack", semu_hart_mailbox_ack(&mailbox), 1);
    require_u32("second ack", semu_hart_mailbox_ack(&mailbox), 2);
    require_u32("ack generation load",
                semu_hart_mailbox_ack_generation(&mailbox), 2);
    require_true("ack preserves pending",
                 semu_hart_mailbox_has(&mailbox,
                                       SEMU_HART_MAILBOX_RFENCE));
}

static void test_event_mask_validation(void)
{
    require_false("zero mask invalid", semu_hart_mailbox_event_mask_valid(0));
    require_true("rfence valid",
                 semu_hart_mailbox_event_mask_valid(
                     SEMU_HART_MAILBOX_RFENCE));
    require_true("combined valid",
                 semu_hart_mailbox_event_mask_valid(
                     SEMU_HART_MAILBOX_RFENCE |
                     SEMU_HART_MAILBOX_HSM_RESUME));
    require_false("unknown bit invalid",
                  semu_hart_mailbox_event_mask_valid(UINT32_C(1) << 31));
    require_false("known plus unknown invalid",
                  semu_hart_mailbox_event_mask_valid(
                      SEMU_HART_MAILBOX_RFENCE | (UINT32_C(1) << 31)));
}

int main(void)
{
    test_init_starts_empty();
    test_request_ors_events_and_clear_preserves_unrelated();
    test_hart_wrappers_use_independent_mailbox_events();
    test_ack_generation_increments_without_clearing_pending();
    test_event_mask_validation();
    return 0;
}
