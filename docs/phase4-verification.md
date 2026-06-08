# Phase 4 Verification

Phase 4 implements cross-hart RFENCE delivery for the threaded SMP runtime.
As of 2026-06-08, this threaded path is the only SMP runtime on `smp-support`.

## Focused Contract Test

The `phase4-rfence-contract-test` target builds a pthread model of the RFENCE
request protocol. It checks:

- hart-mask selection, including all-harts, sparse masks, empty masks, and
  out-of-range bases.
- self-targeted RFENCE applies locally and is excluded from `pending_count`.
- WFI-like targets are signaled, service pending RFENCE, and acknowledge
  completion without requiring an interrupt.
- parked HSM targets check pending RFENCE before sleeping again, so targets
  snapshotted while STARTED still acknowledge if they stop or suspend before
  servicing the request.
- remote targets that contributed to `pending_count` are snapshotted before
  signaling, so later HSM status changes cannot leave the requester waiting for
  a target that was never sent work.
- stopped or suspended targets are invalidated synchronously and do not block
  requester completion.
- a hart waiting to issue its own RFENCE processes an already-pending RFENCE
  first, avoiding request-overwrite deadlock.
- emulator shutdown wakes a requester blocked on RFENCE completion, so stop
  paths do not strand a hart on `pending_count`.
- duplicate pending checks do not decrement completion twice.

Run:

```sh
make phase4-rfence-contract-test
```

## Integration Gates

Before committing Phase 4, run:

```sh
git diff --check
clang-format-20 --dry-run --Werror \
  main.c riscv.h device.h tests/phase4-rfence-contract-test.c
make phase2-threading-contract-test
make phase3-memory-model-contract-test
make phase3-memory-contract-test
make phase4-rfence-contract-test
make clean && make semu
SEMU_DIRECTFB2_TEST=0 SMP=2 bash .ci/device-smoke/test-gpu.sh
```

## Notes

- Threaded RFENCE uses one in-flight request guarded by `rfence.issue_mutex`.
  The mutex stays held until all remote targets acknowledge, so another issuer
  cannot overwrite request fields while target harts are servicing them.
- Target harts read immutable request fields after acquiring their per-hart
  pending flag and do not take the issue mutex; this lets them acknowledge while
  the requester serializes the in-flight request.
- Non-running targets are invalidated synchronously by the requester. They are
  excluded from `pending_count`, preventing waits on stopped/suspended harts.
- Running remote targets are recorded in a per-request snapshot before
  `pending_count` is published. The signal pass uses only that snapshot, rather
  than re-reading HSM state.
- `wait_for_hart_start()` services pending RFENCE before blocking again. This
  lets a hart acknowledge a request even if it leaves STARTED state after being
  snapshotted as a remote running target.
- `semu_set_stopped()` broadcasts the RFENCE completion condvar, and the
  requester wait loop exits if the emulator is stopping before all remote acks
  arrive.
- `RFENCE.VMA_ASID` currently ignores ASID because the local MMU caches are not
  ASID-tagged. It uses the same range invalidation as `RFENCE.VMA`.
