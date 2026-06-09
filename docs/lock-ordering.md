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

The common VirtIO MMIO `QueueNotify` lifecycle path is the first enforced path.
It tracks lifecycle -> transport ordering, uses `backend_lock` only as a
reset/activation barrier, drops the lifecycle rank before waiting on a busy
backend lock, then revalidates generation/readiness under transport before
calling `notify_queue()` under lifecycle -> transport. The backend rank is
released before the callback so actor-backed callbacks can take actor mailbox
locks in order, while status-zero reset cannot increment the device generation
between the final revalidation and actor enqueue.

Activation is not fully enforced by this step. `virtio_mmio_complete_activation()`
still uses `backend_lock` before `transport_lock` while it revalidates and runs
the activation callback, but it now rejects `reset_in_progress` during both
pre-callback and post-callback revalidation. Reset has been split:
`virtio_device_common_reset()` uses `backend_lock` only as a short barrier,
publishes a new generation plus `reset_in_progress` under `transport_lock`,
releases both common locks before callbacks that may wait for actors, clears
transport/queue/ISR/status state, calls backend reset after transport state
has been cleared, and keeps the reset gate set until backend reset returns.
While that gate is set, status-zero reset is idempotent, QueueNotify is accepted
as reset-canceled work, and other MMIO writes are rejected so guest setup,
activation, and config writes cannot race the reset callbacks.
