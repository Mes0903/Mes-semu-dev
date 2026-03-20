#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "feature.h"

#if SEMU_HAS(VIRTIOINPUT)
struct window_backend {
    void (*window_init)(void);
    /* Main loop function that runs on the main thread (for macOS SDL2).
     * If non-NULL, the emulator runs in a background thread while this
     * function handles window events on the main thread.
     * Returns when the emulator should exit.
     */
    void (*window_main_loop)(void);
    /* Called from the emulator thread when semu_run() returns, to unblock
     * window_main_loop() so the main thread can proceed to pthread_join.
     */
    void (*window_shutdown)(void);
    /* Fill bitmap[] with exactly the key codes this backend can generate.
     * Per the virtio-input spec, only advertise key codes this device will
     * actually generate. Returns the minimum byte count (highest set byte
     * index + 1) to use as the virtio-input config "size" field.
     * bitmap_size must be >= 128.
     */
    int (*fill_ev_key_bitmap)(uint8_t *bitmap, size_t bitmap_size);
};

/* Poll and translate pending SDL events on the main thread. Returns true if a
 * quit/close request was observed, which tells the caller to shut down the
 * frontend loop.
 */
bool handle_window_events(void);

/* Fill bitmap[] with exactly the key codes this backend can generate.
 * Per the virtio-input spec, only advertise key codes this device will
 * actually generate. Returns the minimum byte count needed (index of the
 * highest set byte + 1), matching the virtio-input config "size" field.
 * bitmap_size must be >= 128.
 */
int window_fill_ev_key_bitmap(uint8_t *bitmap, size_t bitmap_size);

/* Returns true once the window has been closed (or SDL failed to initialize).
 * Safe to call from any thread, it uses relaxed atomic load.
 */
bool window_is_closed(void);

/* Register the write end of a pipe to be written when the window shuts down.
 * Must be called before window_main_loop(). The SMP poll loop reads the read
 * end so that poll(-1) is unblocked immediately on window close.
 */
void window_set_wake_fd(int fd);
#endif
