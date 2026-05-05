#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TMP_DIR="$(mktemp -d)"
cleanup()
{
    rm -rf "${TMP_DIR}"
    rm -f "${REPO_ROOT}/virtio-gpu-virgl.o" \
        "${REPO_ROOT}/.virtio-gpu-virgl.o.d" \
        "${REPO_ROOT}/window-sw.o" \
        "${REPO_ROOT}/.window-sw.o.d"
}
trap cleanup EXIT

mkdir -p "${TMP_DIR}/include/epoxy" "${TMP_DIR}/lib"

cp "${REPO_ROOT}/tests/fakes/virglrenderer.h" \
    "${TMP_DIR}/include/virglrenderer.h"

cat >"${TMP_DIR}/include/epoxy/gl.h" <<'HEADER'
#pragma once

#include <stdint.h>

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLbitfield;
typedef float GLfloat;
typedef double GLdouble;

#define GL_READ_FRAMEBUFFER 0x8ca8
#define GL_DRAW_FRAMEBUFFER 0x8ca9
#define GL_FRAMEBUFFER_COMPLETE 0x8cd5
#define GL_COLOR_ATTACHMENT0 0x8ce0
#define GL_TEXTURE_2D 0x0de1
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_RGBA8 0x8058
#define GL_RGBA 0x1908
#define GL_BGRA 0x80e1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812f
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_UNPACK_ALIGNMENT 0x0cf5
#define GL_UNPACK_ROW_LENGTH 0x0cf2
#define GL_BLEND 0x0be2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_PROJECTION 0x1701
#define GL_MODELVIEW 0x1700
#define GL_QUADS 0x0007

void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers);
void glGenFramebuffers(GLsizei n, GLuint *framebuffers);
void glDeleteTextures(GLsizei n, const GLuint *textures);
void glGenTextures(GLsizei n, GLuint *textures);
void glBindFramebuffer(GLenum target, GLuint framebuffer);
void glFramebufferTexture2D(GLenum target,
                            GLenum attachment,
                            GLenum textarget,
                            GLuint texture,
                            GLint level);
GLenum glCheckFramebufferStatus(GLenum target);
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void glClear(GLbitfield mask);
void glBindTexture(GLenum target, GLuint texture);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glPixelStorei(GLenum pname, GLint param);
void glTexImage2D(GLenum target,
                  GLint level,
                  GLint internalformat,
                  GLsizei width,
                  GLsizei height,
                  GLint border,
                  GLenum format,
                  GLenum type,
                  const void *pixels);
void glBlitFramebuffer(GLint srcX0,
                       GLint srcY0,
                       GLint srcX1,
                       GLint srcY1,
                       GLint dstX0,
                       GLint dstY0,
                       GLint dstX1,
                       GLint dstY1,
                       GLbitfield mask,
                       GLenum filter);
void glEnable(GLenum cap);
void glDisable(GLenum cap);
void glBlendFunc(GLenum sfactor, GLenum dfactor);
void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glOrtho(GLdouble left,
             GLdouble right,
             GLdouble bottom,
             GLdouble top,
             GLdouble near_val,
             GLdouble far_val);
void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void glBegin(GLenum mode);
void glTexCoord2f(GLfloat s, GLfloat t);
void glVertex2f(GLfloat x, GLfloat y);
void glEnd(void);
HEADER

cat >"${TMP_DIR}/virglrenderer-stub.c" <<'SOURCE'
#include <epoxy/gl.h>
#include "virglrenderer.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers)
{
    (void) n;
    (void) framebuffers;
}

void glGenFramebuffers(GLsizei n, GLuint *framebuffers)
{
    for (GLsizei i = 0; i < n; i++)
        framebuffers[i] = (GLuint) (i + 1);
}

void glDeleteTextures(GLsizei n, const GLuint *textures)
{
    (void) n;
    (void) textures;
}

void glGenTextures(GLsizei n, GLuint *textures)
{
    for (GLsizei i = 0; i < n; i++)
        textures[i] = (GLuint) (i + 1);
}

void glBindFramebuffer(GLenum target, GLuint framebuffer)
{
    (void) target;
    (void) framebuffer;
}

void glFramebufferTexture2D(GLenum target,
                            GLenum attachment,
                            GLenum textarget,
                            GLuint texture,
                            GLint level)
{
    (void) target;
    (void) attachment;
    (void) textarget;
    (void) texture;
    (void) level;
}

GLenum glCheckFramebufferStatus(GLenum target)
{
    (void) target;
    return GL_FRAMEBUFFER_COMPLETE;
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    (void) x;
    (void) y;
    (void) width;
    (void) height;
}

void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
    (void) red;
    (void) green;
    (void) blue;
    (void) alpha;
}

void glClear(GLbitfield mask)
{
    (void) mask;
}

void glBindTexture(GLenum target, GLuint texture)
{
    (void) target;
    (void) texture;
}

void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    (void) target;
    (void) pname;
    (void) param;
}

void glPixelStorei(GLenum pname, GLint param)
{
    (void) pname;
    (void) param;
}

void glTexImage2D(GLenum target,
                  GLint level,
                  GLint internalformat,
                  GLsizei width,
                  GLsizei height,
                  GLint border,
                  GLenum format,
                  GLenum type,
                  const void *pixels)
{
    (void) target;
    (void) level;
    (void) internalformat;
    (void) width;
    (void) height;
    (void) border;
    (void) format;
    (void) type;
    (void) pixels;
}

void glBlitFramebuffer(GLint srcX0,
                       GLint srcY0,
                       GLint srcX1,
                       GLint srcY1,
                       GLint dstX0,
                       GLint dstY0,
                       GLint dstX1,
                       GLint dstY1,
                       GLbitfield mask,
                       GLenum filter)
{
    (void) srcX0;
    (void) srcY0;
    (void) srcX1;
    (void) srcY1;
    (void) dstX0;
    (void) dstY0;
    (void) dstX1;
    (void) dstY1;
    (void) mask;
    (void) filter;
}

void glEnable(GLenum cap)
{
    (void) cap;
}

void glDisable(GLenum cap)
{
    (void) cap;
}

void glBlendFunc(GLenum sfactor, GLenum dfactor)
{
    (void) sfactor;
    (void) dfactor;
}

void glMatrixMode(GLenum mode)
{
    (void) mode;
}

void glLoadIdentity(void)
{
}

void glOrtho(GLdouble left,
             GLdouble right,
             GLdouble bottom,
             GLdouble top,
             GLdouble near_val,
             GLdouble far_val)
{
    (void) left;
    (void) right;
    (void) bottom;
    (void) top;
    (void) near_val;
    (void) far_val;
}

void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
    (void) red;
    (void) green;
    (void) blue;
    (void) alpha;
}

void glBegin(GLenum mode)
{
    (void) mode;
}

void glTexCoord2f(GLfloat s, GLfloat t)
{
    (void) s;
    (void) t;
}

void glVertex2f(GLfloat x, GLfloat y)
{
    (void) x;
    (void) y;
}

void glEnd(void)
{
}

int virgl_renderer_init(void *cookie,
                        int flags,
                        struct virgl_renderer_callbacks *cb)
{
    (void) cookie;
    (void) flags;
    (void) cb;
    return 0;
}

void virgl_renderer_poll(void)
{
}

int virgl_renderer_create_fence(int client_fence_id, uint32_t ctx_id)
{
    (void) client_fence_id;
    (void) ctx_id;
    return 0;
}

int virgl_renderer_context_create_fence(uint32_t ctx_id,
                                        uint32_t flags,
                                        uint32_t ring_idx,
                                        uint64_t fence_id)
{
    (void) ctx_id;
    (void) flags;
    (void) ring_idx;
    (void) fence_id;
    return 0;
}

void virgl_renderer_get_cap_set(uint32_t set,
                                uint32_t *max_ver,
                                uint32_t *max_size)
{
    if (set == 1) {
        *max_ver = 1;
        *max_size = 64;
    } else if (set == 2) {
        *max_ver = 2;
        *max_size = 128;
    } else {
        *max_ver = 0;
        *max_size = 0;
    }
}

void virgl_renderer_fill_caps(uint32_t set, uint32_t version, void *caps)
{
    memset(caps, (int) (0xa0u + set + version), set == 2 ? 128 : 64);
}

void virgl_renderer_reset(void)
{
}

int virgl_renderer_context_create(uint32_t handle,
                                  uint32_t nlen,
                                  const char *name)
{
    (void) handle;
    (void) nlen;
    (void) name;
    return 0;
}

void virgl_renderer_context_destroy(uint32_t handle)
{
    (void) handle;
}

int virgl_renderer_context_create_with_flags(uint32_t ctx_id,
                                             uint32_t ctx_flags,
                                             uint32_t nlen,
                                             const char *name)
{
    (void) ctx_id;
    (void) ctx_flags;
    (void) nlen;
    (void) name;
    return 0;
}

void virgl_renderer_ctx_attach_resource(int ctx_id, int res_handle)
{
    (void) ctx_id;
    (void) res_handle;
}

void virgl_renderer_ctx_detach_resource(int ctx_id, int res_handle)
{
    (void) ctx_id;
    (void) res_handle;
}

int virgl_renderer_resource_create(
    struct virgl_renderer_resource_create_args *args,
    struct iovec *iov,
    uint32_t num_iovs)
{
    (void) args;
    (void) iov;
    (void) num_iovs;
    return 0;
}

void virgl_renderer_resource_unref(uint32_t res_handle)
{
    (void) res_handle;
}

int virgl_renderer_resource_get_info(int res_handle,
                                     struct virgl_renderer_resource_info *info)
{
    (void) res_handle;
    memset(info, 0, sizeof(*info));
    info->width = 1024;
    info->height = 768;
    info->tex_id = 1;
    return 0;
}

int virgl_renderer_resource_attach_iov(int res_handle,
                                       struct iovec *iov,
                                       int num_iovs)
{
    (void) res_handle;
    (void) iov;
    (void) num_iovs;
    return 0;
}

void virgl_renderer_resource_detach_iov(int res_handle,
                                        struct iovec **iov,
                                        int *num_iovs)
{
    (void) res_handle;
    *iov = NULL;
    *num_iovs = 0;
}

int virgl_renderer_transfer_write_iov(uint32_t handle,
                                      uint32_t ctx_id,
                                      int level,
                                      uint32_t stride,
                                      uint32_t layer_stride,
                                      struct virgl_box *box,
                                      uint64_t offset,
                                      struct iovec *iovec,
                                      unsigned int iovec_cnt)
{
    (void) handle;
    (void) ctx_id;
    (void) level;
    (void) stride;
    (void) layer_stride;
    (void) box;
    (void) offset;
    (void) iovec;
    (void) iovec_cnt;
    return 0;
}

int virgl_renderer_transfer_read_iov(uint32_t handle,
                                     uint32_t ctx_id,
                                     uint32_t level,
                                     uint32_t stride,
                                     uint32_t layer_stride,
                                     struct virgl_box *box,
                                     uint64_t offset,
                                     struct iovec *iov,
                                     int iovec_cnt)
{
    (void) handle;
    (void) ctx_id;
    (void) level;
    (void) stride;
    (void) layer_stride;
    (void) box;
    (void) offset;
    (void) iov;
    (void) iovec_cnt;
    return 0;
}

int virgl_renderer_submit_cmd(void *buffer, int ctx_id, int ndw)
{
    (void) buffer;
    (void) ctx_id;
    (void) ndw;
    return 0;
}

void virgl_renderer_force_ctx_0(void)
{
}
SOURCE

"${CC:-cc}" -c "${TMP_DIR}/virglrenderer-stub.c" \
    -I"${TMP_DIR}/include" \
    -o "${TMP_DIR}/virglrenderer-stub.o"
ar rcs "${TMP_DIR}/lib/libvirglrenderer.a" \
    "${TMP_DIR}/virglrenderer-stub.o"

fake_pkg_config="${TMP_DIR}/pkg-config"
cat >"${fake_pkg_config}" <<SCRIPT
#!/usr/bin/env bash
case "\$1" in
    --exists)
        exit 0
        ;;
    --cflags)
        printf '%s\n' '-I${TMP_DIR}/include'
        ;;
    --libs)
        printf '%s\n' '${TMP_DIR}/lib/libvirglrenderer.a'
        ;;
    *)
        exit 2
        ;;
esac
SCRIPT
chmod +x "${fake_pkg_config}"

rm -f "${REPO_ROOT}/virtio-gpu-virgl.o" \
    "${REPO_ROOT}/.virtio-gpu-virgl.o.d" \
    "${REPO_ROOT}/window-sw.o" \
    "${REPO_ROOT}/.window-sw.o.d"
make -s -C "${REPO_ROOT}" ENABLE_VIRGL=1 PKG_CONFIG="${fake_pkg_config}" semu
strings -a "${REPO_ROOT}/semu" >"${TMP_DIR}/semu.strings"
grep -q 'SEMU_VIRGL_BACKEND_LINKED' "${TMP_DIR}/semu.strings"
