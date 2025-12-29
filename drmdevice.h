/**
 * @file drmdevice.h
 * DRM device header file
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

#ifndef __DRMDEVICE_H
#define __DRMDEVICE_H

#ifndef __USE_GNU
#define __USE_GNU
#endif

#include <stdbool.h>
#include <unistd.h>

#include <inttypes.h>

#include <libintl.h>

#ifdef USE_GLES
#include <assert.h>
#include <EGL/egl.h>
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
}

#include "misc.h"
#include "buf2rgb.h"

//#include "videorender.h"
#include "audio.h"
#include "drm.h"
#include "threads.h"
#include "grab.h"

// save!
#include "drmplane.h"

/*****************************************************************************
 * cDrmDevice class
 ****************************************************************************/

class cDrmDevice
{
public:
	cDrmDevice(cVideoRender *, const char *);
	virtual ~cDrmDevice(void);

	int Init(void);
	int Fd(void) { return m_fdDrm; };
	void Close(void);

	// setters and getters
	uint32_t ConnectorId(void) { return m_connectorId; };

	uint64_t DisplayWidth(void) { return m_drmModeInfo.hdisplay; };
	uint64_t DisplayHeight(void) { return m_drmModeInfo.vdisplay; };

	uint32_t CrtcId(void) { return m_crtcId; };
	int UseZpos(void) { return m_useZpos; };
	uint64_t ZposOverlay(void) { return m_zposOverlay; };
	uint64_t ZposPrimary(void) { return m_zposPrimary; };

	cDrmPlane *OsdPlane(void) { return &m_osdPlane; };
	cDrmPlane *VideoPlane(void) { return &m_videoPlane; };
	cDrmPlane *PipPlane(void) { return &m_pipPlane; };
	bool HasPipPlane(void) { return m_pipPlane.GetId(); };

#ifdef USE_GLES
	EGLSurface EglSurface(void) { return m_eglSurface; };
	EGLDisplay EglDisplay(void) { return m_eglDisplay; };
	EGLContext EglContext(void) { return m_eglContext; };
	int GlInitiated(void) { return m_glInitiated; };
	struct gbm_surface *GbmSurface(void) { return m_pGbmSurface; };

	cDrmBuffer *GetBufFromBo(struct gbm_bo *);
#endif
	int SetPropertyRequest(drmModeAtomicReqPtr, uint32_t, uint32_t, const char *, uint64_t);
	void SaveCrtc(void);
	void RestoreCrtc(void);
	int HandleEvent(void);
	int CreatePropertyBlob(uint32_t *);
	void InitEvent(void);

private:
	cVideoRender *m_pRender;               ///< pointer to cVideoRender object

	int m_fdDrm;                           ///< drm file descriptor
	uint32_t m_connectorId;                ///< connector id
	drmModeModeInfo m_drmModeInfo;         ///< mode info
	uint32_t m_crtcId;                     ///< current crtc ID
	uint32_t m_crtcIndex;                  ///< current crtc index
	drmModeCrtc *m_drmModeCrtcSaved;       ///< saved CRTC infos
	drmEventContext m_drmEventCtx;         ///< drm event context

	int m_userReqDisplayWidth;             ///< user requested display width
	int m_userReqDisplayHeight;            ///< user requested display height
	uint32_t m_userReqDisplayRefreshRate;  ///< user requested display refresh rate

	bool m_useZpos;                        ///< is set, if drm hardware can use zpos
	uint64_t m_zposOverlay = 0;            ///< zpos of overlay plane
	uint64_t m_zposPrimary = 0;            ///< zpos of primary plane
	cDrmPlane m_videoPlane;                ///< the video drm plane
	cDrmPlane m_osdPlane;                  ///< the osd drm plane
	uint64_t m_zposPip = 0;                ///< zpos of pip plane
	cDrmPlane m_pipPlane;                  ///< the pip drm plane

	int32_t FindCrtcForConnector(const drmModeRes *, const drmModeConnector *);
#ifdef USE_GLES
	struct gbm_device *m_pGbmDevice;       ///< pointer to the gbm device
	struct gbm_surface *m_pGbmSurface;     ///< pointer to the gbm surface

	EGLSurface m_eglSurface;               ///< EGL surface
	EGLDisplay m_eglDisplay;               ///< EGL display
	EGLContext m_eglContext;               ///< EGL context
	bool m_glInitiated;                    ///< true, if OpenGL/ES context is initiated

	int InitEGL(void);
	EGLConfig GetEGLConfig(void);
	int InitGbm(int, int, uint32_t, uint64_t);
#endif
};

#endif