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
#include <drm_fourcc.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
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
#include "codec.h"
#include "drm.h"

//----------------------------------------------------------------------------
//	Variables
//----------------------------------------------------------------------------
int VideoAudioDelay;
static int VideoDisplayWidth = 0;
static int VideoDisplayHeight = 0;
static uint32_t VideoDisplayRefresh = 0;

static pthread_cond_t PauseCondition;
static pthread_mutex_t PauseMutex;

static pthread_cond_t WaitCleanCondition;
static pthread_mutex_t WaitCleanMutex;

static pthread_t DecodeThread;		///< video decode thread

static pthread_t FilterThread;

static pthread_t DisplayThread;
static pthread_mutex_t DisplayQueue;

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
			Debug2(L_DRM, "GetPropertyValue: Unable to query property.");

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
		Debug2(L_DRM, "GetPropertyValue: Unable to find value for property \'%s\'.",
			propName);
		return -1;
	}

	return 0;
}

static int SetPlanePropertyRequest(drmModeAtomicReqPtr ModeReq, uint32_t objectID, const char *propName, uint64_t value)
{
	VideoRender *render = (VideoRender *)GetVideoRender();
	if (!render) {
		Fatal("failed to get VideoRender");
	}

	struct plane *obj = NULL;

	if (objectID == render->planes[VIDEO_PLANE]->plane_id)
		obj = render->planes[VIDEO_PLANE];
	else if (objectID == render->planes[OSD_PLANE]->plane_id)
		obj = render->planes[OSD_PLANE];

	if (!obj) {
		Error("SetPlanePropertyRequest: Unable to find plane with id %d", objectID);
		return -EINVAL;
	}

	uint32_t i;
	int id = -1;

	for (i = 0; i < obj->props->count_props; i++) {
		if (strcmp(obj->props_info[i]->name, propName) == 0) {
			id = obj->props_info[i]->prop_id;
			break;
		}
	}

	if (id < 0) {
		Error("SetPlanePropertyRequest: Unable to find value for property \'%s\'.",
			propName);
		return -EINVAL;
	}

	return drmModeAtomicAddProperty(ModeReq, objectID, id, value);
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
			Debug2(L_DRM, "SetPropertyRequest: Unable to query property.");

		if (strcmp(propName, Prop->name) == 0) {
			id = Prop->prop_id;
			drmModeFreeProperty(Prop);
			break;
		}

		drmModeFreeProperty(Prop);
	}

	drmModeFreeObjectProperties(objectProps);

	if (id == 0)
		Debug2(L_DRM, "SetPropertyRequest: Unable to find value for property \'%s\'.",
			propName);

	return drmModeAtomicAddProperty(ModeReq, objectID, id, value);
}

void SetPlaneZpos(drmModeAtomicReqPtr ModeReq, struct plane *plane)
{
	SetPlanePropertyRequest(ModeReq, plane->plane_id, "zpos", plane->properties.zpos);
}

void SetPlane(drmModeAtomicReqPtr ModeReq, struct plane *plane)
{
	SetPlanePropertyRequest(ModeReq, plane->plane_id, "CRTC_ID", plane->properties.crtc_id);
	SetPlanePropertyRequest(ModeReq, plane->plane_id, "FB_ID", plane->properties.fb_id);

	SetPlanePropertyRequest(ModeReq, plane->plane_id, "CRTC_X", plane->properties.crtc_x);
	SetPlanePropertyRequest(ModeReq, plane->plane_id, "CRTC_Y", plane->properties.crtc_y);
	SetPlanePropertyRequest(ModeReq, plane->plane_id, "CRTC_W", plane->properties.crtc_w);
	SetPlanePropertyRequest(ModeReq, plane->plane_id, "CRTC_H", plane->properties.crtc_h);


	SetPlanePropertyRequest(ModeReq, plane->plane_id, "SRC_X", plane->properties.src_x);
	SetPlanePropertyRequest(ModeReq, plane->plane_id, "SRC_Y", plane->properties.src_y);
	SetPlanePropertyRequest(ModeReq, plane->plane_id, "SRC_W", plane->properties.src_w << 16);
	SetPlanePropertyRequest(ModeReq, plane->plane_id, "SRC_H", plane->properties.src_h << 16);
}

void DumpPlaneProperties(struct plane *plane)
{
	Info("DumpPlaneProperties (plane_id = %d):", plane->plane_id);
	Info("  CRTC ID: %lld", plane->properties.crtc_id);
	Info("  FB ID  : %lld", plane->properties.fb_id);
	Info("  CRTC X : %lld", plane->properties.crtc_x);
	Info("  CRTC Y : %lld", plane->properties.crtc_y);
	Info("  CRTC W : %lld", plane->properties.crtc_w);
	Info("  CRTC H : %lld", plane->properties.crtc_h);
	Info("  SRC X  : %lld", plane->properties.src_x);
	Info("  SRC Y  : %lld", plane->properties.src_y);
	Info("  SRC W  : %lld", plane->properties.src_w);
	Info("  SRC H  : %lld", plane->properties.src_h);
	Info("  ZPOS   : %lld", plane->properties.zpos);
}

size_t ReadLineFromFile(char *buf, size_t size, char * file)
{
	FILE *fd = NULL;
	size_t character;

	fd = fopen(file, "r");
	if (fd == NULL) {
		Error("Can't open %s", file);
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
	render->CodecMode = CODEC_BY_ID;
	render->NoHwDeint = 0;

	read_size = ReadLineFromFile(txt_buf, bufsize, "/sys/firmware/devicetree/base/compatible");
	if (!read_size) {
		free((void *)txt_buf);
		return;
	}

	read_ptr = txt_buf;

	while(read_size) {

		if (strstr(read_ptr, "bcm2711")) {
			Debug2(L_DRM, "ReadHWPlatform: bcm2711 found, disable HW deinterlacer");
			render->CodecMode = CODEC_V4L2M2M_H264 | CODEC_NO_MPEG_HW;	// set _v4l2m2m for H264, disable mpeg hw decoder
			render->NoHwDeint = 1;
			break;
		}
		if (strstr(read_ptr, "amlogic")) {
			Debug2(L_DRM, "ReadHWPlatform: amlogic found, disable HW deinterlacer");
			render->CodecMode = CODEC_V4L2M2M_H264;	// set _v4l2m2m for H264
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

static int CheckZpos(VideoRender * render, struct plane *plane, uint64_t zpos)
{
	drmModeAtomicReqPtr ModeReq;
	const uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;

	if (!(ModeReq = drmModeAtomicAlloc()))
		Error("CheckZpos: cannot allocate atomic request (%d): %m", errno);

	plane->properties.zpos = zpos;
	SetPlaneZpos(ModeReq, plane);

	if (drmModeAtomicCommit(render->fd_drm, ModeReq, flags, NULL) != 0) {
		Debug2(L_DRM, "CheckZpos: cannot set atomic mode (%d), don't use zpos change: %m", errno);
		render->use_zpos = 0;
		drmModeAtomicFree(ModeReq);
		return 1;
	}

	drmModeAtomicFree(ModeReq);

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
        Fatal("failed to get VideoRender");
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
    EGL_CHECK(eglGetConfigs(render->eglDisplay, NULL, 0, &count));
    if (count < 1) {
        Fatal("no EGL configs to choose from");
    }

    Debug2(L_OPENGL, "%d EGL configs found", count);

    configs = malloc(count * sizeof(*configs));
    if (!configs)
        Fatal("can't allocate space for EGL configs");

    EGL_CHECK(eglChooseConfig(render->eglDisplay, config_attribute_list, configs, count, &matched));
    if (!matched) {
        Fatal("no EGL configs with appropriate attributes");
    }

    Debug2(L_OPENGL, "%d appropriate EGL configs found, which match attributes", matched);

    for (int i = 0; i < matched; ++i) {
        EGLint gbm_format;
        EGL_CHECK(eglGetConfigAttrib(render->eglDisplay, configs[i], EGL_NATIVE_VISUAL_ID, &gbm_format));

        if (gbm_format == GBM_FORMAT_ARGB8888)
            return configs[i];
    }

    Fatal("no matching gbm config found");
}

PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = NULL;
PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC get_platform_surface = NULL;
#endif

static void get_properties(int fd, int plane_id, struct plane *plane)
{
	uint32_t i;
	plane->props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
	if (!plane->props) {
		Error("could not get %u properties: %s",
			plane_id, strerror(errno));
		return;
	}
	plane->props_info = calloc(plane->props->count_props, sizeof(*plane->props_info)); \
	for (i = 0; i < plane->props->count_props; i++) {
		plane->props_info[i] = drmModeGetProperty(fd, plane->props->props[i]);
	}
}

static void free_properties(struct plane *plane)
{
	if (!plane->props)
		return;

	if (!plane->props_info)
		return;

	for (uint32_t i = 0; i < plane->props->count_props; i++) {
		if (plane->props_info[i]) {
			drmModeFreeProperty(plane->props_info[i]);
		}
	}
	drmModeFreeObjectProperties(plane->props);
	free(plane->props_info);
}

void VideoSetDisplay(const char* resolution)
{
	sscanf(resolution, "%dx%d@%d", &VideoDisplayWidth, &VideoDisplayHeight, &VideoDisplayRefresh);
}

static drmModeConnector *find_drm_connector(int fd, drmModeRes *resources)
{
	drmModeConnector *connector = NULL;
	int i;

	for (i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(fd, resources->connectors[i]);
		if (connector && connector->connection == DRM_MODE_CONNECTED) {
			break;
		}
		drmModeFreeConnector(connector);
		connector = NULL;
	}
	return connector;
}

static int32_t find_crtc_for_encoder(const drmModeRes *resources, const drmModeEncoder *encoder)
{
	int i;

	for (i = 0; i < resources->count_crtcs; i++) {
		const uint32_t crtc_mask = 1 << i;
		const uint32_t crtc_id = resources->crtcs[i];
		if (encoder->possible_crtcs & crtc_mask) {
			return crtc_id;
		}
	}

	return -1;
}

static int get_resources(int fd, drmModeRes **resources)
{
	*resources = drmModeGetResources(fd);
	if (*resources == NULL) {
		Error("FindDevice: cannot retrieve DRM resources (%d): %m", errno);
		return -1;
	}
	return 0;
}

#define MAX_DRM_DEVICES 64
static int find_drm_device(drmModeRes **resources)
{
	drmDevicePtr devices[MAX_DRM_DEVICES] = { NULL };
	int num_devices, fd = -1;

	num_devices = drmGetDevices2(0, devices,MAX_DRM_DEVICES);
	if (num_devices < 0) {
		Error("FindDevice: drmGetDevices2 failed: %s", strerror(-num_devices));
		return fd;
	}

	for (int i = 0; i < num_devices && fd < 0; i++) {
		drmDevicePtr device = devices[i];
		int ret;

		if (!(device->available_nodes & (1 << DRM_NODE_PRIMARY)))
			continue;
		fd = open(device->nodes[DRM_NODE_PRIMARY], O_RDWR);
		if (fd < 0)
			continue;

		if (TestCaps(fd)) {
			close(fd);
			fd = -1;
			continue;
		}

		ret = get_resources(fd, resources);
		if (!ret)
			break;
		close(fd);
		fd = -1;
	}
	drmFreeDevices(devices, num_devices);

	if (fd < 0)
		Error("FindDevice: no drm device found!");

	return fd;
}

static int32_t find_crtc_for_connector(VideoRender *render, const drmModeRes *resources, const drmModeConnector *connector)
{
	int i;

	for (i = 0; i < connector->count_encoders; i++) {
		const uint32_t encoder_id = connector->encoders[i];
		drmModeEncoder *encoder = drmModeGetEncoder(render->fd_drm, encoder_id);

		if (encoder) {
			const int32_t crtc_id = find_crtc_for_encoder(resources, encoder);
			drmModeFreeEncoder(encoder);
			if (crtc_id != 0) {
				return crtc_id;
			}
		}
	}

	return -1;
}

static int init_gbm(VideoRender *render, int w, int h, uint32_t format, uint64_t modifier)
{
	render->gbm_device = gbm_create_device(render->fd_drm);
	if (!render->gbm_device) {
		Error("FindDevice: failed to create gbm device!");
		return -1;
	}

	render->gbm_surface = gbm_surface_create(render->gbm_device, w, h, format, modifier);
	if (!render->gbm_surface) {
		Error("FindDevice: failed to create %d x %d surface bo", w, h);
		return -1;
	}

	return 0;
}

#ifdef USE_GLES
static int init_egl(VideoRender *render)
{
	EGLint iMajorVersion, iMinorVersion;

	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	assert(get_platform_display != NULL);
	PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC get_platform_surface = (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
	assert(get_platform_surface != NULL);

	EGL_CHECK(render->eglDisplay = get_platform_display(EGL_PLATFORM_GBM_KHR, render->gbm_device, NULL));
	if (!render->eglDisplay) {
		Error("FindDevice: failed to get eglDisplay");
		return -1;
	}

	if (!eglInitialize(render->eglDisplay, &iMajorVersion, &iMinorVersion)) {
		Error("FindDevice: eglInitialize failed");
		return -1;
	}

	Debug2(L_OPENGL, "FindDevice: Using display %p with EGL version %d.%d", render->eglDisplay, iMajorVersion, iMinorVersion);
	EGL_CHECK(Debug2(L_OPENGL, "  EGL Version: \"%s\"", eglQueryString(render->eglDisplay, EGL_VERSION)));
	EGL_CHECK(Debug2(L_OPENGL, "  EGL Vendor: \"%s\"", eglQueryString(render->eglDisplay, EGL_VENDOR)));
	EGL_CHECK(Debug2(L_OPENGL, "  EGL Extensions: \"%s\"", eglQueryString(render->eglDisplay, EGL_EXTENSIONS)));
	EGL_CHECK(Debug2(L_OPENGL, "  EGL APIs: \"%s\"", eglQueryString(render->eglDisplay, EGL_CLIENT_APIS)));

	EGLConfig eglConfig = get_config();

	EGL_CHECK(eglBindAPI(EGL_OPENGL_ES_API));
	EGL_CHECK(render->eglContext = eglCreateContext(render->eglDisplay, eglConfig, EGL_NO_CONTEXT, context_attribute_list));
	if (!render->eglContext) {
		Error("FindDevice: failed to create eglContext");
		return -1;
	}

	EGL_CHECK(render->eglSurface = get_platform_surface(render->eglDisplay, eglConfig, render->gbm_surface, NULL));
	if (render->eglSurface == EGL_NO_SURFACE) {
		Error("FindDevice: failed to create eglSurface");
		return -1;
	}

	EGLint s_width, s_height;
	EGL_CHECK(eglQuerySurface(render->eglDisplay, render->eglSurface, EGL_WIDTH, &s_width));
	EGL_CHECK(eglQuerySurface(render->eglDisplay, render->eglSurface, EGL_HEIGHT, &s_height));

	Debug2(L_OPENGL, "FindDevice: EGLSurface %p on EGLDisplay %p for %d x %d BO created", render->eglSurface, render->eglDisplay, s_width, s_height);

	render->GlInit = 1;
	Info("EGL context initialized");

	return 0;
}
#endif

static int FindDevice(VideoRender * render)
{
	drmModeRes *resources;
	drmModeConnector *connector;
	drmModeEncoder *encoder = NULL;
	drmModeModeInfo *mode = NULL;
	drmModePlane *plane;
	drmModePlaneRes *plane_res;
	int i;
	uint32_t j, k;

	// find a drm device
	render->fd_drm = find_drm_device(&resources);
	if (render->fd_drm < 0) {
		Error("FindDevice: Could not open device!");
		return -1;
	}

	Debug2(L_DRM, "FindDevice: DRM have %i connectors, %i crtcs, %i encoders",
		resources->count_connectors, resources->count_crtcs,
		resources->count_encoders);

	// find a connected connector
	connector = find_drm_connector(render->fd_drm, resources);
	if (!connector) {
		Error("FindDevice: cannot retrieve DRM connector (%d): %m", errno);
		return -errno;
	}
	render->connector_id = connector->connector_id;

	// find a user requested mode
	if (VideoDisplayWidth) {
		for (i = 0; i < connector->count_modes; i++) {
			drmModeModeInfo *current_mode = &connector->modes[i];
			if(current_mode->hdisplay == VideoDisplayWidth && current_mode->vdisplay == VideoDisplayHeight &&
			   current_mode->vrefresh == VideoDisplayRefresh && !(current_mode->flags & DRM_MODE_FLAG_INTERLACE)) {
				mode = current_mode;
				Debug2(L_DRM, "FindDevice: Use user requested mode: %dx%d@%d", mode->hdisplay, mode->vdisplay, mode->vrefresh);
				break;
			}
		}
		if (!mode)
			Warning("FindDevice: User requested mode not found, try default modes");
	}

	// find preferred mode or the highest resolution mode
	if (!mode) {
		int area;
		for (i = 0, area = 0; i < connector->count_modes; i++) {
			drmModeModeInfo *current_mode = &connector->modes[i];
			if (current_mode->type & DRM_MODE_TYPE_PREFERRED) {
				mode = current_mode;
				break;
			}

			int current_area = current_mode->hdisplay * current_mode->vdisplay;
			if (current_area > area) {
				mode = current_mode;
				area = current_area;
			}
		}
		if (mode)
			Debug2(L_DRM, "FindDevice: Use %s: %dx%d@%d",
				mode->type & DRM_MODE_TYPE_PREFERRED ? "preferred default mode" : "mode with the highest resolution",
				mode->hdisplay, mode->vdisplay, mode->vrefresh);
	}

	if (!mode) {
		Error("FindDevice: No monitor mode found! Give up!");
		return -1;
	}

	memcpy(&render->mode, mode, sizeof(drmModeModeInfo));

	// find encoder
	for (i = 0; i < resources->count_encoders; i++) {
		encoder = drmModeGetEncoder(render->fd_drm, resources->encoders[i]);
		if (encoder->encoder_id == connector->encoder_id)
			break;
		drmModeFreeEncoder(encoder);
		encoder = NULL;
	}

	if (encoder) {
		render->crtc_id = encoder->crtc_id;
		Debug2(L_DRM, "FindDevice: have encoder, render->crtc_id %d", render->crtc_id);
	} else {
		int32_t crtc_id = find_crtc_for_connector(render, resources, connector);
		if (crtc_id == -1) {
			Error("FindDevice: No crtc found!");
			return -errno;
		}

		render->crtc_id = crtc_id;
		Debug2(L_DRM, "FindDevice: have no encoder, render->crtc_id %d", render->crtc_id);
	}

	for (i = 0; i < resources->count_crtcs; i++) {
		if (resources->crtcs[i] == render->crtc_id) {
			render->crtc_index = i;
			break;
		}
	}

	Info("FindDevice: Using Monitor Mode %dx%d@%d, crtc_id %d crtc_idx %d",
		render->mode.hdisplay, render->mode.vdisplay, render->mode.vrefresh, render->crtc_id, render->crtc_index);

	drmModeFreeConnector(connector);


	// find planes
	if ((plane_res = drmModeGetPlaneResources(render->fd_drm)) == NULL) {
		Error("FindDevice: cannot retrieve PlaneResources (%d): %m", errno);
		return -1;
	}

	// allocate local plane structs
	for (i = 0; i < MAX_PLANES; i++) {
		render->planes[i] = calloc(1, sizeof(struct plane));
	}

	// test and list the local plane structs
	struct plane best_primary_video_plane = { .plane_id = 0}; // is the NV12 capable primary plane with the lowest plane_id
	struct plane best_overlay_video_plane = { .plane_id = 0}; // is the NV12 capable overlay plane with the lowest plane_id
	struct plane best_primary_osd_plane = { .plane_id = 0};   // is the AR24 capable primary plane with the highest plane_id
	struct plane best_overlay_osd_plane = { .plane_id = 0};   // is the AR24 capable overlay plane with the highest plane_id

	for (j = 0; j < plane_res->count_planes; j++) {
		plane = drmModeGetPlane(render->fd_drm, plane_res->planes[j]);

		if (plane == NULL) {
			Error("FindDevice: cannot query DRM-KMS plane %d", j);
			continue;
		}

		uint64_t type;
		uint64_t zpos;
		char pixelformats[256];

		if (plane->possible_crtcs & (1 << render->crtc_index)) {
			if (GetPropertyValue(render->fd_drm, plane_res->planes[j],
					     DRM_MODE_OBJECT_PLANE, "type", &type)) {
				Debug2(L_DRM, "FindDevice: Failed to get property 'type'");
			}
			if (GetPropertyValue(render->fd_drm, plane_res->planes[j],
					     DRM_MODE_OBJECT_PLANE, "zpos", &zpos)) {
				Debug2(L_DRM, "FindDevice: Failed to get property 'zpos'");
			} else {
				render->use_zpos = 1;
			}

			Debug2(L_DRM, "FindDevice: %s: id %i possible_crtcs %i",
				(type == DRM_PLANE_TYPE_PRIMARY) ? "PRIMARY " :
				(type == DRM_PLANE_TYPE_OVERLAY) ? "OVERLAY " :
				(type == DRM_PLANE_TYPE_CURSOR) ? "CURSOR " : "UNKNOWN",
				plane->plane_id, plane->possible_crtcs);
			strcpy(pixelformats, "            ");

			// test pixel format and plane caps
			for (k = 0; k < plane->count_formats; k++) {
				if (encoder->possible_crtcs & plane->possible_crtcs) {
					char tmp[10];
					switch (plane->formats[k]) {
						case DRM_FORMAT_NV12:
							snprintf(tmp, sizeof(tmp), " %4.4s", (char *)&plane->formats[k]);
							strcat(pixelformats, tmp);
							if (type == DRM_PLANE_TYPE_PRIMARY && !best_primary_video_plane.plane_id) {
								best_primary_video_plane.plane_id = plane->plane_id;
								best_primary_video_plane.type = type;
								best_primary_video_plane.properties.zpos = zpos;
								strcat(pixelformats, "!  ");
							}
							if (type == DRM_PLANE_TYPE_OVERLAY && !best_overlay_video_plane.plane_id) {
								best_overlay_video_plane.plane_id = plane->plane_id;
								best_overlay_video_plane.type = type;
								best_overlay_video_plane.properties.zpos = zpos;
								strcat(pixelformats, "!  ");
							}
							break;
						case DRM_FORMAT_ARGB8888:
							snprintf(tmp, sizeof(tmp), " %4.4s", (char *)&plane->formats[k]);
							strcat(pixelformats, tmp);
							if (type == DRM_PLANE_TYPE_PRIMARY) {
								best_primary_osd_plane.plane_id = plane->plane_id;
								best_primary_osd_plane.type = type;
								best_primary_osd_plane.properties.zpos = zpos;
								strcat(pixelformats, "!  ");
							}
							if (type == DRM_PLANE_TYPE_OVERLAY) {
								best_overlay_osd_plane.plane_id = plane->plane_id;
								best_overlay_osd_plane.type = type;
								best_overlay_osd_plane.properties.zpos = zpos;
								strcat(pixelformats, "!  ");
							}
							break;
						default:
							break;
					}
				}
			}
			Debug2(L_DRM, "%s", pixelformats);
		}
		drmModeFreePlane(plane);
	}

	// debug output
	if (best_primary_video_plane.plane_id) {
		Debug2(L_DRM, "FindDevice: best_primary_video_plane: plane_id %d, type %s, zpos %lld",
			best_primary_video_plane.plane_id, best_primary_video_plane.type == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "OVERLAY", best_primary_video_plane.properties.zpos);
	}
	if (best_overlay_video_plane.plane_id) {
		Debug2(L_DRM, "FindDevice: best_overlay_video_plane: plane_id %d, type %s, zpos %lld",
			best_overlay_video_plane.plane_id, best_overlay_video_plane.type == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "OVERLAY", best_overlay_video_plane.properties.zpos);
	}
	if (best_primary_osd_plane.plane_id) {
		Debug2(L_DRM, "FindDevice: best_primary_osd_plane: plane_id %d, type %s, zpos %lld",
			best_primary_osd_plane.plane_id, best_primary_osd_plane.type == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "OVERLAY", best_primary_osd_plane.properties.zpos);
	}
	if (best_overlay_osd_plane.plane_id) {
		Debug2(L_DRM, "FindDevice: best_overlay_osd_plane: plane_id %d, type %s, zpos %lld",
			best_overlay_osd_plane.plane_id, best_overlay_osd_plane.type == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "OVERLAY", best_overlay_osd_plane.properties.zpos);
	}

	// See which planes we should use
	if (best_primary_video_plane.plane_id && best_overlay_osd_plane.plane_id) {
		render->planes[VIDEO_PLANE]->plane_id = best_primary_video_plane.plane_id;
		render->planes[VIDEO_PLANE]->type = best_primary_video_plane.type;
		render->planes[VIDEO_PLANE]->properties.zpos = render->zpos_primary = best_primary_video_plane.properties.zpos;
		render->planes[OSD_PLANE]->plane_id = best_overlay_osd_plane.plane_id;
		render->planes[OSD_PLANE]->type = best_overlay_osd_plane.type;
		render->planes[OSD_PLANE]->properties.zpos = render->zpos_overlay = best_overlay_osd_plane.properties.zpos;
	} else if (best_overlay_video_plane.plane_id && best_primary_osd_plane.plane_id) {
		render->planes[VIDEO_PLANE]->plane_id = best_overlay_video_plane.plane_id;
		render->planes[VIDEO_PLANE]->type = best_overlay_video_plane.type;
		render->planes[VIDEO_PLANE]->properties.zpos = render->zpos_overlay = best_overlay_video_plane.properties.zpos;
		render->planes[OSD_PLANE]->plane_id = best_primary_osd_plane.plane_id;
		render->planes[OSD_PLANE]->type = best_primary_osd_plane.type;
		render->planes[OSD_PLANE]->properties.zpos = render->zpos_primary = best_primary_osd_plane.properties.zpos;
		render->use_zpos = 1;
	} else {
		Error("FindDevice: No suitable planes found!");
		return -1;
	}

	// fill the plane's properties to speed up SetPropertyRequest later
	get_properties(render->fd_drm, render->planes[VIDEO_PLANE]->plane_id, render->planes[VIDEO_PLANE]);
	get_properties(render->fd_drm, render->planes[OSD_PLANE]->plane_id, render->planes[OSD_PLANE]);

	// Check, if we can set z-order (meson and rpi have fixed z-order, which cannot be changed)
	if (render->use_zpos && CheckZpos(render, render->planes[VIDEO_PLANE], render->planes[VIDEO_PLANE]->properties.zpos)) {
		render->use_zpos = 0;
	}
	if (render->use_zpos && CheckZpos(render, render->planes[OSD_PLANE], render->planes[OSD_PLANE]->properties.zpos)) {
		render->use_zpos = 0;
	}

	// render->use_zpos was set, if Video is on OVERLAY, and Osd is on PRIMARY
	// Check if the OVERLAY plane really got a higher zpos than the PRIMARY plane
	// If not, change their zpos values or hardcode them to
	// 1 OVERLAY (Video)
	// 0 PRIMARY (Osd)
	if (render->use_zpos && render->zpos_overlay <= render->zpos_primary) {
		char str_zpos[256];
		strcpy(str_zpos, "FindDevice: zpos values are wrong, so ");
		if (render->zpos_overlay == render->zpos_primary) {
			// is this possible?
			strcat(str_zpos, "hardcode them to 0 and 1, because they are equal");
			render->zpos_primary = 0;
			render->zpos_overlay = 1;
		} else {
			strcat(str_zpos, "switch them");
			uint64_t zpos_tmp = render->zpos_primary;
			render->zpos_primary = render->zpos_overlay;
			render->zpos_overlay = zpos_tmp;
		}
		Debug2(L_DRM, "%s", str_zpos);
	}
	drmModeFreePlaneResources(plane_res);
	drmModeFreeEncoder(encoder);
	drmModeFreeResources(resources);

	Info("FindDevice: DRM setup - CRTC: %i video_plane: %i (%s %lld) osd_plane: %i (%s %lld) use_zpos: %d",
		render->crtc_id, render->planes[VIDEO_PLANE]->plane_id,
		render->planes[VIDEO_PLANE]->type == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "OVERLAY",
		render->planes[VIDEO_PLANE]->properties.zpos,
		render->planes[OSD_PLANE]->plane_id,
		render->planes[OSD_PLANE]->type == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "OVERLAY",
		render->planes[OSD_PLANE]->properties.zpos,
		render->use_zpos);

#ifdef USE_GLES
	if (DisableOglOsd)
		return 0;

	// init gbm
	int w, h;
	double pixel_aspect;
	GetScreenSize(&w, &h, &pixel_aspect);

	if (init_gbm(render, w, h, DRM_FORMAT_ARGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING)) {
		Error("FindDevice: failed to init gbm device and surface!");
		return -1;
	}

	// init egl
	if (init_egl(render)) {
		Error("FindDevice: failed to init egl!");
		return -1;
	}
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
			Debug2(L_DRM, "drm_get_buf_from_bo: Using modifier %" PRIx64 "", modifiers[0]);
		}

		// Add FB
		ret = drmModeAddFB2WithModifiers(render->fd_drm, buf->width, buf->height, buf->pix_fmt,
			buf->handle, buf->pitch, buf->offset, modifiers, &buf->fb_id, mod_flags);
	}

	if (ret) {
		if (mod_flags)
			Debug2(L_DRM, "drm_get_buf_from_bo: Modifiers failed!");

		memcpy(buf->handle, (uint32_t [4]){ gbm_bo_get_handle(bo).u32, 0, 0, 0}, 16);
		memcpy(buf->pitch, (uint32_t [4]){ gbm_bo_get_stride(bo), 0, 0, 0}, 16);
		memset(buf->offset, 0, 16);
		ret = drmModeAddFB2(render->fd_drm, buf->width, buf->height, buf->pix_fmt,
			buf->handle, buf->pitch, buf->offset, &buf->fb_id, 0);
	}

	if (ret) {
		Fatal("drm_get_buf_from_bo: cannot create framebuffer (%d): %m", errno);
		free(buf);
		return NULL;
	}

	Debug2(L_DRM, "drm_get_buf_from_bo: New GL buffer %d x %d pix_fmt %4.4s fb_id %d",
		buf->width, buf->height, (char *)&buf->pix_fmt, buf->fb_id);

	gbm_bo_set_user_data(bo, buf, drm_fb_destroy_callback);
	return buf;
}
#endif

static const struct format_info format_info_array[] = {
	{ DRM_FORMAT_NV12, "NV12", 2, { { 8, 1, 1 }, { 16, 2, 2 } }, },
	{ DRM_FORMAT_YUV420, "YU12", 3, { { 8, 1, 1 }, { 8, 2, 2 }, {8, 2, 2 } }, },
	{ DRM_FORMAT_ARGB8888, "AR24", 1, { { 32, 1, 1 } }, },
};

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
static const struct format_info *find_format(uint32_t format)
{
	for (int i = 0; i < (int)ARRAY_SIZE(format_info_array); i++) {
		if (format == format_info_array[i].format)
			return &format_info_array[i];
	}
	return NULL;
}

static int SetupFB(VideoRender * render, struct drm_buf *buf,
			AVDRMFrameDescriptor *primedata, int renderbuffer)
{
	uint64_t modifier[4] = { 0, 0, 0, 0 };
	uint32_t mod_flags = 0;
	buf->handle[0] = buf->handle[1] = buf->handle[2] = buf->handle[3] = 0;
	buf->pitch[0] = buf->pitch[1] = buf->pitch[2] = buf->pitch[3] = 0;
	buf->offset[0] = buf->offset[1] = buf->offset[2] = buf->offset[3] = 0;

	if (primedata) {
		// we have no DRM objects yet, so return
		if (!primedata->nb_objects) {
			Warning("SetupFB: No primedata objects available!");
			return 1;
		}

		if (!render->buffers) {
			for (int object = 0; object < primedata->nb_objects; object++) {

				Debug2(L_DRM, "SetupFB: PRIMEDATA %d x %d nb_objects %i Handle %i size %zu modifier %" PRIx64 "",
					buf->width, buf->height, primedata->nb_objects, primedata->objects[object].fd,
					primedata->objects[object].size, primedata->objects[object].format_modifier);
			}
			for (int layer = 0; layer < primedata->nb_layers; layer++) {
				Debug2(L_DRM, "SetupFB: PRIMEDATA nb_layers %d nb_planes %d pix_fmt %4.4s",
					primedata->nb_layers, primedata->layers[layer].nb_planes,
					(char *)&primedata->layers[layer].format);

				for (int plane = 0; plane < primedata->layers[layer].nb_planes; plane++) {
					Debug2(L_DRM, "SetupFB: PRIMEDATA nb_planes %d object_index %i",
						primedata->layers[layer].nb_planes, primedata->layers[layer].planes[plane].object_index);
				}
			}
		}

		buf->pix_fmt = primedata->layers[0].format;
		buf->num_planes = primedata->layers[0].nb_planes;

		for (int plane = 0; plane < primedata->layers[0].nb_planes; plane++) {

			if (drmPrimeFDToHandle(render->fd_drm,
				primedata->objects[primedata->layers[0].planes[plane].object_index].fd, &buf->handle[plane])) {

				Error("SetupFB: PRIMEDATA Failed to retrieve the Prime Handle %i size %zu (%d): %m",
					primedata->objects[primedata->layers[0].planes[plane].object_index].fd,
					primedata->objects[primedata->layers[0].planes[plane].object_index].size, errno);
				return -errno;
			}

			buf->pitch[plane] = primedata->layers[0].planes[plane].pitch;
			buf->offset[plane] = primedata->layers[0].planes[plane].offset;

			if (primedata->objects[primedata->layers[0].planes[plane].object_index].format_modifier) {
				modifier[plane] =
					primedata->objects[primedata->layers[0].planes[plane].object_index].format_modifier;
				mod_flags = DRM_MODE_FB_MODIFIERS;
			}
		}
	} else {
		const struct format_info *format_info = find_format(buf->pix_fmt);
		if (!format_info) {
			Error("SetupFB: No suitable format found!");
			return 1;
		}

		buf->num_planes = format_info->num_planes;

		for (int i = 0; i < format_info->num_planes; i++) {
			const struct format_plane_info *plane_info = &format_info->planes[i];

			struct drm_mode_create_dumb creq = {
				.width = buf->width / plane_info->xsub,
				.height = buf->height / plane_info->ysub,
				.bpp = plane_info->bitspp,
			};

			if (drmIoctl(render->fd_drm, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0){
				Error("SetupFB: cannot create dumb buffer %dx%d@%d (%d): %m",
					creq.width, creq.height, creq.bpp, errno);
				return -errno;
			}

			buf->handle[i] = creq.handle;
			buf->pitch[i] = creq.pitch;
			buf->size[i] = creq.size;

			struct drm_mode_map_dumb mreq;
			memset(&mreq, 0, sizeof(struct drm_mode_map_dumb));
			mreq.handle = buf->handle[i];

			if (drmIoctl(render->fd_drm, DRM_IOCTL_MODE_MAP_DUMB, &mreq)){
				Error("SetupFB: cannot prepare dumb buffer for mapping (%d): %m", errno);
				return -errno;
			}

			buf->plane[i] = mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, render->fd_drm, mreq.offset);

			if (buf->plane[i] == MAP_FAILED) {
				Error("SetupFB: cannot map dumb buffer (%d): %m", errno);
				return -errno;
			}

			memset(buf->plane[i], 0, buf->size[i]);
		}
	}

	int ret = -1;
	ret = drmModeAddFB2WithModifiers(render->fd_drm, buf->width, buf->height, buf->pix_fmt,
					 buf->handle, buf->pitch, buf->offset, modifier, &buf->fb_id, mod_flags);

	if (ret) {
		if (mod_flags)
			Error("SetupFB: cannot create modifiers framebuffer (%d): %m", errno);

		ret = drmModeAddFB2(render->fd_drm, buf->width, buf->height, buf->pix_fmt,
			buf->handle, buf->pitch, buf->offset, &buf->fb_id, 0);
	}

	if (ret)
		Fatal("SetupFB: cannot create framebuffer (%d): %m", errno);

	render->buffers += renderbuffer;
	Debug2(L_DRM, "SetupFB: Added %sFB fb_id %d width %d height %d pix_fmt %4.4s, render->buffers %d",
		primedata ? "primedata " : "", buf->fb_id, buf->width, buf->height, (char *)&buf->pix_fmt, renderbuffer ? render->buffers : 0);

	return 0;
}

static void DestroyFB(int fd_drm, struct drm_buf *buf)
{
	struct drm_mode_destroy_dumb dreq;

//	Debug("DestroyFB: destroy FB %d", buf->fb_id);

	for (int i = 0; i < buf->num_planes; i++) {
		if (buf->plane[i]) {
			if (munmap(buf->plane[i], buf->size[i]))
				Error("DestroyFB: failed unmap FB (%d): %m", errno);
		}
	}

	if (drmModeRmFB(fd_drm, buf->fb_id) < 0)
		Error("DestroyFB: cannot rm FB (%d): %m", errno);

	for (int i = 0; i < buf->num_planes; i++) {
		if (buf->plane[i]) {
			memset(&dreq, 0, sizeof(dreq));
			dreq.handle = buf->handle[i];

			if (drmIoctl(fd_drm, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq) < 0)
				Error("DestroyFB: cannot destroy dumb buffer (%d): %m", errno);
			buf->handle[i] = 0;

			if (buf->fd_prime) {
				if (close(buf->fd_prime))
					Error("DestroyFB: failed close fd prime %d (%d): %m", buf->fd_prime, errno);
				buf->fd_prime = 0;
			}
		}

		buf->plane[i] = 0;
		buf->size[i] = 0;
		buf->pitch[i] = 0;
		buf->offset[i] = 0;
	}

	if (buf->handle[0]) {
		if (drmIoctl(fd_drm, DRM_IOCTL_GEM_CLOSE, &buf->handle[0]) < 0)
			Error("DestroyFB: cannot close GEM (%d): %m", errno);
	}

	buf->width = 0;
	buf->height = 0;
	buf->fb_id = 0;
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

	Debug("CleanDisplayThread: DRM cleaned.");
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

	drmModeAtomicReqPtr ModeReq;
	uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT;
	if (!(ModeReq = drmModeAtomicAlloc())) {
		Error("Frame2Display: cannot allocate atomic request (%d): %m", errno);
		return;
	}

	if (render->Closing) {
closing:
		// set a black FB
		Debug("Frame2Display: closing, set a black FB");
		buf = &render->buf_black;
		goto page_flip;
	}

dequeue:
	while (!atomic_read(&render->FramesFilled)) {
		if (render->Closing)
			goto closing;
		// We had draw activity on the osd buffer
		if (render->buf_osd && render->buf_osd->dirty) {
			Debug2(L_DRM, "Frame2Display: no video, set a black FB instead");
			buf = &render->buf_black;
			goto page_flip;
		}
		usleep(10000);
	}

	frame = render->FramesRb[render->FramesRead];
	render->FramesRead = (render->FramesRead + 1) % VIDEO_SURFACES_MAX;
	atomic_dec(&render->FramesFilled);
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

		if (SetupFB(render, buf, primedata, 1)) {
			av_frame_free(&frame);
			return;
		}
	}

	render->pts = frame->pts;
	video_pts = frame->pts * 1000 * av_q2d(*render->timebase);
	if(!render->StartCounter && !render->Closing && !render->TrickSpeed) {
		Debug("Frame2Display: start PTS %s", Timestamp2String(video_pts));
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
		Debug2(L_AV_SYNC, "FrameDropped (drop %d, dup %d) Pkts %d deint %d Frames %d AudioUsedBytes %d audio %s video %s Delay %dms diff %dms",
			render->FramesDropped, render->FramesDuped,
			VideoGetPackets(), atomic_read(&render->FramesDeintFilled),
			atomic_read(&render->FramesFilled), AudioUsedBytes(), Timestamp2String(audio_pts),
			Timestamp2String(video_pts), VideoAudioDelay, diff);
		av_frame_free(&frame);

		if (!render->StartCounter)
			render->StartCounter++;
		goto dequeue;
	}

	if (diff > 35 && !render->TrickSpeed && !(abs(diff) > 5000)) {
		render->FramesDuped++;
		Debug2(L_AV_SYNC, "FrameDuped (drop %d, dup %d) Pkts %d deint %d Frames %d AudioUsedBytes %d audio %s video %s Delay %dms diff %dms",
			render->FramesDropped, render->FramesDuped,
			VideoGetPackets(), atomic_read(&render->FramesDeintFilled),
			atomic_read(&render->FramesFilled), AudioUsedBytes(), Timestamp2String(audio_pts),
			Timestamp2String(video_pts), VideoAudioDelay, diff);
		usleep(20000);
		goto audioclock;
	}

	if (abs(diff) > 5000) {	// more than 5s
		if (video_pts)
			Debug2(L_AV_SYNC, "More then 5s Pkts %d deint %d Frames %d AudioUsedBytes %d audio %s video %s Delay %dms diff %dms",
				VideoGetPackets(), atomic_read(&render->FramesDeintFilled),
				atomic_read(&render->FramesFilled), AudioUsedBytes(), Timestamp2String(audio_pts),
				Timestamp2String(video_pts), VideoAudioDelay, diff);
		else
			Debug2(L_AV_SYNC, "Video frame with AV_NOPTS_VALUE arrived ... Pkts %d deint %d Frames %d AudioUsedBytes %d audio %s video %s Delay %dms diff %dms",
				VideoGetPackets(), atomic_read(&render->FramesDeintFilled),
				atomic_read(&render->FramesFilled), AudioUsedBytes(), Timestamp2String(audio_pts),
				Timestamp2String(video_pts), VideoAudioDelay, diff);
				av_frame_free(&frame);
	}

	if (!render->TrickSpeed)
		render->StartCounter++;

	if (render->TrickSpeed)
		usleep(20000 * render->TrickSpeed);

	buf->frame = frame;

// handle the video plane
page_flip:
	render->act_buf = buf;
	// Get video size and position and set crtc rect
	uint64_t DispWidth = render->mode.hdisplay;
	uint64_t DispHeight = render->mode.vdisplay;
	uint64_t DispX = 0;
	uint64_t DispY = 0;

	if (render->video.is_scaled) {
		DispWidth = (uint64_t)render->video.width;
		DispHeight = (uint64_t)render->video.height;
		DispX = (uint64_t)render->video.x;
		DispY = (uint64_t)render->video.y;
	}

	uint64_t PicWidth = DispWidth;
	uint64_t PicHeight = DispHeight;

	// resize frame to fit into video area/ screen and keep the aspect ratio
	if (frame) {
		// use frame->sample_aspect_ratio of 1.0f if undefined (0.0f), otherwise we have division by 0
		double frame_sar = av_q2d(frame->sample_aspect_ratio) ? av_q2d(frame->sample_aspect_ratio) : 1.0f;

		// frame b*h < display b*h, e.g. fit a 4:3 frame into 16:9 display or area
		if (1000 * DispWidth / DispHeight > 1000 * frame->width / frame->height * frame_sar) {
			PicWidth = DispHeight * frame->width / frame->height * frame_sar;
			if (PicWidth <= 0 || PicWidth > DispWidth) {
				PicWidth = DispWidth;
			}
		// frame b*h >= display b*h, e.g. fit a 16:9 frame into 4:3 display or area
		} else {
			PicHeight = DispWidth * frame->height / frame->width / frame_sar;
			if (PicHeight <= 0 || PicHeight > DispHeight) {
				PicHeight = DispHeight;
			}
		}
	}

	render->planes[VIDEO_PLANE]->properties.crtc_id = render->crtc_id;
	render->planes[VIDEO_PLANE]->properties.fb_id = buf->fb_id;
	render->planes[VIDEO_PLANE]->properties.crtc_x = DispX + (DispWidth - PicWidth) / 2;
	render->planes[VIDEO_PLANE]->properties.crtc_y = DispY + (DispHeight - PicHeight) / 2;
	render->planes[VIDEO_PLANE]->properties.crtc_w = PicWidth;
	render->planes[VIDEO_PLANE]->properties.crtc_h = PicHeight;
	render->planes[VIDEO_PLANE]->properties.src_x = 0;
	render->planes[VIDEO_PLANE]->properties.src_y = 0;
	render->planes[VIDEO_PLANE]->properties.src_w = buf->width;
	render->planes[VIDEO_PLANE]->properties.src_h = buf->height;

	SetPlane(ModeReq, render->planes[VIDEO_PLANE]);

// handle the osd plane
	// We had draw activity on the osd buffer
	if (render->buf_osd && render->buf_osd->dirty) {
		if (render->use_zpos) {
			render->planes[VIDEO_PLANE]->properties.zpos = render->OsdShown ? render->zpos_primary : render->zpos_overlay;
			render->planes[OSD_PLANE]->properties.zpos = render->OsdShown ? render->zpos_overlay : render->zpos_primary;
			SetPlaneZpos(ModeReq, render->planes[VIDEO_PLANE]);
			SetPlaneZpos(ModeReq, render->planes[OSD_PLANE]);

			Debug2(L_DRM, "Frame2Display: SetPlaneZpos: video->plane_id %d -> zpos %lld, osd->plane_id %d -> zpos %lld",
				render->planes[VIDEO_PLANE]->plane_id, render->planes[VIDEO_PLANE]->properties.zpos,
				render->planes[OSD_PLANE]->plane_id, render->planes[OSD_PLANE]->properties.zpos);
		}

		render->planes[OSD_PLANE]->properties.crtc_id = render->crtc_id;
		render->planes[OSD_PLANE]->properties.fb_id = render->buf_osd->fb_id;
		render->planes[OSD_PLANE]->properties.crtc_x = 0;
		render->planes[OSD_PLANE]->properties.crtc_y = 0;
		render->planes[OSD_PLANE]->properties.crtc_w = render->OsdShown ? render->buf_osd->width : 0;
		render->planes[OSD_PLANE]->properties.crtc_h = render->OsdShown ? render->buf_osd->height : 0;
		render->planes[OSD_PLANE]->properties.src_x = 0;
		render->planes[OSD_PLANE]->properties.src_y = 0;
		render->planes[OSD_PLANE]->properties.src_w = render->OsdShown ? render->buf_osd->width : 0;
		render->planes[OSD_PLANE]->properties.src_h = render->OsdShown ? render->buf_osd->height : 0;

		SetPlane(ModeReq, render->planes[OSD_PLANE]);
		Debug2(L_DRM, "Frame2Display: SetPlane OSD (fb = %lld)", render->planes[OSD_PLANE]->properties.fb_id);
		render->buf_osd->dirty = 0;
	}

	if (drmModeAtomicCommit(render->fd_drm, ModeReq, flags, NULL) != 0) {
		DumpPlaneProperties(render->planes[OSD_PLANE]);
		if (render->act_buf)
			DumpPlaneProperties(render->planes[VIDEO_PLANE]);

		drmModeAtomicFree(ModeReq);
		Error("Frame2Display: page flip failed (%d): %m", errno);
	}

	drmModeAtomicFree(ModeReq);

	if (render->lastframe)
		av_frame_free(&render->lastframe);
	if (render->act_buf && (render->act_buf->fb_id != render->buf_black.fb_id))
		render->lastframe = render->act_buf->frame;
}

///
///	Display a video frame.
///
static void *DisplayHandlerThread(void * arg)
{
	VideoRender * render = (VideoRender *)arg;

	pthread_setcancelstate (PTHREAD_CANCEL_ENABLE, NULL);
	pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

	while (1) {
		pthread_testcancel();

		if (render->VideoPaused) {
			pthread_mutex_lock(&PauseMutex);
			pthread_cond_wait(&PauseCondition, &PauseMutex);
			pthread_mutex_unlock(&PauseMutex);
		}

		Frame2Display(render);

		if (drmHandleEvent(render->fd_drm, &render->ev) != 0)
			Error("DisplayHandlerThread: drmHandleEvent failed!");

		if (render->Closing &&
		    (!render->act_buf || (render->act_buf->fb_id == render->buf_black.fb_id))) {
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
	if (DisableOglOsd) {
		memset((void *)render->buf_osd->plane[0], 0,
			(size_t)(render->buf_osd->pitch[0] * render->buf_osd->height));
	} else {
		struct drm_buf *buf;

		EGL_CHECK(eglSwapBuffers(render->eglDisplay, render->eglSurface));
		render->next_bo = gbm_surface_lock_front_buffer(render->gbm_surface);
		assert(render->next_bo);

		buf = drm_get_buf_from_bo(render, render->next_bo);
		if (!buf) {
			Error("Failed to get GL buffer");
			return;
		}

		render->buf_osd = buf;

		// release old buffer for writing again
		if (render->bo)
			gbm_surface_release_buffer(render->gbm_surface, render->bo);

		// rotate bos and create and keep bo as old_bo to make it free'able
		render->old_bo = render->bo;
		render->bo = render->next_bo;

		Debug2(L_OPENGL, "VideoOsdClear(GL): eglSwapBuffers eglDisplay %p eglSurface %p (%i x %i, %i)", render->eglDisplay, render->eglSurface, buf->width, buf->height, buf->pitch[0]);
	}
#else
	memset((void *)render->buf_osd->plane[0], 0,
		(size_t)(render->buf_osd->pitch[0] * render->buf_osd->height));
#endif

	render->buf_osd->dirty = 1;
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
void VideoOsdDrawARGB(VideoRender * render, __attribute__ ((unused)) int xi,
		__attribute__ ((unused)) int yi, __attribute__ ((unused)) int width,
		int height, int pitch, const uint8_t * argb, int x, int y)
{
#ifdef USE_GLES
	if (DisableOglOsd) {
		for (int i = 0; i < height; ++i) {
			memcpy(render->buf_osd->plane[0] + x * 4 + (i + y) * render->buf_osd->pitch[0],
				argb + i * pitch, (size_t)pitch);
		}
	} else {
		struct drm_buf *buf;

		EGL_CHECK(eglSwapBuffers(render->eglDisplay, render->eglSurface));
		render->next_bo = gbm_surface_lock_front_buffer(render->gbm_surface);
		assert(render->next_bo);

		buf = drm_get_buf_from_bo(render, render->next_bo);
		if (!buf) {
			Error("Failed to get GL buffer");
			return;
		}

		render->buf_osd = buf;

		// release old buffer for writing again
		if (render->bo)
			gbm_surface_release_buffer(render->gbm_surface, render->bo);

		// rotate bos and create and keep bo as old_bo to make it free'able
		render->old_bo = render->bo;
		render->bo = render->next_bo;

		Debug2(L_OPENGL, "VideoOsdDrawARGB(GL): eglSwapBuffers eglDisplay %p eglSurface %p (%i x %i, %i)", render->eglDisplay, render->eglSurface, buf->width, buf->height, buf->pitch[0]);
	}
#else
	for (int i = 0; i < height; ++i) {
		memcpy(render->buf_osd->plane[0] + x * 4 + (i + y) * render->buf_osd->pitch[0],
			argb + i * pitch, (size_t)pitch);
	}
#endif
	render->buf_osd->dirty = 1;
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

	Debug("video: display thread started");

	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

	for (;;) {
		pthread_testcancel();

		// manage fill frame output ring buffer
		if (VideoDecodeInput(render->Stream)) {
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

	Debug("video: video thread canceled");

	if (DecodeThread) {
		Debug("VideoThreadExit: cancel decode thread");
		// FIXME: can't cancel locked
		if (pthread_cancel(DecodeThread))
			Error("VideoThreadExit: can't queue cancel video display thread");
		if (pthread_join(DecodeThread, &retval) || retval != PTHREAD_CANCELED)
			Error("VideoThreadExit: can't cancel video display thread");
		DecodeThread = 0;

		pthread_cond_destroy(&PauseCondition);
		pthread_mutex_destroy(&PauseMutex);
	}

	if (DisplayThread) {
		Debug("VideoThreadExit: cancel display thread");
		if (pthread_cancel(DisplayThread))
			Error("VideoThreadExit: can't cancel DisplayHandlerThread thread");
		if (pthread_join(DisplayThread, &retval) || retval != PTHREAD_CANCELED)
			Error("VideoThreadExit: can't cancel video display thread");
		DisplayThread = 0;
	}
}

///
///	Video display wakeup.
///
///	New video arrived, wakeup video thread.
///
void VideoThreadWakeup(VideoRender * render, int decoder, int display)
{
	Debug("VideoThreadWakeup: VideoThreadWakeup");

	if (decoder && !DecodeThread) {
		Debug("DisplayThreadWakeup: VideoThreadWakeup");
		pthread_cond_init(&PauseCondition,NULL);
		pthread_mutex_init(&PauseMutex, NULL);

		pthread_cond_init(&WaitCleanCondition,NULL);
		pthread_mutex_init(&WaitCleanMutex, NULL);

		pthread_create(&DecodeThread, NULL, DecodeHandlerThread, render);
		pthread_setname_np(DecodeThread, "decoding thread");
	}

	if (display && !DisplayThread) {
		Debug("VideoThreadWakeup: DisplayThreadWakeup");
		pthread_create(&DisplayThread, NULL, DisplayHandlerThread, render);
		pthread_setname_np(DisplayThread, "display thread");
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
		Error("video/DRM: out of memory");
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
			Debug("video: should only be called from inside the thread");
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
		Debug2(L_CODEC, "Video_get_format: PixelFormat: %s video_ctx->pix_fmt: %s sw_pix_fmt: %s Codecname: %s",
			av_get_pix_fmt_name(*fmt), av_get_pix_fmt_name(video_ctx->pix_fmt),
			av_get_pix_fmt_name(video_ctx->sw_pix_fmt), video_ctx->codec->name);
		if (*fmt == AV_PIX_FMT_DRM_PRIME) {
			return AV_PIX_FMT_DRM_PRIME;
		}

		if (*fmt == AV_PIX_FMT_YUV420P) {
			return AV_PIX_FMT_YUV420P;
		}
		fmt++;
	}
	Warning("Video_get_format: No pixel format found! Set default format.");

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

			if (SetupFB(render, buf, NULL, 1)) {
				Error("EnqueueFB: SetupFB FB %i x %i failed",
					buf->width, buf->height);
			}

			if (drmPrimeHandleToFD(render->fd_drm, buf->handle[0],
				DRM_CLOEXEC | DRM_RDWR, &buf->fd_prime))
				Error("EnqueueFB: Failed to retrieve the Prime FD (%d): %m",
					errno);
		}
	}

	buf = &render->bufs[render->enqueue_buffer];

	for (i = 0; i < inframe->height; ++i) {
		memcpy(buf->plane[0] + i * buf->pitch[0],
			inframe->data[0] + i * inframe->linesize[0], inframe->linesize[0]);
	}
	for (i = 0; i < inframe->height / 2; ++i) {
		memcpy(buf->plane[1] + i * buf->pitch[1],
			inframe->data[1] + i * inframe->linesize[1], inframe->linesize[1]);
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

	if (render->Closing) {
		av_frame_free(&frame);
		return;
	}

	pthread_mutex_lock(&DisplayQueue);
	render->FramesRb[render->FramesWrite] = frame;
	render->FramesWrite = (render->FramesWrite + 1) % VIDEO_SURFACES_MAX;
	atomic_inc(&render->FramesFilled);
	pthread_mutex_unlock(&DisplayQueue);

	if (render->enqueue_buffer == VIDEO_SURFACES_MAX + 1)
		render->enqueue_buffer = 0;
	else render->enqueue_buffer++;
}

///
//	Filter thread.
//
static void *FilterHandlerThread(void * arg)
{
	VideoRender * render = (VideoRender *)arg;
	AVFrame *frame = 0;
	int ret = 0;

	while (1) {
		while (!atomic_read(&render->FramesDeintFilled) && !render->Closing) {
			usleep(10000);
		}
getinframe:
		if (atomic_read(&render->FramesDeintFilled)) {
			frame = render->FramesDeintRb[render->FramesDeintRead];
			render->FramesDeintRead = (render->FramesDeintRead + 1) % VIDEO_SURFACES_MAX;
			atomic_dec(&render->FramesDeintFilled);
			if (frame->interlaced_frame) {
				render->Filter_Frames += 2;
			} else {
				render->Filter_Frames++;
			}
		}
		if (render->Closing) {
			if (frame) {
				av_frame_free(&frame);
			}
			if (atomic_read(&render->FramesDeintFilled)) {
				goto getinframe;
			}
			frame = NULL;
		}

		if (av_buffersrc_add_frame_flags(render->buffersrc_ctx,
			frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
			Warning("FilterHandlerThread: can't add_frame.");
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
				Error("FilterHandlerThread: can't get filtered frame: %s",
					av_err2str(ret));
				av_frame_free(&filt_frame);
				break;
			}
fillframe:
			if (render->Closing) {
				av_frame_free(&filt_frame);
				break;
			}

			if (atomic_read(&render->FramesFilled) < VIDEO_SURFACES_MAX) {
				if (filt_frame->format == AV_PIX_FMT_NV12) {
					if (render->Filter_Bug)
						filt_frame->pts = filt_frame->pts / 2;	// ffmpeg bug
					render->Filter_Frames--;
					EnqueueFB(render, filt_frame);
				} else {
					pthread_mutex_lock(&DisplayQueue);
					render->FramesRb[render->FramesWrite] = filt_frame;
					render->FramesWrite = (render->FramesWrite + 1) % VIDEO_SURFACES_MAX;
					atomic_inc(&render->FramesFilled);
					pthread_mutex_unlock(&DisplayQueue);
					render->Filter_Frames--;
				}
			} else {
				usleep(10000);
				goto fillframe;
			}
		}
	}

closing:
	avfilter_graph_free(&render->filter_graph);
	render->Filter_Frames = 0;
	Debug("FilterHandlerThread: Thread Exit.");
	pthread_cleanup_push(ThreadExitHandler, render);
	pthread_cleanup_pop(1);
	pthread_exit((void *)pthread_self());
}

///
//	Filter init.
//
//	@retval 0	filter initialised
//	@retval	-1	filter initialise failed
//
int VideoFilterInit(VideoRender * render, const AVCodecContext * video_ctx,
		AVFrame * frame)
{
	int ret;
	char args[512];
	const char *filter_descr = NULL;
	render->filter_graph = avfilter_graph_alloc();
	if (!render->filter_graph) {
		Error("VideoFilterInit: Cannot alloc filter graph");
		return -1;
	}

	const AVFilter *buffersrc  = avfilter_get_by_name("buffer");
	const AVFilter *buffersink = avfilter_get_by_name("buffersink");
	render->Filter_Bug = 0;

	int interlaced = frame->interlaced_frame;

	if (video_ctx->framerate.num > 0) {
		if (video_ctx->framerate.num / video_ctx->framerate.den > 30)
			interlaced = 0;
		else
			interlaced = 1;
	}

	if (video_ctx->codec_id == AV_CODEC_ID_HEVC)
		interlaced = 0;

	if (interlaced) {
		if (frame->format == AV_PIX_FMT_DRM_PRIME) {
			filter_descr = "deinterlace_v4l2m2m";
		} else if (frame->format == AV_PIX_FMT_YUV420P) {
			filter_descr = "bwdif=1:-1:0";
			render->Filter_Bug = 1;
		}
	} else if (frame->format == AV_PIX_FMT_YUV420P)
		filter_descr = "scale";

#if LIBAVFILTER_VERSION_INT < AV_VERSION_INT(7,16,100)
	avfilter_register_all();
#endif

	snprintf(args, sizeof(args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		video_ctx->width, video_ctx->height, frame->format,
		video_ctx->pkt_timebase.num ? video_ctx->pkt_timebase.num : 1,
		video_ctx->pkt_timebase.num ? video_ctx->pkt_timebase.den : 1,
		video_ctx->sample_aspect_ratio.num != 0 ? video_ctx->sample_aspect_ratio.num : 1,
		video_ctx->sample_aspect_ratio.num != 0 ? video_ctx->sample_aspect_ratio.den : 1);

	Debug2(L_CODEC, "VideoFilterInit: filter=\"%s\" args=\"%s\"",
		filter_descr, args);

	ret = avfilter_graph_create_filter(&render->buffersrc_ctx, buffersrc, "in",
		args, NULL, render->filter_graph);
	if (ret < 0) {
		Error("VideoFilterInit: Cannot create buffer source (%d)", ret);
		avfilter_graph_free(&render->filter_graph);
		return -1;
	}

	AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
	memset(par, 0, sizeof(*par));
	par->format = AV_PIX_FMT_NONE;
	par->hw_frames_ctx = frame->hw_frames_ctx;
	ret = av_buffersrc_parameters_set(render->buffersrc_ctx, par);
	if (ret < 0) {
		Error("VideoFilterInit: Cannot av_buffersrc_parameters_set (%d)", ret);
		av_free(par);
		avfilter_graph_free(&render->filter_graph);
		return -1;
	}

	av_free(par);

	ret = avfilter_graph_create_filter(&render->buffersink_ctx, buffersink, "out",
		NULL, NULL, render->filter_graph);
	if (ret < 0) {
		Error("VideoFilterInit: Cannot create buffer sink (%d)", ret);
		avfilter_graph_free(&render->filter_graph);
		return -1;
	}

	if (frame->format != AV_PIX_FMT_DRM_PRIME) {
		enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_NV12, AV_PIX_FMT_NONE };
		ret = av_opt_set_int_list(render->buffersink_ctx, "pix_fmts", pix_fmts,
			AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
		if (ret < 0) {
			Error("VideoFilterInit: Cannot set output pixel format (%d)", ret);
			avfilter_graph_free(&render->filter_graph);
			return -1;
		}
	}

	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs  = avfilter_inout_alloc();

	outputs->name       = av_strdup("in");
	outputs->filter_ctx = render->buffersrc_ctx;
	outputs->pad_idx    = 0;
	outputs->next       = NULL;

	inputs->name       = av_strdup("out");
	inputs->filter_ctx = render->buffersink_ctx;
	inputs->pad_idx    = 0;
	inputs->next       = NULL;

	ret = avfilter_graph_parse_ptr(render->filter_graph, filter_descr,
		&inputs, &outputs, NULL);
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	if (ret < 0) {
		Error("VideoFilterInit: avfilter_graph_parse_ptr failed (%d)", ret);
		avfilter_graph_free(&render->filter_graph);
		return -1;
	}

	ret = avfilter_graph_config(render->filter_graph, NULL);
	if (ret < 0) {
		Error("VideoFilterInit: avfilter_graph_config failed (%d)", ret);
		avfilter_graph_free(&render->filter_graph);
		return -1;
	}

	return 0;
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
		Warning("VideoRenderFrame: error_flag or FRAME_FLAG_CORRUPT");
	}
fillframe:
	if (render->Closing) {
		av_frame_free(&frame);
		return;
	}

	if (frame->format == AV_PIX_FMT_YUV420P || (frame->interlaced_frame &&
		frame->format == AV_PIX_FMT_DRM_PRIME && !render->NoHwDeint)) {

		if (!FilterThread) {
			Debug2(L_CODEC, "VideoRenderFrame: try to create FilterThread");
			if (VideoFilterInit(render, video_ctx, frame)) {
				av_frame_free(&frame);
				return;
			} else {
				Debug2(L_CODEC, "VideoRenderFrame: FilterThread created");
				pthread_create(&FilterThread, NULL, FilterHandlerThread, render);
				pthread_setname_np(FilterThread, "filter thread");
			}
		}

		if (atomic_read(&render->FramesDeintFilled) < VIDEO_SURFACES_MAX) {
			render->FramesDeintRb[render->FramesDeintWrite] = frame;
			render->FramesDeintWrite = (render->FramesDeintWrite + 1) % VIDEO_SURFACES_MAX;
			atomic_inc(&render->FramesDeintFilled);
		} else {
			usleep(10000);
			goto fillframe;
		}
	} else {
		if (frame->format == AV_PIX_FMT_DRM_PRIME) {
			pthread_mutex_lock(&DisplayQueue);
			if (atomic_read(&render->FramesFilled) < VIDEO_SURFACES_MAX && !render->Filter_Frames) {
				render->FramesRb[render->FramesWrite] = frame;
				render->FramesWrite = (render->FramesWrite + 1) % VIDEO_SURFACES_MAX;
				atomic_inc(&render->FramesFilled);
				pthread_mutex_unlock(&DisplayQueue);
			} else {
				pthread_mutex_unlock(&DisplayQueue);
				usleep(10000);
				goto fillframe;
			}

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
	Debug("VideoGetClock: %s",
		Timestamp2String(render->pts * 1000 * av_q2d(*render->timebase)));
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
	Debug("StartVideo: reset PauseCondition StartCounter %d Closing %d TrickSpeed %d",
		render->StartCounter, render->Closing, render->TrickSpeed);
	pthread_cond_signal(&PauseCondition);
}

///
///	Set closing stream flag.
///
///	@param hw_render	video hardware render
///
void VideoSetClosing(VideoRender * render)
{
	Debug("VideoSetClosing: buffers %d StartCounter %d",
		render->buffers, render->StartCounter);

	if (render->buffers){
		render->Closing = 1;

		if (render->VideoPaused) {
			StartVideo(render);
		}


		pthread_mutex_lock(&WaitCleanMutex);
		Debug("VideoSetClosing: pthread_cond_wait");
		pthread_cond_wait(&WaitCleanCondition, &WaitCleanMutex);
		pthread_mutex_unlock(&WaitCleanMutex);
		Debug("VideoSetClosing: NACH pthread_cond_wait");
	}
	render->StartCounter = 0;
	render->FramesDuped = 0;
	render->FramesDropped = 0;
	render->TrickSpeed = 0;
}

///
//	Pause video.
//
void VideoPause(VideoRender * render)
{
	Debug("VideoPause:");
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
	Debug("VideoSetTrickSpeed: set trick speed %d", speed);
	render->TrickSpeed = speed;
	if (speed) {
		render->Closing = 0;	// ???
	}

	if (render->VideoPaused) {
		StartVideo(render);
	}
}

///
//	Play video.
//
void VideoPlay(VideoRender * render)
{
	Debug("VideoPlay:");
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
    Debug("no grab service");

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
    Debug("no grab service");
    Warning("grab unsupported");

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
///	Get video size.
///
///	@param[out] width		video stream width
///	@param[out] height		video stream height
///	@param[out] aspect_ratio	video stream aspect ratio
///
void VideoGetVideoSize(VideoDecoder *decoder, int *width, int *height, double *aspect_ratio)
{
	*width = 0;
	*height = 0;
	*aspect_ratio = 1.0f;

	if (!decoder)
		return;

	AVCodecContext *ctx;
	ctx = decoder->VideoCtx;

	if (!ctx)
		return;

	*width = ctx->coded_width;
	*height = ctx->coded_height;
	if (ctx->coded_height > 0)
		*aspect_ratio = (double)(ctx->coded_width) / (double)(ctx->coded_height);
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
		Error("VideoInit: FindDevice() failed");
	}

	ReadHWPlatform(render);

	render->bufs[0].width = render->bufs[1].width = 0;
	render->bufs[0].height = render->bufs[1].height = 0;
	render->bufs[0].pix_fmt = render->bufs[1].pix_fmt = DRM_FORMAT_NV12;

	// osd FB
#ifndef USE_GLES
	if (!render->buf_osd)
		render->buf_osd = calloc(1, sizeof(struct drm_buf));
	render->buf_osd->pix_fmt = DRM_FORMAT_ARGB8888;
	render->buf_osd->width = render->mode.hdisplay;
	render->buf_osd->height = render->mode.vdisplay;
	if (SetupFB(render, render->buf_osd, NULL, 0)){
		Fatal("VideoOsdInit: SetupFB FB OSD failed!");
	}
#else
	if (DisableOglOsd) {
		if (!render->buf_osd)
			render->buf_osd = calloc(1, sizeof(struct drm_buf));
		render->buf_osd->pix_fmt = DRM_FORMAT_ARGB8888;
		render->buf_osd->width = render->mode.hdisplay;
		render->buf_osd->height = render->mode.vdisplay;
		if (SetupFB(render, render->buf_osd, NULL, 0)){
			Fatal("VideoOsdInit: SetupFB FB OSD failed!");
		}
	}
#endif

	// black fb
	render->buf_black.pix_fmt = DRM_FORMAT_NV12;
	render->buf_black.width = render->mode.hdisplay;
	render->buf_black.height = render->mode.vdisplay;
	Debug2(L_DRM, "Videoinit: Try to create a black FB");
	if (SetupFB(render, &render->buf_black, NULL, 0))
		Error("VideoInit: SetupFB black FB %i x %i failed",
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

	if (drmModeCreatePropertyBlob(render->fd_drm, &render->mode, sizeof(render->mode), &modeID) != 0)
		Error("Failed to create mode property blob.");
	if (!(ModeReq = drmModeAtomicAlloc()))
		Error("cannot allocate atomic request (%d): %m", errno);

	SetPropertyRequest(ModeReq, render->fd_drm, render->crtc_id,
						DRM_MODE_OBJECT_CRTC, "MODE_ID", modeID);
	SetPropertyRequest(ModeReq, render->fd_drm, render->connector_id,
						DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", render->crtc_id);
	SetPropertyRequest(ModeReq, render->fd_drm, render->crtc_id,
						DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);

	// Osd plane
	// We don't have the buf_osd for OpenGL yet, so we can't set anything. Set src and FbId later when osd was drawn,
	// but initially move the OSD behind the VIDEO
#ifndef USE_GLES
	render->planes[OSD_PLANE]->properties.crtc_id = render->crtc_id;
	render->planes[OSD_PLANE]->properties.fb_id = render->buf_osd->fb_id;
	render->planes[OSD_PLANE]->properties.crtc_x = 0;
	render->planes[OSD_PLANE]->properties.crtc_y = 0;
	render->planes[OSD_PLANE]->properties.crtc_w = render->mode.hdisplay;
	render->planes[OSD_PLANE]->properties.crtc_h = render->mode.vdisplay;
	render->planes[OSD_PLANE]->properties.src_x = 0;
	render->planes[OSD_PLANE]->properties.src_y = 0;
	render->planes[OSD_PLANE]->properties.src_w = render->buf_osd->width;
	render->planes[OSD_PLANE]->properties.src_h = render->buf_osd->height;

	SetPlane(ModeReq, render->planes[OSD_PLANE]);
#else
	if (DisableOglOsd) {
		render->planes[OSD_PLANE]->properties.crtc_id = render->crtc_id;
		render->planes[OSD_PLANE]->properties.fb_id = render->buf_osd->fb_id;
		render->planes[OSD_PLANE]->properties.crtc_x = 0;
		render->planes[OSD_PLANE]->properties.crtc_y = 0;
		render->planes[OSD_PLANE]->properties.crtc_w = render->mode.hdisplay;
		render->planes[OSD_PLANE]->properties.crtc_h = render->mode.vdisplay;
		render->planes[OSD_PLANE]->properties.src_x = 0;
		render->planes[OSD_PLANE]->properties.src_y = 0;
		render->planes[OSD_PLANE]->properties.src_w = render->buf_osd->width;
		render->planes[OSD_PLANE]->properties.src_h = render->buf_osd->height;

		SetPlane(ModeReq, render->planes[OSD_PLANE]);
	}
#endif
	if (render->use_zpos) {
		render->planes[VIDEO_PLANE]->properties.zpos = render->zpos_overlay;
		SetPlaneZpos(ModeReq, render->planes[VIDEO_PLANE]);
#ifdef USE_GLES
		render->planes[OSD_PLANE]->properties.zpos = render->zpos_primary;
		SetPlaneZpos(ModeReq, render->planes[OSD_PLANE]);
#endif
	}

	render->planes[VIDEO_PLANE]->properties.crtc_id = render->crtc_id;
	render->planes[VIDEO_PLANE]->properties.fb_id = render->buf_black.fb_id;
	render->planes[VIDEO_PLANE]->properties.crtc_x = 0;
	render->planes[VIDEO_PLANE]->properties.crtc_y = 0;
	render->planes[VIDEO_PLANE]->properties.crtc_w = render->mode.hdisplay;
	render->planes[VIDEO_PLANE]->properties.crtc_h = render->mode.vdisplay;
	render->planes[VIDEO_PLANE]->properties.src_x = 0;
	render->planes[VIDEO_PLANE]->properties.src_y = 0;
	render->planes[VIDEO_PLANE]->properties.src_w = render->buf_black.width;
	render->planes[VIDEO_PLANE]->properties.src_h = render->buf_black.height;

	// Black Buffer for video plane
	SetPlane(ModeReq, render->planes[VIDEO_PLANE]);

	if (drmModeAtomicCommit(render->fd_drm, ModeReq, flags, NULL) != 0) {
#ifndef USE_GLES
		DumpPlaneProperties(render->planes[OSD_PLANE]);
#endif
		DumpPlaneProperties(render->planes[VIDEO_PLANE]);

		drmModeAtomicFree(ModeReq);
		Fatal("VideoInit: cannot set atomic mode (%d): %m", errno);
	}

	drmModeAtomicFree(ModeReq);

	render->OsdShown = 0;

	// init variables page flip
	memset(&render->ev, 0, sizeof(render->ev));
	render->ev.version = 2;

	// Wakeup DisplayHandlerThread
	VideoThreadWakeup(render, 0, 1);
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

		for (int i = 0; i < MAX_PLANES; i++) {
			if (render->planes[i]) {
				free_properties(render->planes[i]);
				free(render->planes[i]);
			}
		}

		DestroyFB(render->fd_drm, &render->buf_black);
#ifdef USE_GLES
		if (DisableOglOsd) {
			if (render->buf_osd) {
				DestroyFB(render->fd_drm, render->buf_osd);
				free(render->buf_osd);
			}
		} else {
			if (render->next_bo)
				gbm_bo_destroy(render->next_bo);
			if (render->old_bo)
				gbm_bo_destroy(render->old_bo);
		}
#else
		if (render->buf_osd) {
			DestroyFB(render->fd_drm, render->buf_osd);
			free(render->buf_osd);
		}
#endif

		close(render->fd_drm);
	}
}

///
///	Set size and position of the video.
///
void VideoSetOutputPosition(VideoRender *render, int x, int y, int width, int height)
{
	render->video.x = x;
	render->video.y = y;
	render->video.width = width;
	render->video.height = height;

	if (render->video.x == 0 &&
	    render->video.y == 0 &&
	    render->video.width == 0 &&
	    render->video.height == 0)
		render->video.is_scaled = 0;
	else
		render->video.is_scaled = 1;

	Debug("VideoSetOutputPosition %d %d %d %d%s", x, y, width, height, render->video.is_scaled ? ", video is scaled" : "");
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
