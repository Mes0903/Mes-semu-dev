# Lock Ordering Contract

The SMP runtime must keep VM coordination, device frontends, actors, queues, and
host backends in one monotonic lock order. New common substrate code should use
`lock-order.h` when it takes more than one of these lock classes or when a test
needs to prove that a path rejects inversions.

## Required Order

```text
VM lifecycle lock
  -> per-device transport/config lock
     -> actor mailbox lock
        -> queue-local state
           -> backend-local short critical section
```

A path may skip ranks. It must not acquire an earlier rank while a later rank is
held by the same thread. Same-rank nesting is allowed only when the caller has a
separate, documented per-object ordering rule; the lock-order helper still
requires same-rank guards to be released as a stack.

## Forbidden Waits And Calls

These are stronger than rank checks and must be reviewed directly:

- do not wait for an actor while holding a transport/config lock
- do not wait for hart pause acknowledgement while holding a PLIC/device lock
- do not call SDL while holding any VirtIO, vgpu, or backend lock
- do not call host blocking I/O while holding the lifecycle or transport lock
- do not close a PortAudio stream while holding a virtqueue lock

Actor notifications, IRQ publication, and lifecycle state transitions should
capture the minimum state under locks, release locks before blocking waits, and
then revalidate generation/state before publishing guest-visible completion.

## Helper Semantics

`lock-order.h` tracks a per-thread stack of ranks. `semu_lock_order_enter()`
returns `-EDEADLK` for rank inversion, `-EPERM` for out-of-stack release, and
leaves the current rank unchanged on failure. The pthread wrappers return
negative errno values and update tracking only when the mutex operation succeeds.

The helper is intentionally opt-in. It does not make raw `pthread_mutex_lock()`
call sites safe, and it does not detect cross-thread lock cycles without wrapper
coverage on the participating paths.

## Current Coverage And Follow-Ups

The common VirtIO MMIO `QueueNotify` lifecycle path is the first enforced path:
it tracks lifecycle -> transport -> backend ordering and deliberately drops the
lifecycle rank before waiting on a busy backend lock.

Known transitional exception: `virtio_mmio_complete_activation()` and
`virtio_device_common_reset()` still serialize backend callbacks with
`backend_lock` before taking `transport_lock` for generation/status checks and
state publication. Refactoring those callbacks needs a wider activation/reset
protocol change so the backend callback exclusion and generation revalidation
can be kept without backend -> transport nesting. Do not mechanically wrap those
paths until that protocol is split.
