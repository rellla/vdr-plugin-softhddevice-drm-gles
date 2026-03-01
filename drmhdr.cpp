/**
 * @file drmhdr.cpp
 * HDR class
 *
 * This code is mostly taken from https://github.com/jojo61/vdr-plugin-softhdcuvid
 * and it seems this was at least inspired by libweston (https://gitlab.freedesktop.org/wayland/weston
 * which is published under the MIT license.
 *
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

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mastering_display_metadata.h>
}

#include <drm_mode.h>

#include "drmhdr.h"
#include "logger.h"
#include "videorender.h"

enum hdrMetadataEotf {
	EOTF_TRADITIONAL_GAMMA_SDR,
	EOTF_TRADITIONAL_GAMMA_HDR,
	EOTF_ST2084,
	EOTF_HLG,
};

enum metadataId {
	METADATA_TYPE1,
};

static inline uint16_t EncodeXYY(float xyy)
{
	return static_cast<uint16_t>(xyy * 50000.0f + 0.5f);
}

/*****************************************************************************
 * cHdrMetadata class
 ****************************************************************************/
cHdrMetadata::cHdrMetadata(cVideoRender *render)
	: m_pRender(render)
{
}

bool cHdrMetadata::Build(struct hdr_output_metadata *data, int colorPrimaries, int colorTrc, AVFrameSideData *sd1, AVFrameSideData *sd2)
{
	if (!m_pRender->CanHandleHdr())
		return -1;

	// @todo: check, what this does
	// clean up FFMEPG stuff
	if (colorTrc == AVCOL_TRC_BT2020_10)
		colorTrc = AVCOL_TRC_ARIB_STD_B67;

	if (m_colorPrimaries == colorPrimaries && m_colorTrc == colorTrc && !sd1 && !sd2)
		return -1; // nothing to do

	AVMasteringDisplayMetadata *md = nullptr;
	if (sd1)
		md = reinterpret_cast<AVMasteringDisplayMetadata *>(sd1->data);

	AVContentLightMetadata *ld = nullptr;
	if (sd2)
		ld = reinterpret_cast<AVContentLightMetadata *>(sd2->data);

	if (md && !memcmp(md, &m_mdSave, sizeof(m_mdSave))) {
		if (ld && !memcmp(ld, &m_ldSave, sizeof(m_ldSave))) {
			return -1;
		}
	} else if (ld && !memcmp(ld, &m_ldSave, sizeof(m_ldSave))) {
		return -1;
	}

	if (ld)
		memcpy(&m_ldSave, ld, sizeof(m_ldSave));
	if (md)
		memcpy(&m_mdSave, md, sizeof(m_mdSave));

	LOGDEBUG2(L_DRM, "HDR %s: Update HDR to TRC %d color %d", __FUNCTION__, colorTrc, colorPrimaries);

	m_colorPrimaries = colorPrimaries;
	m_colorTrc = colorTrc;

	enum hdrMetadataEotf eotf;
	switch (colorTrc) {
		case AVCOL_TRC_BT2020_10:    // 14
		case AVCOL_TRC_BT2020_12:
		case AVCOL_TRC_ARIB_STD_B67: // 18 HLG
			eotf = EOTF_HLG;
			break;
		case AVCOL_TRC_SMPTE2084:    // 16
			eotf = EOTF_ST2084;
			break;
		case AVCOL_TRC_BT709:        // 1
		case AVCOL_TRC_UNSPECIFIED:  // 2
		default:
			eotf = EOTF_TRADITIONAL_GAMMA_SDR;
			break;
	}

	struct colorspace *cs;
	switch (colorPrimaries) {
		case AVCOL_PRI_BT2020:       // 9
			cs = colorspace_lookup("BT.2020");
			break;
		case AVCOL_PRI_BT470BG:      // 5
			cs = colorspace_lookup("BT.470 B/G"); // BT.601
			break;
		case AVCOL_PRI_BT709:        // 1
		case AVCOL_PRI_UNSPECIFIED:  // 2
		default:
			cs = colorspace_lookup("BT.709");
			break;
	}

	int maxLum = 4000;
	int minLum = 0050;
	if (md) { // we got Metadata
		if (md->has_primaries) {
			LOGDEBUG2(L_DRM, "HDR %s: Mastering Display Metadata:", __FUNCTION__);
			LOGDEBUG2(L_DRM, "HDR %s: has_primaries: %d has_luminance: %d", __FUNCTION__,
			          md->has_primaries, md->has_luminance);
			LOGDEBUG2(L_DRM, "HDR %s: r(%5.4f,%5.4f) g(%5.4f,%5.4f) b(%5.4f %5.4f) wp(%5.4f, %5.4f)", __FUNCTION__,
			          av_q2d(md->display_primaries[0][0]), av_q2d(md->display_primaries[0][1]), av_q2d(md->display_primaries[1][0]),
			          av_q2d(md->display_primaries[1][1]), av_q2d(md->display_primaries[2][0]), av_q2d(md->display_primaries[2][1]),
			          av_q2d(md->white_point[0]), av_q2d(md->white_point[1]));
			LOGDEBUG2(L_DRM, "HDR %s: min_luminance= %f, max_luminance= %f", __FUNCTION__,
			          av_q2d(md->min_luminance), av_q2d(md->max_luminance));

			cs = &m_hdr10;
			cs->r.f[0] = (float)md->display_primaries[0][0].num / (float)md->display_primaries[0][0].den;
			cs->r.f[1] = (float)md->display_primaries[0][1].num / (float)md->display_primaries[0][1].den;
			cs->g.f[0] = (float)md->display_primaries[1][0].num / (float)md->display_primaries[1][0].den;
			cs->g.f[1] = (float)md->display_primaries[1][1].num / (float)md->display_primaries[1][1].den;
			cs->b.f[0] = (float)md->display_primaries[2][0].num / (float)md->display_primaries[2][0].den;
			cs->b.f[1] = (float)md->display_primaries[2][1].num / (float)md->display_primaries[2][1].den;
			cs->whitepoint.f[0] = (float)md->white_point[0].num / (float)md->white_point[0].den;
			cs->whitepoint.f[1] = (float)md->white_point[1].num / (float)md->white_point[1].den;
		}
		if (md->has_luminance) {
			maxLum = static_cast<uint16_t>(av_q2d(md->max_luminance) + 0.5);
			minLum = static_cast<uint16_t>(av_q2d(md->min_luminance) * 10000 + 0.5);
			LOGDEBUG2(L_DRM, "HDR %s: maxLum %d minLum %d", __FUNCTION__, maxLum, minLum);
		}
	}

	int maxCLL = 1500;
	int maxFALL = 400;
	if (ld) {
		maxCLL = ld->MaxCLL;
		maxFALL = ld->MaxFALL;
		LOGDEBUG2(L_DRM, "HDR %s: Has maxCLL %d maxFALL %d", __FUNCTION__, maxCLL, maxFALL);
	}

	data->metadata_type = METADATA_TYPE1;
	data->hdmi_metadata_type1.eotf = eotf;
	data->hdmi_metadata_type1.metadata_type = METADATA_TYPE1;
	data->hdmi_metadata_type1.display_primaries[0].x = EncodeXYY(cs->r.f[0]);
	data->hdmi_metadata_type1.display_primaries[0].y = EncodeXYY(cs->r.f[1]);
	data->hdmi_metadata_type1.display_primaries[1].x = EncodeXYY(cs->g.f[0]);
	data->hdmi_metadata_type1.display_primaries[1].y = EncodeXYY(cs->g.f[1]);
	data->hdmi_metadata_type1.display_primaries[2].x = EncodeXYY(cs->b.f[0]);
	data->hdmi_metadata_type1.display_primaries[2].y = EncodeXYY(cs->b.f[1]);
	data->hdmi_metadata_type1.white_point.x = EncodeXYY(cs->whitepoint.f[0]);
	data->hdmi_metadata_type1.white_point.y = EncodeXYY(cs->whitepoint.f[1]);
	data->hdmi_metadata_type1.max_display_mastering_luminance = maxLum;
	data->hdmi_metadata_type1.min_display_mastering_luminance = minLum;
	data->hdmi_metadata_type1.max_cll = maxCLL;
	data->hdmi_metadata_type1.max_fall = maxFALL;

	LOGDEBUG2(L_DRM, "HDR %s: blob metadata set at %p", __FUNCTION__, data);

	return 0;
}
