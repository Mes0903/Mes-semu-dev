#pragma once

#if !SEMU_HAS(VIRGL)
#error Only valid when VirGL is enabled.
#endif

#include <virglrenderer.h>

virgl_renderer_gl_context vgpu_window_virgl_create_context(
    int scanout_idx,
    struct virgl_renderer_gl_ctx_param *param);
void vgpu_window_virgl_destroy_context(virgl_renderer_gl_context ctx);
int vgpu_window_virgl_make_current(int scanout_idx,
                                   virgl_renderer_gl_context ctx);
