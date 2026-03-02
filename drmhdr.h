/**
 * @file drmhdr.h
 * HDR class header file
 *
 * This code is mostly taken from https://github.com/jojo61/vdr-plugin-softhdcuvid
 * and it seems this was at least inspired by libweston (https://gitlab.freedesktop.org/wayland/weston
 * which is published under the MIT license.
 *
 * @copyright (c) 2026 by Andreas Baierl. All Rights Reserved.
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

#ifndef __DRMHDR_H
#define __DRMHDR_H

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mastering_display_metadata.h>
}

#include <drm_mode.h>

/*****************************************************************************
 * color spaces
 ****************************************************************************/
struct vector {
	float f[4];
};

struct colorspace {
	struct vector r, g, b;
	struct vector whitepoint;
	const char *name;
	const char *whitepoint_name;
};

static struct colorspace bt709 = {
	.r =          {{ 0.640f,  0.330f,  }},
	.g =          {{ 0.300f,  0.600f,  }},
	.b =          {{ 0.150f,  0.060f,  }},
	.whitepoint = {{ 0.3127f, 0.3290f, }},
	.name = "BT.709",
	.whitepoint_name = "D65",
};

static struct colorspace bt2020 = {
	.r =          {{ 0.708f,  0.292f,  }},
	.g =          {{ 0.170f,  0.797f,  }},
	.b =          {{ 0.131f,  0.046f,  }},
	.whitepoint = {{ 0.3127f, 0.3290f, }},
	.name = "BT.2020",
	.whitepoint_name = "D65",
};


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

/*****************************************************************************
 * cH264Parser class
 ****************************************************************************/
class cVideoRender;

class cHdrMetadata
{
public:
	cHdrMetadata(cVideoRender *render) : m_pRender(render) { };
	bool Build(struct hdr_output_metadata *,int, int, AVFrameSideData *, AVFrameSideData *);
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
