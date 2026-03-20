#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "feature.h"

#if SEMU_HAS(VIRTIOINPUT)
/* Host-side events produced by the window backend. The SDL/main thread only
 * translates platform input into this neutral form. The emulator thread
 * consumes it and updates the guest-facing virtio-input device state.
 */
typedef enum {
    WINDOW_EVENT_KEYBOARD_KEY = 0,
    WINDOW_EVENT_MOUSE_BUTTON,
    WINDOW_EVENT_MOUSE_MOTION,
    WINDOW_EVENT_MOUSE_WHEEL,
} window_event_type_t;

/* One queued backend event. The payload is intentionally host-centric so the
 * producer does not need to touch virtio queues or guest RAM.
 */
typedef struct {
    window_event_type_t type;
    union {
        struct {
            uint32_t key;
            uint32_t value;
        } key;
        struct {
            uint32_t button;
            bool pressed;
        } button;
        struct {
            uint32_t x;
            uint32_t y;
        } motion;
        struct {
            int32_t dx;
            int32_t dy;
        } wheel;
    } u;
} window_event_t;

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

/* Pop one translated backend event. Called by the emulator thread while
 * draining work that arrived from the SDL/main thread.
 */
bool window_pop_event(window_event_t *event);

/* Reopen the producer wake gate after the emulator thread drains the current
 * batch of queued events. Returns true if the queue remained empty across the
 * rearm, or false if the producer raced and more events are already pending.
 */
bool window_rearm_wake(void);

/* Returns true once the backend has published work for the emulator thread.
 * This is a cheap fast-path check used to skip queue-drain bookkeeping when
 * no translated input events are pending.
 */
bool window_events_maybe_pending(void);

/* Returns true once the window has been closed (or SDL failed to initialize).
 * Safe to call from any thread, it uses relaxed atomic load.
 */
bool window_is_closed(void);

/* Register the write end of a pipe used to wake the emulator thread when the
 * backend has queued work for it, such as input events or window shutdown.
 * Must be called before window_main_loop(). The SMP poll loop reads the read
 * end so that poll(-1) is unblocked immediately when backend work arrives.
 */
void window_set_wake_fd(int fd);

/* Wake the emulator thread when the backend has queued work for it. Multiple
 * queued events may be serviced by a single wakeup. The pipe is only used to
 * break poll(-1), not to carry the event payload itself.
 */
void window_wake_backend(void);
#endif
