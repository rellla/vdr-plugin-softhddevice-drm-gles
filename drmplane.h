/**
 * @file drmplane.h
 * DRM plane class header
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

#ifndef __DRMPLANE_H
#define __DRMPLANE_H

#include <xf86drm.h>
#include <xf86drmMode.h>

/**
 * cDrmPlane - DRM plane class
 */
class cDrmPlane {
public:
	cDrmPlane(void);
	virtual ~cDrmPlane(void);
	void SetParams(uint64_t, uint64_t,
	               uint64_t, uint64_t, uint64_t, uint64_t,
	               uint64_t, uint64_t, uint64_t, uint64_t);
	void DumpParameters(void);
	void FillProperties(int);
	void FreeProperties(void);
	void SetPlaneZpos(drmModeAtomicReqPtr);
	void SetPlane(drmModeAtomicReqPtr);
	int HasZpos(int);

	// getters and setters
	uint32_t GetId(void) { return m_planeId; };
	void SetId(uint32_t id) { m_planeId = id; };
	uint64_t GetType(void) { return m_type; };
	void SetType(uint64_t type) { m_type = type; };
	uint64_t GetCrtcId(void) { return m_crtcId; };
	uint64_t GetFbId(void) { return m_fbId; };
	uint64_t GetCrtcX(void) { return m_crtcX; };
	uint64_t GetCrtcY(void) { return m_crtcY; };
	uint64_t GetCrtcW(void) { return m_crtcW; };
	uint64_t GetCrtcH(void) { return m_crtcH; };
	uint64_t GetSrcX(void) { return m_srcX; };
	uint64_t GetSrcY(void) { return m_srcY; };
	uint64_t GetSrcW(void) { return m_srcW; };
	uint64_t GetSrcH(void) { return m_srcH; };
	uint64_t GetZpos(void) { return m_zpos; };
	void SetZpos(uint64_t zpos) { m_zpos = zpos; };

	int GetCountProps(void) { return m_props->count_props; };
	char *GetPropsInfoName(int prop) { return m_propsInfo[prop]->name; };
	uint32_t GetPropsInfoPropId(int prop) { return m_propsInfo[prop]->prop_id; };
	drmModeObjectProperties *GetProps(void) { return m_props; };
	void SetProps(drmModeObjectProperties *props) { m_props = props; };
	drmModePropertyRes **GetPropsInfo(void) { return m_propsInfo; };
	drmModePropertyRes **GetPropsInfoElem(int elem) { return &m_propsInfo[elem]; };

private:
	uint32_t m_planeId;                 ///< the plane's ID
	uint64_t m_type;                    ///< type: DRM_PLANE_TYPE_PRIMARY or
	                                    ///<       DRM_PLANE_TYPE_OVERLAY
	drmModeObjectProperties *m_props;
	drmModePropertyRes **m_propsInfo;

	// The modesetting parameters for a drm commit
	uint64_t m_crtcId;                ///< CRTC_ID
	uint64_t m_fbId;                  ///< FB_ID
	uint64_t m_crtcX;                 ///< CRTC_X
	uint64_t m_crtcY;                 ///< CRTC_Y
	uint64_t m_crtcW;                 ///< CRTC_W
	uint64_t m_crtcH;                 ///< CRTC_H
	uint64_t m_srcX;                  ///< SRC_X
	uint64_t m_srcY;                  ///< SRC_Y
	uint64_t m_srcW;                  ///< SRC_W
	uint64_t m_srcH;                  ///< SRC_H
	uint64_t m_zpos;                  ///< ZPOS

	int SetPropertyRequest(drmModeAtomicReqPtr, const char *, uint64_t);
};

#endif
