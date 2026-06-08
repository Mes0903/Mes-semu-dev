# SMP Phase 1 Device Ownership Audit

Phase 1 keeps the coroutine execution path intact. The mutex fields added to `emu_state_t` are placeholders for Phase 2 and are initialized during `semu_init()`, but MMIO locking remains a no-op until hart execution moves to OS threads.

Current note (2026-06-08): this audit records Phase 1 history. `smp-support`
now builds threaded-only, and MMIO locking is active unconditionally.

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

## PR2 RAM DMA Substrate Audit

PR2 adds `guest-types.h` and the `ram_dma_t` substrate in `ram_access.h` / `ram_access.c`, but intentionally does not migrate virtio devices or queue parsing. The following direct RAM paths remain legacy transitional code and are not actor-safe until later phases move them behind common virtq and DMA helpers:

- `device.h`: virtio net, block, rng, input, gpu, sound, and filesystem device state still retains `uint32_t *ram` pointers supplied by `semu_init()`.
- `main.c`: runtime RAM mapping, kernel/DTB/initrd loading, hart `ram_base` setup, and legacy `ram_read()` / `ram_write()` callbacks still operate on the raw `emu->ram` mapping. `emu_state_t::ram_dma` is initialized for the new substrate but is not yet used by devices.
- `virtio-blk.c`, `virtio-rng.c`, `virtio-net.c`, `virtio-input.c`, `virtio-gpu.c`, `virtio-snd.c`, and `virtio-fs.c`: queue notification handlers still parse descriptors with direct `ram[...]` indexing and use `(uintptr_t) ram + guest_addr` style casts for payload buffers. Some paths use the existing atomic word/subword helpers for ring indexes, but payload access and descriptor walking are still raw host-pointer operations.
- `riscv.c` / `riscv.h`: CPU RAM fast paths keep per-hart raw RAM cache pointers for instruction/data access. These are CPU execution internals, not device DMA helpers, and are outside the virtio actor migration in this slice.

The new `ram_dma_t` API bounds-checks typed guest physical byte ranges, avoids returning long-lived host pointers, writes bytes through atomic subword helpers, invalidates overlapping LR/SC reservations at 32-bit word granularity, and records atomic dirty byte/range state. Later actor phases should route virtqueue descriptor parsing and device payload DMA through this common substrate before treating device actors as RAM-safe.
