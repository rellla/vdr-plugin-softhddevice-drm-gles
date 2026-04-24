// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file drmdevice.h
 * DRM Device Header File
 *
 * @copyright 2018 by zille.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __DRMDEVICE_H
#define __DRMDEVICE_H

#include <array>
#include <cstdint>

#ifdef USE_GLES
#include <EGL/egl.h>
#endif

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "config.h"
#include "drmplane.h"

#ifdef USE_GLES
class cDrmBuffer;
#endif

class cVideoRender;

/**
 * DRM Display Interface
 * @defgroup drm DRM Module
 */

/**
 * Whitelist of possible drm modes
 *
 * @ingroup drm
 */
inline constexpr std::array DrmModeWhitelist = {
	// 2160p (4K)
	sDrmMode{ 3840, 2160, 60.00, false },
	sDrmMode{ 3840, 2160, 59.94, false },
	sDrmMode{ 3840, 2160, 50.00, false },
	sDrmMode{ 3840, 2160, 30.00, false  },
	sDrmMode{ 3840, 2160, 29.97, false  },
	sDrmMode{ 3840, 2160, 25.00, false  },
	sDrmMode{ 3840, 2160, 24.00, false  },
	sDrmMode{ 3840, 2160, 23.98, false  },

	// 1080p (FullHD progressive)
	sDrmMode{ 1920, 1080, 60.00, false },
	sDrmMode{ 1920, 1080, 59.94, false },
	sDrmMode{ 1920, 1080, 50.00, false },
	sDrmMode{ 1920, 1080, 30.00, false },
	sDrmMode{ 1920, 1080, 29.97, false },
	sDrmMode{ 1920, 1080, 25.00, false },
	sDrmMode{ 1920, 1080, 24.00, false },
	sDrmMode{ 1920, 1080, 23.98, false },

	// 1080i (FullHD interlaced)
	sDrmMode{ 1920, 1080, 30.00, true  }, // 1928x1080@60i
	sDrmMode{ 1920, 1080, 29.97, true  }, // 1928x1080@59.94i
	sDrmMode{ 1920, 1080, 25.00, true  }, // 1928x1080@50i

	// 720p (HDready)
	sDrmMode{ 1280, 720, 60.00,  false },
	sDrmMode{ 1280, 720, 59.94,  false },
	sDrmMode{ 1280, 720, 50.00,  false },
};

/**
 * DRM Device
 *
 * @ingroup drm
 */
class cDrmDevice {
public:
	cDrmDevice(cVideoRender *, cSoftHdConfig *);
	~cDrmDevice(void);

	int Init(void);
	int ReInit(void);
	int InitGbm(void);
	int InitEGL(void);
	int Fd(void) { return m_fdDrm; };
	void Close(void);

	// setters and getters
	uint32_t ConnectorId(void) { return m_connectorId; };

	uint64_t DisplayWidth(void) { return m_drmModeInfo.hdisplay; };
	uint64_t DisplayHeight(void) { return m_drmModeInfo.vdisplay; };
	uint64_t OsdWidth(void) { return m_userReqOsdWidth ? m_userReqOsdWidth : DisplayWidth(); };
	uint64_t OsdHeight(void) { return m_userReqOsdWidth ? m_userReqOsdHeight : DisplayHeight(); };

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
	void SaveCrtc(void);
	void RestoreCrtc(void);
	int HandleEvent(void);
	void InitEvent(void);

	bool CanHandleHdr(void) { return m_hdrMetadata != 0; };
	bool CanHandleMode(sDrmMode *);

	// drmModeAtomic* wrapper functions
	drmModeAtomicReqPtr ModeAtomicAlloc(void) { return drmModeAtomicAlloc(); };
	int ModeAtomicCommit(drmModeAtomicReqPtr req, uint32_t flags, void *user_data) { return drmModeAtomicCommit(m_fdDrm, req, flags, user_data); };
	void ModeAtomicFree(drmModeAtomicReqPtr req) { drmModeAtomicFree(req); };
	int SetConnectorCrtcId(drmModeAtomicReqPtr);
	int SetConnectorHdrOutputMetadata(drmModeAtomicReqPtr, uint32_t);
	int SetConnectorColorspace(drmModeAtomicReqPtr, uint32_t);
	int SetVideoPlaneColorEncoding(drmModeAtomicReqPtr, uint32_t);
	int SetVideoPlaneColorRange(drmModeAtomicReqPtr, uint32_t);
	int GetVideoPlaneColorRange(uint64_t *);
	int SetCrtcModeId(drmModeAtomicReqPtr, uint32_t);
	int SetCrtcActive(drmModeAtomicReqPtr, uint32_t);
	int CreateModeBlob(uint32_t *);
	int DestroyModeBlob(uint32_t);
	int CreateHdrBlob(struct hdr_output_metadata *, size_t, uint32_t *);
	int SetConnectorHdrBlobProperty(uint32_t);
	int DestroyHdrBlob(uint32_t);

private:
	cVideoRender *m_pRender;               ///< pointer to cVideoRender object
	cSoftHdConfig *m_pConfig;              ///< pointer to cSoftHdConfig object

	int m_fdDrm = -1;                      ///< drm file descriptor
	uint32_t m_connectorId;                ///< connector id
	const char *m_connectorName = nullptr; ///< drm connector name
	drmModeModeInfo m_drmModeInfo;         ///< mode info
	uint32_t m_crtcId;                     ///< current crtc ID
	uint32_t m_crtcIndex;                  ///< current crtc index
	uint32_t m_hdrMetadata = 0;            ///< property id of HDR_OUTPUT_METADATA
	drmModeCrtc *m_drmModeCrtcSaved;       ///< saved CRTC infos
	drmEventContext m_drmEventCtx;         ///< drm event context

	const char *m_userDrmDevice = nullptr; ///< user requested drm device
	const char *m_userDrmConnector = nullptr; ///< user requested drm connector
	int m_userReqDisplayWidth = 0;         ///< user requested display width
	int m_userReqDisplayHeight;            ///< user requested display height
	double m_userReqDisplayRefreshRate;    ///< user requested display refresh rate
	bool m_userReqDisplayInterlaced;       ///< user requested display interlaced mode
	int m_userReqOsdWidth = 0;             ///< user requested osd width
	int m_userReqOsdHeight;                ///< user requested osd height

	bool m_useZpos = false;                ///< is set, if drm hardware can use zpos
	uint64_t m_zposOverlay = 0;            ///< zpos of overlay plane
	uint64_t m_zposPrimary = 0;            ///< zpos of primary plane
	cDrmPlane m_videoPlane;                ///< the video drm plane
	cDrmPlane m_osdPlane;                  ///< the osd drm plane
	uint64_t m_zposPip = 0;                ///< zpos of pip plane
	cDrmPlane m_pipPlane;                  ///< the pip drm plane

	int CreatePropertyBlob(uint32_t *);
	int GetPropertyValue(uint32_t, uint32_t, const char *, uint64_t *);
	uint32_t GetPropertyID(uint32_t, uint32_t, const char *);

	int SetPropertyRequest(drmModeAtomicReqPtr, uint32_t, uint32_t, const char *, uint64_t);
	drmModeConnector *FindDrmConnector(int, drmModeRes *, const char *);
	int32_t FindCrtcForConnector(const drmModeRes *, const drmModeConnector *);
	int FindMode(void);
#ifdef USE_GLES
	struct gbm_device *m_pGbmDevice;       ///< pointer to the gbm device
	struct gbm_surface *m_pGbmSurface;     ///< pointer to the gbm surface

	EGLSurface m_eglSurface;               ///< EGL surface
	EGLDisplay m_eglDisplay;               ///< EGL display
	EGLContext m_eglContext;               ///< EGL context
	bool m_glInitiated;                    ///< true, if OpenGL/ES context is initiated

	EGLConfig GetEGLConfig(void);
#endif
};

#endif
