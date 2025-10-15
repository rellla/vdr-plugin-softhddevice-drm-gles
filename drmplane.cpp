/**
 * @file plane.h
 * DRM plane class
 *
 * This file defines cDrmPlane, which is a class to describe
 * planes, that are used for modesetting in the DRM system.
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
 * GNU Affero General Public License for more details.\
 */

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <cinttypes>

#include "drmplane.h"
#include "logger.h"

/*****************************************************************************
 * cDrmPlane class
 ****************************************************************************/

cDrmPlane::cDrmPlane(void)
{
	m_planeId = 0;
	m_zpos = 0;
}

cDrmPlane::~cDrmPlane(void)
{
}

/**
 * Fill the plane properties
 *
 * This "caches" the properties within the class
 *
 * @param fd             drm file descriptor
 */
void cDrmPlane::FillProperties(int fd)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, GetId(), DRM_MODE_OBJECT_PLANE);
	if (!props) {
		LOGERROR("drmplane: %s: could not get %u properties: %s", GetId(), __FUNCTION__, strerror(errno));
		return;
	}

	SetProps(props);

	m_propsInfo = (drmModePropertyRes **)calloc(m_props->count_props, sizeof(*m_propsInfo));
	for (uint32_t i = 0; i < m_props->count_props; i++) {
		m_propsInfo[i] = drmModeGetProperty(fd, m_props->props[i]);
	}
}

/**
 * Free the previously filled plane properties
 */
void cDrmPlane::FreeProperties(void)
{
	if (!GetProps())
		return;

	if (!GetPropsInfo())
		return;

	for (uint32_t i = 0; i < m_props->count_props; i++) {
		if (m_propsInfo[i])
			drmModeFreeProperty(m_propsInfo[i]);

	}
	drmModeFreeObjectProperties(m_props);
	free(m_propsInfo);
}

/**
 * Set the modesetting parameters of a plane
 *
 * These values are used for drm modesetting
 *
 * @param crtcId       Mode object ID of the crtc
 * @param fbId         Mode object ID of the drm framebuffer
 * @param crtcX        X offset of the destination rect
 * @param crtcY        Y offset of the destination rect
 * @param crtcW        width of the destination rect
 * @param crtcH        height of the destination rect
 * @param srcX         X offset of the source rect
 * @param srcY         Y offset of the source rect
 * @param srcW         width of the source rect
 * @param srccH        height of the source rect
 */
void cDrmPlane::SetParams(uint64_t crtcId, uint64_t fbId,
                          uint64_t crtcX, uint64_t crtcY, uint64_t crtcW, uint64_t crtcH,
                          uint64_t srcX,  uint64_t srcY,  uint64_t srcW,  uint64_t srcH)
{
	m_crtcId = crtcId;
	m_fbId = fbId;
	m_crtcX = crtcX;
	m_crtcY = crtcY;
	m_crtcW = crtcW;
	m_crtcH = crtcH;
	m_srcX = srcX;
	m_srcY = srcY;
	m_srcW = srcW;
	m_srcH = srcH;
}

/**
 * Add the properties to the mode setting request
 *
 * @param ModeReq      pointer to the atomic mode request
 * @param propName     name of the property to set
 * @param value        property value
 */
int cDrmPlane::SetPropertyRequest(drmModeAtomicReqPtr ModeReq, const char *propName, uint64_t value)
{
	int id = -1;

	for (int i = 0; i < GetCountProps(); i++) {
		if (strcmp(GetPropsInfoName(i), propName) == 0) {
			id = GetPropsInfoPropId(i);
			break;
		}
	}

	if (id < 0) {
		LOGERROR("drmplane: %s: Unable to find value for property \'%s\'.",
			__FUNCTION__, propName);
		return -EINVAL;
	}

	return drmModeAtomicAddProperty(ModeReq, GetId(), id, value);
}

/**
 * Set the plane zpos property
 *
 * @param ModeReq       pointer to the atomic mode request
 */
void cDrmPlane::SetPlaneZpos(drmModeAtomicReqPtr ModeReq)
{
	SetPropertyRequest(ModeReq, "zpos", GetZpos());
}

/**
 * Set all other plane properties
 *
 * @param ModeReq        pointer to the atomic mode request
 */
void cDrmPlane::SetPlane(drmModeAtomicReqPtr ModeReq)
{
	SetPropertyRequest(ModeReq, "CRTC_ID", GetCrtcId());
	SetPropertyRequest(ModeReq, "FB_ID",   GetFbId());

	SetPropertyRequest(ModeReq, "CRTC_X",  GetCrtcX());
	SetPropertyRequest(ModeReq, "CRTC_Y",  GetCrtcY());
	SetPropertyRequest(ModeReq, "CRTC_W",  GetCrtcW());
	SetPropertyRequest(ModeReq, "CRTC_H",  GetCrtcH());

	SetPropertyRequest(ModeReq, "SRC_X",   GetSrcX());
	SetPropertyRequest(ModeReq, "SRC_Y",   GetSrcY());
	SetPropertyRequest(ModeReq, "SRC_W",   GetSrcW() << 16);
	SetPropertyRequest(ModeReq, "SRC_H",   GetSrcH() << 16);
}

/**
 * Check, if the plane is able to set the zpos property
 *
 * @param fdDrm     drm file descriptor
 *
 * @returns 1       plane can use zpos
 * @returns 0       plane can't use zpos
 */
int cDrmPlane::HasZpos(int fdDrm)
{
	drmModeAtomicReqPtr ModeReq;
	const uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;

	if (!(ModeReq = drmModeAtomicAlloc())) {
		LOGERROR("drmplane: %s: cannot allocate atomic request (%d): %m", __FUNCTION__, errno);
		return 0;
	}

	SetPlaneZpos(ModeReq);

	if (drmModeAtomicCommit(fdDrm, ModeReq, flags, NULL) != 0) {
		LOGDEBUG2(L_DRM, "drmplane: %s: cannot set atomic mode (%d), don't use zpos change: %m",
			__FUNCTION__, errno);
		drmModeAtomicFree(ModeReq);
		return 0;
	}

	drmModeAtomicFree(ModeReq);

	return 1;
}

/**
 * Dump the plane parameter modesetting values
 */
void cDrmPlane::DumpParameters(void)
{
	LOGERROR("DumpParameters (plane_id = %d):", GetId());
	LOGERROR("  CRTC ID: %" PRIu64 "",          GetCrtcId());
	LOGERROR("  FB ID  : %" PRIu64 "",          GetFbId());
	LOGERROR("  CRTC X : %" PRIu64 "",          GetCrtcX());
	LOGERROR("  CRTC Y : %" PRIu64 "",          GetCrtcY());
	LOGERROR("  CRTC W : %" PRIu64 "",          GetCrtcW());
	LOGERROR("  CRTC H : %" PRIu64 "",          GetCrtcH());
	LOGERROR("  SRC X  : %" PRIu64 "",          GetSrcX());
	LOGERROR("  SRC Y  : %" PRIu64 "",          GetSrcY());
	LOGERROR("  SRC W  : %" PRIu64 "",          GetSrcW());
	LOGERROR("  SRC H  : %" PRIu64 "",          GetSrcH());
	LOGERROR("  ZPOS   : %" PRIu64 "",          GetZpos());
}
