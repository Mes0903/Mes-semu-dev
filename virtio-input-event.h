#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "feature.h"

#if SEMU_HAS(VIRTIOINPUT)
/* Per virtio-input spec, config string/bitmap payloads are 128 bytes. */
#define VIRTIO_INPUT_CFG_PAYLOAD_SIZE 128

/* Host-side input commands produced by the window backend. The SDL/main
 * thread translates platform input into this neutral form. The emulator
 * thread consumes it and updates the guest-facing virtio-input device state.
 */
enum vinput_cmd_type {
    VINPUT_CMD_KEYBOARD_KEY = 0,
    VINPUT_CMD_MOUSE_BUTTON,
    VINPUT_CMD_MOUSE_MOTION,
    VINPUT_CMD_MOUSE_WHEEL,
};

/* One queued backend input command. All payloads are small POD values
 * embedded directly in the union, so the producer does not need to touch
 * virtio queues, guest RAM, or heap allocation.
 */
struct vinput_cmd {
    enum vinput_cmd_type type;
    union {
        struct {
            uint32_t key;
            uint32_t value;
        } keyboard_key;
        struct {
            uint32_t button;
            bool pressed;
        } mouse_button;
        struct {
            int32_t dx;
            int32_t dy;
        } mouse_motion;
        struct {
            int32_t dx;
            int32_t dy;
        } mouse_wheel;
    } u;
};

/* Poll and translate pending SDL events on the main thread. Returns true if a
 * quit/close request was observed, which tells the caller to shut down the
 * frontend loop.
 */
bool vinput_handle_events(void);

/* Pop one translated backend input event. Called by the emulator thread while
 * draining work that arrived from the SDL/main thread.
 */
bool vinput_pop_cmd(struct vinput_cmd *cmd);

/* Reopen the producer wake gate after the emulator thread drains the current
 * batch of queued input events. Returns true if the queue remained empty
 * across the rearm, or false if the producer raced and more events are
 * already pending.
 */
bool vinput_rearm_cmd_wake(void);

/* Returns true once the backend has published input work for the emulator
 * thread. This is a cheap fast-path check used to skip queue-drain bookkeeping
 * when no translated input events are pending.
 */
bool vinput_may_have_pending_cmds(void);

/* Clear all events in the SPSC queue. The queue is shared by the keyboard and
 * mouse frontend paths, so a reset on either virtio-input device drops pending
 * events for both.
 */
void vinput_reset_host_events(void);

/* Fill bitmap[] with exactly the key codes this backend can generate.
 * Per the virtio-input spec, only advertise key codes this device will
 * actually generate. Returns the minimum byte count needed (index of the
 * highest set byte + 1), matching the virtio-input config "size" field.
 * bitmap_size must be >= VIRTIO_INPUT_CFG_PAYLOAD_SIZE.
 */
int virtio_input_fill_ev_key_bitmap(uint8_t *bitmap, size_t bitmap_size);
#endif /* SEMU_HAS(VIRTIOINPUT) */
