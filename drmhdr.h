// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file drmhdr.h
 * HDR (High Dynamic Range) Header File
 *
 * This code is mostly taken from https://github.com/jojo61/vdr-plugin-softhdcuvid
 * and it seems this was at least inspired by libweston (https://gitlab.freedesktop.org/wayland/weston
 * which is published under the MIT license.
 *
 * @copyright 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __DRMHDR_H
#define __DRMHDR_H

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mastering_display_metadata.h>
}

#include <drm_mode.h>

class cVideoRender;

/**
 * @addtogroup drm
 * @{
 */

struct vector {
	float f[4];
};

struct colorspace {
	struct vector r, g, b;
	struct vector whitepoint;
	const char *name;
	const char *whitepoint_name;
};

/**
 * BT709 Color Space
 */
static struct colorspace bt709 = {
	.r =          {{ 0.640f,  0.330f,  }},
	.g =          {{ 0.300f,  0.600f,  }},
	.b =          {{ 0.150f,  0.060f,  }},
	.whitepoint = {{ 0.3127f, 0.3290f, }},
	.name = "BT.709",
	.whitepoint_name = "D65",
};

/**
 * BT2020 Color Space
 */
static struct colorspace bt2020 = {
	.r =          {{ 0.708f,  0.292f,  }},
	.g =          {{ 0.170f,  0.797f,  }},
	.b =          {{ 0.131f,  0.046f,  }},
	.whitepoint = {{ 0.3127f, 0.3290f, }},
	.name = "BT.2020",
	.whitepoint_name = "D65",
};


/**
 * BT470bg Color Space
 */
static struct colorspace bt470bg = {
	.r =          {{ 0.640f,  0.330f,  }},
	.g =          {{ 0.290f,  0.600f,  }},
	.b =          {{ 0.150f,  0.060f,  }},
	.whitepoint = {{ 0.3127f, 0.3290f, }},
	.name = "BT.470 B/G",
	.whitepoint_name = "D65",
};

static struct colorspace *const colorspaces[] = {
	&bt709, &bt2020, &bt470bg,
};

#define ARRAY_LENGTH(a) (sizeof(a) / sizeof(a)[0])
static inline struct colorspace *colorspace_lookup(const char *name)
{
    unsigned i;

    if (!name)
        return NULL;

    for (i = 0; i < ARRAY_LENGTH(colorspaces); i++) {
        struct colorspace *c = colorspaces[i];
        if (!strcmp(c->name, name))
            return c;
    }

    return NULL;
}

/** @} */

/**
 * HDR Metadata
 *
 * @ingroup drm
 */
class cHdrMetadata {
public:
	cHdrMetadata(cVideoRender *render) : m_pRender(render) { };
	int Build(struct hdr_output_metadata *,int, int, AVFrameSideData *, AVFrameSideData *);
	int GetColorPrimaries(void) { return m_colorPrimaries; };
	int GetColorTrc(void) { return m_colorTrc; };

private:
	cVideoRender *m_pRender;                       ///< pointer to cVideoRender object
	int m_colorPrimaries = -1;                     ///< saved color primaries
	int m_colorTrc = -1;                           ///< saved transfer charateristics
	AVMasteringDisplayMetadata m_mdMetadata = { }; ///< saved mastering display metadata fron AVFrame sidedata
	AVContentLightMetadata m_clMetadata = { };     ///< saved content light metadata fron AVFrame sidedata
	struct colorspace m_hdr10;                     ///< hdr colorspace
};

#endif
