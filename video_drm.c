///
///	@file video.c	@brief Video module
///
///	Copyright (c) 2009 - 2015 by Johns.  All Rights Reserved.
///	Copyright (c) 2018 by zille.  All Rights Reserved.
///
///	Contributor(s):
///
///	License: AGPLv3
///
///	This program is free software: you can redistribute it and/or modify
///	it under the terms of the GNU Affero General Public License as
///	published by the Free Software Foundation, either version 3 of the
///	License.
///
///	This program is distributed in the hope that it will be useful,
///	but WITHOUT ANY WARRANTY; without even the implied warranty of
///	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
///	GNU Affero General Public License for more details.
///
///	$Id$
//////////////////////////////////////////////////////////////////////////////

///
///	@defgroup Video The video module.
///
///	This module contains all video rendering functions.
///

#ifndef __USE_GNU
#define __USE_GNU
#endif

#include <stdbool.h>
#include <unistd.h>

#include <inttypes.h>

#include <libintl.h>
#define _(str) gettext(str)		///< gettext shortcut
#define _N(str) str			///< gettext_noop shortcut

#ifdef USE_GLES
#include <assert.h>
#endif
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
//#include <sys/utsname.h>
#include <drm_fourcc.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
//#include <libavutil/time.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>

#ifdef USE_GLES
#include <gbm.h>
#include "gles_private.h"
#endif

#include "misc.h"
#include "video.h"
#include "audio.h"

//----------------------------------------------------------------------------
//	Variables
//----------------------------------------------------------------------------
int VideoAudioDelay;

static pthread_cond_t PauseCondition;
static pthread_mutex_t PauseMutex;

static pthread_cond_t WaitCleanCondition;
static pthread_mutex_t WaitCleanMutex;

static pthread_t DecodeThread;		///< video decode thread

static pthread_t DisplayThread;

static pthread_t FilterThread;

//----------------------------------------------------------------------------
//	Helper functions
//----------------------------------------------------------------------------

static void ReleaseFrame( __attribute__ ((unused)) void *opaque, uint8_t *data)
{
	AVDRMFrameDescriptor *primedata = (AVDRMFrameDescriptor *)data;

	av_free(primedata);
}

static void ThreadExitHandler( __attribute__ ((unused)) void * arg)
{
	FilterThread = 0;
}

int GetPropertyValue(int fd_drm, uint32_t objectID,
		     uint32_t objectType, const char *propName, uint64_t *value)
{
	uint32_t i;
	int found = 0;
	drmModePropertyPtr Prop;
	drmModeObjectPropertiesPtr objectProps =
		drmModeObjectGetProperties(fd_drm, objectID, objectType);

	for (i = 0; i < objectProps->count_props; i++) {
		if ((Prop = drmModeGetProperty(fd_drm, objectProps->props[i])) == NULL)
			fprintf(stderr, "GetPropertyValue: Unable to query property.\n");

		if (strcmp(propName, Prop->name) == 0) {
			*value = objectProps->prop_values[i];
			found = 1;
		}

		drmModeFreeProperty(Prop);

		if (found)
			break;
	}

	drmModeFreeObjectProperties(objectProps);

	if (!found) {
#ifdef DRM_DEBUG
		fprintf(stderr, "GetPropertyValue: Unable to find value for property \'%s\'.\n",
			propName);
#endif
		return -1;
	}

	return 0;
}

static int SetPropertyRequest(drmModeAtomicReqPtr ModeReq, int fd_drm,
					uint32_t objectID, uint32_t objectType,
					const char *propName, uint64_t value)
{
	uint32_t i;
	uint64_t id = 0;
	drmModePropertyPtr Prop;
	drmModeObjectPropertiesPtr objectProps =
		drmModeObjectGetProperties(fd_drm, objectID, objectType);

	for (i = 0; i < objectProps->count_props; i++) {
		if ((Prop = drmModeGetProperty(fd_drm, objectProps->props[i])) == NULL)
			fprintf(stderr, "SetPropertyRequest: Unable to query property.\n");

		if (strcmp(propName, Prop->name) == 0) {
			id = Prop->prop_id;
			drmModeFreeProperty(Prop);
			break;
		}

		drmModeFreeProperty(Prop);
	}

	drmModeFreeObjectProperties(objectProps);

	if (id == 0)
		fprintf(stderr, "SetPropertyRequest: Unable to find value for property \'%s\'.\n",
			propName);

	return drmModeAtomicAddProperty(ModeReq, objectID, id, value);
}

void SetPlaneFbId(VideoRender * render, drmModeAtomicReqPtr ModeReq, uint32_t plane_id, uint64_t fb_id)
{
	SetPropertyRequest(ModeReq, render->fd_drm, plane_id,
					DRM_MODE_OBJECT_PLANE, "FB_ID", fb_id);
}

void SetPlaneCrtcId(VideoRender * render, drmModeAtomicReqPtr ModeReq, uint32_t plane_id, uint64_t crtc_id)
{
	SetPropertyRequest(ModeReq, render->fd_drm, plane_id,
				DRM_MODE_OBJECT_PLANE, "CRTC_ID", crtc_id);
}

void SetPlaneCrtc(VideoRender * render, drmModeAtomicReqPtr ModeReq, uint32_t plane_id,
		  int crtc_x, int crtc_y, int crtc_w, int crtc_h)
{
	SetPropertyRequest(ModeReq, render->fd_drm, plane_id,
						DRM_MODE_OBJECT_PLANE, "CRTC_X", crtc_x);
	SetPropertyRequest(ModeReq, render->fd_drm, plane_id,
						DRM_MODE_OBJECT_PLANE, "CRTC_Y", crtc_y);
	SetPropertyRequest(ModeReq, render->fd_drm, plane_id,
						DRM_MODE_OBJECT_PLANE, "CRTC_W", crtc_w);
	SetPropertyRequest(ModeReq, render->fd_drm, plane_id,
						DRM_MODE_OBJECT_PLANE, "CRTC_H", crtc_h);
}

void SetPlaneSrc(VideoRender * render, drmModeAtomicReqPtr ModeReq, uint32_t plane_id,
		 int src_x, int src_y, int src_w, int src_h)
{
	SetPropertyRequest(ModeReq, render->fd_drm, plane_id,
						DRM_MODE_OBJECT_PLANE, "SRC_X", src_x);
	SetPropertyRequest(ModeReq, render->fd_drm, plane_id,
						DRM_MODE_OBJECT_PLANE, "SRC_Y", src_y);
	SetPropertyRequest(ModeReq, render->fd_drm, plane_id,
						DRM_MODE_OBJECT_PLANE, "SRC_W", src_w << 16);
	SetPropertyRequest(ModeReq, render->fd_drm, plane_id,
						DRM_MODE_OBJECT_PLANE, "SRC_H", src_h << 16);
}

void SetPlaneZpos(VideoRender * render, drmModeAtomicReqPtr ModeReq, uint32_t plane_id, uint64_t zpos)
{
	SetPropertyRequest(ModeReq, render->fd_drm, plane_id,
			DRM_MODE_OBJECT_PLANE, "zpos", zpos);
}

void SetPlane(VideoRender * render, drmModeAtomicReqPtr ModeReq, uint32_t plane_id,
				uint64_t crtc_id, uint64_t fb_id,
				uint64_t crtc_x, uint64_t crtc_y, uint64_t crtc_w, uint64_t crtc_h,
				uint64_t src_x, uint64_t src_y, uint64_t src_w, uint64_t src_h)
{
	SetPlaneCrtcId(render, ModeReq, plane_id, crtc_id);
	SetPlaneFbId(render, ModeReq, plane_id, fb_id);
	SetPlaneCrtc(render, ModeReq, plane_id, crtc_x, crtc_y, crtc_w, crtc_h);
	SetPlaneSrc(render, ModeReq, plane_id, src_x, src_y, src_w, src_h);
}

///
/// If primary plane support only rgb and overlay plane nv12
/// must the zpos change. At the end it must change back.
/// @param backward		if set change to origin.
///
void SetChangePlanes(VideoRender * render, drmModeAtomicReqPtr ModeReq, int back)
{
	uint64_t zpos_video;
	uint64_t zpos_osd;

	if (back) {
		zpos_video = render->zpos_overlay;
		zpos_osd = render->zpos_primary;
	} else {
		zpos_video = render->zpos_primary;
		zpos_osd = render->zpos_overlay;
	}

	SetPlaneZpos(render, ModeReq, render->video_plane, zpos_video);
	SetPlaneZpos(render, ModeReq, render->osd_plane, zpos_osd);
}

size_t ReadLineFromFile(char *buf, size_t size, char * file)
{
	FILE *fd = NULL;
	size_t character;

	fd = fopen(file, "r");
	if (fd == NULL) {
		fprintf(stderr, "Can't open %s\n", file);
		return 0;
	}

	character = getline(&buf, &size, fd);

	fclose(fd);

	return character;
}

void ReadHWPlatform(VideoRender * render)
{
	char *txt_buf;
	char *read_ptr;
	size_t bufsize = 128;
	size_t read_size;

	txt_buf = (char *) calloc(bufsize, sizeof(char));
	render->CodecMode = 0;
	render->NoHwDeint = 0;

	read_size = ReadLineFromFile(txt_buf, bufsize, "/sys/firmware/devicetree/base/compatible");
	if (!read_size) {
		free((void *)txt_buf);
		return;
	}

	read_ptr = txt_buf;

	while(read_size) {

		if (strstr(read_ptr, "bcm2711")) {
#ifdef DEBUG
			printf("ReadHWPlatform: bcm2711 found\n");
#endif
			render->CodecMode = 3;	// set _v4l2m2m for H264
			render->NoHwDeint = 1;
			break;
		}

		read_size -= (strlen(read_ptr) + 1);
		read_ptr = (char *)&read_ptr[(strlen(read_ptr) + 1)];
	}
	free((void *)txt_buf);
}

static int TestCaps(int fd)
{
	uint64_t test;

	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &test) < 0 || test == 0)
		return 1;

	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0)
		return 1;

	if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0)
		return 1;

	if (drmGetCap(fd, DRM_CAP_PRIME, &test) < 0)
		return 1;

	if (drmGetCap(fd, DRM_PRIME_CAP_EXPORT, &test) < 0)
		return 1;

	if (drmGetCap(fd, DRM_PRIME_CAP_IMPORT, &test) < 0)
		return 1;

	return 0;
}

#ifdef USE_GLES
static const EGLint context_attribute_list[] =
{
    EGL_CONTEXT_CLIENT_VERSION, 2,
    EGL_NONE
};

EGLConfig get_config(void)
{
    VideoRender *render = (VideoRender *)GetVideoRender();
    if (!render) {
        fprintf(stderr, "failed to get VideoRender\n");
        abort();
    }

    EGLint config_attribute_list[] = {
        EGL_BUFFER_SIZE, 32,
        EGL_STENCIL_SIZE, EGL_DONT_CARE,
        EGL_DEPTH_SIZE, EGL_DONT_CARE,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };
    EGLConfig *configs;
    EGLint matched;
    EGLint count;
    EGL_CHECK(assert(eglGetConfigs(render->eglDisplay, NULL, 0, &count) == EGL_TRUE));
    if (count < 1) {
        fprintf(stderr, "no EGL configs to choose from\n");
        abort();
    }

#ifdef GL_DEBUG
    fprintf(stderr, "%d EGL configs found\n", count);
#endif

    configs = malloc(count * sizeof(*configs));
    if (!configs)
        abort();

    EGL_CHECK(assert(eglChooseConfig(render->eglDisplay, config_attribute_list, configs, count, &matched) == EGL_TRUE));
    if (!matched) {
        fprintf(stderr, "no EGL configs with appropriate attributes\n");
        abort();
    }

#ifdef GL_DEBUG
    fprintf(stderr, "%d appropriate EGL configs found, which match attributes\n", matched);
#endif

    for (int i = 0; i < matched; ++i) {
        EGLint gbm_format;
        EGL_CHECK(assert(eglGetConfigAttrib(render->eglDisplay, configs[i], EGL_NATIVE_VISUAL_ID, &gbm_format) == EGL_TRUE));

        if (gbm_format == GBM_FORMAT_ARGB8888)
            return configs[i];
    }

    fprintf(stderr, "no matching gbm config found\n");
    abort();
}

PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = NULL;
PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC get_platform_surface = NULL;
#endif

static int FindDevice(VideoRender * render)
{
	drmModeRes *resources;
	drmModeConnector *connector;
	drmModeEncoder *encoder = 0;
	drmModeModeInfo *mode;
	drmModePlane *plane;
	drmModePlaneRes *plane_res;
	int hdr;
	uint32_t vrefresh;
	uint32_t j, k;
	int i;

	render->fd_drm = open("/dev/dri/card0", O_RDWR);
	if (render->fd_drm < 0) {
		fprintf(stderr, "FindDevice: cannot open /dev/dri/card0!\n");
		return -errno;
	}

	if (TestCaps(render->fd_drm)) {
		close(render->fd_drm);

		render->fd_drm = open("/dev/dri/card1", O_RDWR);
		if (render->fd_drm < 0) {
			fprintf(stderr, "FindDevice: cannot open /dev/dri/card1!\n");
			return -errno;
		}

		if (TestCaps(render->fd_drm)) {
			fprintf(stderr, "FindDevice: No DRM device available!\n");
			return -1;
		}
	}

	if ((resources = drmModeGetResources(render->fd_drm)) == NULL){
		fprintf(stderr, "FindDevice: cannot retrieve DRM resources (%d): %m\n", errno);
		return -errno;
	}

#ifdef DRM_DEBUG
	Info(_("FindDevice: DRM have %i connectors, %i crtcs, %i encoders\n"),
		resources->count_connectors, resources->count_crtcs,
		resources->count_encoders);
#endif

	// find all available connectors
	for (i = 0; i < resources->count_connectors; i++) {
		hdr = 0;
		vrefresh = 50;
		connector = drmModeGetConnector(render->fd_drm, resources->connectors[i]);
		if (!connector) {
			fprintf(stderr, "FindDevice: cannot retrieve DRM connector (%d): %m\n", errno);
		return -errno;
		}

		if (connector != NULL && connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
			render->connector_id = connector->connector_id;

			// FIXME: use default encoder/crtc pair
			if ((encoder = drmModeGetEncoder(render->fd_drm, connector->encoder_id)) == NULL){
				fprintf(stderr, "FindDevice: cannot retrieve encoder (%d): %m\n", errno);
				return -errno;
			}
			render->crtc_id = encoder->crtc_id;
		}
		// search Modes
search_mode:
		for (i = 0; i < connector->count_modes; i++) {
			mode = &connector->modes[i];
			// Mode HD
			if(mode->hdisplay == 1920 && mode->vdisplay == 1080 &&
				mode->vrefresh == vrefresh &&
				!(mode->flags & DRM_MODE_FLAG_INTERLACE) && !hdr) {
				memcpy(&render->mode, &connector->modes[i], sizeof(drmModeModeInfo));
			}
			// Mode HDready
			if(mode->hdisplay == 1280 && mode->vdisplay == 720 &&
				mode->vrefresh == vrefresh &&
				!(mode->flags & DRM_MODE_FLAG_INTERLACE) && hdr) {
				memcpy(&render->mode, &connector->modes[i], sizeof(drmModeModeInfo));
			}
		}
		if (!render->mode.hdisplay || !render->mode.vdisplay) {
			if (!hdr) {
				hdr = 1;
				goto search_mode;
			}
			if (vrefresh == 50) {
				vrefresh = 60;
				hdr = 0;
				goto search_mode;
			}
		}
		drmModeFreeConnector(connector);
	}

	if (!render->mode.hdisplay || !render->mode.vdisplay)
		Fatal(_("FindDevice: No Monitor Mode found! Give up!\n"));
	Info(_("FindDevice: Found Monitor Mode %dx%d@%d\n"),
		render->mode.hdisplay, render->mode.vdisplay, render->mode.vrefresh);

	// find first plane
	if ((plane_res = drmModeGetPlaneResources(render->fd_drm)) == NULL)
		fprintf(stderr, "FindDevice: cannot retrieve PlaneResources (%d): %m\n", errno);

	for (j = 0; j < plane_res->count_planes; j++) {
		plane = drmModeGetPlane(render->fd_drm, plane_res->planes[j]);

		if (plane == NULL)
			fprintf(stderr, "FindDevice: cannot query DRM-KMS plane %d\n", j);

		for (i = 0; i < resources->count_crtcs; i++) {
			if (plane->possible_crtcs & (1 << i))
				break;
		}

		uint64_t type;
		if (GetPropertyValue(render->fd_drm, plane_res->planes[j],
				     DRM_MODE_OBJECT_PLANE, "type", &type)) {
			fprintf(stderr, "Failed to get property 'type'\n");
		}

#ifdef DRM_DEBUG // If more then 2 crtcs this must rewriten!!!
		fprintf(stderr, "FindDevice: Plane id %i crtc_id %i possible_crtcs %i possible CRTC %i type %s\n",
			plane->plane_id, plane->crtc_id, plane->possible_crtcs, resources->crtcs[i],
			(type == DRM_PLANE_TYPE_PRIMARY) ? "primary plane" :
			(type == DRM_PLANE_TYPE_OVERLAY) ? "overlay plane" :
			(type == DRM_PLANE_TYPE_CURSOR) ? "cursor plane" : "No plane type");
		fprintf(stderr, "FindDevice: PixelFormats");
#endif
		// test pixel format and plane caps
		for (k = 0; k < plane->count_formats; k++) {
			if (encoder->possible_crtcs & plane->possible_crtcs) {
#ifdef DRM_DEBUG
				fprintf(stderr, " %4.4s", (char *)&plane->formats[k]);
#endif
				switch (plane->formats[k]) {
					case DRM_FORMAT_NV12:
						if (!render->video_plane) {
							if (type != DRM_PLANE_TYPE_PRIMARY) {
								// We have found a NV12 plane as OVERLAY_PLANE
								// so we use the zpos to switch between them
								if (!GetPropertyValue(render->fd_drm, plane_res->planes[j],
										     DRM_MODE_OBJECT_PLANE, "zpos", &render->zpos_overlay)) {
									render->use_zpos = 1;
#ifdef DRM_DEBUG
									fprintf(stderr, "\nVIDEO on OVERLAY zpos %d (=render->zpos_overlay)\n", (int)render->zpos_overlay);
#endif
								}
							}
							render->video_plane = plane->plane_id;
							if (plane->plane_id == render->osd_plane)
								render->osd_plane = 0;
						}
						break;
					case DRM_FORMAT_ARGB8888:
						if (!render->osd_plane) {
							if (type != DRM_PLANE_TYPE_OVERLAY) {
								if (!GetPropertyValue(render->fd_drm, plane_res->planes[j],
										     DRM_MODE_OBJECT_PLANE, "zpos", &render->zpos_primary)) {
									render->use_zpos = 1;
#ifdef DRM_DEBUG
									fprintf(stderr, "\nOSD on PRIMARY zpos %d (=render->zpos_primary)\n", (int)render->zpos_primary);
#endif
								}
							}
							render->osd_plane = plane->plane_id;
						}
						break;
					default:
						break;
				}
			}
		}
#ifdef DRM_DEBUG
		fprintf(stderr, "\n");
#endif
		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(plane_res);
	drmModeFreeEncoder(encoder);
	drmModeFreeResources(resources);

#ifdef USE_GLES
	render->gbm_device = gbm_create_device(render->fd_drm);
	if (!render->gbm_device) {
		fprintf(stderr, "failed to create gbm device!\n");
		return -1;
	}

	int w, h;
	double pixel_aspect;
	GetScreenSize(&w, &h, &pixel_aspect);

	render->gbm_surface = gbm_surface_create(render->gbm_device, w, h, DRM_FORMAT_ARGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!render->gbm_surface) {
		fprintf(stderr, "initGBM: failed to create %d x %d surface bo\n", w, h);
		return -1;
	}

	EGLint iMajorVersion, iMinorVersion;

	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	assert(get_platform_display != NULL);
	PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC get_platform_surface = (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
	assert(get_platform_surface != NULL);

	EGL_CHECK(assert((render->eglDisplay = get_platform_display(EGL_PLATFORM_GBM_MESA, render->gbm_device, NULL)) != EGL_NO_DISPLAY));
	EGL_CHECK(assert(eglInitialize(render->eglDisplay, &iMajorVersion, &iMinorVersion) == EGL_TRUE));

	EGLConfig eglConfig = get_config();

	EGL_CHECK(assert(eglBindAPI(EGL_OPENGL_ES_API) == EGL_TRUE));
	EGL_CHECK(assert((render->eglContext = eglCreateContext(render->eglDisplay, eglConfig, EGL_NO_CONTEXT, context_attribute_list)) != EGL_NO_CONTEXT));

	EGL_CHECK(assert((render->eglSurface = get_platform_surface(render->eglDisplay, eglConfig, render->gbm_surface, NULL)) != EGL_NO_SURFACE));

	EGLint s_width, s_height;
	EGL_CHECK(assert(eglQuerySurface(render->eglDisplay, render->eglSurface, EGL_WIDTH, &s_width) == EGL_TRUE));
	EGL_CHECK(assert(eglQuerySurface(render->eglDisplay, render->eglSurface, EGL_HEIGHT, &s_height) == EGL_TRUE));

#ifdef GL_DEBUG
	if (render->eglSurface != EGL_NO_SURFACE)
		fprintf(stderr, "EGLSurface %p on EGLDisplay %p for %d x %d BO created\n", render->eglSurface, render->eglDisplay, s_width, s_height);
#endif
	render->GlInit = 1;
#endif

#ifdef DRM_DEBUG
	Info(_("FindDevice: DRM setup CRTC: %i video_plane: %i osd_plane %i use_zpos %d\n"),
		render->crtc_id, render->video_plane, render->osd_plane, render->use_zpos);
	fprintf(stderr, "FindDevice: DRM setup CRTC: %i video_plane: %i osd_plane %i use_zpos %d\n",
		render->crtc_id, render->video_plane, render->osd_plane, render->use_zpos);
#endif
	return 0;
}

#ifdef USE_GLES
static void drm_fb_destroy_callback(struct gbm_bo *bo, void *data)
{
	int drm_fd = gbm_device_get_fd(gbm_bo_get_device(bo));
	struct drm_buf *buf = data;

	if (buf->fb_id)
		drmModeRmFB(drm_fd, buf->fb_id);

	free(buf);
}

__attribute__ ((weak)) union gbm_bo_handle
gbm_bo_get_handle_for_plane(struct gbm_bo *bo, int plane);

__attribute__ ((weak)) uint64_t
gbm_bo_get_modifier(struct gbm_bo *bo);

__attribute__ ((weak)) int
gbm_bo_get_plane_count(struct gbm_bo *bo);

__attribute__ ((weak)) uint32_t
gbm_bo_get_stride_for_plane(struct gbm_bo *bo, int plane);

__attribute__ ((weak)) uint32_t
gbm_bo_get_offset(struct gbm_bo *bo, int plane);

struct drm_buf *drm_get_buf_from_bo(VideoRender *render, struct gbm_bo *bo)
{
	struct drm_buf *buf = gbm_bo_get_user_data(bo);
	uint32_t mod_flags = 0;
	int ret = -1;

	// the buffer was already allocated
	if (buf)
		return buf;

	buf = calloc(1, sizeof *buf);
	buf->bo = bo;

	buf->width = gbm_bo_get_width(bo);
	buf->height = gbm_bo_get_height(bo);
	buf->pix_fmt = gbm_bo_get_format(bo);

	if (gbm_bo_get_handle_for_plane && gbm_bo_get_modifier &&
            gbm_bo_get_plane_count && gbm_bo_get_stride_for_plane &&
            gbm_bo_get_offset) {
		uint64_t modifiers[4] = {0};
		modifiers[0] = gbm_bo_get_modifier(bo);
		const int num_planes = gbm_bo_get_plane_count(bo);
		for (int i = 0; i < num_planes; i++) {
			buf->handle[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
			buf->pitch[i] = gbm_bo_get_stride_for_plane(bo, i);
			buf->offset[i] = gbm_bo_get_offset(bo, i);
			modifiers[i] = modifiers[0];
		}

		if (modifiers[0]) {
			mod_flags = DRM_MODE_FB_MODIFIERS;
#ifdef GL_DEBUG
			fprintf(stderr, "drm_get_buf_from_bo: Using modifier %" PRIx64 "\n", modifiers[0]);
#endif
		}

		// Add FB
		ret = drmModeAddFB2WithModifiers(render->fd_drm, buf->width, buf->height, buf->pix_fmt,
			buf->handle, buf->pitch, buf->offset, modifiers, &buf->fb_id, mod_flags);
	}

	if (ret) {
#ifdef GL_DEBUG
		if (mod_flags)
			fprintf(stderr, "drm_get_buf_from_bo: Modifiers failed!\n");
#endif

		memcpy(buf->handle, (uint32_t [4]){ gbm_bo_get_handle(bo).u32, 0, 0, 0}, 16);
		memcpy(buf->pitch, (uint32_t [4]){ gbm_bo_get_stride(bo), 0, 0, 0}, 16);
		memset(buf->offset, 0, 16);
		ret = drmModeAddFB2(render->fd_drm, buf->width, buf->height, buf->pix_fmt,
			buf->handle, buf->pitch, buf->offset, &buf->fb_id, 0);
	}

	if (ret) {
#ifdef GL_DEBUG
		fprintf(stderr, "drm_get_buf_from_bo: cannot create framebuffer (%d): %m\n", errno);
		Fatal(_("drm_get_buf_from_bo: cannot create framebuffer (%d): %m\n"), errno);
#endif
		free(buf);
		return NULL;
	}

#ifdef GL_DEBUG
	fprintf(stderr, "drm_get_buf_from_bo: New GL buffer %d x %d pix_fmt %4.4s fb_id %d\n",
		buf->width, buf->height, (char *)&buf->pix_fmt, buf->fb_id);
#endif
	gbm_bo_set_user_data(bo, buf, drm_fb_destroy_callback);
	return buf;
}
#endif

static int SetupFB(VideoRender * render, struct drm_buf *buf,
			AVDRMFrameDescriptor *primedata)
{
	struct drm_mode_create_dumb creq;
	uint64_t modifier[4] = { 0, 0, 0, 0 };
	uint32_t mod_flags = 0;
	buf->handle[0] = buf->handle[1] = buf->handle[2] = buf->handle[3] = 0;
	buf->pitch[0] = buf->pitch[1] = buf->pitch[2] = buf->pitch[3] = 0;
	buf->offset[0] = buf->offset[1] = buf->offset[2] = buf->offset[3] = 0;

	if (primedata) {
		uint32_t prime_handle;

		buf->pix_fmt = primedata->layers[0].format;

		if (drmPrimeFDToHandle(render->fd_drm, primedata->objects[0].fd, &prime_handle))
			fprintf(stderr, "SetupFB: Failed to retrieve the Prime Handle %i size %zu (%d): %m\n",
				primedata->objects[0].fd, primedata->objects[0].size, errno);
#ifdef DRM_DEBUG
		if (!render->buffers)
			fprintf(stderr, "SetupFB: %d x %d nb_objects %d nb_layers %d nb_planes %d size %zu pix_fmt %4.4s modifier %" PRIx64 "\n",
				buf->width, buf->height, primedata->nb_objects, primedata->nb_layers,
				primedata->layers[0].nb_planes, primedata->objects[0].size,
				(char *)&buf->pix_fmt, primedata->objects[0].format_modifier);
#endif
		for (int plane = 0; plane < primedata->layers[0].nb_planes; plane++) {
			buf->handle[plane] = prime_handle;
			buf->pitch[plane] = primedata->layers[0].planes[plane].pitch;
			buf->offset[plane] = primedata->layers[0].planes[plane].offset;
			if (primedata->objects[0].format_modifier) {
				modifier[plane] =
					primedata->objects[primedata->layers[0].planes[plane].object_index].format_modifier;
				mod_flags = DRM_MODE_FB_MODIFIERS;
			}
#ifdef DRM_DEBUG
			if (!render->buffers)
				fprintf(stderr, "SetupFB: plane %d pitch %d offset %d\n",
					plane, buf->pitch[plane], buf->offset[plane]);
#endif
		}
	} else {
		memset(&creq, 0, sizeof(struct drm_mode_create_dumb));
		creq.width = buf->width;
		creq.height = buf->height;
		// 32 bpp for ARGB, 8 bpp for YUV420 and NV12
		if (buf->pix_fmt == DRM_FORMAT_ARGB8888)
			creq.bpp = 32;
		else
			creq.bpp = 12;

		if (drmIoctl(render->fd_drm, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0){
			fprintf(stderr, "SetupFB: cannot create dumb buffer (%d): %m\n", errno);
			fprintf(stderr, "SetupFB: width %d height %d bpp %d\n",
				creq.width, creq.height, creq.bpp);
			return -errno;
		}

		buf->size = creq.size;

		if (buf->pix_fmt == DRM_FORMAT_YUV420) {
			buf->pitch[0] = buf->width;
			buf->pitch[2] = buf->pitch[1] = buf->pitch[0] / 2;

			buf->offset[0] = 0;
			buf->offset[1] = buf->pitch[0] * buf->height;
			buf->offset[2] = buf->offset[1] + buf->pitch[1] * buf->height / 2;
			buf->handle[2] = buf->handle[1] = buf->handle[0] = creq.handle;
		}

		if (buf->pix_fmt == DRM_FORMAT_NV12) {
			buf->pitch[1] = buf->pitch[0] = buf->width;

			buf->offset[0] = 0;
			buf->offset[1] = buf->pitch[0] * buf->height;
			buf->handle[1] = buf->handle[0] = creq.handle;
		}

		if (buf->pix_fmt == DRM_FORMAT_ARGB8888) {
			buf->pitch[0] = creq.pitch;

			buf->offset[0] = 0;
			buf->handle[0] = creq.handle;
		}
	}

	if (drmModeAddFB2WithModifiers(render->fd_drm, buf->width, buf->height, buf->pix_fmt,
			buf->handle, buf->pitch, buf->offset, modifier, &buf->fb_id, mod_flags)) {

		fprintf(stderr, "SetupFB: cannot create modifiers framebuffer (%d): %m\n", errno);
		Fatal(_("SetupFB: cannot create modifiers framebuffer (%d): %m\n"), errno);
	}

	if (primedata)
		return 0;

	struct drm_mode_map_dumb mreq;
	memset(&mreq, 0, sizeof(struct drm_mode_map_dumb));
	mreq.handle = buf->handle[0];

	if (drmIoctl(render->fd_drm, DRM_IOCTL_MODE_MAP_DUMB, &mreq)){
		fprintf(stderr, "SetupFB: cannot map dumb buffer (%d): %m\n", errno);
		return -errno;
	}

	buf->plane[0] = mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, render->fd_drm, mreq.offset);
	if (buf->plane[0] == MAP_FAILED) {
		fprintf(stderr, "SetupFB: cannot mmap dumb buffer (%d): %m\n", errno);
		return -errno;
	}
	buf->plane[1] = buf->plane[0] + buf->offset[1];
	buf->plane[2] = buf->plane[0] + buf->offset[2];
#ifdef DRM_DEBUG
	if (!render->buffers)
		fprintf(stderr, "SetupFB: fb_id %d width %d height %d pix_fmt %4.4s\n",
			buf->fb_id, buf->width, buf->height, (char *)&buf->pix_fmt);
#endif

	return 0;
}

/*static void Drm_page_flip_event( __attribute__ ((unused)) int fd,
					__attribute__ ((unused)) unsigned int frame,
					__attribute__ ((unused)) unsigned int sec,
					__attribute__ ((unused)) unsigned int usec,
					__attribute__ ((unused)) void *data)
{
}*/

static void DestroyFB(int fd_drm, struct drm_buf *buf)
{
	struct drm_mode_destroy_dumb dreq;

//	fprintf(stderr, "DestroyFB: destroy FB %d\n", buf->fb_id);

	if (buf->plane[0]) {
		if (munmap(buf->plane[0], buf->size))
				fprintf(stderr, "DestroyFB: failed unmap FB (%d): %m\n", errno);
	}

	if (drmModeRmFB(fd_drm, buf->fb_id) < 0)
		fprintf(stderr, "DestroyFB: cannot remake FB (%d): %m\n", errno);

	if (buf->plane[0]) {
		memset(&dreq, 0, sizeof(dreq));
		dreq.handle = buf->handle[0];

		if (drmIoctl(fd_drm, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq) < 0)
			fprintf(stderr, "DestroyFB: cannot destroy dumb buffer (%d): %m\n", errno);
		buf->handle[0] = 0;

		if (buf->fd_prime) {
			if (close(buf->fd_prime))
				fprintf(stderr, "DestroyFB: failed close fd prime (%d): %m\n", errno);
		}
	}

	if (buf->handle[0]) {
		if (drmIoctl(fd_drm, DRM_IOCTL_GEM_CLOSE, &buf->handle[0]) < 0)
			fprintf(stderr, "DestroyFB: cannot close GEM (%d): %m\n", errno);
	}

	buf->width = 0;
	buf->height = 0;
	buf->fb_id = 0;
	buf->plane[0] = 0;
	buf->size = 0;
	buf->fd_prime = 0;
}

///
/// Clean DRM
///
static void CleanDisplayThread(VideoRender * render)
{
	AVFrame *frame;
	int i;

	if (render->lastframe) {
		av_frame_free(&render->lastframe);
	}

dequeue:
	if (atomic_read(&render->FramesFilled)) {

		frame = render->FramesRb[render->FramesRead];

		render->FramesRead = (render->FramesRead + 1) % VIDEO_SURFACES_MAX;
		atomic_dec(&render->FramesFilled);

		av_frame_free(&frame);
		goto dequeue;
	}

	if (FilterThread)
		render->Filter_Close = 1;

	// Destroy FBs
	if (render->buffers) {
		for (i = 0; i < render->buffers; ++i) {
			DestroyFB(render->fd_drm, &render->bufs[i]);
		}
		render->buffers = 0;
		render->enqueue_buffer = 0;
	}

	pthread_cond_signal(&WaitCleanCondition);

	render->Closing = 0;
#ifdef DEBUG
	fprintf(stderr, "CleanDisplayThread: DRM cleaned.\n");
#endif
}

///
///	Draw a video frame.
///
static void Frame2Display(VideoRender * render)
{
	struct drm_buf *buf = 0;
	AVFrame *frame = NULL;
	AVDRMFrameDescriptor *primedata = NULL;
	int64_t audio_pts;
	int64_t video_pts;
	int i;

	if (render->Closing) {
closing:
		// set a black FB
#ifdef DEBUG
	fprintf(stderr, "Frame2Display: set a black FB\n");
#endif
		buf = &render->buf_black;
		goto page_flip;
	}

dequeue:
	while (!atomic_read(&render->FramesFilled)) {
		if (render->Closing)
			goto closing;
		usleep(10000);
	}

	frame = render->FramesRb[render->FramesRead];
	primedata = (AVDRMFrameDescriptor *)frame->data[0];

	// search or made fd / FB combination
	for (i = 0; i < render->buffers; i++) {
		if (render->bufs[i].fd_prime == primedata->objects[0].fd) {
			buf = &render->bufs[i];
			break;
		}
	}
	if (buf == 0) {
		buf = &render->bufs[render->buffers];
		buf->width = (uint32_t)frame->width;
		buf->height = (uint32_t)frame->height;
		buf->fd_prime = primedata->objects[0].fd;

		SetupFB(render, buf, primedata);
		render->buffers++;
	}

	render->pts = frame->pts;
	video_pts = frame->pts * 1000 * av_q2d(*render->timebase);
	if(!render->StartCounter && !render->Closing && !render->TrickSpeed) {
#ifdef DEBUG
		fprintf(stderr, "Frame2Display: start PTS %s\n", Timestamp2String(video_pts));
#endif
avready:
		if (AudioVideoReady(video_pts)) {
			usleep(10000);
			if (render->Closing)
				goto closing;
			goto avready;
		}
	}

audioclock:
	audio_pts = AudioGetClock();

	if (render->Closing)
		goto closing;

	if (audio_pts == (int64_t)AV_NOPTS_VALUE && !render->TrickSpeed) {
		usleep(20000);
		goto audioclock;
	}

	int diff = video_pts - audio_pts - VideoAudioDelay;

	if (diff < -5 && !render->TrickSpeed && !(abs(diff) > 5000)) {
		render->FramesDropped++;
#ifdef AV_SYNC_DEBUG
		fprintf(stderr, "FrameDropped Pkts %d deint %d Frames %d AudioUsedBytes %d audio %s video %s Delay %dms diff %dms\n",
			VideoGetPackets(), atomic_read(&render->FramesDeintFilled),
			atomic_read(&render->FramesFilled), AudioUsedBytes(), Timestamp2String(audio_pts),
			Timestamp2String(video_pts), VideoAudioDelay, diff);
#endif
		av_frame_free(&frame);
		render->FramesRead = (render->FramesRead + 1) % VIDEO_SURFACES_MAX;
		atomic_dec(&render->FramesFilled);

		if (!render->StartCounter)
			render->StartCounter++;
		goto dequeue;
	}

	if (diff > 35 && !render->TrickSpeed && !(abs(diff) > 5000)) {
		render->FramesDuped++;
#ifdef AV_SYNC_DEBUG
		fprintf(stderr, "FrameDuped Pkts %d deint %d Frames %d AudioUsedBytes %d audio %s video %s Delay %dms diff %dms\n",
			VideoGetPackets(), atomic_read(&render->FramesDeintFilled),
			atomic_read(&render->FramesFilled), AudioUsedBytes(), Timestamp2String(audio_pts),
			Timestamp2String(video_pts), VideoAudioDelay, diff);
#endif
		usleep(20000);
		goto audioclock;
	}

#ifdef AV_SYNC_DEBUG
	if (abs(diff) > 5000) {	// more than 5s
		fprintf(stderr, "More then 5s Pkts %d deint %d Frames %d AudioUsedBytes %d audio %s video %s Delay %dms diff %dms\n",
			VideoGetPackets(), atomic_read(&render->FramesDeintFilled),
			atomic_read(&render->FramesFilled), AudioUsedBytes(), Timestamp2String(audio_pts),
			Timestamp2String(video_pts), VideoAudioDelay, diff);
	}
#endif

	if (!render->TrickSpeed)
		render->StartCounter++;

	if (render->TrickSpeed)
		usleep(20000 * render->TrickSpeed);

	buf->frame = frame;
	render->FramesRead = (render->FramesRead + 1) % VIDEO_SURFACES_MAX;
	atomic_dec(&render->FramesFilled);

page_flip:
	render->act_buf = buf;

	drmModeAtomicReqPtr ModeReq;
	uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT;
	if (!(ModeReq = drmModeAtomicAlloc()))
		fprintf(stderr, "Frame2Display: cannot allocate atomic request (%d): %m\n", errno);

	uint64_t PicWidth = render->mode.hdisplay;
	if (frame)
		PicWidth = render->mode.vdisplay * av_q2d(frame->sample_aspect_ratio) *
		frame->width / frame->height;
	if (!PicWidth || PicWidth > render->mode.hdisplay)
		PicWidth = render->mode.hdisplay;

	// handle the video plane
	uint64_t buf_width_tmp;
	GetPropertyValue(render->fd_drm, render->video_plane, DRM_MODE_OBJECT_PLANE, "SRC_W", &buf_width_tmp);
	if (buf->width != (buf_width_tmp >> 16))
			SetPlaneSrc(render, ModeReq, render->video_plane, 0, 0, buf->width, buf->height);

	uint64_t pic_width_tmp;
	GetPropertyValue(render->fd_drm, render->video_plane, DRM_MODE_OBJECT_PLANE, "CRTC_W", &pic_width_tmp);
	if (PicWidth != pic_width_tmp)
			SetPlaneCrtc(render, ModeReq, render->video_plane,
				(render->mode.hdisplay - PicWidth) / 2, 0, PicWidth, render->mode.vdisplay);

	SetPlaneFbId(render, ModeReq, render->video_plane, buf->fb_id);

	// handle the osd plane
	if (render->OsdShown) {
#ifdef USE_GLES
		if (render->use_zpos) {
			if (render->buf_osd_gl && !render->buf_osd_gl->init) {
				SetPlane(render, ModeReq, render->osd_plane, render->crtc_id, render->buf_osd_gl->fb_id,
					 0, 0, render->buf_osd_gl->width, render->buf_osd_gl->height,
					 0, 0, render->buf_osd_gl->width, render->buf_osd_gl->height);
				render->buf_osd_gl->init = 1;
			}

			if (render->buf_osd_gl && render->buf_osd_gl->dirty) {
				SetPlaneZpos(render, ModeReq, render->video_plane, render->zpos_primary);
				SetPlaneZpos(render, ModeReq, render->osd_plane, render->zpos_overlay);
				render->buf_osd_gl->dirty = 0;
			}
		} else {
			if (render->buf_osd_gl && render->buf_osd_gl->dirty) {
				SetPlane(render, ModeReq, render->osd_plane, render->crtc_id, render->buf_osd_gl->fb_id,
					 0, 0, render->buf_osd_gl->width, render->buf_osd_gl->height,
					 0, 0, render->buf_osd_gl->width, render->buf_osd_gl->height);
				render->buf_osd_gl->dirty = 0;
			}
		}
#else
		if (render->buf_osd.dirty) {
			uint64_t value;
			if (render->use_zpos) {
				if (GetPropertyValue(render->fd_drm, render->osd_plane, DRM_MODE_OBJECT_PLANE, "zpos", &value))
					fprintf(stderr, "Failed to get property 'zpos'\n");
				if (render->zpos_overlay != value) {
					SetChangePlanes(render, ModeReq, 0);
				}
			}
			if (GetPropertyValue(render->fd_drm, render->osd_plane, DRM_MODE_OBJECT_PLANE, "FB_ID", &value))
				fprintf(stderr, "Failed to get property 'FB_ID'\n");
			if (!value) {
				SetPlane(render, ModeReq, render->osd_plane, render->crtc_id, render->buf_osd.fb_id,
					 0, 0, render->buf_osd.width, render->buf_osd.height,
					 0, 0, render->buf_osd.width, render->buf_osd.height);
			}
			render->buf_osd.dirty = 0;
		}
#endif
	} else {
#ifdef USE_GLES
		if (render->use_zpos) {
			if (render->buf_osd_gl && !render->buf_osd_gl->init) {
				SetPlane(render, ModeReq, render->osd_plane, render->crtc_id, render->buf_osd_gl->fb_id,
					 0, 0, render->buf_osd_gl->width, render->buf_osd_gl->height,
					 0, 0, render->buf_osd_gl->width, render->buf_osd_gl->height);
				render->buf_osd_gl->init = 1;
			}
			if (render->buf_osd_gl && render->buf_osd_gl->dirty) {
				SetPlaneZpos(render, ModeReq, render->video_plane, render->zpos_overlay);
				SetPlaneZpos(render, ModeReq, render->osd_plane, render->zpos_primary);
				render->buf_osd_gl->dirty = 0;
			}
		} else {
			if (render->buf_osd_gl && render->buf_osd_gl->dirty) {
				SetPlane(render, ModeReq, render->osd_plane, render->crtc_id, render->buf_osd_gl->fb_id,
					 0, 0, render->buf_osd_gl->width, render->buf_osd_gl->height,
					 0, 0, 0, 0);
				render->buf_osd_gl->dirty = 0;
			}
		}
#else
		if (render->buf_osd.dirty) {
			uint64_t value;
			if (render->use_zpos) {
				if (GetPropertyValue(render->fd_drm, render->osd_plane, DRM_MODE_OBJECT_PLANE, "zpos", &value))
					fprintf(stderr, "Failed to get property 'zpos'\n");
				if (render->zpos_overlay == value) {
					SetChangePlanes(render, ModeReq, 1);
				}
			} else {
				SetPlane(render, ModeReq, render->osd_plane, render->crtc_id, render->buf_osd.fb_id,
					 0, 0, render->buf_osd.width, render->buf_osd.height, 0, 0, 0, 0);
			}
			render->buf_osd.dirty = 0;
		}
#endif
	}

	if (drmModeAtomicCommit(render->fd_drm, ModeReq, flags, NULL) != 0)
		fprintf(stderr, "Frame2Display: cannot page flip to FB %i (%d): %m\n",
			buf->fb_id, errno);

	drmModeAtomicFree(ModeReq);
}

///
///	Display a video frame.
///
static void *DisplayHandlerThread(void * arg)
{
	VideoRender * render = (VideoRender *)arg;

	pthread_setcancelstate (PTHREAD_CANCEL_ENABLE, NULL);
	pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

	while ((atomic_read(&render->FramesFilled)) < 2 ){
		usleep(10000);
	}

	while (1) {
		pthread_testcancel();

		if (render->VideoPaused) {
			pthread_mutex_lock(&PauseMutex);
			pthread_cond_wait(&PauseCondition, &PauseMutex);
			pthread_mutex_unlock(&PauseMutex);
		}

		Frame2Display(render);

		if (drmHandleEvent(render->fd_drm, &render->ev) != 0)
			fprintf(stderr, "DisplayHandlerThread: drmHandleEvent failed!\n");

/*#ifdef AV_SYNC_DEBUG
		static uint32_t last_tick;
		uint32_t tick;

		tick = GetMsTicks();
		if (tick - last_tick > 21) {
			Debug(3, "DisplayHandlerThread: StartCounter %4d %dms\n",
				render->StartCounter, tick - last_tick);
			fprintf(stderr, "DisplayHandlerThread: StartCounter %4d FramesFilled %d %dms\n",
				render->StartCounter, atomic_read(&render->FramesFilled), tick - last_tick);
		}
		last_tick = tick;
#endif*/

		if (render->lastframe) {
			av_frame_free(&render->lastframe);
		}
		render->lastframe = render->act_buf->frame;

		if (render->Closing && render->buf_black.fb_id == render->act_buf->fb_id) {
			CleanDisplayThread(render);
		}
	}
	pthread_exit((void *)pthread_self());
}

//----------------------------------------------------------------------------
//	OSD
//----------------------------------------------------------------------------

///
///	Clear the OSD.
///
///
void VideoOsdClear(VideoRender * render)
{
#ifdef USE_GLES
	struct drm_buf *buf;

	EGL_CHECK(eglSwapBuffers(render->eglDisplay, render->eglSurface));
	render->next_bo = gbm_surface_lock_front_buffer(render->gbm_surface);
	assert(render->next_bo);

	buf = drm_get_buf_from_bo(render, render->next_bo);
	if (!buf) {
		fprintf(stderr, "Failed to get GL buffer\n");
		return;
	}

	render->buf_osd_gl = buf;
	render->buf_osd_gl->dirty = 1;

	// release old buffer for writing again
	if (render->bo)
		gbm_surface_release_buffer(render->gbm_surface, render->bo);

	// rotate bos and create and keep bo as old_bo to make it free'able
	render->old_bo = render->bo;
	render->bo = render->next_bo;

#ifdef GL_DEBUG
	fprintf(stderr, "VideoOsdClear(GL): eglSwapBuffers eglDisplay %p eglSurface %p (%i x %i, %i)\n", render->eglDisplay, render->eglSurface, buf->width, buf->height, buf->pitch[0]);
#endif
#else
	memset((void *)render->buf_osd.plane[0], 0,
		(size_t)(render->buf_osd.pitch[0] * render->buf_osd.height));
	render->buf_osd.dirty = 1;
#endif

	render->OsdShown = 0;
}

///
///	Draw an OSD ARGB image.
///
///	@param xi	x-coordinate in argb image
///	@param yi	y-coordinate in argb image
///	@paran height	height in pixel in argb image
///	@paran width	width in pixel in argb image
///	@param pitch	pitch of argb image
///	@param argb	32bit ARGB image data
///	@param x	x-coordinate on screen of argb image
///	@param y	y-coordinate on screen of argb image
///
#ifdef USE_GLES
void VideoOsdDrawARGB(VideoRender * render, __attribute__ ((unused)) int xi,
		__attribute__ ((unused)) int yi, __attribute__ ((unused)) int width, __attribute__ ((unused)) int height, __attribute__ ((unused)) int pitch,
		__attribute__ ((unused)) const uint8_t * argb, __attribute__ ((unused)) int x, __attribute__ ((unused)) int y)
#else
void VideoOsdDrawARGB(VideoRender * render, __attribute__ ((unused)) int xi,
		__attribute__ ((unused)) int yi, __attribute__ ((unused)) int width,
		int height, int pitch, const uint8_t * argb, int x, int y)
#endif
{
#ifdef USE_GLES
	struct drm_buf *buf;

	EGL_CHECK(eglSwapBuffers(render->eglDisplay, render->eglSurface));
	render->next_bo = gbm_surface_lock_front_buffer(render->gbm_surface);
	assert(render->next_bo);

	buf = drm_get_buf_from_bo(render, render->next_bo);
	if (!buf) {
		fprintf(stderr, "Failed to get GL buffer\n");
		return;
	}

	render->buf_osd_gl = buf;
	render->buf_osd_gl->dirty = 1;

	// release old buffer for writing again
	if (render->bo)
		gbm_surface_release_buffer(render->gbm_surface, render->bo);

	// rotate bos and create and keep bo as old_bo to make it free'able
	render->old_bo = render->bo;
	render->bo = render->next_bo;

#ifdef GL_DEBUG
	fprintf(stderr, "VideoOsdDrawARGB(GL): eglSwapBuffers eglDisplay %p eglSurface %p (%i x %i, %i)\n", render->eglDisplay, render->eglSurface, buf->width, buf->height, buf->pitch[0]);
#endif
#else
	int i;

	for (i = 0; i < height; ++i) {
		memcpy(render->buf_osd.plane[0] + x * 4 + (i + y) * render->buf_osd.pitch[0],
			argb + i * pitch, (size_t)pitch);
	}
	render->buf_osd.dirty = 1;
#endif

	render->OsdShown = 1;
}

//----------------------------------------------------------------------------
//	Thread
//----------------------------------------------------------------------------

///
///	Video render thread.
///
static void *DecodeHandlerThread(void *arg)
{
	VideoRender * render = (VideoRender *)arg;

	Debug(3, "video: display thread started\n");

	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

	for (;;) {
		pthread_testcancel();

		// manage fill frame output ring buffer
		if (atomic_read(&render->FramesDeintFilled) < VIDEO_SURFACES_MAX &&
			atomic_read(&render->FramesFilled) < VIDEO_SURFACES_MAX) {

			if (VideoDecodeInput(render->Stream))
				usleep(10000);

		} else {
			usleep(10000);
		}
	}
	pthread_exit((void *)pthread_self());
}

///
///	Exit and cleanup video threads.
///
void VideoThreadExit(void)
{
	void *retval;

	Debug(3, "video: video thread canceled\n");

	if (DecodeThread) {
#ifdef DEBUG
		fprintf(stderr, "VideoThreadExit: cancel decode thread\n");
#endif
		// FIXME: can't cancel locked
		if (pthread_cancel(DecodeThread)) {
			Error(_("video: can't queue cancel video display thread\n"));
			fprintf(stderr, "VideoThreadExit: can't queue cancel video display thread\n");
		}
		if (pthread_join(DecodeThread, &retval) || retval != PTHREAD_CANCELED) {
			Error(_("video: can't cancel video display thread\n"));
			fprintf(stderr, "VideoThreadExit: can't cancel video display thread\n");
		}
		DecodeThread = 0;

		pthread_cond_destroy(&PauseCondition);
		pthread_mutex_destroy(&PauseMutex);
	}

	if (DisplayThread) {
#ifdef DEBUG
		fprintf(stderr, "VideoThreadExit: cancel display thread\n");
#endif
		if (pthread_cancel(DisplayThread)) {
			Error(_("video: can't cancel DisplayHandlerThread thread\n"));
			fprintf(stderr, "VideoThreadExit: can't cancel DisplayHandlerThread thread\n");
		}
		if (pthread_join(DisplayThread, &retval) || retval != PTHREAD_CANCELED) {
			Error(_("video: can't cancel video display thread\n"));
			fprintf(stderr, "VideoThreadExit: can't cancel video display thread\n");
		}
		DisplayThread = 0;
	}
}

///
///	Video display wakeup.
///
///	New video arrived, wakeup video thread.
///
void VideoThreadWakeup(VideoRender * render)
{
#ifdef DEBUG
	fprintf(stderr, "VideoThreadWakeup: VideoThreadWakeup\n");
#endif
	if (!DecodeThread) {
		pthread_cond_init(&PauseCondition,NULL);
		pthread_mutex_init(&PauseMutex, NULL);

		pthread_cond_init(&WaitCleanCondition,NULL);
		pthread_mutex_init(&WaitCleanMutex, NULL);

		pthread_create(&DecodeThread, NULL, DecodeHandlerThread, render);
		pthread_setname_np(DecodeThread, "softhddev video");
	}

	if (!DisplayThread) {
		pthread_create(&DisplayThread, NULL, DisplayHandlerThread, render);
	}
}

//----------------------------------------------------------------------------
//	Video API
//----------------------------------------------------------------------------

///
///	Allocate new video hw render.
///
///	@param stream	video stream
///
///	@returns a new initialized video hardware render.
///
VideoRender *VideoNewRender(VideoStream * stream)
{
	VideoRender *render;

	if (!(render = calloc(1, sizeof(*render)))) {
		Error(_("video/DRM: out of memory\n"));
		return NULL;
	}
	atomic_set(&render->FramesFilled, 0);
	atomic_set(&render->FramesDeintFilled, 0);
	render->Stream = stream;
	render->Closing = 0;
	render->enqueue_buffer = 0;
	render->VideoPaused = 0;

	return render;
}

///
///	Destroy a video render.
///
///	@param render	video render
///
void VideoDelRender(VideoRender * render)
{
    if (render) {
#ifdef DEBUG
		if (!pthread_equal(pthread_self(), DecodeThread)) {
			Debug(3, "video: should only be called from inside the thread\n");
		}
#endif
		free(render);
		return;
    }
}

///
///	Callback to negotiate the PixelFormat.
///
///	@param hw_render	video hardware render
///	@param video_ctx	ffmpeg video codec context
///	@param fmt		is the list of formats which are supported by
///				the codec, it is terminated by -1 as 0 is a
///				valid format, the formats are ordered by
///				quality.
///
enum AVPixelFormat Video_get_format(__attribute__ ((unused))VideoRender * render,
		AVCodecContext * video_ctx, const enum AVPixelFormat *fmt)
{
	while (*fmt != AV_PIX_FMT_NONE) {
#ifdef CODEC_DEBUG
		fprintf(stderr, "Video_get_format: PixelFormat %s ctx_fmt %s sw_pix_fmt %s Codecname: %s\n",
			av_get_pix_fmt_name(*fmt), av_get_pix_fmt_name(video_ctx->pix_fmt),
			av_get_pix_fmt_name(video_ctx->sw_pix_fmt), video_ctx->codec->name);
#endif
		if (*fmt == AV_PIX_FMT_DRM_PRIME) {
			return AV_PIX_FMT_DRM_PRIME;
		}

		if (*fmt == AV_PIX_FMT_YUV420P) {
			return AV_PIX_FMT_YUV420P;
		}
		fmt++;
	}
	fprintf(stderr, "Video_get_format: No pixel format found! Set default format.\n");

	return avcodec_default_get_format(video_ctx, fmt);
}

void EnqueueFB(VideoRender * render, AVFrame *inframe)
{
	struct drm_buf *buf = 0;
	AVDRMFrameDescriptor * primedata;
	AVFrame *frame;
	int i;

	if (!render->buffers) {
		for (int i = 0; i < VIDEO_SURFACES_MAX + 2; i++) {
			buf = &render->bufs[i];
			buf->width = (uint32_t)inframe->width;
			buf->height = (uint32_t)inframe->height;
			buf->pix_fmt = DRM_FORMAT_NV12;

			if (SetupFB(render, buf, NULL))
				fprintf(stderr, "EnqueueFB: SetupFB FB %i x %i failed\n",
					buf->width, buf->height);
			else {
				render->buffers++;
			}

			if (drmPrimeHandleToFD(render->fd_drm, buf->handle[0],
				DRM_CLOEXEC | DRM_RDWR, &buf->fd_prime))
				fprintf(stderr, "EnqueueFB: Failed to retrieve the Prime FD (%d): %m\n",
					errno);
		}
	}

	buf = &render->bufs[render->enqueue_buffer];

	for (i = 0; i < inframe->height; ++i) {
		memcpy(buf->plane[0] + i * inframe->width,
			inframe->data[0] + i * inframe->linesize[0], inframe->width);
	}
	for (i = 0; i < inframe->height / 2; ++i) {
		memcpy(buf->plane[1] + i * inframe->width,
			inframe->data[1] + i * inframe->linesize[1], inframe->width);
	}

	frame = av_frame_alloc();
	frame->pts = inframe->pts;
	frame->width = inframe->width;
	frame->height = inframe->height;
	frame->format = AV_PIX_FMT_DRM_PRIME;
	frame->sample_aspect_ratio.num = inframe->sample_aspect_ratio.num;
	frame->sample_aspect_ratio.den = inframe->sample_aspect_ratio.den;

	primedata = av_mallocz(sizeof(AVDRMFrameDescriptor));
	primedata->objects[0].fd = buf->fd_prime;
	frame->data[0] = (uint8_t *)primedata;
	frame->buf[0] = av_buffer_create((uint8_t *)primedata, sizeof(*primedata),
				ReleaseFrame, NULL, AV_BUFFER_FLAG_READONLY);

	av_frame_free(&inframe);

	render->FramesRb[render->FramesWrite] = frame;
	render->FramesWrite = (render->FramesWrite + 1) % VIDEO_SURFACES_MAX;
	atomic_inc(&render->FramesFilled);

	if (render->enqueue_buffer == VIDEO_SURFACES_MAX + 1)
		render->enqueue_buffer = 0;
	else render->enqueue_buffer++;
}

/**
**	Filter thread.
*/
static void *FilterHandlerThread(void * arg)
{
	VideoRender * render = (VideoRender *)arg;
	AVFrame *frame = 0;
	int ret = 0;

	while (1) {
		while (!atomic_read(&render->FramesDeintFilled) && !render->Filter_Close) {
			usleep(10000);
		}
getinframe:
		if (atomic_read(&render->FramesDeintFilled)) {
			frame = render->FramesDeintRb[render->FramesDeintRead];
			render->FramesDeintRead = (render->FramesDeintRead + 1) % VIDEO_SURFACES_MAX;
			atomic_dec(&render->FramesDeintFilled);
		}
		if (render->Filter_Close) {
			if (frame)
				av_frame_free(&frame);
			if (atomic_read(&render->FramesDeintFilled)) {
				goto getinframe;
			}
			frame = NULL;
		}
		if (av_buffersrc_add_frame_flags(render->buffersrc_ctx,
			frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
			fprintf(stderr, "FilterHandlerThread: can't add_frame.\n");
		} else {
			av_frame_free(&frame);
		}

		while (1) {
			AVFrame *filt_frame = av_frame_alloc();
			ret = av_buffersink_get_frame(render->buffersink_ctx, filt_frame);

			if (ret == AVERROR(EAGAIN)) {
				av_frame_free(&filt_frame);
				break;
			}
			if (ret == AVERROR_EOF) {
				av_frame_free(&filt_frame);
				goto closing;
			}
			if (ret < 0) {
				fprintf(stderr, "FilterHandlerThread: can't get filtered frame: %s\n",
					av_err2str(ret));
				av_frame_free(&filt_frame);
				break;
			}
fillframe:
			if (render->Filter_Close) {
				av_frame_free(&filt_frame);
				break;
			}
			if (atomic_read(&render->FramesFilled) < VIDEO_SURFACES_MAX && !render->Closing) {
				if (filt_frame->format == AV_PIX_FMT_NV12) {
					if (render->Filter_Bug)
						filt_frame->pts = filt_frame->pts / 2;	// ffmpeg bug
					EnqueueFB(render, filt_frame);
				} else {
					render->FramesRb[render->FramesWrite] = filt_frame;
					render->FramesWrite = (render->FramesWrite + 1) % VIDEO_SURFACES_MAX;
					atomic_inc(&render->FramesFilled);
				}
			} else {
				usleep(10000);
				goto fillframe;
			}
		}
	}

closing:
	avfilter_graph_free(&render->filter_graph);
	render->Filter_Close = 0;
#ifdef DEBUG
	fprintf(stderr, "FilterHandlerThread: Thread Exit.\n");
#endif
	pthread_cleanup_push(ThreadExitHandler, render);
	pthread_cleanup_pop(1);
	pthread_exit((void *)pthread_self());
}

/**
**	Filter init.
**
**	@retval 0	filter initialised
**	@retval	-1	filter initialise failed
*/
int VideoFilterInit(VideoRender * render, const AVCodecContext * video_ctx,
		AVFrame * frame)
{
	char args[512];
	const char *filter_descr = NULL;
	const AVFilter *buffersrc  = avfilter_get_by_name("buffer");
	const AVFilter *buffersink = avfilter_get_by_name("buffersink");
	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs  = avfilter_inout_alloc();
	render->filter_graph = avfilter_graph_alloc();
	render->Filter_Bug = 0;

	if (frame->interlaced_frame) {
		if (frame->format == AV_PIX_FMT_DRM_PRIME)
			filter_descr = "deinterlace_v4l2m2m";
		else if (frame->format == AV_PIX_FMT_YUV420P) {
			filter_descr = "bwdif=1:-1:0";
			render->Filter_Bug = 1;
		}
	} else if (frame->format == AV_PIX_FMT_YUV420P)
		filter_descr = "scale";
#ifdef DEBUG
	fprintf(stderr, "VideoFilterInit: filter %s\n",
		filter_descr);
#endif

#if LIBAVFILTER_VERSION_INT < AV_VERSION_INT(7,16,100)
	avfilter_register_all();
#endif

	snprintf(args, sizeof(args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		video_ctx->width, video_ctx->height, frame->format,
		video_ctx->time_base.num, video_ctx->time_base.den,
		video_ctx->sample_aspect_ratio.num, video_ctx->sample_aspect_ratio.den);

	if (avfilter_graph_create_filter(&render->buffersrc_ctx, buffersrc, "src",
		args, NULL, render->filter_graph) < 0) {
		fprintf(stderr, "VideoFilterInit: Cannot create buffer source\n");
		goto fail;
	}

	AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
	par->format = AV_PIX_FMT_NONE;
	par->hw_frames_ctx = frame->hw_frames_ctx;
	if (av_buffersrc_parameters_set(render->buffersrc_ctx, par) < 0) {
		fprintf(stderr, "VideoFilterInit: Cannot av_buffersrc_parameters_set\n");
		goto fail;
	}

	av_free(par);

	if (avfilter_graph_create_filter(&render->buffersink_ctx, buffersink, "out",
		NULL, NULL, render->filter_graph) < 0)
			fprintf(stderr, "VideoFilterInit: Cannot create buffer sink\n");

	if (frame->format != AV_PIX_FMT_DRM_PRIME) {
		enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_NV12, AV_PIX_FMT_NONE };
		if (av_opt_set_int_list(render->buffersink_ctx, "pix_fmts", pix_fmts,
				AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN) < 0) {
			fprintf(stderr, "VideoFilterInit: Cannot set output pixel format\n");
			goto fail;
		}
	}

	outputs->name       = av_strdup("in");
	outputs->filter_ctx = render->buffersrc_ctx;
	outputs->pad_idx    = 0;
	outputs->next       = NULL;

	inputs->name       = av_strdup("out");
	inputs->filter_ctx = render->buffersink_ctx;
	inputs->pad_idx    = 0;
	inputs->next       = NULL;

	if ((avfilter_graph_parse_ptr(render->filter_graph, filter_descr,
		&inputs, &outputs, NULL)) < 0) {

		fprintf(stderr, "VideoFilterInit: avfilter_graph_parse_ptr failed\n");
		goto fail;
	}

	if ((avfilter_graph_config(render->filter_graph, NULL)) < 0) {
		fprintf(stderr, "VideoFilterInit: avfilter_graph_config failed\n");
		goto fail;
	}

	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	return 0;

fail:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	if (!render->NoHwDeint) {
#ifdef DEBUG
		fprintf(stderr, "VideoFilterInit: can't config HW Deinterlacer!\n");
#endif
		render->NoHwDeint = 1;
	}

	avfilter_graph_free(&render->filter_graph);

	return -1;
}

///
///	Display a ffmpeg frame
///
///	@param render	video render
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
void VideoRenderFrame(VideoRender * render,
    AVCodecContext * video_ctx, AVFrame * frame)
{
	if (!render->StartCounter) {
		render->timebase = &video_ctx->pkt_timebase;
	}

	if (frame->decode_error_flags || frame->flags & AV_FRAME_FLAG_CORRUPT) {
		fprintf(stderr, "VideoRenderFrame: error_flag or FRAME_FLAG_CORRUPT\n");
	}

	if (render->Closing) {
		av_frame_free(&frame);
		return;
	}

	if (frame->format == AV_PIX_FMT_YUV420P || (frame->interlaced_frame &&
		frame->format == AV_PIX_FMT_DRM_PRIME && !render->NoHwDeint)) {

		if (!FilterThread) {
			if (VideoFilterInit(render, video_ctx, frame)) {
				av_frame_free(&frame);
				return;
			} else {
				pthread_create(&FilterThread, NULL, FilterHandlerThread, render);
				pthread_setname_np(FilterThread, "softhddev deint");
			}
		}

		render->FramesDeintRb[render->FramesDeintWrite] = frame;
		render->FramesDeintWrite = (render->FramesDeintWrite + 1) % VIDEO_SURFACES_MAX;
		atomic_inc(&render->FramesDeintFilled);
	} else {
		if (frame->format == AV_PIX_FMT_DRM_PRIME) {
			render->FramesRb[render->FramesWrite] = frame;
			render->FramesWrite = (render->FramesWrite + 1) % VIDEO_SURFACES_MAX;
			atomic_inc(&render->FramesFilled);
		} else {
			EnqueueFB(render, frame);
		}
	}
}

///
///	Get video clock.
///
///	@param hw_decoder	video hardware decoder
///
///	@note this isn't monoton, decoding reorders frames, setter keeps it
///	monotonic
///
int64_t VideoGetClock(const VideoRender * render)
{
#ifdef DEBUG
	fprintf(stderr, "VideoGetClock: %s\n",
		Timestamp2String(render->pts * 1000 * av_q2d(*render->timebase)));
#endif
	return render->pts;
}

///
///	send start condition to video thread.
///
///	@param hw_render	video hardware render
///
void StartVideo(VideoRender * render)
{
	render->VideoPaused = 0;
	render->StartCounter = 0;
#ifdef DEBUG
	fprintf(stderr, "StartVideo: reset PauseCondition StartCounter %d Closing %d TrickSpeed %d\n",
		render->StartCounter, render->Closing, render->TrickSpeed);
#endif
	pthread_cond_signal(&PauseCondition);
}

///
///	Set closing stream flag.
///
///	@param hw_render	video hardware render
///
void VideoSetClosing(VideoRender * render)
{
#ifdef DEBUG
	fprintf(stderr, "VideoSetClosing: buffers %d StartCounter %d\n",
		render->buffers, render->StartCounter);
#endif

	if (render->buffers){
		render->Closing = 1;

		if (render->VideoPaused) {
			StartVideo(render);
		}


		pthread_mutex_lock(&WaitCleanMutex);
#ifdef DEBUG
		fprintf(stderr, "VideoSetClosing: pthread_cond_wait\n");
#endif
		pthread_cond_wait(&WaitCleanCondition, &WaitCleanMutex);
		pthread_mutex_unlock(&WaitCleanMutex);
#ifdef DEBUG
		fprintf(stderr, "VideoSetClosing: NACH pthread_cond_wait\n");
#endif
	}
	render->StartCounter = 0;
	render->FramesDuped = 0;
	render->FramesDropped = 0;
	render->TrickSpeed = 0;
}

/**
**	Pause video.
*/
void VideoPause(VideoRender * render)
{
#ifdef DEBUG
	fprintf(stderr, "VideoPause:\n");
#endif
	render->VideoPaused = 1;
}

///
///	Set trick play speed.
///
///	@param hw_render	video hardware render
///	@param speed		trick speed (0 = normal)
///
void VideoSetTrickSpeed(VideoRender * render, int speed)
{
#ifdef DEBUG
	fprintf(stderr, "VideoSetTrickSpeed: set trick speed %d\n", speed);
#endif
	render->TrickSpeed = speed;
	if (speed) {
		render->Closing = 0;	// ???
	}

	if (render->VideoPaused) {
		StartVideo(render);
	}
}

/**
**	Play video.
*/
void VideoPlay(VideoRender * render)
{
#ifdef DEBUG
	fprintf(stderr, "VideoPlay:\n");
#endif
	if (render->TrickSpeed) {
		render->TrickSpeed = 0;
	}

	StartVideo(render);
}

///
///	Grab full screen image.
///
///	@param size[out]	size of allocated image
///	@param width[in,out]	width of image
///	@param height[in,out]	height of image
///
uint8_t *VideoGrab(int *size, int *width, int *height, int write_header)
{
    Debug(3, "video: no grab service\n");

    (void)write_header;
    (void)size;
    (void)width;
    (void)height;
    return NULL;
}

///
///	Grab image service.
///
///	@param size[out]	size of allocated image
///	@param width[in,out]	width of image
///	@param height[in,out]	height of image
///
uint8_t *VideoGrabService(int *size, int *width, int *height)
{
    Debug(3, "video: no grab service\n");
	Warning(_("softhddev: grab unsupported\n"));

    (void)size;
    (void)width;
    (void)height;
    return NULL;
}

///
///	Get render statistics.
///
///	@param hw_render	video hardware render
///	@param[out] duped	duped frames
///	@param[out] dropped	dropped frames
///	@param[out] count	number of decoded frames
///
void VideoGetStats(VideoRender * render, int *duped,
    int *dropped, int *counter)
{
    *duped = render->FramesDuped;
    *dropped = render->FramesDropped;
    *counter = render->StartCounter;
}

//----------------------------------------------------------------------------
//	Setup
//----------------------------------------------------------------------------

///
///	Get screen size.
///
///	@param[out] width	video stream width
///	@param[out] height	video stream height
///	@param[out] aspect_num	video stream aspect numerator
///	@param[out] aspect_den	video stream aspect denominator
///
void VideoGetScreenSize(VideoRender * render, int *width, int *height,
		double *pixel_aspect)
{
	*width = render->mode.hdisplay;
	*height = render->mode.vdisplay;
	*pixel_aspect = (double)16 / (double)9;
}

///
///	Set audio delay.
///
///	@param ms	delay in ms
///
void VideoSetAudioDelay(int ms)
{
	VideoAudioDelay = ms;
}

///
///	Initialize video output module.
///
void VideoInit(VideoRender * render)
{
	unsigned int i;

	if (FindDevice(render)){
		fprintf(stderr, "VideoInit: FindDevice() failed\n");
	}

	ReadHWPlatform(render);

	render->bufs[0].width = render->bufs[1].width = 0;
	render->bufs[0].height = render->bufs[1].height = 0;
	render->bufs[0].pix_fmt = render->bufs[1].pix_fmt = DRM_FORMAT_NV12;

	// osd FB
#ifndef USE_GLES
	render->buf_osd.pix_fmt = DRM_FORMAT_ARGB8888;
	render->buf_osd.width = render->mode.hdisplay;
	render->buf_osd.height = render->mode.vdisplay;
	if (SetupFB(render, &render->buf_osd, NULL)){
		fprintf(stderr, "VideoOsdInit: SetupFB FB OSD failed\n");
		Fatal(_("VideoOsdInit: SetupFB FB OSD failed!\n"));
	}
#endif

	// black fb
	render->buf_black.pix_fmt = DRM_FORMAT_NV12;
	render->buf_black.width = 720;
	render->buf_black.height = 576;
	if (SetupFB(render, &render->buf_black, NULL))
		fprintf(stderr, "VideoInit: SetupFB black FB %i x %i failed\n",
			render->buf_black.width, render->buf_black.height);

	for (i = 0; i < render->buf_black.width * render->buf_black.height; ++i) {
		render->buf_black.plane[0][i] = 0x10;
		if (i < render->buf_black.width * render->buf_black.height / 2)
		render->buf_black.plane[1][i] = 0x80;
	}

	// save actual modesetting
	render->saved_crtc = drmModeGetCrtc(render->fd_drm, render->crtc_id);

	drmModeAtomicReqPtr ModeReq;
	const uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
	uint32_t modeID = 0;
	uint32_t prime_plane;
	uint32_t overlay_plane;

	if (render->use_zpos) {
		prime_plane = render->osd_plane;
		overlay_plane = render->video_plane;
	} else {
		prime_plane = render->video_plane;
		overlay_plane = render->osd_plane;
	}

	if (drmModeCreatePropertyBlob(render->fd_drm, &render->mode, sizeof(render->mode), &modeID) != 0)
		fprintf(stderr, "Failed to create mode property blob.\n");
	if (!(ModeReq = drmModeAtomicAlloc()))
		fprintf(stderr, "cannot allocate atomic request (%d): %m\n", errno);

	SetPropertyRequest(ModeReq, render->fd_drm, render->crtc_id,
						DRM_MODE_OBJECT_CRTC, "MODE_ID", modeID);
	SetPropertyRequest(ModeReq, render->fd_drm, render->connector_id,
						DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", render->crtc_id);
	SetPropertyRequest(ModeReq, render->fd_drm, render->crtc_id,
						DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);
	SetPlaneCrtc(render, ModeReq, prime_plane, 0, 0, render->mode.hdisplay, render->mode.vdisplay);

	if (render->use_zpos) {
		// Primary plane
#ifndef USE_GLES
		SetPlaneSrc(render, ModeReq, prime_plane, 0, 0, render->buf_osd.width, render->buf_osd.height);
		SetPlaneFbId(render, ModeReq, prime_plane, render->buf_osd.fb_id);
#else
		// We don't have the buf_osd_gl yet, so we can't set anything
		// Initially move the OSD behind the VIDEO
		SetPlaneZpos(render, ModeReq, render->video_plane, render->zpos_overlay);
		SetPlaneZpos(render, ModeReq, render->osd_plane, render->zpos_primary);
#endif
		// Black Buffer
		SetPlaneCrtc(render, ModeReq, overlay_plane, 0, 0, render->mode.hdisplay, render->mode.vdisplay);
		SetPlaneCrtcId(render, ModeReq, overlay_plane, render->crtc_id);
		SetPlaneSrc(render, ModeReq, overlay_plane, 0, 0, render->buf_black.width, render->buf_black.height);
		SetPlaneFbId(render, ModeReq, overlay_plane, render->buf_black.fb_id);
	}

	if (drmModeAtomicCommit(render->fd_drm, ModeReq, flags, NULL) != 0)
		fprintf(stderr, "cannot set atomic mode (%d): %m\n", errno);

	drmModeAtomicFree(ModeReq);

	render->OsdShown = 0;

	// init variables page flip
//    if (render->ev.page_flip_handler != Drm_page_flip_event) {
		memset(&render->ev, 0, sizeof(render->ev));
//		render->ev.version = DRM_EVENT_CONTEXT_VERSION;
		render->ev.version = 2;
//		render->ev.page_flip_handler = Drm_page_flip_event;
//	}
}

///
///	Cleanup video output module.
///
void VideoExit(VideoRender * render)
{
	VideoThreadExit();

	if (render) {
		// restore saved CRTC configuration
		if (render->saved_crtc){
			drmModeSetCrtc(render->fd_drm, render->saved_crtc->crtc_id, render->saved_crtc->buffer_id,
				render->saved_crtc->x, render->saved_crtc->y, &render->connector_id, 1, &render->saved_crtc->mode);
			drmModeFreeCrtc(render->saved_crtc);
		}

		DestroyFB(render->fd_drm, &render->buf_black);
#ifdef USE_GLES
		if (render->next_bo)
			gbm_bo_destroy(render->next_bo);

		if (render->old_bo)
			gbm_bo_destroy(render->old_bo);
#else
		DestroyFB(render->fd_drm, &render->buf_osd);
#endif

		close(render->fd_drm);
	}
}

const char *VideoGetDecoderName(const char *codec_name)
{
	if (!(strcmp("h264", codec_name)))
		return "h264_v4l2m2m";

	return codec_name;
}

int VideoCodecMode(VideoRender * render)
{
	return render->CodecMode;
}
