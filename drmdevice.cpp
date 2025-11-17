/**
 * @file drmdevice.cpp
 * DRM device class
 *
 * This file defines cDrmDevice, which keeps some functions
 * to interact with the DRM (display) system.
 *
 * @copyright (c) 2018 by zille.  All Rights Reserved.
 * @copyright (c) 2025 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPLv3
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.}
 */

// @todo: sort out header includes

#ifndef __USE_GNU
#define __USE_GNU
#endif

#include <stdbool.h>
#include <unistd.h>

#include <inttypes.h>

#include <libintl.h>

#ifdef USE_GLES
#include <assert.h>
#endif
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <drm_fourcc.h>

#include "logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>

#include "misc.h"
}
#include "buf2rgb.h"

#include "videorender.h"
#include "audio.h"
#include "drm.h"
#include "threads.h"
#include "grab.h"
#include "drmdevice.h"

/*****************************************************************************
 * cDrmDevice class
 ****************************************************************************/

/**
 * cDrmDevice constructor
 *
 * @param render         pointer to cVideoRender object
 */
cDrmDevice::cDrmDevice(cVideoRender *render, const char* resolution)
{
	m_pRender = render;
	m_useZpos = false;
	m_userReqDisplayWidth = 0;

	if (resolution)
		sscanf(resolution, "%dx%d@%d", &m_userReqDisplayWidth, &m_userReqDisplayHeight, &m_userReqDisplayRefreshRate);
}

/**
 * cDrmDevice destructor
 */
cDrmDevice::~cDrmDevice(void)
{
}

static int get_resources(int fd, drmModeRes **resources)
{
	*resources = drmModeGetResources(fd);
	if (*resources == NULL) {
		LOGERROR("drmdevice: %s: cannot retrieve DRM resources (%d): %m", __FUNCTION__, errno);
		return -1;
	}
	return 0;
}

/**
 * Test drm capabilities
 *
 * @returns 0 if all caps match, 1 on mismatch
 */
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

#define MAX_DRM_DEVICES 64
/**
 * Find and open a suitable device with the wanted capabilities
 *
 * @returns the file descriptor of the opened device
 */
static int FindDrmDevice(drmModeRes **resources)
{
	drmDevicePtr devices[MAX_DRM_DEVICES] = { NULL };
	int num_devices, fd = -1;

	num_devices = drmGetDevices2(0, devices,MAX_DRM_DEVICES);
	if (num_devices < 0) {
		LOGERROR("drmdevice: %s: drmGetDevices2 failed: %s", __FUNCTION__, strerror(-num_devices));
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
		LOGERROR("drmdevice: %s: no drm device found!", __FUNCTION__);

	return fd;
}

/**
 * Find a suitable connector, preferably a connected one
 */
static drmModeConnector *FindDrmConnector(int fd, drmModeRes *resources)
{
	drmModeConnector *connector = NULL;
	int i;

	// search for a connected connector
	for (i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(fd, resources->connectors[i]);
		if (connector && connector->connection == DRM_MODE_CONNECTED)
			return connector;
		drmModeFreeConnector(connector);
		connector = NULL;
	}

	// search for a not connected connector, but with available modes
	// this is a workaround for RPI: in case we don't have a monitor connected
	// we can load an edid file at boot time, where the available modes are listed.
	// To bring softhddevice up, we also have to go through the not connected connectors
	for (i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(fd, resources->connectors[i]);
		if (connector && connector->count_modes > 0)
			return connector;
		drmModeFreeConnector(connector);
		connector = NULL;
	}

	// we couldn't find a connector
	return connector;
}

/**
 * Gets a property value
 */
static int GetPropertyValue(int fdDrm, uint32_t objectID,
                            uint32_t objectType, const char *propName, uint64_t *value)
{
	uint32_t i;
	int found = 0;
	drmModePropertyPtr Prop;
	drmModeObjectPropertiesPtr objectProps =
		drmModeObjectGetProperties(fdDrm, objectID, objectType);

	for (i = 0; i < objectProps->count_props; i++) {
		if ((Prop = drmModeGetProperty(fdDrm, objectProps->props[i])) == NULL)
			LOGDEBUG2(L_DRM, "drmdevice: %s: Unable to query property", __FUNCTION__);

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
		LOGDEBUG2(L_DRM, "drmdevice: %s: Unable to find value for property \'%s\'.", __FUNCTION__, propName);
		return -1;
	}

	return 0;
}

/**
 * Initiate the drm device
 *
 * @returns 0 on success, a negative value on error
 */
int cDrmDevice::Init(void)
{
	drmModeRes *resources;
	drmModeConnector *connector;
	drmModeEncoder *encoder = NULL;
	drmModeModeInfo *drmmode = NULL;
	drmModePlane *plane;
	drmModePlaneRes *planeRes;
	int i;
	uint32_t j, k;

	// find a drm device
	m_fdDrm = FindDrmDevice(&resources);
	if (m_fdDrm < 0) {
		LOGERROR("drmdevice: %s: Could not open device!", __FUNCTION__);
		return -1;
	}

	LOGDEBUG2(L_DRM, "drmdevice: %s: DRM have %i connectors, %i crtcs, %i encoders", __FUNCTION__,
		resources->count_connectors, resources->count_crtcs,
		resources->count_encoders);

	// find a connector
	connector = FindDrmConnector(m_fdDrm, resources);
	if (!connector) {
		LOGERROR("drmdevice: %s: cannot retrieve DRM connector (%d): %m", __FUNCTION__, errno);
		return -errno;
	}
	m_connectorId = connector->connector_id;

	// find a user requested mode
	if (m_userReqDisplayWidth) {
		for (i = 0; i < connector->count_modes; i++) {
			drmModeModeInfo *current_mode = &connector->modes[i];
			if(current_mode->hdisplay == m_userReqDisplayWidth && current_mode->vdisplay == m_userReqDisplayHeight &&
			   current_mode->vrefresh == m_userReqDisplayRefreshRate && !(current_mode->flags & DRM_MODE_FLAG_INTERLACE)) {
				drmmode = current_mode;
				LOGDEBUG2(L_DRM, "drmdevice: %s: Use user requested mode: %dx%d@%d", __FUNCTION__, drmmode->hdisplay, drmmode->vdisplay, drmmode->vrefresh);
				break;
			}
		}
		if (!drmmode)
			LOGWARNING("drmdevice: %s: User requested mode not found, try default modes", __FUNCTION__);
	}

	uint32_t preferred_hz[3] = {50, 60, 0};

	// find the highest resolution mode with 50, 60 or any refresh rate
	if (!drmmode) {
		j = 0;
		int width;
		while (!drmmode && preferred_hz[j]) {
			for (i = 0, width = 0; i < connector->count_modes; i++) {
				drmModeModeInfo *current_mode = &connector->modes[i];
				if (preferred_hz[j] && current_mode->vrefresh != preferred_hz[j])
					continue;

				int current_width = current_mode->hdisplay;
				if (current_width > width) {
					drmmode = current_mode;
					width = current_width;
				}
			}
			j++;
		}

		if (drmmode)
			LOGDEBUG2(L_DRM, "drmdevice: %s: Use mode with the biggest width: %dx%d@%d", __FUNCTION__,
				drmmode->hdisplay, drmmode->vdisplay, drmmode->vrefresh);
	}

	if (!drmmode) {
		LOGERROR("drmdevice: %s: No monitor mode found! Probably no monitor connected, giving up!", __FUNCTION__);
		return -1;
	}

	memcpy(&m_drmModeInfo, drmmode, sizeof(drmModeModeInfo));

	// find encoder
	for (i = 0; i < resources->count_encoders; i++) {
		encoder = drmModeGetEncoder(m_fdDrm, resources->encoders[i]);
		if (encoder->encoder_id == connector->encoder_id)
			break;
		drmModeFreeEncoder(encoder);
		encoder = NULL;
	}

	if (encoder) {
		m_crtcId = encoder->crtc_id;
		LOGDEBUG2(L_DRM, "drmdevice: %s: have encoder, m_crtcId %d", __FUNCTION__, m_crtcId);
	} else {
		int32_t crtc_id = FindCrtcForConnector(resources, connector);
		if (crtc_id == -1) {
			LOGERROR("drmdevice: %s: No crtc found!", __FUNCTION__);
			return -errno;
		}

		m_crtcId = crtc_id;
		LOGDEBUG2(L_DRM, "drmdevice: %s: have no encoder, m_crtcId %d", __FUNCTION__, m_crtcId);
	}

	for (i = 0; i < resources->count_crtcs; i++) {
		if (resources->crtcs[i] == m_crtcId) {
			m_crtcIndex = i;
			break;
		}
	}

	LOGINFO("DRM Setup: Using Monitor Mode %dx%d@%d, m_crtcId %d crtc_idx %d",
		m_drmModeInfo.hdisplay, m_drmModeInfo.vdisplay, m_drmModeInfo.vrefresh, m_crtcId, m_crtcIndex);

	drmModeFreeConnector(connector);


	// find planes
	if ((planeRes = drmModeGetPlaneResources(m_fdDrm)) == NULL) {
		LOGERROR("drmdevice: %s: cannot retrieve PlaneResources (%d): %m", __FUNCTION__, errno);
		return -1;
	}

	// test and list the local planes
	cDrmPlane best_primary_video_plane; // is the NV12 capable primary plane with the lowest plane_id
	cDrmPlane best_overlay_video_plane; // is the NV12 capable overlay plane with the lowest plane_id
	cDrmPlane best_primary_osd_plane;   // is the AR24 capable primary plane with the highest plane_id
	cDrmPlane best_overlay_osd_plane;   // is the AR24 capable overlay plane with the highest plane_id

	for (j = 0; j < planeRes->count_planes; j++) {
		plane = drmModeGetPlane(m_fdDrm, planeRes->planes[j]);

		if (plane == NULL) {
			LOGERROR("drmdevice: %s: cannot query DRM-KMS plane %d", __FUNCTION__, j);
			continue;
		}

		uint64_t type;
		uint64_t zpos;
		char pixelformats[256];

		if (plane->possible_crtcs & (1 << m_crtcIndex)) {
			if (GetPropertyValue(m_fdDrm, planeRes->planes[j],
						 DRM_MODE_OBJECT_PLANE, "type", &type)) {
				LOGDEBUG2(L_DRM, "drmdevice: %s: Failed to get property 'type'", __FUNCTION__);
			}
			if (GetPropertyValue(m_fdDrm, planeRes->planes[j],
						 DRM_MODE_OBJECT_PLANE, "zpos", &zpos)) {
				LOGDEBUG2(L_DRM, "drmdevice: %s: Failed to get property 'zpos'", __FUNCTION__);
			} else {
				m_useZpos = true;
			}

			LOGDEBUG2(L_DRM, "drmdevice: %s: %s: id %i possible_crtcs %i", __FUNCTION__,
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
							if (type == DRM_PLANE_TYPE_PRIMARY && !best_primary_video_plane.GetId()) {
								best_primary_video_plane.SetId(plane->plane_id);
								best_primary_video_plane.SetType(type);
								best_primary_video_plane.SetZpos(zpos);
								strcat(pixelformats, "!  ");
							}
							if (type == DRM_PLANE_TYPE_OVERLAY && !best_overlay_video_plane.GetId()) {
								best_overlay_video_plane.SetId(plane->plane_id);
								best_overlay_video_plane.SetType(type);
								best_overlay_video_plane.SetZpos(zpos);
								strcat(pixelformats, "!  ");
							}
							break;
						case DRM_FORMAT_ARGB8888:
							snprintf(tmp, sizeof(tmp), " %4.4s", (char *)&plane->formats[k]);
							strcat(pixelformats, tmp);
							if (type == DRM_PLANE_TYPE_PRIMARY) {
								best_primary_osd_plane.SetId(plane->plane_id);
								best_primary_osd_plane.SetType(type);
								best_primary_osd_plane.SetZpos(zpos);
								strcat(pixelformats, "!  ");
							}
							if (type == DRM_PLANE_TYPE_OVERLAY) {
								best_overlay_osd_plane.SetId(plane->plane_id);
								best_overlay_osd_plane.SetType(type);
								best_overlay_osd_plane.SetZpos(zpos);
								strcat(pixelformats, "!  ");
							}
							break;
						default:
							break;
					}
				}
			}
			LOGDEBUG2(L_DRM, "drmdevice: %s", __FUNCTION__, pixelformats);
		}
		drmModeFreePlane(plane);
	}

	// debug output
	if (best_primary_video_plane.GetId()) {
		LOGDEBUG2(L_DRM, "drmdevice: %s: best_primary_video_plane: plane_id %d, type %s, zpos %" PRIu64 "", __FUNCTION__,
			best_primary_video_plane.GetId(), best_primary_video_plane.GetType() == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "OVERLAY", best_primary_video_plane.GetZpos());
	}
	if (best_overlay_video_plane.GetId()) {
		LOGDEBUG2(L_DRM, "drmdevice: %s: best_overlay_video_plane: plane_id %d, type %s, zpos %" PRIu64 "", __FUNCTION__,
			best_overlay_video_plane.GetId(), best_overlay_video_plane.GetType() == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "OVERLAY", best_overlay_video_plane.GetZpos());
	}
	if (best_primary_osd_plane.GetId()) {
		LOGDEBUG2(L_DRM, "drmdevice: %s: best_primary_osd_plane: plane_id %d, type %s, zpos %" PRIu64 "", __FUNCTION__,
			best_primary_osd_plane.GetId(), best_primary_osd_plane.GetType() == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "OVERLAY", best_primary_osd_plane.GetZpos());
	}
	if (best_overlay_osd_plane.GetId()) {
		LOGDEBUG2(L_DRM, "drmdevice: %s: best_overlay_osd_plane: plane_id %d, type %s, zpos %" PRIu64 "", __FUNCTION__,
			best_overlay_osd_plane.GetId(), best_overlay_osd_plane.GetType() == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "OVERLAY", best_overlay_osd_plane.GetZpos());
	}

	// See which planes we should use
	if (best_primary_video_plane.GetId() && best_overlay_osd_plane.GetId()) {
		m_videoPlane.SetId(best_primary_video_plane.GetId());
		m_videoPlane.SetType(best_primary_video_plane.GetType());
		m_zposPrimary = best_primary_video_plane.GetZpos();
		m_videoPlane.SetZpos(m_zposPrimary);
		m_osdPlane.SetId(best_overlay_osd_plane.GetId());
		m_osdPlane.SetType(best_overlay_osd_plane.GetType());
		m_zposOverlay = best_overlay_osd_plane.GetZpos();
		m_osdPlane.SetZpos(m_zposOverlay);
	} else if (best_overlay_video_plane.GetId() && best_primary_osd_plane.GetId()) {
		m_videoPlane.SetId(best_overlay_video_plane.GetId());
		m_videoPlane.SetType(best_overlay_video_plane.GetType());
		m_zposOverlay = best_overlay_video_plane.GetZpos();
		m_videoPlane.SetZpos(m_zposOverlay);
		m_osdPlane.SetId(best_primary_osd_plane.GetId());
		m_osdPlane.SetType(best_primary_osd_plane.GetType());
		m_zposPrimary = best_primary_osd_plane.GetZpos();
		m_osdPlane.SetZpos(m_zposPrimary);
		m_useZpos = true;
	} else {
		LOGERROR("drmdevice: %s: No suitable planes found!", __FUNCTION__);
		return -1;
	}

	// fill the plane's properties to speed up SetPropertyRequest later
	m_videoPlane.FillProperties(m_fdDrm);
	m_osdPlane.FillProperties(m_fdDrm);

	// Check, if we can set z-order (meson and rpi have fixed z-order, which cannot be changed)
	if (m_useZpos && !m_videoPlane.HasZpos(m_fdDrm)) {
		m_useZpos = false;
	}
	if (m_useZpos && !m_osdPlane.HasZpos(m_fdDrm)) {
		m_useZpos = false;
	}

	// m_useZpos was set, if video is on OVERLAY, and osd is on PRIMARY
	// Check if the OVERLAY plane really got a higher zpos than the PRIMARY plane
	// If not, change their zpos values or hardcode them to
	// 1 OVERLAY (Video)
	// 0 PRIMARY (Osd)
	if (m_useZpos && m_zposOverlay <= m_zposPrimary) {
		char str_zpos[256];
		strcpy(str_zpos, "drmdevice: Init: zpos values are wrong, so ");
		if (m_zposOverlay == m_zposPrimary) {
			// is this possible?
			strcat(str_zpos, "hardcode them to 0 and 1, because they are equal");
			m_zposPrimary = 0;
			m_zposOverlay = 1;
		} else {
			strcat(str_zpos, "switch them");
			uint64_t zpos_tmp = m_zposPrimary;
			m_zposPrimary = m_zposOverlay;
			m_zposOverlay = zpos_tmp;
		}
		LOGDEBUG2(L_DRM, "%s", str_zpos);
	}
	drmModeFreePlaneResources(planeRes);
	drmModeFreeEncoder(encoder);
	drmModeFreeResources(resources);

	LOGINFO("DRM setup - CRTC: %i video_plane: %i (%s %" PRIu64 ") osd_plane: %i (%s %" PRIu64 ") m_useZpos: %d",
		m_crtcId,
		m_videoPlane.GetId(),
		m_videoPlane.GetType() == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "OVERLAY",
		m_videoPlane.GetZpos(),
		m_osdPlane.GetId(),
		m_osdPlane.GetType() == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "OVERLAY",
		m_osdPlane.GetZpos(),
		m_useZpos);

#ifdef USE_GLES
	if (m_pRender->OglOsdDisabled())
		return 0;

	// init gbm
	int w, h;
	double pixel_aspect;
	GetScreenSize(&w, &h, &pixel_aspect);

	if (InitGbm(w, h, DRM_FORMAT_ARGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING)) {
		LOGERROR("drmdevice: %s: failed to init gbm device and surface!", __FUNCTION__);
		return -1;
	}

	// init egl
	if (InitEGL()) {
		LOGERROR("drmdevice: %s: failed to init egl!", __FUNCTION__);
		return -1;
	}
#endif

	return 0;
}

/**
 * Get the display size
 *
 * @param[out] width           display width
 * @param[out] height          display height
 * @param[out] pixelAspect     display aspect ratio
 *                             (currently hardcoded to 16/9)
 */
void cDrmDevice::GetScreenSize(int *width, int *height, double *pixel_aspect)
{
	*width = m_drmModeInfo.hdisplay;
	*height = m_drmModeInfo.vdisplay;
	*pixel_aspect = (double)16 / (double)9;
}

#ifdef USE_GLES
/**
 * Init gbm device and surface
 *
 * @param w            gbm surface width
 * @param h            gbm surface height
 * @param format       gbm pixel format
 * @param modifier     gbm buffer modifier
 *
 * @returns 0          on success
 * @returns -1         on error
 */
int cDrmDevice::InitGbm(int w, int h, uint32_t format, uint64_t modifier)
{
	m_pGbmDevice = gbm_create_device(m_fdDrm);
	if (!m_pGbmDevice) {
		LOGERROR("drmdevice: %s: failed to create gbm device!", __FUNCTION__);
		return -1;
	}

	m_pGbmSurface = gbm_surface_create(m_pGbmDevice, w, h, format, modifier);
	if (!m_pGbmSurface) {
		LOGERROR("drmdevice: %s: failed to create %d x %d surface bo", __FUNCTION__, w, h);
		return -1;
	}

	return 0;
}

PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = NULL;
PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC get_platform_surface = NULL;

static const EGLint context_attribute_list[] =
{
	EGL_CONTEXT_CLIENT_VERSION, 2,
	EGL_NONE
};

/**
 * Get a suitable EGLConfig
 */
EGLConfig cDrmDevice::GetEGLConfig(void)
{
	EGLint config_attribute_list[] = {
		EGL_BUFFER_SIZE, 32,
		EGL_STENCIL_SIZE, EGL_DONT_CARE,
		EGL_DEPTH_SIZE, EGL_DONT_CARE,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_NONE
	};
	EGLConfig *configs = nullptr;
	EGLint matched = 0;
	EGLint count = 0;
	EGL_CHECK(eglGetConfigs(m_eglDisplay, NULL, 0, &count));
	if (count < 1)
		LOGFATAL("drmdevice: %s: no EGL configs to choose from", __FUNCTION__);

	LOGDEBUG2(L_OPENGL, "drmdevice: %s: %d EGL configs found", __FUNCTION__, count);

	configs = (EGLConfig *)malloc(count * sizeof(*configs));
	if (!configs)
		LOGFATAL("drmdevice: %s: can't allocate space for EGL configs", __FUNCTION__);

	EGL_CHECK(eglChooseConfig(m_eglDisplay, config_attribute_list, configs, count, &matched));
	if (!matched) {
		free(configs);
		LOGFATAL("drmdevice: %s: no EGL configs with appropriate attributes", __FUNCTION__);
	}

	LOGDEBUG2(L_OPENGL, "drmdevice: %s: %d appropriate EGL configs found, which match attributes", __FUNCTION__, matched);

	EGLConfig chosen = NULL;
	for (int i = 0; i < matched; ++i) {
		EGLint gbm_format;
		EGL_CHECK(eglGetConfigAttrib(m_eglDisplay, configs[i], EGL_NATIVE_VISUAL_ID, &gbm_format));

		if (gbm_format == GBM_FORMAT_ARGB8888) {
			chosen = configs[i];
			break;
		}
	}

	free(configs);
	if (chosen == NULL)
		LOGFATAL("drmdevice: %s: no matching gbm config found", __FUNCTION__);

	return chosen;
}

/**
 * Init EGL
 *
 * @returns 0       on success
 * @returns -1      on error
 */
int cDrmDevice::InitEGL(void)
{
	EGLint iMajorVersion, iMinorVersion;

	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	assert(get_platform_display != NULL);
	PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC get_platform_surface = (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
	assert(get_platform_surface != NULL);

	EGL_CHECK(m_eglDisplay = get_platform_display(EGL_PLATFORM_GBM_KHR, m_pGbmDevice, NULL));
	if (!m_eglDisplay) {
		LOGERROR("drmdevice: %s: failed to get eglDisplay", __FUNCTION__);
		return -1;
	}

	if (!eglInitialize(m_eglDisplay, &iMajorVersion, &iMinorVersion)) {
		LOGERROR("drmdevice: %s: eglInitialize failed", __FUNCTION__);
		return -1;
	}

	LOGDEBUG2(L_OPENGL, "drmdevice: %s: Using display %p with EGL version %d.%d", __FUNCTION__, m_eglDisplay, iMajorVersion, iMinorVersion);
	EGL_CHECK(LOGDEBUG2(L_OPENGL, "  EGL Version: \"%s\"", eglQueryString(m_eglDisplay, EGL_VERSION)));
	EGL_CHECK(LOGDEBUG2(L_OPENGL, "  EGL Vendor: \"%s\"", eglQueryString(m_eglDisplay, EGL_VENDOR)));
	EGL_CHECK(LOGDEBUG2(L_OPENGL, "  EGL Extensions: \"%s\"", eglQueryString(m_eglDisplay, EGL_EXTENSIONS)));
	EGL_CHECK(LOGDEBUG2(L_OPENGL, "  EGL APIs: \"%s\"", eglQueryString(m_eglDisplay, EGL_CLIENT_APIS)));

	EGLConfig eglConfig = GetEGLConfig();

	EGL_CHECK(eglBindAPI(EGL_OPENGL_ES_API));
	EGL_CHECK(m_eglContext = eglCreateContext(m_eglDisplay, eglConfig, EGL_NO_CONTEXT, context_attribute_list));
	if (!m_eglContext) {
		LOGERROR("drmdevice: %s: failed to create eglContext", __FUNCTION__);
		return -1;
	}

	EGL_CHECK(m_eglSurface = get_platform_surface(m_eglDisplay, eglConfig, m_pGbmSurface, NULL));
	if (m_eglSurface == EGL_NO_SURFACE) {
		LOGERROR("drmdevice: %s: failed to create eglSurface", __FUNCTION__);
		return -1;
	}

	EGLint s_width, s_height;
	EGL_CHECK(eglQuerySurface(m_eglDisplay, m_eglSurface, EGL_WIDTH, &s_width));
	EGL_CHECK(eglQuerySurface(m_eglDisplay, m_eglSurface, EGL_HEIGHT, &s_height));

	LOGDEBUG2(L_OPENGL, "drmdevice: %s: GLSurface %p on EGLDisplay %p for %d x %d BO created", __FUNCTION__, m_eglSurface, m_eglDisplay, s_width, s_height);

	m_glInitiated = true;
	LOGINFO("DRM Setup: EGL context initialized");

	return 0;
}

static void drm_fb_destroy_callback(struct gbm_bo *bo, void *data)
{
	int drm_fd = gbm_device_get_fd(gbm_bo_get_device(bo));
	cDrmBuffer *buf = (cDrmBuffer *)data;

	if (buf->Id())
		drmModeRmFB(drm_fd, buf->Id());

	delete(buf);
}

__attribute__ ((weak)) union gbm_bo_handle
gbm_bo_get_handle_for_plane(struct gbm_bo *bo, int plane);

__attribute__ ((weak)) int
gbm_bo_get_fd(struct gbm_bo *bo);

__attribute__ ((weak)) uint64_t
gbm_bo_get_modifier(struct gbm_bo *bo);

__attribute__ ((weak)) int
gbm_bo_get_plane_count(struct gbm_bo *bo);

__attribute__ ((weak)) uint32_t
gbm_bo_get_stride_for_plane(struct gbm_bo *bo, int plane);

__attribute__ ((weak)) uint32_t
gbm_bo_get_offset(struct gbm_bo *bo, int plane);

/**
 * Get a buffer from a gbm buffer object
 *
 * @param bo        gbm buffer object
 *
 * @returns         a gbm buffer corresponding to the gbm buffer object
 */
cDrmBuffer *cDrmDevice::GetBufFromBo(struct gbm_bo *bo)
{
	cDrmBuffer *buf = (cDrmBuffer *)gbm_bo_get_user_data(bo);
	uint32_t mod_flags = 0;
	int ret = -1;

	// the buffer was already allocated
	if (buf)
		return buf;

	buf = new cDrmBuffer(m_fdDrm, gbm_bo_get_width(bo), gbm_bo_get_height(bo), gbm_bo_get_format(bo), bo);

	if (gbm_bo_get_handle_for_plane && gbm_bo_get_modifier &&
	    gbm_bo_get_plane_count && gbm_bo_get_stride_for_plane &&
	    gbm_bo_get_offset) {
		uint64_t modifiers[4] = {0};
		modifiers[0] = gbm_bo_get_modifier(bo);
		const int num_planes = gbm_bo_get_plane_count(bo);
		buf->SetNumPlanes(num_planes);
		for (int i = 0; i < num_planes; i++) {
			buf->SetHandle(i, gbm_bo_get_handle_for_plane(bo, i).u32);
			buf->SetPitch(i, gbm_bo_get_stride_for_plane(bo, i));
			buf->SetOffset(i, gbm_bo_get_offset(bo, i));
			modifiers[i] = modifiers[0];
			buf->SetSize(i, buf->Height() * buf->Pitch(i));
			LOGDEBUG2(L_DRM, "drmdevice: %s: %d: handle %d pitch %d, offset %d, size %d", __FUNCTION__, i, buf->Handle(i), buf->Pitch(i), buf->Offset(i), buf->Size(i));
		}
		buf->SetNumObjects(1);
		buf->SetObjectIndex(0, 0);
		buf->SetFdPrime(0, gbm_bo_get_fd(bo));

		if (modifiers[0]) {
			mod_flags = DRM_MODE_FB_MODIFIERS;
			LOGDEBUG2(L_DRM, "drmdevice: %s: Using modifier %" PRIx64 "", __FUNCTION__, modifiers[0]);
		}

		uint32_t id;
		// Add FB
		ret = drmModeAddFB2WithModifiers(m_fdDrm, buf->Width(), buf->Height(), buf->PixFmt(),
			buf->Handle(), buf->Pitch(), buf->Offset(), modifiers, &id, mod_flags);
		buf->SetId(id);
	}

	if (ret) {
		if (mod_flags)
			LOGDEBUG2(L_DRM, "drmdevice: %s: Modifiers failed!", __FUNCTION__);

		buf->SetNumPlanes(1);
		uint32_t tmpHandle[4] = { gbm_bo_get_handle(bo).u32, 0, 0, 0};
		uint32_t tmpStride[4] = { gbm_bo_get_stride(bo), 0, 0, 0};
		uint32_t tmpSize[4]   = { buf->Height() * buf->Width() * buf->Pitch(0), 0, 0, 0};
		memcpy(buf->Handle(), tmpHandle, sizeof(tmpHandle));
		memcpy(buf->Pitch(), tmpStride, sizeof(tmpStride));
		memcpy(buf->Size(), tmpSize, sizeof(tmpSize));
		memset(buf->Offset(), 0, 16);
		buf->SetNumObjects(1);
		buf->SetObjectIndex(0, 0);
		buf->SetFdPrime(0, gbm_bo_get_fd(bo));

		uint32_t id;
		ret = drmModeAddFB2(m_fdDrm, buf->Width(), buf->Height(), buf->PixFmt(),
			buf->Handle(), buf->Pitch(), buf->Offset(), &id, 0);
		buf->SetId(id);
	}

	if (ret) {
		LOGFATAL("drmdevice: %s: cannot create framebuffer (%d): %m", __FUNCTION__, errno);
		delete buf;
		return NULL;
	}

	uint32_t pixFmt = buf->PixFmt();
	LOGDEBUG2(L_DRM, "drmdevice: %s: New GL buffer %d x %d pix_fmt %4.4s fb_id %d", __FUNCTION__,
		buf->Width(), buf->Height(), (char *)&pixFmt, buf->Id());

	gbm_bo_set_user_data(bo, buf, drm_fb_destroy_callback);
	return buf;
}
#endif

/**
 * Finds the CRTC_ID for the given encoder
 */
static int32_t FindCrtcForEncoder(const drmModeRes *resources, const drmModeEncoder *encoder)
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

/**
 * Finds the CRTC_ID for the given connector
 */
int32_t cDrmDevice::FindCrtcForConnector(const drmModeRes *resources, const drmModeConnector *connector)
{
	int i;

	for (i = 0; i < connector->count_encoders; i++) {
		const uint32_t encoder_id = connector->encoders[i];
		drmModeEncoder *encoder = drmModeGetEncoder(m_fdDrm, encoder_id);

		if (encoder) {
			const int32_t crtc_id = FindCrtcForEncoder(resources, encoder);
			drmModeFreeEncoder(encoder);
			if (crtc_id != 0) {
				return crtc_id;
			}
		}
	}

	return -1;
}

/**
 * Close drm file handle
 */
void cDrmDevice::Close(void)
{
	close(m_fdDrm);
}

/**
 * Creates a property blob
 */
int cDrmDevice::CreatePropertyBlob(uint32_t *modeID)
{
	return drmModeCreatePropertyBlob(m_fdDrm, &m_drmModeInfo, sizeof(m_drmModeInfo), modeID);
}

/**
 * Add a property to a request
 */
int cDrmDevice::SetPropertyRequest(drmModeAtomicReqPtr ModeReq,
					uint32_t objectID, uint32_t objectType,
					const char *propName, uint64_t value)
{
	uint32_t i;
	uint64_t id = 0;
	drmModePropertyPtr Prop;
	drmModeObjectPropertiesPtr objectProps =
		drmModeObjectGetProperties(m_fdDrm, objectID, objectType);

	for (i = 0; i < objectProps->count_props; i++) {
		if ((Prop = drmModeGetProperty(m_fdDrm, objectProps->props[i])) == NULL)
			LOGDEBUG2(L_DRM, "drmdevice: %s: Unable to query property", __FUNCTION__);

		if (strcmp(propName, Prop->name) == 0) {
			id = Prop->prop_id;
			drmModeFreeProperty(Prop);
			break;
		}

		drmModeFreeProperty(Prop);
	}

	drmModeFreeObjectProperties(objectProps);

	if (id == 0)
		LOGDEBUG2(L_DRM, "drmdevice: %s Unable to find value for property \'%s\'.", __FUNCTION__, propName);

	return drmModeAtomicAddProperty(ModeReq, objectID, id, value);
}

/**
 * Saves information of a CRTC
 */
void cDrmDevice::SaveCrtc(void)
{
	m_drmModeCrtcSaved = drmModeGetCrtc(m_fdDrm, m_crtcId);
}

/**
 * Restore information of a CRTC
 */
void cDrmDevice::RestoreCrtc(void)
{
	if (m_drmModeCrtcSaved) {
		drmModeSetCrtc(m_fdDrm, m_drmModeCrtcSaved->crtc_id, m_drmModeCrtcSaved->buffer_id,
			m_drmModeCrtcSaved->x, m_drmModeCrtcSaved->y, &m_connectorId, 1, &m_drmModeCrtcSaved->mode);
		drmModeFreeCrtc(m_drmModeCrtcSaved);
	}
}

/**
 * Polls for a drm event
 */
int cDrmDevice::HandleEvent(void)
{
	return drmHandleEvent(m_fdDrm, &m_drmEventCtx);
}

/**
 * Init the event context
 */
void cDrmDevice::InitEvent(void)
{
	memset(&m_drmEventCtx, 0, sizeof(m_drmEventCtx));
	m_drmEventCtx.version = 2;
}
