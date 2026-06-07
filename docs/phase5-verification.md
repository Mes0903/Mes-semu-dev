# Phase 5 Verification

Phase 5 completes the HSM lifecycle audit. Most HSM runtime work was added in
Phase 2, but this phase fixes and verifies a suspend/resume race that only
shows up when a suspended hart is resumed before its thread parks in
`wait_for_hart_start()`.

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

Run:

```sh
make phase5-hsm-contract-test
```

## Integration Gates

Before committing Phase 5, run:

```sh
git diff --check
clang-format-20 --dry-run --Werror \
  main.c riscv.h tests/phase5-hsm-contract-test.c
make phase5-hsm-contract-test
make phase2-threading-contract-test
make phase3-memory-model-contract-test
make phase3-memory-contract-test
make phase4-rfence-contract-test
make clean && make semu
make clean && ENABLE_THREADED=1 make semu
SEMU_DIRECTFB2_TEST=0 ENABLE_THREADED=1 SMP=2 bash .ci/device-smoke/test-gpu.sh
SEMU_DIRECTFB2_TEST=0 SMP=2 bash .ci/device-smoke/test-gpu.sh
```

## Notes

- `HART_START` still uses `STOPPED -> START_PENDING` CAS and publishes
  `STARTED` only after target PC/register state is initialized. It also clears
  stale resume-pending state defensively.
- `HART_STOP` and `HART_SUSPEND` still use `ERR_USER` to break out of the
  current guest execution slice; `vm_step_many()` and `vm_step()` continue to
  gate instruction fetch on `SBI_HSM_STATE_STARTED`.
- `hsm_resume_pending` decouples resume-context application from observing the
  intermediate `SBI_HSM_STATE_SUSPENDED` state. This closes the fast-resume
  race where an interrupt wake can switch the hart back to STARTED before the
  hart thread enters `wait_for_hart_start()`.
- The threaded hart loop processes pending HSM resume both after parked waits
  and immediately before each STARTED execution slice.
