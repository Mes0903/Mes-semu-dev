# SMP Phase 1 Device Ownership Audit

Phase 1 keeps the coroutine execution path intact. The mutex fields added to `emu_state_t` are placeholders for Phase 2 and are initialized during `semu_init()`, but MMIO locking remains a no-op until hart execution moves to OS threads.

## Device State

- PLIC: `plic_state_t` is owned by the emulator state. Hart MMIO and peripheral interrupt refresh both update `ip`, `ie`, `masked`, and `active`; Phase 2 should guard these paths with `plic_lock`.
- ACLINT: `mtimer_state_t`, `mswi_state_t`, and `sswi_state_t` are per-emulator devices. Phase 1 converts their cross-hart arrays to atomics and adds `mtimer_lock`, `mswi_lock`, and `sswi_lock` placeholders for MMIO register access.
- UART: `u8250_state_t` is owned by the emulator state. UART readiness polling and MMIO access share `pending_ints`, `in_ready`, and waiter fields; Phase 2 should guard these paths with `uart_lock` or move polling to the I/O thread under the same lock.
- VirtIO block, rng, fs, gpu, input, snd, net: each device has a per-device lock placeholder in `emu_state_t`. Queue notify handlers read the avail ring with acquire helpers and publish used ring idx with release helpers. Phase 2 should take the relevant lock around MMIO register access and queue processing.

## Global Or Static State

- `virtio-input.c`: process-wide host event queues and static device registry are protected today by the existing input mutex for host-event injection. The new per-device locks should cover MMIO/config/queue paths; Phase 2 must avoid holding both lock classes in inconsistent order.
- `virtio-gpu.c`: process-wide GPU config/data is shared by config reads and command processing. The `vgpu_lock` placeholder should cover both paths once MMIO can run on multiple hart threads.
- `window-sw.c`: SDL/window state stays owned by the main/window thread. The emulator observes shutdown through existing atomic/backend wake mechanisms; no broad device lock should be added around the SDL event loop.
- `virtio-net.c`: backend state must be single-threaded or protected. Phase 2 should run poll/refresh and QueueNotify TX/RX under `vnet_lock`.
- `virtio-snd.c`: PortAudio creates a callback thread. Phase 2 locking must account for callback access to audio queue state and keep callback critical sections short.

## Phase 1 Conclusion

No device ownership transfer happens in Phase 1. The structural changes are limited to initialized lock placeholders plus RAM/ring ordering helpers, so coroutine behavior should remain unchanged.
