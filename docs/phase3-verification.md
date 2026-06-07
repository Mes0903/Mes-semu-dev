# Phase 3 Verification

Phase 3 hardens the threaded SMP memory model around guest atomics:
LR/SC reservation state, AMO word operations, and FENCE/FENCE.I.

## Focused Contract Test

The `phase3-memory-model-contract-test` target builds a small pthread model that
checks:

- AMOADD does not lose concurrent increments.
- AMOMIN/AMOMAX signed and unsigned compare semantics.
- LR/SC check, store, and reservation invalidation are one critical section.
- Same-word regular stores invalidate active LR reservations before a later SC.

The `phase3-memory-contract-test` target links against `riscv.c` with
`SEMU_FEATURE_THREADED=1` and executes real encoded guest instructions for:

- AMOADD.W on RAM from two host threads.
- LR.W/SC.W invalidation by SW, SH, and SB.
- AMOADD.W to out-of-RAM/MMIO faulting without callback RMW side effects.

Run:

```sh
make phase3-memory-model-contract-test
make phase3-memory-contract-test
```

## Integration Gates

Before committing Phase 3, run:

```sh
git diff --check
clang-format-20 --dry-run --Werror \
  main.c riscv.c riscv.h \
  tests/phase3-memory-model-contract-test.c tests/phase3-memory-contract-test.c
make phase3-memory-model-contract-test
make phase3-memory-contract-test
make clean && make semu
make clean && ENABLE_THREADED=1 make semu
SEMU_DIRECTFB2_TEST=0 ENABLE_THREADED=1 SMP=2 bash .ci/device-smoke/test-gpu.sh
SEMU_DIRECTFB2_TEST=0 SMP=2 bash .ci/device-smoke/test-gpu.sh
```

## Notes

- Threaded builds serialize LR/SC reservation creation, regular guest stores,
  and AMO RMWs with one reservation mutex. This is conservative but keeps the
  Phase 3 correctness boundary narrow.
- AMOs are accepted only for RAM-backed word addresses. MMIO/out-of-RAM AMOs now
  raise Store/AMO access fault instead of using non-atomic device callbacks.
- `FENCE` executes a host seq-cst fence. `FENCE.I` also invalidates the local
  MMU and instruction caches around seq-cst fences. Cross-hart RFENCE behavior
  remains a Phase 4 item.
