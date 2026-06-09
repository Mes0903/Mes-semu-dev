# Virtio Actor Error Policy

This policy keeps guest protocol mistakes device-local while reserving actor or
VM failure for conditions that break emulator invariants.

| Condition | Policy | Current anchors |
| --- | --- | --- |
| Malformed descriptor chain | Treat as transport corruption. Stop processing that queue entry, set `DEVICE_NEEDS_RESET`, expose a test counter where the transport has one, and do not spin or log forever. | `virtq_pop()` failures in actor-backed devices call device fail paths; mmio activation failure is covered by `tests/test-virtio-mmio.c`. |
| Unsupported device command | Complete the request with the device-specific unsupported/invalid response when the protocol has one. Do not fail the VM or device for a validly described guest command. | virtio-gpu undefined commands return `VIRTIO_GPU_RESP_ERR_UNSPEC` from `virtio_gpu_cmd_undefined_handler()` because the protocol has generic error responses but no dedicated unsupported opcode. |
| Transient host I/O error | Retry or defer only for restartable errors such as `EINTR` or `EAGAIN`. The actor must remain wakeable by reset and stop while work is deferred. | Device backend specific. Shared actor wait paths return on stop/reset/fail. |
| Permanent host I/O error | Complete the request with a device-specific I/O error when possible. Mark `DEVICE_NEEDS_RESET` only if the backend contract can no longer be maintained. | Device backend specific. |
| Actor invariant failure or OOM | Mark the device failed, wake synchronous waiters, set `DEVICE_NEEDS_RESET`, and transition the actor to `FAILED`. VM `FAILED` is reserved for essential device failure or possible memory corruption. | `virtio_actor_fail()` and wait tests in `tests/test-virtio-actor.c`. |
| Reset while host I/O pending | Bump generation, stop accepting new queue work, wake the actor, cancel or drain old-generation work, discard stale completions, then ack reset. | Actor generation and completion tests in `tests/test-virtio-actor.c`; full reset coordinator split remains Step 5.5. |

Synchronous actor requests must carry lifecycle/device generation and use a
bounded wait or failure path. Stale completions from an old generation must not
publish used-ring entries, IRQs, display payloads, or backend state.
