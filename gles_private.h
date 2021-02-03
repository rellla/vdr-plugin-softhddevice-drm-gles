#ifndef __GLES_PRIVATE_H
#define __GLES_PRIVATE_H

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglplatform.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <drm_fourcc.h>

/* Hack:
 * xlib.h via eglplatform.h: #define Status int
 * X.h via eglplatform.h: #define CurrentTime 0L
 *
 * revert it, because it conflicts with vdr variables.
 */
#undef Status
#undef CurrentTime

#ifdef __cplusplus
extern "C" {
#endif

//typedef char GLchar;

struct gbm {
    struct gbm_device *dev;
    struct gbm_bo *bo;
    struct gbm_surface *surface;
    uint32_t format;
    uint32_t flags;
    int width, height;
    EGLImage img;
    uint32_t gem_handle;
    uint32_t pitch;
    uint32_t offset;
    int dma_buf_fd;
};

void eglCheckError(const char *stmt, const char *fname, int line);
void glCheckError(const char *stmt, const char *fname, int line);
void eglAcquireContext(void);
void eglReleaseContext(void);
#ifdef WRITE_PNG
int writeImage(char* filename, int width, int height, void *buffer, char* title);
#endif

#ifdef GL_DEBUG
#define GL_CHECK(stmt) do { \
    stmt; \
    glCheckError(#stmt, __FILE__, __LINE__); \
    } while (0)
#else
#define GL_CHECK(stmt) stmt
#endif

#ifdef GL_DEBUG
#define EGL_CHECK(stmt) do { \
    stmt; \
    eglCheckError(#stmt, __FILE__, __LINE__); \
    } while (0)
#else
#define EGL_CHECK(stmt) stmt
#endif


#ifdef __cplusplus
}
#endif

#endif
