#include <SDL.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if SEMU_HAS(VIRGL)
#include <epoxy/gl.h>
#include "vgpu-gl.h"
#include "vgpu-renderer.h"
#endif
#if SEMU_HAS(VIRTIOGPU)
#include "vgpu-display.h"
#include "virtio-gpu.h"
#endif
#if SEMU_HAS(VIRTIOINPUT)
#include "virtio-input-event.h"
#endif
#include "window.h"

#define WINDOW_LOG_PREFIX "[SEMU WINDOW] "

static int wake_write_fd = -1;
static bool sdl_initialized = false;
static bool headless_mode = false;
static bool should_exit = false;

#if SEMU_HAS(VIRTIOINPUT)
static bool mouse_grabbed = false;
static SDL_Window *sdl_input_window;
#else
#define SDL_EVENT_WAIT_TIMEOUT_MS 1 /* ms */
#define SDL_EVENT_BURST_LIMIT 64U
#endif

#if SEMU_HAS(VIRTIOGPU)
/* SDL-owned retained state for a single plane. Textures live only on the SDL
 * thread and are updated from immutable CPU-frame display resources.
 */
struct sdl_plane_info {
    uint32_t width;
    uint32_t height;
    uint32_t sdl_format;
    bool alpha_blend;
    SDL_Texture *texture;
};

/* SDL-owned retained state for one scanout. 'window_init_sw()' creates the
 * window/renderer, then 'window_drain_display_queue()' updates the primary and
 * cursor planes from queued display payloads before rendering them.
 */
struct sdl_scanout_info {
    struct sdl_plane_info primary_plane;
    struct sdl_plane_info cursor_plane;
    SDL_Rect cursor_rect;
    uint32_t cursor_hot_x;
    uint32_t cursor_hot_y;
    uint32_t window_width;
    uint32_t window_height;

    SDL_Window *window;
    SDL_Renderer *renderer;
#if SEMU_HAS(VIRGL)
    SDL_GLContext gl_context;
    GLuint gl_primary_fb;
    GLuint gl_cursor_texture;
    uint32_t gl_cursor_width;
    uint32_t gl_cursor_height;
    bool gl_primary_valid;
    bool gl_cursor_valid;
    struct vgpu_display_gl_scanout_payload gl_primary;
#endif
};

static struct sdl_scanout_info sdl_scanouts[VIRTIO_GPU_MAX_SCANOUTS];
#endif

#if SEMU_HAS(VIRTIOGPU)
enum window_debug_stage {
    WINDOW_DEBUG_STAGE_IDLE = 0,
    WINDOW_DEBUG_STAGE_HANDLE_EVENTS,
    WINDOW_DEBUG_STAGE_DRAIN_RENDERER,
    WINDOW_DEBUG_STAGE_EXEC_RENDERER,
    WINDOW_DEBUG_STAGE_DRAIN_DISPLAY,
    WINDOW_DEBUG_STAGE_APPLY_DISPLAY_CMD,
    WINDOW_DEBUG_STAGE_APPLY_GL_FRAME,
    WINDOW_DEBUG_STAGE_RENDER_SCANOUT,
    WINDOW_DEBUG_STAGE_RENDER_GL_MAKE_CURRENT,
    WINDOW_DEBUG_STAGE_RENDER_GL_BLIT,
    WINDOW_DEBUG_STAGE_RENDER_GL_SWAP,
};

static uint32_t window_debug_stage;
static uint32_t window_debug_stage_scanout;
static uint32_t window_debug_stage_cmd;
static uint64_t window_debug_sdl_events;
static uint64_t window_debug_renderer_drains;
static uint64_t window_debug_renderer_requests;
static uint64_t window_debug_display_drains;
static uint64_t window_debug_display_cmds;
static uint64_t window_debug_scanout_renders;
static bool window_debug_progress_stop;
static bool window_debug_progress_started;
static pthread_t window_debug_progress_thread;
static FILE *window_debug_progress_file;
static uint32_t window_debug_progress_interval_ms;

static const char *window_debug_stage_name(uint32_t stage)
{
    switch ((enum window_debug_stage) stage) {
    case WINDOW_DEBUG_STAGE_IDLE:
        return "idle";
    case WINDOW_DEBUG_STAGE_HANDLE_EVENTS:
        return "handle-events";
    case WINDOW_DEBUG_STAGE_DRAIN_RENDERER:
        return "drain-renderer";
    case WINDOW_DEBUG_STAGE_EXEC_RENDERER:
        return "exec-renderer";
    case WINDOW_DEBUG_STAGE_DRAIN_DISPLAY:
        return "drain-display";
    case WINDOW_DEBUG_STAGE_APPLY_DISPLAY_CMD:
        return "apply-display-cmd";
    case WINDOW_DEBUG_STAGE_APPLY_GL_FRAME:
        return "apply-gl-frame";
    case WINDOW_DEBUG_STAGE_RENDER_SCANOUT:
        return "render-scanout";
    case WINDOW_DEBUG_STAGE_RENDER_GL_MAKE_CURRENT:
        return "render-gl-make-current";
    case WINDOW_DEBUG_STAGE_RENDER_GL_BLIT:
        return "render-gl-blit";
    case WINDOW_DEBUG_STAGE_RENDER_GL_SWAP:
        return "render-gl-swap";
    }
    return "unknown";
}

static const char *window_debug_display_cmd_name(uint32_t cmd)
{
    if (cmd == UINT32_MAX)
        return "none";

    switch ((enum vgpu_display_cmd_type) cmd) {
    case VGPU_DISPLAY_CMD_PRIMARY_SET:
        return "PRIMARY_SET";
    case VGPU_DISPLAY_CMD_PRIMARY_CLEAR:
        return "PRIMARY_CLEAR";
    case VGPU_DISPLAY_CMD_CURSOR_SET:
        return "CURSOR_SET";
    case VGPU_DISPLAY_CMD_CURSOR_CLEAR:
        return "CURSOR_CLEAR";
    case VGPU_DISPLAY_CMD_CURSOR_MOVE:
        return "CURSOR_MOVE";
    }
    return "UNKNOWN";
}

#if SEMU_HAS(VIRGL)
static const char *window_debug_renderer_req_name(uint32_t type)
{
    switch ((enum vgpu_renderer_request_type) type) {
    case VGPU_RENDERER_REQ_INIT:
        return "INIT";
    case VGPU_RENDERER_REQ_RESET:
        return "RESET";
    case VGPU_RENDERER_REQ_POLL:
        return "POLL";
    case VGPU_RENDERER_REQ_CTRL:
        return "CTRL";
    case VGPU_RENDERER_REQ_SHUTDOWN:
        return "SHUTDOWN";
    }
    return "UNKNOWN";
}

static const char *window_debug_gpu_cmd_name(uint32_t cmd)
{
    switch (cmd) {
    case VIRTIO_GPU_CMD_CTX_CREATE:
        return "CTX_CREATE";
    case VIRTIO_GPU_CMD_CTX_DESTROY:
        return "CTX_DESTROY";
    case VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE:
        return "CTX_ATTACH_RESOURCE";
    case VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE:
        return "CTX_DETACH_RESOURCE";
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_3D:
        return "RESOURCE_CREATE_3D";
    case VIRTIO_GPU_CMD_RESOURCE_UNREF:
        return "RESOURCE_UNREF";
    case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
        return "RESOURCE_ATTACH_BACKING";
    case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
        return "RESOURCE_DETACH_BACKING";
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
        return "TRANSFER_TO_HOST_2D";
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:
        return "TRANSFER_TO_HOST_3D";
    case VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D:
        return "TRANSFER_FROM_HOST_3D";
    case VIRTIO_GPU_CMD_SUBMIT_3D:
        return "SUBMIT_3D";
    case VIRTIO_GPU_CMD_SET_SCANOUT:
        return "SET_SCANOUT";
    case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
        return "RESOURCE_FLUSH";
    case VIRTIO_GPU_CMD_GET_CAPSET_INFO:
        return "GET_CAPSET_INFO";
    case VIRTIO_GPU_CMD_GET_CAPSET:
        return "GET_CAPSET";
    }
    return "UNKNOWN";
}
#endif

static void window_debug_set_stage(enum window_debug_stage stage,
                                   uint32_t scanout_id,
                                   uint32_t cmd)
{
    __atomic_store_n(&window_debug_stage_scanout, scanout_id, __ATOMIC_RELAXED);
    __atomic_store_n(&window_debug_stage_cmd, cmd, __ATOMIC_RELAXED);
    __atomic_store_n(&window_debug_stage, (uint32_t) stage, __ATOMIC_RELEASE);
}

static bool window_debug_env_enabled(const char *value)
{
    if (!value || !value[0])
        return false;
    return strcmp(value, "0") != 0 && strcmp(value, "false") != 0 &&
           strcmp(value, "FALSE") != 0 && strcmp(value, "no") != 0 &&
           strcmp(value, "NO") != 0;
}

static uint32_t window_debug_parse_interval_ms(void)
{
    const char *value = getenv("SEMU_VGPU_PROGRESS_LOG_INTERVAL");
    if (!value || !value[0])
        return 2000;

    char *end = NULL;
    double seconds = strtod(value, &end);
    if (end == value || seconds <= 0.0)
        return 2000;
    if (seconds < 0.1)
        seconds = 0.1;
    if (seconds > 60.0)
        seconds = 60.0;
    return (uint32_t) (seconds * 1000.0);
}

static void window_debug_log_progress(FILE *out)
{
    time_t now = time(NULL);
    uint32_t stage = __atomic_load_n(&window_debug_stage, __ATOMIC_ACQUIRE);
    uint32_t stage_scanout =
        __atomic_load_n(&window_debug_stage_scanout, __ATOMIC_RELAXED);
    uint32_t stage_cmd =
        __atomic_load_n(&window_debug_stage_cmd, __ATOMIC_RELAXED);

    fprintf(out,
            "[SEMU VGPU PROGRESS] t=%lld stage=%s scanout=%" PRIu32
            " display_cmd=%s sdl_events=%" PRIu64 " renderer_drains=%" PRIu64
            " renderer_reqs=%" PRIu64 " display_drains=%" PRIu64
            " display_cmds=%" PRIu64 " renders=%" PRIu64 "\n",
            (long long) now, window_debug_stage_name(stage), stage_scanout,
            window_debug_display_cmd_name(stage_cmd),
            __atomic_load_n(&window_debug_sdl_events, __ATOMIC_RELAXED),
            __atomic_load_n(&window_debug_renderer_drains, __ATOMIC_RELAXED),
            __atomic_load_n(&window_debug_renderer_requests, __ATOMIC_RELAXED),
            __atomic_load_n(&window_debug_display_drains, __ATOMIC_RELAXED),
            __atomic_load_n(&window_debug_display_cmds, __ATOMIC_RELAXED),
            __atomic_load_n(&window_debug_scanout_renders, __ATOMIC_RELAXED));

    struct vgpu_display_debug_stats display = {0};
    vgpu_display_debug_snapshot(&display);
    fprintf(out,
            "[SEMU VGPU PROGRESS] display scanouts=%" PRIu32 " q=%" PRIu32
            " head=%" PRIu32 " tail=%" PRIu32 " queued=%" PRIu64
            " dropped=%" PRIu64 " popped=%" PRIu64 " stale=%" PRIu64
            " primary_set=%" PRIu64 " cursor_set=%" PRIu64
            " cursor_move=%" PRIu64 " primary_clear=%" PRIu64
            " cursor_clear=%" PRIu64 " unavailable=%u\n",
            display.scanout_count, display.queue_depth, display.queue_head,
            display.queue_tail, display.cmds_queued, display.cmds_dropped,
            display.cmds_popped, display.stale_cmds_dropped,
            display.primary_sets_published, display.cursor_sets_published,
            display.cursor_moves_published, display.primary_clears,
            display.cursor_clears, display.unavailable ? 1U : 0U);

    struct virtio_gpu_debug_stats gpu = {0};
    virtio_gpu_debug_snapshot(&gpu);
    fprintf(out,
            "[SEMU VGPU PROGRESS] virtio generation=%" PRIu32
            " pending_ctrls=%" PRIu32 " dispatch=%u deferred=%" PRIu64
            " defer_dropped=%" PRIu64 " fence_done=%" PRIu64
            " token_done=%" PRIu64 "\n",
            gpu.ctrl_generation, gpu.pending_ctrls_active,
            gpu.dispatch_active ? 1U : 0U, gpu.ctrl_responses_deferred,
            gpu.ctrl_responses_defer_dropped, gpu.ctrl_responses_completed,
            gpu.ctrl_token_responses_completed);

#if SEMU_HAS(VIRTIOINPUT)
    struct vinput_host_debug_stats host_input = {0};
    vinput_debug_snapshot(&host_input);
    fprintf(
        out,
        "[SEMU VGPU PROGRESS] input-host k_q=%" PRIu32 " m_q=%" PRIu32
        " wake=%u k_push/drop/pop=%" PRIu64 "/%" PRIu64 "/%" PRIu64
        " m_push/drop/pop=%" PRIu64 "/%" PRIu64 "/%" PRIu64 " sdl_key=%" PRIu64
        "/%" PRIu64 " sdl_btn=%" PRIu64 "/%" PRIu64 " sdl_motion=%" PRIu64
        "/%" PRIu64 " last_motion=%" PRId32 ",%" PRId32 " sdl_wheel=%" PRIu64
        "/%" PRIu64 " last_wheel=%" PRId32 ",%" PRId32 "\n",
        host_input.keyboard_queue_depth, host_input.mouse_queue_depth,
        host_input.wake_pending ? 1U : 0U, host_input.keyboard_cmds_pushed,
        host_input.keyboard_cmds_dropped, host_input.keyboard_cmds_popped,
        host_input.mouse_cmds_pushed, host_input.mouse_cmds_dropped,
        host_input.mouse_cmds_popped, host_input.sdl_keydown,
        host_input.sdl_keyup, host_input.sdl_mouse_button_down,
        host_input.sdl_mouse_button_up, host_input.sdl_mouse_motion_observed,
        host_input.sdl_mouse_motion_published, host_input.last_motion_dx,
        host_input.last_motion_dy, host_input.sdl_mouse_wheel_observed,
        host_input.sdl_mouse_wheel_published, host_input.last_wheel_dx,
        host_input.last_wheel_dy);

    struct virtio_input_debug_stats guest_input = {0};
    virtio_input_debug_snapshot(&guest_input);
    fprintf(out,
            "[SEMU VGPU PROGRESS] input-guest key=%" PRIu64 " pageup=%" PRIu64
            " buttons=%" PRIu64 " motion_batches=%" PRIu64
            " scroll_batches=%" PRIu64 " rel_x/y/hwheel/wheel=%" PRIu64
            "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 " eventq_writes k/m=%" PRIu64
            "/%" PRIu64 " eventq_drops k/m=%" PRIu64 "/%" PRIu64
            " last_key=%" PRIu16 ":%" PRIu32 " last_mouse_rel=%" PRIu16
            ":%" PRId32 "\n",
            guest_input.keyboard_keys, guest_input.keyboard_pageup_keys,
            guest_input.mouse_buttons, guest_input.mouse_motion_batches,
            guest_input.mouse_scroll_batches, guest_input.mouse_rel_x,
            guest_input.mouse_rel_y, guest_input.mouse_rel_hwheel,
            guest_input.mouse_rel_wheel, guest_input.keyboard_eventq_writes,
            guest_input.mouse_eventq_writes, guest_input.keyboard_eventq_drops,
            guest_input.mouse_eventq_drops, guest_input.last_keyboard_code,
            guest_input.last_keyboard_value, guest_input.last_mouse_rel_code,
            guest_input.last_mouse_rel_value);
#endif

#if SEMU_HAS(VIRGL)
    struct vgpu_renderer_debug_stats renderer = {0};
    vgpu_renderer_debug_snapshot(&renderer);
    fprintf(out,
            "[SEMU VGPU PROGRESS] renderer req_q=%" PRIu32 " comp_q=%" PRIu32
            " submitted=%" PRIu64 " submit_drop=%" PRIu64 " popped=%" PRIu64
            " completed=%" PRIu64 " complete_drop=%" PRIu64
            " done_popped=%" PRIu64 " exec=%" PRIu64 "/%" PRIu64
            " current_seq=%" PRIu64 " current_req=%s current_cmd=%s/%" PRIu32
            " token=%" PRIu32 " gen=%" PRIu32 "\n",
            renderer.request_depth, renderer.completion_depth,
            renderer.requests_submitted, renderer.requests_dropped,
            renderer.requests_popped, renderer.completions_submitted,
            renderer.completions_dropped, renderer.completions_popped,
            renderer.execute_started, renderer.execute_finished,
            renderer.current_execute_seq,
            window_debug_renderer_req_name(renderer.current_request_type),
            window_debug_gpu_cmd_name(renderer.current_command_type),
            renderer.current_command_type, renderer.current_token_id,
            renderer.current_generation);

    struct vgpu_virgl_debug_stats virgl = {0};
    vgpu_virgl_debug_snapshot(&virgl);
    fprintf(
        out,
        "[SEMU VGPU PROGRESS] virgl pending_fences=%" PRIu32
        " poll_pending=%u poll=%" PRIu64 "/%" PRIu64 " poll_drop=%" PRIu64
        " fences=%" PRIu64 "/%" PRIu64 " ctrl=%" PRIu64 "/%" PRIu64
        " scanout=%" PRIu64 " scanout_drop=%" PRIu64 " last_ctx0_fence=%" PRIu64
        " last_ctx_fence=%" PRIu64 " ctx=%" PRIu32 " ring=%" PRIu32 "\n",
        virgl.pending_fences, virgl.poll_request_pending ? 1U : 0U,
        virgl.poll_requests_submitted, virgl.poll_requests_executed,
        virgl.poll_requests_dropped, virgl.fences_created,
        virgl.fences_completed, virgl.ctrl_requests_started,
        virgl.ctrl_requests_completed, virgl.scanouts_published,
        virgl.scanouts_dropped, virgl.last_ctx0_fence, virgl.last_context_fence,
        virgl.last_context_ctx_id, virgl.last_context_ring_idx);
#endif

    fflush(out);
}

static void *window_debug_progress_main(void *arg)
{
    FILE *out = arg;
    fprintf(out,
            "[SEMU VGPU PROGRESS] monitor started interval_ms=%" PRIu32 "\n",
            window_debug_progress_interval_ms);
    fflush(out);

    while (!__atomic_load_n(&window_debug_progress_stop, __ATOMIC_ACQUIRE)) {
        window_debug_log_progress(out);

        uint32_t slept_ms = 0;
        while (
            slept_ms < window_debug_progress_interval_ms &&
            !__atomic_load_n(&window_debug_progress_stop, __ATOMIC_ACQUIRE)) {
            uint32_t step_ms = window_debug_progress_interval_ms - slept_ms;
            if (step_ms > 100)
                step_ms = 100;
            usleep(step_ms * 1000U);
            slept_ms += step_ms;
        }
    }

    window_debug_log_progress(out);
    fprintf(out, "[SEMU VGPU PROGRESS] monitor stopped\n");
    fflush(out);
    return NULL;
}

static void window_debug_progress_start(void)
{
    const char *enabled = getenv("SEMU_VGPU_PROGRESS_LOG");
    const char *path = getenv("SEMU_VGPU_PROGRESS_LOG_FILE");
    if (enabled && !window_debug_env_enabled(enabled))
        return;
    if (!window_debug_env_enabled(enabled) && (!path || !path[0]))
        return;

    window_debug_progress_interval_ms = window_debug_parse_interval_ms();
    FILE *out = stderr;
    if (path && path[0]) {
        out = fopen(path, "a");
        if (!out) {
            fprintf(stderr,
                    WINDOW_LOG_PREFIX
                    "%s(): failed to open progress log '%s'\n",
                    __func__, path);
            out = stderr;
        }
    }

    window_debug_progress_file = out;
    __atomic_store_n(&window_debug_progress_stop, false, __ATOMIC_RELEASE);
    if (pthread_create(&window_debug_progress_thread, NULL,
                       window_debug_progress_main, out) != 0) {
        fprintf(stderr,
                WINDOW_LOG_PREFIX "%s(): failed to start progress monitor\n",
                __func__);
        if (out != stderr)
            fclose(out);
        window_debug_progress_file = NULL;
        return;
    }

    window_debug_progress_started = true;
}

static void window_debug_progress_stop_monitor(void)
{
    if (!window_debug_progress_started)
        return;

    __atomic_store_n(&window_debug_progress_stop, true, __ATOMIC_RELEASE);
    pthread_join(window_debug_progress_thread, NULL);
    window_debug_progress_started = false;

    if (window_debug_progress_file && window_debug_progress_file != stderr)
        fclose(window_debug_progress_file);
    window_debug_progress_file = NULL;
}
#else
enum window_debug_stage {
    WINDOW_DEBUG_STAGE_IDLE = 0,
    WINDOW_DEBUG_STAGE_HANDLE_EVENTS,
};

static uint64_t window_debug_sdl_events;

static void window_debug_set_stage(enum window_debug_stage stage,
                                   uint32_t scanout_id,
                                   uint32_t cmd)
{
    (void) stage;
    (void) scanout_id;
    (void) cmd;
}
#endif

static void window_set_wake_fd_sw(int fd)
{
    wake_write_fd = fd;
}

static void window_wake_backend_sw(void)
{
    if (wake_write_fd >= 0) {
        char byte = 1;
        /* Best-effort wakeup: the pipe is non-blocking, and the byte value has
         * no meaning beyond making the read end readable.
         */
        ssize_t bytes_written = write(wake_write_fd, &byte, 1);
        (void) bytes_written;
    }
}

static void window_wake_frontend_sw(void)
{
    if (!sdl_initialized || headless_mode)
        return;

    SDL_Event event = {0};
    event.type = SDL_USEREVENT;
    SDL_PushEvent(&event);
}

static void window_shutdown_sw(void)
{
    /* Both user-driven close and emulator-driven shutdown funnel through the
     * same flag so the main thread and emulator thread observe one exit state.
     */
    __atomic_store_n(&should_exit, true, __ATOMIC_RELAXED);
    /* Unblock any 'poll(-1)' in the SMP emulator loop immediately. */
    window_wake_backend_sw();
}

static bool window_is_closed_sw(void)
{
    return __atomic_load_n(&should_exit, __ATOMIC_RELAXED);
}

#if SEMU_HAS(VIRTIOINPUT)
/* Main-thread-only helper for relative-pointer devices. SDL's grab and
 * relative mouse APIs are part of the windowing backend, so callers use this
 * to switch between normal host-pointer mode and guest-directed mouse mode.
 */
static void window_set_mouse_grab_sw(bool grabbed)
{
    if (headless_mode || !sdl_input_window) {
        mouse_grabbed = false;
        return;
    }

    if (mouse_grabbed == grabbed)
        return;

    if (grabbed) {
        if (SDL_SetRelativeMouseMode(SDL_TRUE) < 0) {
            fprintf(stderr,
                    "window_set_mouse_grab_sw(): failed to enable relative "
                    "mouse mode: %s\n",
                    SDL_GetError());
            return;
        }
        SDL_SetWindowGrab(sdl_input_window, SDL_TRUE);
        SDL_ShowCursor(SDL_DISABLE);
    } else {
        SDL_SetWindowGrab(sdl_input_window, SDL_FALSE);
        SDL_SetRelativeMouseMode(SDL_FALSE);
        SDL_ShowCursor(SDL_ENABLE);
    }

    mouse_grabbed = grabbed;
}

static bool window_is_mouse_grabbed_sw(void)
{
    return mouse_grabbed;
}
#endif

#if SEMU_HAS(VIRTIOGPU)
static bool vgpu_format_to_sdl_format(enum virtio_gpu_formats virtio_gpu_format,
                                      uint32_t *sdl_format)
{
    switch (virtio_gpu_format) {
    case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_ARGB8888;
        return true;
    case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_XRGB8888;
        return true;
    case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_BGRA8888;
        return true;
    case VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_BGRX8888;
        return true;
    case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_ABGR8888;
        return true;
    case VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_RGBX8888;
        return true;
    case VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_RGBA8888;
        return true;
    case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_XBGR8888;
        return true;
    default:
        return false;
    }
}

static void sdl_plane_info_reset(struct sdl_plane_info *plane)
{
    bool alpha_blend = plane->alpha_blend;
    if (plane->texture)
        SDL_DestroyTexture(plane->texture);
    memset(plane, 0, sizeof(*plane));
    plane->alpha_blend = alpha_blend;
}

static void sdl_plane_info_cleanup(struct sdl_plane_info *plane)
{
    if (plane->texture)
        SDL_DestroyTexture(plane->texture);
    memset(plane, 0, sizeof(*plane));
}

static void sdl_scanout_info_cleanup(struct sdl_scanout_info *scanout)
{
    sdl_plane_info_cleanup(&scanout->primary_plane);
    sdl_plane_info_cleanup(&scanout->cursor_plane);

#if SEMU_HAS(VIRGL)
    if (scanout->gl_context)
        SDL_GL_MakeCurrent(scanout->window, scanout->gl_context);
    if (scanout->gl_primary_fb)
        glDeleteFramebuffers(1, &scanout->gl_primary_fb);
    if (scanout->gl_cursor_texture)
        glDeleteTextures(1, &scanout->gl_cursor_texture);
    if (scanout->gl_context)
        SDL_GL_MakeCurrent(scanout->window, NULL);
    if (scanout->gl_context)
        SDL_GL_DeleteContext(scanout->gl_context);
#endif
    if (scanout->renderer)
        SDL_DestroyRenderer(scanout->renderer);
    if (scanout->window)
        SDL_DestroyWindow(scanout->window);

    memset(scanout, 0, sizeof(*scanout));
}

static bool sdl_scanout_is_ready(const struct sdl_scanout_info *scanout)
{
    if (!scanout->window)
        return false;

#if SEMU_HAS(VIRGL)
    if (scanout->gl_context)
        return true;
#endif
    return scanout->renderer != NULL;
}

static void sdl_scanout_clear_primary(struct sdl_scanout_info *scanout)
{
    sdl_plane_info_reset(&scanout->primary_plane);
#if SEMU_HAS(VIRGL)
    scanout->gl_primary_valid = false;
#endif
}

static void sdl_scanout_clear_cursor(struct sdl_scanout_info *scanout)
{
    memset(&scanout->cursor_rect, 0, sizeof(scanout->cursor_rect));
    scanout->cursor_hot_x = 0;
    scanout->cursor_hot_y = 0;
    sdl_plane_info_reset(&scanout->cursor_plane);
#if SEMU_HAS(VIRGL)
    scanout->gl_cursor_width = 0;
    scanout->gl_cursor_height = 0;
    scanout->gl_cursor_valid = false;
#endif
}

static bool sdl_plane_info_get_sdl_format(
    const struct sdl_plane_info *plane,
    const struct vgpu_display_payload *payload,
    uint32_t *sdl_format)
{
    /* The plane keeps its SDL objects across frames, but the payload format is
     * still per-update data. Resolve the incoming VirtIO-GPU format first,
     * then adjust it below if this plane requires alpha.
     */
    const struct vgpu_display_cpu_payload *frame = &payload->cpu;
    if (!vgpu_format_to_sdl_format(frame->format, sdl_format)) {
        fprintf(stderr, "%s(): invalid resource format %u\n", __func__,
                (uint32_t) frame->format);
        return false;
    }

    /* Cursor textures need an alpha-capable SDL format. If the incoming format
     * is an XRGB/XBGR/BGRX/RGBX variant, switch to the matching alpha version
     * so the high byte is preserved as transparency instead of being ignored.
     */
    if (plane->alpha_blend) {
        switch (*sdl_format) {
        case SDL_PIXELFORMAT_XRGB8888:
            *sdl_format = SDL_PIXELFORMAT_ARGB8888;
            break;
        case SDL_PIXELFORMAT_BGRX8888:
            *sdl_format = SDL_PIXELFORMAT_BGRA8888;
            break;
        case SDL_PIXELFORMAT_RGBX8888:
            *sdl_format = SDL_PIXELFORMAT_RGBA8888;
            break;
        case SDL_PIXELFORMAT_XBGR8888:
            *sdl_format = SDL_PIXELFORMAT_ABGR8888;
            break;
        default:
            break;
        }
    }

    return true;
}

static SDL_Texture *sdl_plane_info_create_texture(
    SDL_Renderer *renderer,
    const struct sdl_plane_info *plane,
    const struct vgpu_display_cpu_payload *frame,
    uint32_t sdl_format)
{
    SDL_Texture *texture =
        SDL_CreateTexture(renderer, sdl_format, SDL_TEXTUREACCESS_STREAMING,
                          frame->width, frame->height);
    if (!texture) {
        fprintf(stderr, "%s(): failed to create texture: %s\n", __func__,
                SDL_GetError());
        return NULL;
    }

    if (plane->alpha_blend) {
        if (SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND) < 0) {
            fprintf(stderr, "%s(): failed to enable texture blending: %s\n",
                    __func__, SDL_GetError());
        }
    }

    return texture;
}

static bool sdl_plane_info_update_texture(
    SDL_Renderer *renderer,
    struct sdl_plane_info *plane,
    const struct vgpu_display_payload *payload,
    const char *plane_name)
{
    if (!renderer)
        return false;

    const struct vgpu_display_cpu_payload *frame = &payload->cpu;
    uint32_t sdl_format;
    if (!sdl_plane_info_get_sdl_format(plane, payload, &sdl_format))
        return false;

    bool reuse_texture = plane->texture && plane->width == frame->width &&
                         plane->height == frame->height &&
                         plane->sdl_format == sdl_format;
    SDL_Texture *texture = plane->texture;

    if (!reuse_texture) {
        texture =
            sdl_plane_info_create_texture(renderer, plane, frame, sdl_format);
        if (!texture)
            return false;
    }

    /* Keep the retained plane state unchanged until the new pixels are known
     * to be uploaded successfully.
     */
    if (SDL_UpdateTexture(texture, NULL, frame->pixels, frame->stride) != 0) {
        fprintf(stderr, "%s(): failed to update %s texture: %s\n", __func__,
                plane_name, SDL_GetError());
        if (!reuse_texture)
            SDL_DestroyTexture(texture);
        return false;
    }

    if (!reuse_texture) {
        if (plane->texture)
            SDL_DestroyTexture(plane->texture);
        plane->texture = texture;
    }
    plane->width = frame->width;
    plane->height = frame->height;
    plane->sdl_format = sdl_format;
    return true;
}

static bool sdl_cursor_rect_update_position(SDL_Rect *rect,
                                            int32_t x,
                                            int32_t y,
                                            uint32_t hot_x,
                                            uint32_t hot_y)
{
    int64_t rect_x = (int64_t) x - (int64_t) hot_x;
    int64_t rect_y = (int64_t) y - (int64_t) hot_y;

    if (rect_x < INT_MIN || rect_x > INT_MAX || rect_y < INT_MIN ||
        rect_y > INT_MAX) {
        fprintf(stderr,
                WINDOW_LOG_PREFIX
                "%s(): cursor position out of SDL range "
                "(x=%" PRId32 " y=%" PRId32 " hot_x=%u hot_y=%u)\n",
                __func__, x, y, (unsigned) hot_x, (unsigned) hot_y);
        return false;
    }

    rect->x = (int) rect_x;
    rect->y = (int) rect_y;
    return true;
}

static bool sdl_scanout_apply_cursor_frame(
    struct sdl_scanout_info *scanout,
    const struct vgpu_display_payload *payload,
    int32_t x,
    int32_t y,
    uint32_t hot_x,
    uint32_t hot_y)
{
    const struct vgpu_display_cpu_payload *frame = &payload->cpu;
    struct sdl_plane_info *plane = &scanout->cursor_plane;
    SDL_Rect new_cursor_rect = scanout->cursor_rect;

    if (frame->width > INT_MAX || frame->height > INT_MAX) {
        fprintf(stderr,
                WINDOW_LOG_PREFIX
                "%s(): cursor size out of SDL range (%ux%u)\n",
                __func__, frame->width, frame->height);
        return false;
    }

    if (!sdl_cursor_rect_update_position(&new_cursor_rect, x, y, hot_x, hot_y))
        return false;

    if (!sdl_plane_info_update_texture(scanout->renderer, plane, payload,
                                       "cursor"))
        return false;

    scanout->cursor_hot_x = hot_x;
    scanout->cursor_hot_y = hot_y;
    new_cursor_rect.w = (int) frame->width;
    new_cursor_rect.h = (int) frame->height;
    scanout->cursor_rect = new_cursor_rect;
    return true;
}

#if SEMU_HAS(VIRGL)
static void sdl_scanout_detach_gl_context(struct sdl_scanout_info *scanout)
{
    (void) scanout;
    int ret = SDL_GL_MakeCurrent(NULL, NULL);
    (void) ret;
}

static bool sdl_scanout_apply_gl_cursor_frame(
    struct sdl_scanout_info *scanout,
    const struct vgpu_display_payload *payload,
    int32_t x,
    int32_t y,
    uint32_t hot_x,
    uint32_t hot_y)
{
    if (!scanout->gl_context || payload->kind != VGPU_DISPLAY_PAYLOAD_CPU)
        return false;

    const struct vgpu_display_cpu_payload *frame = &payload->cpu;
    SDL_Rect new_cursor_rect = scanout->cursor_rect;
    uint32_t src_sdl_format;

    if (frame->width > INT_MAX || frame->height > INT_MAX) {
        fprintf(stderr,
                WINDOW_LOG_PREFIX
                "%s(): cursor size out of GL/SDL range (%ux%u)\n",
                __func__, frame->width, frame->height);
        return false;
    }
    uint64_t required_stride = (uint64_t) frame->width * 4u;
    if (frame->bits_per_pixel != 32 || frame->stride < required_stride ||
        frame->stride % 4u != 0) {
        fprintf(stderr,
                WINDOW_LOG_PREFIX
                "%s(): unsupported cursor layout (%u bpp stride=%u width=%u)\n",
                __func__, frame->bits_per_pixel, frame->stride, frame->width);
        return false;
    }
    if (required_stride > INT_MAX || frame->stride > INT_MAX) {
        fprintf(stderr,
                WINDOW_LOG_PREFIX
                "%s(): cursor pitch out of SDL range (stride=%u row=%" PRIu64
                ")\n",
                __func__, frame->stride, required_stride);
        return false;
    }
    if (!sdl_plane_info_get_sdl_format(&scanout->cursor_plane, payload,
                                       &src_sdl_format))
        return false;
    if (!sdl_cursor_rect_update_position(&new_cursor_rect, x, y, hot_x, hot_y))
        return false;

    uint64_t rgba_size = required_stride * frame->height;
    if (frame->height != 0 && rgba_size / frame->height != required_stride) {
        fprintf(stderr, "%s(): cursor conversion size overflow\n", __func__);
        return false;
    }
    if (rgba_size > SIZE_MAX) {
        fprintf(stderr, "%s(): cursor conversion buffer too large\n", __func__);
        return false;
    }

    void *rgba_pixels = SDL_malloc((size_t) rgba_size);
    if (!rgba_pixels) {
        fprintf(stderr, "%s(): failed to allocate cursor conversion buffer\n",
                __func__);
        return false;
    }

    if (SDL_ConvertPixels((int) frame->width, (int) frame->height,
                          src_sdl_format, frame->pixels, (int) frame->stride,
                          SDL_PIXELFORMAT_RGBA32, rgba_pixels,
                          (int) required_stride) < 0) {
        fprintf(stderr, "%s(): failed to convert cursor pixels: %s\n", __func__,
                SDL_GetError());
        SDL_free(rgba_pixels);
        return false;
    }

    if (SDL_GL_MakeCurrent(scanout->window, scanout->gl_context) < 0) {
        fprintf(stderr, "%s(): failed to make GL context current: %s\n",
                __func__, SDL_GetError());
        SDL_free(rgba_pixels);
        return false;
    }

    if (!scanout->gl_cursor_texture)
        glGenTextures(1, &scanout->gl_cursor_texture);

    glBindTexture(GL_TEXTURE_2D, scanout->gl_cursor_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint) frame->width);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei) frame->width,
                 (GLsizei) frame->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba_pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    SDL_free(rgba_pixels);

    scanout->cursor_hot_x = hot_x;
    scanout->cursor_hot_y = hot_y;
    new_cursor_rect.w = (int) frame->width;
    new_cursor_rect.h = (int) frame->height;
    scanout->cursor_rect = new_cursor_rect;
    scanout->gl_cursor_width = frame->width;
    scanout->gl_cursor_height = frame->height;
    scanout->gl_cursor_valid = true;

    sdl_scanout_detach_gl_context(scanout);
    return true;
}

static bool sdl_scanout_apply_gl_frame(
    struct sdl_scanout_info *scanout,
    const struct vgpu_display_gl_scanout_payload *frame)
{
    if (!scanout->gl_context)
        return false;

    window_debug_set_stage(WINDOW_DEBUG_STAGE_APPLY_GL_FRAME, 0,
                           VGPU_DISPLAY_CMD_PRIMARY_SET);
    if (SDL_GL_MakeCurrent(scanout->window, scanout->gl_context) < 0) {
        fprintf(stderr, "%s(): failed to make GL context current: %s\n",
                __func__, SDL_GetError());
        return false;
    }

    if (!scanout->gl_primary_fb)
        glGenFramebuffers(1, &scanout->gl_primary_fb);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, scanout->gl_primary_fb);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, frame->texture_id, 0);
    if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) !=
        GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "%s(): incomplete VirGL scanout framebuffer\n",
                __func__);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        sdl_scanout_detach_gl_context(scanout);
        return false;
    }

    scanout->gl_primary = *frame;
    scanout->gl_primary_valid = true;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    sdl_scanout_detach_gl_context(scanout);
    return true;
}

static void sdl_scanout_render_gl_cursor(struct sdl_scanout_info *scanout,
                                         int window_width,
                                         int window_height)
{
    if (!scanout->gl_cursor_valid || !scanout->gl_cursor_texture ||
        scanout->cursor_rect.w <= 0 || scanout->cursor_rect.h <= 0)
        return;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, (GLdouble) window_width, (GLdouble) window_height, 0.0, -1.0,
            1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, scanout->gl_cursor_texture);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    GLfloat x0 = (GLfloat) scanout->cursor_rect.x;
    GLfloat y0 = (GLfloat) scanout->cursor_rect.y;
    GLfloat x1 = (GLfloat) (scanout->cursor_rect.x + scanout->cursor_rect.w);
    GLfloat y1 = (GLfloat) (scanout->cursor_rect.y + scanout->cursor_rect.h);

    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f);
    glVertex2f(x0, y0);
    glTexCoord2f(1.0f, 0.0f);
    glVertex2f(x1, y0);
    glTexCoord2f(1.0f, 1.0f);
    glVertex2f(x1, y1);
    glTexCoord2f(0.0f, 1.0f);
    glVertex2f(x0, y1);
    glEnd();

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
}

static void sdl_scanout_render_gl(struct sdl_scanout_info *scanout)
{
    if (!scanout->gl_context)
        return;

    window_debug_set_stage(WINDOW_DEBUG_STAGE_RENDER_GL_MAKE_CURRENT, 0,
                           UINT32_MAX);
    if (SDL_GL_MakeCurrent(scanout->window, scanout->gl_context) < 0)
        return;

    int width = 0, height = 0;
    SDL_GetWindowSize(scanout->window, &width, &height);
    if (width <= 0 || height <= 0) {
        sdl_scanout_detach_gl_context(scanout);
        return;
    }

    glViewport(0, 0, width, height);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (scanout->gl_primary_valid) {
        const struct vgpu_display_gl_scanout_payload *frame =
            &scanout->gl_primary;
        GLint src_x0 = (GLint) frame->src_x;
        GLint src_x1 = (GLint) (frame->src_x + frame->src_w);
        GLint src_y0 = (GLint) frame->src_y;
        GLint src_y1 = (GLint) (frame->src_y + frame->src_h);
        /* Match QEMU's VirGL scanout presentation: Y_0_TOP resources are
         * already scanout-oriented. Normal GL-origin resources need a reversed
         * read rectangle when blitted into the window framebuffer.
         */
        if (!frame->y_0_top) {
            src_y0 = (GLint) (frame->src_y + frame->src_h);
            src_y1 = (GLint) frame->src_y;
        }

        window_debug_set_stage(WINDOW_DEBUG_STAGE_RENDER_GL_BLIT, 0,
                               UINT32_MAX);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, scanout->gl_primary_fb);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(src_x0, src_y0, src_x1, src_y1, 0, 0, width, height,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    }

    sdl_scanout_render_gl_cursor(scanout, width, height);
    window_debug_set_stage(WINDOW_DEBUG_STAGE_RENDER_GL_SWAP, 0, UINT32_MAX);
    SDL_GL_SwapWindow(scanout->window);
    sdl_scanout_detach_gl_context(scanout);
}
#endif

static void sdl_scanout_render(struct sdl_scanout_info *scanout)
{
    __atomic_add_fetch(&window_debug_scanout_renders, 1, __ATOMIC_RELAXED);
    window_debug_set_stage(WINDOW_DEBUG_STAGE_RENDER_SCANOUT, 0, UINT32_MAX);
#if SEMU_HAS(VIRGL)
    if (scanout->gl_context) {
        sdl_scanout_render_gl(scanout);
        return;
    }
#endif
    SDL_RenderClear(scanout->renderer);

    if (scanout->primary_plane.texture)
        SDL_RenderCopy(scanout->renderer, scanout->primary_plane.texture, NULL,
                       NULL);

    if (scanout->cursor_plane.texture)
        SDL_RenderCopy(scanout->renderer, scanout->cursor_plane.texture, NULL,
                       &scanout->cursor_rect);

    SDL_RenderPresent(scanout->renderer);
}

static void window_drain_display_queue(void)
{
    __atomic_add_fetch(&window_debug_display_drains, 1, __ATOMIC_RELAXED);
    window_debug_set_stage(WINDOW_DEBUG_STAGE_DRAIN_DISPLAY, 0, UINT32_MAX);

    bool dirty_scanouts[VIRTIO_GPU_MAX_SCANOUTS] = {0};
    bool cursor_dirty_scanouts[VIRTIO_GPU_MAX_SCANOUTS] = {0};
    struct vgpu_display_cmd cmd;

    /* Drain display bridge commands, update only SDL-owned state, then render
     * each affected scanout once. The bridge publishes reliable clear
     * generations and filters stale lossy frame/move queue entries.
     */
    while (vgpu_display_pop_cmd(&cmd)) {
        __atomic_add_fetch(&window_debug_display_cmds, 1, __ATOMIC_RELAXED);
        window_debug_set_stage(WINDOW_DEBUG_STAGE_APPLY_DISPLAY_CMD,
                               cmd.scanout_id, (uint32_t) cmd.type);
        /* 'scanout_id' was validated by the guest-facing backend before the
         * command entered the display bridge.
         */
        struct sdl_scanout_info *scanout = &sdl_scanouts[cmd.scanout_id];
        if (!sdl_scanout_is_ready(scanout)) {
            vgpu_display_release_cmd(&cmd);
            continue;
        }

        switch (cmd.type) {
        case VGPU_DISPLAY_CMD_PRIMARY_CLEAR:
            sdl_scanout_clear_primary(scanout);
            dirty_scanouts[cmd.scanout_id] = true;
            break;
        case VGPU_DISPLAY_CMD_CURSOR_CLEAR:
            sdl_scanout_clear_cursor(scanout);
            cursor_dirty_scanouts[cmd.scanout_id] = true;
            break;
        case VGPU_DISPLAY_CMD_PRIMARY_SET:
            /* Use '|=' to keep earlier dirty state for this scanout. A failed
             * upload leaves the old texture visible and does not dirty the
             * scanout by itself.
             */
            if (cmd.u.primary_set.payload->kind == VGPU_DISPLAY_PAYLOAD_CPU) {
#if SEMU_HAS(VIRGL)
                if (scanout->gl_context) {
                    scanout->gl_primary_valid = false;
                    dirty_scanouts[cmd.scanout_id] = true;
                    break;
                }
#endif
                dirty_scanouts[cmd.scanout_id] |= sdl_plane_info_update_texture(
                    scanout->renderer, &scanout->primary_plane,
                    cmd.u.primary_set.payload, "primary");
#if SEMU_HAS(VIRGL)
            } else if (cmd.u.primary_set.payload->kind ==
                       VGPU_DISPLAY_PAYLOAD_GL_SCANOUT) {
                dirty_scanouts[cmd.scanout_id] |= sdl_scanout_apply_gl_frame(
                    scanout, &cmd.u.primary_set.payload->gl);
#endif
            }
            break;
        case VGPU_DISPLAY_CMD_CURSOR_SET:
            /* Use '|=' to keep earlier dirty state for this scanout. A failed
             * upload leaves the old cursor visible and does not dirty the
             * scanout by itself.
             */
#if SEMU_HAS(VIRGL)
            if (scanout->gl_context) {
                cursor_dirty_scanouts[cmd.scanout_id] |=
                    sdl_scanout_apply_gl_cursor_frame(
                        scanout, cmd.u.cursor_set.payload, cmd.u.cursor_set.x,
                        cmd.u.cursor_set.y, cmd.u.cursor_set.hot_x,
                        cmd.u.cursor_set.hot_y);
                break;
            }
#endif
            cursor_dirty_scanouts[cmd.scanout_id] |=
                sdl_scanout_apply_cursor_frame(
                    scanout, cmd.u.cursor_set.payload, cmd.u.cursor_set.x,
                    cmd.u.cursor_set.y, cmd.u.cursor_set.hot_x,
                    cmd.u.cursor_set.hot_y);
            break;
        case VGPU_DISPLAY_CMD_CURSOR_MOVE: {
            int old_cursor_x = scanout->cursor_rect.x;
            int old_cursor_y = scanout->cursor_rect.y;
            if (!sdl_cursor_rect_update_position(
                    &scanout->cursor_rect, cmd.u.cursor_move.x,
                    cmd.u.cursor_move.y, scanout->cursor_hot_x,
                    scanout->cursor_hot_y))
                break;
            if (old_cursor_x == scanout->cursor_rect.x &&
                old_cursor_y == scanout->cursor_rect.y)
                break;
#if SEMU_HAS(VIRGL)
            if (scanout->gl_context && !scanout->gl_cursor_valid)
                break;
#endif
            cursor_dirty_scanouts[cmd.scanout_id] = true;
            break;
        }
        }

        vgpu_display_release_cmd(&cmd);
    }

    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        if ((!dirty_scanouts[i] && !cursor_dirty_scanouts[i]) ||
            !sdl_scanout_is_ready(&sdl_scanouts[i]))
            continue;
        sdl_scanout_render(&sdl_scanouts[i]);
    }
}
#endif

#if SEMU_HAS(VIRGL)
static void window_drain_renderer_queue(void)
{
    __atomic_add_fetch(&window_debug_renderer_drains, 1, __ATOMIC_RELAXED);
    window_debug_set_stage(WINDOW_DEBUG_STAGE_DRAIN_RENDERER, 0, UINT32_MAX);

    struct vgpu_renderer_request request;
    while (vgpu_renderer_pop_request(&request)) {
        __atomic_add_fetch(&window_debug_renderer_requests, 1,
                           __ATOMIC_RELAXED);
        window_debug_set_stage(WINDOW_DEBUG_STAGE_EXEC_RENDERER, 0, UINT32_MAX);
        vgpu_renderer_debug_note_execute_begin(&request);
        vgpu_virgl_execute_renderer_request(&request);
        vgpu_renderer_debug_note_execute_end();
    }
}
#endif

/* Main loop runs on the main thread */
static void window_main_loop_sw(void)
{
    if (headless_mode) {
        /* Block until the emulator calls 'window_shutdown_sw()', so 'main()'
         * can proceed to 'pthread_join()' rather than stopping the emulator
         * immediately. There is no SDL event loop in this mode, so the main
         * thread just polls the shared close flag.
         */
        while (!window_is_closed_sw())
            usleep(10000);
        return;
    }

    /* relaxed ordering is sufficient: the only consequence of reading a stale
     * false is a few extra loop iterations. Ordering with the emulator thread
     * is provided by 'pthread_join()', not by this flag.
     */
    while (!window_is_closed_sw()) {
#if SEMU_HAS(VIRTIOINPUT)
        window_debug_set_stage(WINDOW_DEBUG_STAGE_HANDLE_EVENTS, 0, UINT32_MAX);
        if (vinput_handle_events()) {
            /* User closed the window. Set the flag so 'window_shutdown_sw()'
             * (called from the emulator thread) does not race with us, then
             * return normally so 'main()' can 'pthread_join()' the emulator
             * thread and collect its exit code.
             */
            window_shutdown_sw();
            return;
        }
        __atomic_add_fetch(&window_debug_sdl_events, 1, __ATOMIC_RELAXED);
#else
        SDL_Event e;
        /* Without 'virtio-input', there is no SDL event pump to wake on display
         * commands. Use a short timeout so 'VIRTIOGPU'-only builds periodically
         * drain the display bridge; a future SDL user-event bridge could make
         * this fully event-driven.
         */
        window_debug_set_stage(WINDOW_DEBUG_STAGE_HANDLE_EVENTS, 0, UINT32_MAX);
        if (SDL_WaitEventTimeout(&e, SDL_EVENT_WAIT_TIMEOUT_MS)) {
            uint32_t processed = 0;
            do {
                if (e.type == SDL_QUIT) {
                    window_shutdown_sw();
                    return;
                }
                processed++;
            } while (processed < SDL_EVENT_BURST_LIMIT && SDL_PollEvent(&e));
            __atomic_add_fetch(&window_debug_sdl_events, processed,
                               __ATOMIC_RELAXED);
        }
#endif

#if SEMU_HAS(VIRGL)
        window_drain_renderer_queue();
#endif
#if SEMU_HAS(VIRTIOGPU)
        window_drain_display_queue();
#endif
        window_debug_set_stage(WINDOW_DEBUG_STAGE_IDLE, 0, UINT32_MAX);
    }
}

static void window_init_sw(bool headless, uint32_t width, uint32_t height)
{
#if SEMU_HAS(VIRGL)
    vgpu_renderer_set_wake_backend(window_wake_backend_sw);
#endif

    if (headless) {
        headless_mode = true;
#if SEMU_HAS(VIRTIOGPU)
        vgpu_display_set_unavailable();
#endif
        return;
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr,
                "window_init_sw(): failed to initialize SDL: %s\n"
                "Running in headless mode.\n",
                SDL_GetError());
        headless_mode = true;
#if SEMU_HAS(VIRTIOGPU)
        vgpu_display_set_unavailable();
#endif
        return;
    }
    sdl_initialized = true;
#if SEMU_HAS(VIRGL)
    vgpu_renderer_set_wake_frontend(window_wake_frontend_sw);
#endif

#if SEMU_HAS(VIRTIOGPU)
    /* The current machine setup registers exactly one scanout before calling
     * 'window_init_sw()', so materialize scanout 0 directly here. If semu grows
     * multiple scanouts later, this can be extended to iterate all registered
     * scanouts or restored to an explicit per-scanout setup path.
     */
    struct sdl_scanout_info *scanout = &sdl_scanouts[0];
#if SEMU_HAS(VIRGL)
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
    scanout->window = SDL_CreateWindow("semu", SDL_WINDOWPOS_UNDEFINED,
                                       SDL_WINDOWPOS_UNDEFINED, width, height,
                                       SDL_WINDOW_SHOWN
#if SEMU_HAS(VIRGL)
                                           | SDL_WINDOW_OPENGL
#endif
    );
    if (!scanout->window) {
        fprintf(stderr,
                "window_init_sw(): failed to create SDL window for display "
                "0: %s\n"
                "Running in headless mode.\n",
                SDL_GetError());
        headless_mode = true;
        SDL_Quit();
        sdl_initialized = false;
        vgpu_display_set_unavailable();
        return;
    }

#if SEMU_HAS(VIRGL)
    scanout->gl_context = SDL_GL_CreateContext(scanout->window);
    if (!scanout->gl_context ||
        SDL_GL_MakeCurrent(scanout->window, scanout->gl_context) < 0) {
        fprintf(stderr,
                "window_init_sw(): failed to create GL context for display "
                "0: %s\n"
                "Running in headless mode.\n",
                SDL_GetError());
        SDL_DestroyWindow(scanout->window);
        scanout->window = NULL;
        headless_mode = true;
        SDL_Quit();
        sdl_initialized = false;
        vgpu_display_set_unavailable();
        return;
    }
#else
    scanout->renderer =
        SDL_CreateRenderer(scanout->window, -1, SDL_RENDERER_ACCELERATED);
    if (!scanout->renderer) {
        fprintf(stderr,
                "window_init_sw(): accelerated renderer not available, "
                "trying software renderer: %s\n",
                SDL_GetError());
        scanout->renderer =
            SDL_CreateRenderer(scanout->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!scanout->renderer) {
        fprintf(stderr,
                "window_init_sw(): failed to create renderer for display "
                "0: %s\n"
                "Running in headless mode.\n",
                SDL_GetError());
        SDL_DestroyWindow(scanout->window);
        scanout->window = NULL;
        headless_mode = true;
        SDL_Quit();
        sdl_initialized = false;
        vgpu_display_set_unavailable();
        return;
    }
#endif

    scanout->window_width = width;
    scanout->window_height = height;
    scanout->cursor_plane.alpha_blend = true;

#if SEMU_HAS(VIRTIOINPUT)
    if (!sdl_input_window)
        sdl_input_window = scanout->window;
#endif

#if SEMU_HAS(VIRGL)
    glViewport(0, 0, (GLsizei) width, (GLsizei) height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(scanout->window);
    sdl_scanout_detach_gl_context(scanout);
#else
    SDL_SetRenderDrawColor(scanout->renderer, 0, 0, 0, 255);
    SDL_RenderClear(scanout->renderer);
    SDL_RenderPresent(scanout->renderer);
#endif
    window_debug_progress_start();
#else /* !SEMU_HAS(VIRTIOGPU) */
    sdl_input_window = SDL_CreateWindow("semu", SDL_WINDOWPOS_UNDEFINED,
                                        SDL_WINDOWPOS_UNDEFINED, width, height,
                                        SDL_WINDOW_SHOWN);
    if (!sdl_input_window) {
        fprintf(stderr,
                "window_init_sw(): failed to create SDL window: %s\n"
                "Running in headless mode.\n",
                SDL_GetError());
        headless_mode = true;
        SDL_Quit();
        sdl_initialized = false;
        return;
    }
#endif
}

static void window_cleanup_sw(void)
{
#if SEMU_HAS(VIRTIOGPU)
    window_debug_progress_stop_monitor();
#endif

#if SEMU_HAS(VIRGL)
    vgpu_renderer_set_wake_frontend(NULL);
    vgpu_renderer_set_wake_backend(NULL);
#endif

#if SEMU_HAS(VIRTIOINPUT)
    if (sdl_initialized)
        window_set_mouse_grab_sw(false);
    /* Keep cleanup idempotent when SDL was never initialized or grab release
     * returned early.
     */
    mouse_grabbed = false;
#endif

    wake_write_fd = -1;

#if SEMU_HAS(VIRTIOGPU)
    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++)
        sdl_scanout_info_cleanup(&sdl_scanouts[i]);

    struct vgpu_display_cmd cmd;
    while (vgpu_display_pop_cmd(&cmd))
        vgpu_display_release_cmd(&cmd);
#elif SEMU_HAS(VIRTIOINPUT)
    if (sdl_input_window)
        SDL_DestroyWindow(sdl_input_window);
#endif

#if SEMU_HAS(VIRTIOINPUT)
    sdl_input_window = NULL;
#endif

    if (sdl_initialized) {
        SDL_Quit();
        sdl_initialized = false;
    }

    /* Cleanup normally runs before process exit. Reset frontend flags anyway
     * so a future re-init path cannot inherit stale headless/shutdown state.
     */
    headless_mode = false;
    should_exit = false;
}

#if SEMU_HAS(VIRGL)
virgl_renderer_gl_context vgpu_window_virgl_create_context(
    int scanout_idx,
    struct virgl_renderer_gl_ctx_param *param)
{
    if (scanout_idx < 0 || scanout_idx >= VIRTIO_GPU_MAX_SCANOUTS)
        return NULL;

    struct sdl_scanout_info *scanout = &sdl_scanouts[scanout_idx];
    if (!scanout->window || !scanout->gl_context)
        return NULL;

    if (SDL_GL_MakeCurrent(scanout->window, scanout->gl_context) < 0)
        return NULL;

    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    if (param) {
        if (param->major_ver)
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, param->major_ver);
        if (param->minor_ver)
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, param->minor_ver);
        if (param->compat_ctx)
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                                SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    }

    virgl_renderer_gl_context ctx = SDL_GL_CreateContext(scanout->window);
    sdl_scanout_detach_gl_context(scanout);
    return ctx;
}

void vgpu_window_virgl_destroy_context(virgl_renderer_gl_context ctx)
{
    if (ctx)
        SDL_GL_DeleteContext(ctx);
}

int vgpu_window_virgl_make_current(int scanout_idx,
                                   virgl_renderer_gl_context ctx)
{
    if (scanout_idx < 0 || scanout_idx >= VIRTIO_GPU_MAX_SCANOUTS)
        return -1;

    struct sdl_scanout_info *scanout = &sdl_scanouts[scanout_idx];
    if (!scanout->window)
        return -1;

    return SDL_GL_MakeCurrent(scanout->window, ctx);
}
#endif

const struct window_backend g_window = {
    .window_init = window_init_sw,
    .window_main_loop = window_main_loop_sw,
    .window_shutdown = window_shutdown_sw,
    .window_cleanup = window_cleanup_sw,
    .window_is_closed = window_is_closed_sw,
    .window_set_wake_fd = window_set_wake_fd_sw,
    .window_wake_backend = window_wake_backend_sw,
    .window_wake_frontend = window_wake_frontend_sw,
#if SEMU_HAS(VIRTIOINPUT)
    .window_set_mouse_grab = window_set_mouse_grab_sw,
    .window_is_mouse_grabbed = window_is_mouse_grabbed_sw,
#endif
};
