# Phase 1 Verification Checklist

Scope: Phase 1 of `dev-docs/MesDevLog/AOS-SMP/smp-multithread-plan-en.md`.
Phase 1 is a behavior-preserving state-separation phase; the coroutine path
must continue to work while shared state becomes ready for OS-threaded harts.

## Focused Contract Tests

Run the standalone contract test:

```sh
cc -std=c11 -O2 -Wall -Wextra -pthread \
  tests/phase1-threading-contract-test.c \
  -o /tmp/phase1-threading-contract-test
/tmp/phase1-threading-contract-test
```

Expected result: exit 0 with no stderr.

This test covers the Phase 1 synchronization contracts:

- `sip` updates use atomic RMW, so concurrent timer/external/software bits are
  not lost.
- CSR `sip` writes update only writable SSI state and preserve timer/external
  pending bits.
- RAM byte and halfword stores use a CAS loop over the containing 32-bit cell,
  so adjacent byte/halfword writers do not lose each other's lanes.
- PTE A/D updates use atomic `fetch_or`, so concurrent A and D setters preserve
  both bits plus existing PTE bits.
- The global LR/SC reservation table invalidates same-word reservations,
  preserves different-word reservations, makes SC fail after an intervening
  store, makes valid SC succeed, and clears `any_reservation_active` after the
  last reservation is invalidated.

## Existing Focused Regression Tests

Run these after Phase 1 lands:

```sh
cc -O2 -g -Wall -Wextra -include common.h -I. \
  tests/aclint-swi-test.c aclint.c \
  -o /tmp/aclint-swi-test
/tmp/aclint-swi-test
```

Expected result: exit 0. This guards the SSI source composition behavior that
must remain correct after `sip` atomics or per-source flags are introduced.

```sh
cc -O2 -g -Wall -Wextra -include common.h -D SEMU_BOOT_TARGET_TIME=10 -I. \
  tests/timer-atomic-contract-test.c utils.c \
  -o /tmp/timer-atomic-contract-test
/tmp/timer-atomic-contract-test
```

Expected result: exit 0. This guards the existing atomic boot-complete contract.

```sh
cc -O2 -g -Wall -Wextra -include common.h -D SEMU_BOOT_TARGET_TIME=10 -I. \
  -pthread \
  tests/timer-thread-safety-test.c utils.c \
  -o /tmp/timer-thread-safety-test
/tmp/timer-thread-safety-test
```

Expected result: exit 0. This keeps the Phase 0 timer thread-safety gate alive.

## Build And Coroutine Smoke

Run after Phase 1 implementation merges:

```sh
make semu
```

Expected result: successful build.

```sh
make SMP=4 minimal.dtb
scripts/verify-dtb.sh minimal.dtb 4
```

Expected result: DTB has 4 harts.

```sh
bash .ci/device-smoke/test-gpu.sh
SMP=2 bash .ci/device-smoke/test-gpu.sh
```

Expected result: both GPU smoke runs pass on the coroutine path. The known TAP
allocation warning is acceptable only if the guest still boots, logs in, binds
`virtio_gpu`, exposes `/dev/dri/card0`, and reaches DirectFB2
`DRMKMS/System`.

## Phase 1 Review Points

- `hart_t::sip` is not updated by plain load/store RMW. All set/clear paths
  must use `fetch_or`/`fetch_and`, or the implementation must use source
  fields merged by atomic loads.
- CSR `sip` writes must not overwrite non-writable pending timer/external bits.
  If per-source flags are used, CSR writes should only affect the writable SSWI
  source.
- `vm_step` and interrupt dispatch must read `sip` through the new atomic or
  source-merge API, not by stale plain reads.
- `ram.c` and the fast RAM paths in `riscv.c` must route SB/SH through the same
  CAS subword helper. It is not enough to fix only the slow `ram_write()` path.
- PTE A/D bit updates in `mmu_translate()` must use atomic `fetch_or`; a plain
  load-mask-store can lose either A or D.
- LR/SC must use the global reservation table consistently for LR, SC, and
  regular store invalidation. Same 32-bit physical word invalidates; different
  words do not.
- Reservation invalidation and SC check/store must be centralized through the
  table helpers. Phase 1 may keep those helpers non-atomic because execution is
  still coroutine-based; Phase 3 is responsible for adding the table
  lock/spinlock around the check/invalidate/store critical section.
- Coroutine wrappers such as `emu_tick_peripherals()` must remain behaviorally
  intact until the threaded path is enabled.
- GPU smoke must be rerun with default `SMP=1` and `SMP=2`; Phase 1 is not
  allowed to regress the coroutine acceptance gate.
