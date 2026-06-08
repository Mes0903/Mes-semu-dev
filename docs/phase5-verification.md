# Phase 5 Verification

Phase 5 completes the HSM lifecycle audit. Most HSM runtime work was added in
Phase 2, but this phase fixes and verifies suspend/resume and stop/start races
that only show up when a hart changes state while its owner thread is still
finishing the SBI HSM ecall path.

## Focused Contract Test

The `phase5-hsm-contract-test` target links the HSM handler and threaded helper
code directly enough to exercise the real suspend/resume path. It checks:

- `HART_SUSPEND` records an explicit pending resume before publishing
  `SBI_HSM_STATE_SUSPENDED`.
- a non-retentive suspend that is resumed immediately through the interrupt
  wake path still applies the required resume context before the next STARTED
  execution slice: `pc=resume_addr`, `satp=0`, `sstatus.SIE=0`, S-mode,
  `a0=hartid`, and `a1=opaque`.
- a retentive suspend fast-resume clears the pending marker without clobbering
  PC, SATP, SIE, or general registers.
- a pending interrupt that is already visible before suspend finalization wakes
  the hart after `SUSPEND_PENDING -> SUSPENDED`, then applies the
  non-retentive resume context on the next STARTED slice.
- `HART_STOP` remains `STOP_PENDING` until the owner thread parks, so another
  hart cannot restart the target before the stop ecall has actually unwound.
- `HART_START` and non-retentive resume clear both SATP and the derived
  `page_table` pointer, preventing stale virtual translation state from being
  reused for a physical secondary-entry address such as `0x000000b8`.

Run:

```sh
make phase5-hsm-contract-test
```

## Integration Gates

Before committing Phase 5, run:

```sh
git diff --check
clang-format-20 --dry-run --Werror \
  main.c riscv.c riscv.h tests/phase5-hsm-contract-test.c
bash -n .ci/device-smoke/test-hsm-hotplug.sh
make phase5-hsm-contract-test
make phase2-threading-contract-test
make phase3-memory-model-contract-test
make phase3-memory-contract-test
make phase4-rfence-contract-test
make clean && make semu
SMP=2 .ci/device-smoke/test-hsm-hotplug.sh
SEMU_DIRECTFB2_TEST=0 SMP=2 bash .ci/device-smoke/test-gpu.sh
```

## Notes

- `HART_START` still uses `STOPPED -> START_PENDING` CAS and publishes
  `STARTED` only after target PC/register state is initialized. It also clears
  stale resume-pending state defensively.
- `HART_STOP` and `HART_SUSPEND` still use `ERR_USER` to break out of the
  current guest execution slice. They publish `STOP_PENDING` or
  `SUSPEND_PENDING` first, and `semu_finish_hsm_park()` publishes the final
  parked state after the owner thread has left the guest slice.
- `hsm_resume_pending` decouples resume-context application from observing the
  intermediate `SBI_HSM_STATE_SUSPENDED` state. This closes the fast-resume
  race where an interrupt wake can switch the hart back to STARTED before the
  hart thread enters `wait_for_hart_start()`.
- The threaded hart loop processes pending HSM resume both after parked waits
  and immediately before each STARTED execution slice.
- CPU hotplug exposed a stale MMU-derived-state bug: Linux restarts cpu1 at a
  physical secondary entry around `0x000000b8`, so HSM paths that directly set
  `satp = 0` must also clear the derived `page_table` pointer. The exported
  `mmu_set_satp()` helper now keeps those fields synchronized.
