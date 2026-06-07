# Phase 2 Threading Verification

Scope: Phase 2 threading infrastructure for `semu`, including the opt-in
`ENABLE_THREADED=1` runtime, pthread hart lifecycle, per-device MMIO mutex
activation, WFI/HSM wakeup plumbing, timer/SWI wake delivery, and regression
coverage for the default coroutine path.

## Contract Test

The `phase2-threading-contract-test` target builds and runs the model in both
coroutine/default and threaded configurations:

```sh
make phase2-threading-contract-test
```

Coverage:

- HSM START succeeds only from STOPPED, initializes target PC/opaque, and wakes
  the target.
- HSM START from STARTED or SUSPENDED is denied and does not rewrite target
  state or emit a wake.
- HART_STOP and HART_SUSPEND park a threaded guest loop; guest steps do not
  continue while the hart is not STARTED.
- WFI missed-wakeup protocol: interrupt publication before/during the wait
  still releases the waiter and clears `in_wfi`.
- Device lock helper is a no-op in the default build and a real serializing
  lock in the threaded build.
- I/O stop/wake can release a scheduler wait without deadlock.
- Signaling a suspended hart alone does not resume it; an enabled pending
  interrupt is required.

Latest result: pass for both generated binaries.

## Integration Results

Fresh local verification on 2026-06-07:

```sh
git diff --check
clang-format-20 --dry-run --Werror   main.c riscv.c riscv.h device.h utils.c utils.h aclint.c uart.c   tests/phase2-threading-contract-test.c
make phase2-threading-contract-test
make clean
make semu
make clean
ENABLE_THREADED=1 make semu
SEMU_DIRECTFB2_TEST=0 ENABLE_THREADED=1 SMP=2 bash .ci/device-smoke/test-gpu.sh
SEMU_TEST_TIMEOUT=300 ENABLE_THREADED=1 SMP=2 bash .ci/device-smoke/test-gpu.sh
SMP=2 bash .ci/device-smoke/test-gpu.sh
SEMU_TEST_TIMEOUT=240 SEMU_DIRECTFB2_TEST=0 ENABLE_THREADED=1 SMP=4 bash .ci/device-smoke/test-gpu.sh
```

All commands above pass. The GPU smoke logs still contain the known local TAP
and ALSA warnings; they are non-fatal. The successful guest checks include
boot/login, `/dev/dri/card0`, `virtio_gpu` binding, and DirectFB2
`DRMKMS/System` for the full GPU runs.

The threaded SMP=4 basic GPU smoke does not reliably fit the default 90 second
CI timeout in this environment, but passes with `SEMU_TEST_TIMEOUT=240`.

Additional SMP=4 guest dmesg check:

```text
[    0.000000] SLUB: HWalign=64, Order=0-3, MinObjects=0, CPUs=4, Nodes=1
[    0.000000] rcu:  RCU restricting CPUs from NR_CPUS=32 to nr_cpu_ids=4.
[    0.017000] smp: Bringing up secondary CPUs ...
[    0.032000] smp: Brought up 1 node, 4 CPUs
```

## Remaining Risks Carried Forward

These are not claimed complete by Phase 2:

- LR/SC and AMO host-side atomicity still need Phase 3 memory-model work.
- RFENCE/TLB shootdown correctness remains Phase 4 work.
- Device pending is sampled under each device lock and then pushed into PLIC;
  all writers also refresh PLIC, but a stricter final-state protocol can be
  revisited if later stress tests expose stale PLIC active bits.
- Threaded SMP=4 is functionally passing but slower than the default 90 second
  smoke timeout on this host.
